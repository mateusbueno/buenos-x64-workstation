#pragma once
// Stream manager (Story 1.17, FR1 / AR20 / AR13 partial / NFR2 partial / NFR17): the host side of streaming a sample's tail from the SSD ahead of the playhead.
//
//   RT thread (the engine)         I/O edge threads (here)                       the SSD
//   StreamService::open  --------> notification queue (MPMC, bounded) --> workers --> BlockSource::read   (blocking pread + decode)
//   reads the ring, publishes <--- fills the ring buffer, publishes progress <---/
//   consumed_lo, close
//
// The RT thread only pushes small announcement records (from start_voice, the serial phase; render workers only close() and store seek words), reads ring buffers and stores atomics: no syscall, lock, allocation or wait. Everything slow (reads, decoding, thread
// scheduling) is on the I/O threads, which are not real time and run below the audio thread (ports::ThreadPriority).
//
// Ring pool. `ring_count` rings are allocated once, at construction, in ONE locked, pre-faulted arena (HeadBuffer): the pool is bounded and nothing is allocated on the audio
// thread. `ring_count` is the voice capacity of the engine. Ring size (frames of the SAMPLE, a ring is addressed by sample frame, slot = frame % ring_frames):
//
//   lookahead   = ceil((p99.99 + margin) * sample_rate * max_pitch_ratio) + ceil(block_frames * max_pitch_ratio) + guard
//                 (what a voice consumes while one read is outstanding, plus the frames one engine block reads beyond the position it last published; block_frames = 64 = core::kBlockFrames)
//   ring_frames = round_up(max(2 * chunk_frames + lookahead, 3 * chunk_frames), 64)    (one chunk being filled, one being consumed, and the latency; never under 3 chunks)
//
// i.e. the head sizing's latency term plus two read chunks. The bytes are ring_count * ring_frames * 2 channels * 4 and are a stream buffer budget of their own (StreamConfig::
// budget_bytes, reported by buffer_bytes()), separate from the head budget of the tier profile (head_budget_bytes counts heads and resident loop segments only).
//
// Lifecycle of a ring (state): kFree -> kReserved (RT claims it) -> kActive (RT published the stream, a request is queued) -> kDraining (RT gave it back) -> kReserved ...
// ABA safety: a ring is reused only when `io_busy` is 0. An I/O thread sets io_busy (seq_cst), THEN checks that the ring is kActive in the epoch of its job, and only then
// reads or writes the ring or its source; it clears io_busy when it is done. A fill that arrives after the voice ended therefore either never starts (state / epoch check) or
// finishes before the ring can be claimed again; a late fill can never land in a recycled ring. RT never waits for io_busy: a draining ring that is still busy is skipped.
//
// The queue only announces new streams (and wakes a worker that is between passes). The work itself is discovered from the ring states: every worker scans ALL rings and serves
// the most urgent one it can lock (a per-ring try-lock, so two threads never fill the same ring). A worker stuck inside a stalled read therefore holds ONE ring; the other workers
// serve the rest. A worker that sees the queue non-empty stops after the chunk it is reading, so a new stream waits for at most one read, not for a whole pass.
// Seeks (loop wraps) need no queue: the voice stores the new start in the ring (seek_seq / seek_start) and the scan polls it.
//
// Errors (EIO, short read, file gone, decode failure) never reach the audio thread as anything but missing data: the voice is starved and counts an underrun. Here each is
// counted (read_errors()), logged through the typed error handler / recent_errors(), and the stream retries after a back-off, so a transient error recovers.
//
// Inline mode (io_threads == 0): no threads; pump() runs the workers' work on the calling thread, deterministically (tests, offline render).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "velora/host/head_memory.hpp"
#include "velora/ports/stream.hpp"
#include "velora/ports/thread_priority.hpp"

