#pragma once
// Streaming ports (Story 1.17): what the sample engine and the host exchange so that the part of a sample past its resident head arrives from the SSD ahead of the
// playhead without the audio thread ever waiting. Plain structs, atomics and two small interfaces, standard library only.
//
//   BlockSource    (host -> I/O threads)  reads frames [start, start + n) of a sample decoded to planar float. BLOCKING; called by I/O edge threads only, never by RT.
//                  The Linux io_uring + O_DIRECT implementation of this port is dev-board work (TODO(board)); the Mac prototype is a portable pread reader.
//   StreamService  (engine -> host, RT)   open / close a stream and nothing else. Every call is non-blocking: it only touches atomics and pre-allocated memory.
//   StreamRing     the shared state of ONE stream: a position-addressed ring buffer (frame i of the sample lives at slot i % capacity) with a single consumer (the
//                  voice on the audio / a render worker thread) and a single producer (one I/O edge thread). The lifecycle (free -> reserved -> active -> draining)
//                  and the handshake that makes a recycled ring ABA-safe are implemented by the host's StreamManager; the engine only uses the inline helpers below.
//
// Data protocol (acquire / release, see engine/core/README.md "Streaming"):
//   * `avail` = segment (16 bits, never 0) << 48 | filled_end (48 bits). The producer writes frames, then publishes filled_end with a release store; the consumer
//     reads `avail` with an acquire load and may read every frame in [seg_start, filled_end) of the segment it expects (the consumer knows `seg_start`: it asked for it).
//   * `consumed_lo` is published by the consumer (release): it will never read a frame below it in this segment. The producer writes a frame x only while
//     x < consumed_lo + capacity, so a slot is never overwritten while the consumer can still read it.
//   * A SEEK (loop wrap) is `consumed_lo = seek_start = S; seek_seq = next segment` by the consumer; the producer abandons the old segment and restarts at S. Consumer
//     frames of the old segment are never read again (the voice then plays from resident memory until S).
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "velora/ports/nonblocking.hpp"

namespace velora::ports {

enum class BlockReadStatus : std::uint8_t {
  kOk = 0,
  kIoError,     // EIO and friends, file gone
  kShortRead,   // the file ends before the requested bytes (truncated)
  kOutOfRange,  // frames beyond the sample
  kDecodeError, // the block did not decode / failed its checks
  kClosed,      // the source is shutting down
};
inline const char* to_string(BlockReadStatus s) noexcept {
  switch (s) {
    case BlockReadStatus::kOk: return "ok";
    case BlockReadStatus::kIoError: return "io_error";
    case BlockReadStatus::kShortRead: return "short_read";
    case BlockReadStatus::kOutOfRange: return "out_of_range";
    case BlockReadStatus::kDecodeError: return "decode_error";
    case BlockReadStatus::kClosed: return "closed";
  }
  return "?";
}

class BlockSource {
 public:
  virtual ~BlockSource() = default;
  // Frames [start, start + frames) of instrument-local sample `sample`, decoded to float exactly as the resident loader decodes them (bit for bit), planar: `left`
  // always, `right` only for a 2-channel sample (nullptr for mono). Blocking; I/O edge threads only. Thread-safe: several I/O threads may read at once.
  virtual BlockReadStatus read(std::uint32_t sample, std::uint64_t start, std::uint32_t frames, float* left, float* right) const noexcept = 0;
};

inline constexpr std::uint64_t kStreamFrameMask = (std::uint64_t{1} << 48) - 1;
constexpr std::uint64_t stream_pack_avail(std::uint32_t segment, std::uint64_t filled_end) noexcept {
  return (static_cast<std::uint64_t>(segment & 0xFFFFu) << 48) | (filled_end & kStreamFrameMask);
}
constexpr std::uint32_t stream_next_segment(std::uint32_t segment) noexcept { return (segment & 0xFFFFu) == 0xFFFFu ? 1u : ((segment & 0xFFFFu) + 1u); }

struct alignas(64) StreamRing {
  enum State : std::uint32_t { kFree = 0, kReserved = 1, kActive = 2, kDraining = 3 };
  // Fixed at pool construction.
  float* left = nullptr;       // `capacity` frames
  float* right = nullptr;      // `capacity` frames (written only for 2-channel streams)
  std::uint32_t capacity = 0;
  std::uint32_t index = 0;
  // Lifecycle and the ABA guard (host-managed): `epoch` counts the lives of this ring; `io_busy` is 1 while an I/O thread may touch the ring or its source.
  std::atomic<std::uint32_t> state{kFree};
  std::atomic<std::uint32_t> epoch{0};
  std::atomic<std::uint32_t> io_busy{0};
  // The stream (written by the opener while the ring is kReserved, read by the I/O thread after it sees kActive).
  const BlockSource* source = nullptr;
  std::uint32_t sample = 0;
  std::uint64_t total_frames = 0;
  std::uint8_t channels = 1;
  // Consumer -> producer.
  alignas(64) std::atomic<std::uint64_t> consumed_lo{0};
  std::atomic<std::uint64_t> seek_start{0};
  std::atomic<std::uint32_t> seek_seq{0};
  // Producer -> consumer.
  alignas(64) std::atomic<std::uint64_t> avail{0};
  std::atomic<std::uint32_t> read_failed{0};  // diagnostic: the producer's last read of this segment failed (cleared by a seek)
};

// Implemented by the host's stream manager and called by whatever owns a BlockSource when it is about to destroy it (the destructor of the instrument that owns the
// reader): returns true once no I/O thread can still be inside, or about to enter, a read of a source whose stream has ended. false = it gave up after a timeout (an I/O
// thread is stuck inside a read): the caller must then NOT free the source or anything the read uses (it leaks them). See StreamManager::fence().
class StreamFence {
 public:
  virtual ~StreamFence() = default;
  virtual bool fence() noexcept = 0;
};

struct StreamOpen {
  const BlockSource* source = nullptr;
  std::uint32_t sample = 0;       // instrument-local sample index (BlockSource::read)
  std::uint64_t total_frames = 0; // the stored length of the sample
  std::uint8_t channels = 1;
  std::uint64_t start = 0;        // first frame the ring must hold (the head end)
};

class StreamService {
 public:
  virtual ~StreamService() = default;
  // Real-time safe. A ring in segment 1 starting at `start`, or nullptr when none can be had (pool exhausted, request queue full): the voice then fades at the head
  // end like Story 1.16 and the failure is counted by the host (StreamManager::open_failures()).
  virtual StreamRing* open(const StreamOpen& request) noexcept VELORA_NONBLOCKING = 0;
  // Real-time safe. Gives the ring back (a voice ended, was stolen or released). A fill that is in flight is discarded by the epoch / busy handshake. Exactly once per open().
  virtual void close(StreamRing* ring) noexcept VELORA_NONBLOCKING = 0;
};

}  // namespace velora::ports