namespace velora::host {

namespace detail {
class MpmcQueue;  // stream_manager.cpp
}

struct StreamConfig {
  std::uint32_t io_threads = 2;         // I/O edge threads; 0 = inline mode (pump())
  std::uint32_t ring_count = 0;         // the pool: normally the sample engine's voice capacity (every streaming voice holds one ring)
  std::uint32_t chunk_frames = 4096;    // frames per read (formatlib block size: one FLAC block per read)
  std::uint64_t p9999_ns = 2'000'000;   // the qualified SSD latency at p99.99 (same figure as HeadSizing)
  std::uint64_t margin_ns = 1'000'000;
  std::uint32_t sample_rate = 48000;    // the highest sample rate of the libraries
  double max_pitch_ratio = 2.0;         // the highest playback pitch ratio the lookahead is sized for
  std::uint32_t guard_frames = 4;
  std::uint32_t ring_frames = 0;        // 0 = derived (stream_ring_frames())
  std::uint32_t block_frames = 64;      // the engine block the voices render per call (core::kBlockFrames): a voice reads up to ceil(block * ratio) frames ahead of its published position
  std::uint32_t poll_us = 200;          // an idle I/O thread polls this often (the board build waits on io_uring completions instead)
  std::uint32_t fence_timeout_ms = 5000;  // StreamManager::fence gives up after this long (a read that never returns); see fence()
  std::uint64_t budget_bytes = 0;       // 0 = no limit; else a pool bigger than this is refused (kBudgetExceeded)
  HeadMemoryOptions memory;             // the ring arena is locked like the heads (allow_unlocked for development)
  ports::ThreadPriority* priority = nullptr;  // applied once per I/O thread (ThreadRole::kIoEdge); nullptr = none
};

// The formula above (an explicit StreamConfig::ring_frames replaces the 2 * chunk + lookahead term). Multiple of 64, at least 3 * chunk_frames.
std::uint32_t stream_ring_frames(const StreamConfig& cfg) noexcept;
// Bytes of the pool for `cfg` (rings x ring_frames x 2 x 4), page rounded like the arena.
std::uint64_t stream_pool_bytes(const StreamConfig& cfg) noexcept;

// One reader error, typed (the host-side error log).
struct StreamError {
  ports::BlockReadStatus status = ports::BlockReadStatus::kOk;
  std::uint32_t ring = 0;
  std::uint32_t sample = 0;
  std::uint64_t start = 0;
  std::uint32_t frames = 0;
};

struct StreamStats {
  std::uint64_t reads = 0;            // successful chunk reads
  std::uint64_t frames_read = 0;
  std::uint64_t read_errors = 0;      // reads that failed (typed in the error log)
  std::uint64_t open_failures = 0;    // open() returned nullptr: no free ring or the request queue was full
  std::uint64_t pool_exhausted = 0;   //   ... of which no free ring
  std::uint64_t opens = 0;            // streams opened
  std::uint64_t closes = 0;
  std::uint64_t discarded = 0;        // jobs dropped because their ring ended or was recycled (epoch / state check): late fills that never happened
  std::uint32_t priority_applied = 0; // I/O threads whose priority was applied
  std::uint64_t fence_timeouts = 0;   // fence() gave up (an I/O thread stuck in a read): the caller leaked what the read uses
};

class StreamManager final : public ports::StreamService, public ports::StreamFence {
 public:
  enum class Status : std::uint8_t { kOk = 0, kBadConfig, kBudgetExceeded, kLockFailed, kNoMemory, kThreadFailed };
  // Allocates the pool and starts the I/O threads. nullptr with `status` set on failure; nothing stays allocated or running then.
  static std::unique_ptr<StreamManager> create(const StreamConfig& cfg, Status* status = nullptr);
  ~StreamManager() override;
  StreamManager(const StreamManager&) = delete;
  StreamManager& operator=(const StreamManager&) = delete;

  // ports::StreamService (real-time safe).
  ports::StreamRing* open(const ports::StreamOpen& request) noexcept VELORA_NONBLOCKING override;
  void close(ports::StreamRing* ring) noexcept VELORA_NONBLOCKING override;
  // ports::StreamFence: returns true once no I/O thread is inside a read for a ring that is no longer active. A source's destructor calls it (see BundleInstrument). After
  // StreamConfig::fence_timeout_ms (a read that never returns) it counts a timeout (stats().fence_timeouts), writes one line to stderr and returns FALSE: the stuck thread may still be
  // using the source, so the caller must leak the source, its reader and the memory the read touches rather than free them (BundleInstrument does). The stuck ring is never
  // reused (it stays busy), so no memory the thread can write is ever handed to another stream.
  bool fence() noexcept override;

  // Inline mode only (io_threads == 0): runs the work of every pending request and ring until none has anything left to read (or `max_reads` chunk reads were done). Returns the
  // reads done. A no-op returning 0 with threads.
  std::uint32_t pump(std::uint32_t max_reads = 0xFFFFFFFFu) noexcept;

  const StreamConfig& config() const noexcept { return cfg_; }
  std::uint32_t ring_count() const noexcept { return ring_count_; }
  std::uint32_t ring_frames() const noexcept { return ring_frames_; }
  std::uint64_t buffer_bytes() const noexcept { return buffer_bytes_; }   // the stream buffer budget actually used (the mapped arena)
  bool buffers_locked() const noexcept;
  std::uint32_t io_thread_count() const noexcept { return cfg_.io_threads; }  // 0 = inline mode
  // Rings that are not free: active (a voice holds it), reserved, or draining (given back, an in-flight read may still be finishing). A leak test expects 0 after every
  // voice ended and the I/O threads went idle (rings_in_use_settled()).
  std::uint32_t rings_active() const noexcept;
  std::uint32_t rings_draining() const noexcept;
  std::uint32_t rings_free() const noexcept;
  StreamStats stats() const noexcept;
  // The typed error log: the handler is called on an I/O thread (set it before the first stream); recent_errors() returns the last kErrorLogSize entries.
  void set_error_handler(std::function<void(const StreamError&)> handler);
  std::vector<StreamError> recent_errors() const;
  static constexpr std::size_t kErrorLogSize = 64;

 private:
  struct Worker;
  struct JobState;
  explicit StreamManager(const StreamConfig& cfg);
  bool init(Status& st);
  void note_error(const StreamError& e);

  StreamConfig cfg_;
  std::uint32_t ring_count_ = 0, ring_frames_ = 0;
  std::uint64_t buffer_bytes_ = 0;
  std::unique_ptr<HeadBuffer> arena_;
  std::unique_ptr<ports::StreamRing[]> rings_;
  std::unique_ptr<JobState[]> jobs_;      // per ring: the producer's state, owned by whoever holds its try-lock
  std::unique_ptr<detail::MpmcQueue> queue_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::atomic<std::uint32_t> hint_{0};
  std::atomic<bool> stop_{false};
  // Counters (relaxed).
  std::atomic<std::uint64_t> reads_{0}, frames_read_{0}, read_errors_{0}, open_failures_{0}, pool_exhausted_{0}, opens_{0}, closes_{0}, discarded_{0};
  std::atomic<std::uint32_t> priority_applied_{0};
  std::atomic<std::uint64_t> fence_timeouts_{0};
  mutable std::mutex err_mu_;
  std::function<void(const StreamError&)> err_handler_;
  std::vector<StreamError> err_log_;
  std::size_t err_next_ = 0;
};

}  // namespace velora::host
