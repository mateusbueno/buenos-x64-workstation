#include "velora/host/stream_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <new>

namespace velora::host {

namespace ports = velora::ports;

std::uint32_t stream_ring_frames(const StreamConfig& cfg) noexcept {
  const double seconds = static_cast<double>(cfg.p9999_ns + cfg.margin_ns) * 1e-9;
  const double ratio = std::isfinite(cfg.max_pitch_ratio) && cfg.max_pitch_ratio > 0.0 ? std::min(cfg.max_pitch_ratio, 1024.0) : 1.0;
  const double lookahead = std::ceil(seconds * static_cast<double>(cfg.sample_rate) * ratio) + std::ceil(static_cast<double>(cfg.block_frames) * ratio) + static_cast<double>(cfg.guard_frames);
  const std::uint64_t chunk = cfg.chunk_frames == 0 ? 4096u : cfg.chunk_frames;
  std::uint64_t frames = cfg.ring_frames != 0 ? cfg.ring_frames : 2 * chunk + static_cast<std::uint64_t>(std::min(lookahead, 4.0e9));
  frames = std::max<std::uint64_t>(frames, 3 * chunk);
  frames = (frames + 63) / 64 * 64;
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, 0x7FFFFFC0u));
}

std::uint64_t stream_pool_bytes(const StreamConfig& cfg) noexcept {
  const std::uint64_t payload = static_cast<std::uint64_t>(cfg.ring_count) * stream_ring_frames(cfg) * 2u * sizeof(float);
  return head_arena_bytes(static_cast<std::size_t>(payload));
}

namespace detail {

// Bounded multi-producer multi-consumer queue (Vyukov): a push and a pop are a few atomic operations, never a lock or an allocation.
class MpmcQueue {
 public:
  explicit MpmcQueue(std::size_t capacity_pow2) : mask_(capacity_pow2 - 1), cells_(new Cell[capacity_pow2]) {
    for (std::size_t i = 0; i < capacity_pow2; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }
  bool push(std::uint64_t v) noexcept {
    Cell* cell;
    std::size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
      cell = &cells_[pos & mask_];
      const std::size_t seq = cell->seq.load(std::memory_order_acquire);
      const std::intptr_t dif = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
      if (dif == 0) {
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
      } else if (dif < 0) {
        return false;  // full
      } else {
        pos = head_.load(std::memory_order_relaxed);
      }
    }
    cell->value = v;
    cell->seq.store(pos + 1, std::memory_order_release);
    return true;
  }
  bool pop(std::uint64_t& v) noexcept {
    Cell* cell;
    std::size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      cell = &cells_[pos & mask_];
      const std::size_t seq = cell->seq.load(std::memory_order_acquire);
      const std::intptr_t dif = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
      if (dif == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
      } else if (dif < 0) {
        return false;  // empty
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
    v = cell->value;
    cell->seq.store(pos + mask_ + 1, std::memory_order_release);
    return true;
  }
  // A hint: is a record ready to pop? (No synchronisation beyond the cell's sequence word.)
  bool nonempty() const noexcept {
    const std::size_t pos = tail_.load(std::memory_order_relaxed);
    const std::size_t seq = cells_[pos & mask_].seq.load(std::memory_order_acquire);
    return static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1) >= 0;
  }

 private:
  struct Cell {
    std::atomic<std::size_t> seq{0};
    std::uint64_t value = 0;
  };
  const std::size_t mask_;
  std::unique_ptr<Cell[]> cells_;
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace detail

namespace {

constexpr std::uint64_t pack_request(std::uint32_t ring, std::uint32_t epoch) noexcept { return (static_cast<std::uint64_t>(ring) << 32) | epoch; }

std::size_t pow2_at_least(std::size_t n) noexcept {
  std::size_t p = 2;
  while (p < n) p <<= 1;
  return p;
}

std::int64_t now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr std::int64_t kRetryStepNs = 5'000'000;   // after a failed read: wait 5 ms x the number of consecutive failures ...
constexpr std::int64_t kRetryMaxNs = 500'000'000;  // ... at most 500 ms, then retry

}  // namespace

// The producer's state of one ring. Owned (every field but the atomics) by the thread that holds `lock`; the atomics are hints the scan reads without it.
struct StreamManager::JobState {
  std::atomic<bool> lock{false};
  std::atomic<std::uint32_t> epoch_pub{0};   // the epoch of the life this state belongs to (0: none yet)
  std::atomic<std::uint32_t> seg_pub{0};     // the segment the producer is in
  std::atomic<std::uint64_t> fill_pub{0};    // filled_end
  std::atomic<std::uint64_t> wake_lo{0};     // while consumed_lo is below this the ring is full / complete: nothing to read (UINT64_MAX: complete)
  std::uint32_t epoch = 0;
  bool init = false, first = true;
  const ports::BlockSource* src = nullptr;
  std::uint32_t sample = 0;
  std::uint64_t total = 0;
  std::uint8_t channels = 1;
  std::uint32_t seg_seen = 0;
  std::uint64_t fill_end = 0;
  std::uint32_t failures = 0;
  std::int64_t retry_at = 0;
};

struct StreamManager::Worker {
  struct Cand {
    std::int64_t lead;
    std::uint32_t ring;
  };
  explicit Worker(StreamManager& m) : mgr(m), stage_l(2u * m.cfg_.chunk_frames), stage_r(2u * m.cfg_.chunk_frames) { cand.reserve(m.ring_count_); }

  StreamManager& mgr;
  std::vector<float> stage_l, stage_r;  // two chunks: the first read of a segment may be extended by one
  std::vector<Cand> cand;
  std::uint32_t scan_from = 0;
  std::thread thread;

  // One pass: drain the notifications, rank the streams that have something to read, serve them most urgent first (one chunk each). Stops early when a new stream was announced.
  // Returns the chunk reads done.
  std::uint32_t pass(std::uint32_t max_reads) noexcept {
    std::uint64_t rq;
    while (mgr.queue_->pop(rq)) {
      const ports::StreamRing& g = mgr.rings_[static_cast<std::uint32_t>(rq >> 32)];
      if (g.state.load(std::memory_order_acquire) != ports::StreamRing::kActive || g.epoch.load(std::memory_order_acquire) != static_cast<std::uint32_t>(rq & 0xFFFFFFFFu))
        mgr.discarded_.fetch_add(1, std::memory_order_relaxed);  // announced for a life that already ended (or a ring already recycled): nothing to do
    }
    cand.clear();
    const std::uint32_t n = mgr.ring_count_;
    for (std::uint32_t k = 0; k < n; ++k) {
      const std::uint32_t r = (scan_from + k) % n;
      const ports::StreamRing& g = mgr.rings_[r];
      if (g.state.load(std::memory_order_relaxed) != ports::StreamRing::kActive) continue;
      const JobState& j = mgr.jobs_[r];
      const std::uint32_t ep = g.epoch.load(std::memory_order_relaxed);
      std::int64_t lead = -1;  // a stream whose life has not been started is the most urgent
      if (j.epoch_pub.load(std::memory_order_relaxed) == ep) {
        const std::uint64_t lo = g.consumed_lo.load(std::memory_order_relaxed);
        if (j.seg_pub.load(std::memory_order_relaxed) == g.seek_seq.load(std::memory_order_relaxed) && lo < j.wake_lo.load(std::memory_order_relaxed)) continue;  // full or complete
        lead = static_cast<std::int64_t>(j.fill_pub.load(std::memory_order_relaxed)) - static_cast<std::int64_t>(lo);
      }
      cand.push_back({lead, r});
    }
    scan_from = (scan_from + 1) % n;  // equal urgency: the start of the scan rotates
    std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.lead < b.lead; });
    std::uint32_t done = 0;
    for (const Cand& c : cand) {
      if (done >= max_reads || mgr.queue_->nonempty()) break;  // a new stream waits for at most the read in progress
      JobState& j = mgr.jobs_[c.ring];
      if (j.lock.exchange(true, std::memory_order_acquire)) continue;  // another worker is serving it
      const bool read = serve(mgr.rings_[c.ring], j);
      j.lock.store(false, std::memory_order_release);
      if (read) ++done;
    }
    return done;
  }

  // Serves one chunk of one stream under the ring's io_busy handshake (the caller holds the job lock). True when a chunk was read.
  bool serve(ports::StreamRing& g, JobState& j) noexcept {
    const std::uint32_t ep = g.epoch.load(std::memory_order_seq_cst);
    g.io_busy.store(1, std::memory_order_seq_cst);
    if (g.state.load(std::memory_order_seq_cst) != ports::StreamRing::kActive || g.epoch.load(std::memory_order_seq_cst) != ep) {
      g.io_busy.store(0, std::memory_order_seq_cst);
      mgr.discarded_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (!j.init || j.epoch != ep) {  // a new life of the ring: describe it from the ring (published before kActive)
      j.epoch = ep;
      j.init = true;
      j.src = g.source;
      j.sample = g.sample;
      j.total = g.total_frames;
      j.channels = g.channels;
      j.seg_seen = 0;
      j.fill_end = 0;
      j.first = true;
      j.failures = 0;
      j.retry_at = 0;
      j.epoch_pub.store(ep, std::memory_order_relaxed);
    }
    const std::uint32_t sq = g.seek_seq.load(std::memory_order_acquire);
    if (sq != j.seg_seen) {  // a new segment (the open itself, or a loop wrap): restart there
      j.seg_seen = sq;
      j.fill_end = g.seek_start.load(std::memory_order_relaxed);
      j.first = true;
      j.failures = 0;
      j.retry_at = 0;
      g.read_failed.store(0, std::memory_order_relaxed);
      g.avail.store(ports::stream_pack_avail(sq, j.fill_end), std::memory_order_release);
      j.seg_pub.store(sq, std::memory_order_relaxed);
      j.fill_pub.store(j.fill_end, std::memory_order_relaxed);
      j.wake_lo.store(0, std::memory_order_relaxed);
    }
    if (j.retry_at != 0 && now_ns() < j.retry_at) {  // backing off after a failed read (time based)
      g.io_busy.store(0, std::memory_order_seq_cst);
      return false;
    }
    const std::uint64_t cap = g.capacity;
    const std::uint64_t lo = g.consumed_lo.load(std::memory_order_acquire);
    if (j.fill_end < lo) {  // the voice moved past what was read (it was starved): skip what it no longer needs
      j.fill_end = lo;
      j.first = true;
    }
    const std::uint64_t limit = std::min<std::uint64_t>(lo + cap, j.total);
    const std::uint64_t chunk = mgr.cfg_.chunk_frames;
    if (j.fill_end >= limit) {  // the ring is full (or the sample is complete)
      j.wake_lo.store(j.fill_end >= j.total ? ~std::uint64_t{0} : j.fill_end + chunk / 4 - cap, std::memory_order_relaxed);
      g.io_busy.store(0, std::memory_order_seq_cst);
      return false;
    }
    // Reads end on chunk (= FLAC block) boundaries: one block decode per read. The FIRST read of a segment (after the open, a loop seek or a skip) that would be a sliver of
    // less than half a chunk is extended by a whole chunk, so the head only has to cover the latency of ONE read, not of two.
    std::uint64_t boundary = (j.fill_end / chunk + 1) * chunk;
    if (j.first && boundary - j.fill_end < chunk / 2) boundary += chunk;
    const std::uint64_t end = std::min(boundary, limit);
    const std::uint64_t n = end - j.fill_end;
    if (end < boundary && end < j.total && n < chunk / 4) {  // the ring window only allows a sliver: wait for the voice to free more rather than re-decode a block for it
      j.wake_lo.store(j.fill_end + chunk / 4 - cap, std::memory_order_relaxed);
      g.io_busy.store(0, std::memory_order_seq_cst);
      return false;
    }
    const ports::BlockReadStatus st = j.src->read(j.sample, j.fill_end, static_cast<std::uint32_t>(n), stage_l.data(), j.channels == 2 ? stage_r.data() : nullptr);
    if (st != ports::BlockReadStatus::kOk) {
      g.read_failed.store(1, std::memory_order_relaxed);
      ++j.failures;
      j.retry_at = now_ns() + std::min(kRetryStepNs * static_cast<std::int64_t>(j.failures), kRetryMaxNs);
      StreamError e;
      e.status = st;
      e.ring = g.index;
      e.sample = j.sample;
      e.start = j.fill_end;
      e.frames = static_cast<std::uint32_t>(n);
      g.io_busy.store(0, std::memory_order_seq_cst);
      if ((j.failures & (j.failures - 1)) == 0) mgr.note_error(e);  // logged on the 1st, 2nd, 4th, 8th ... consecutive failure (a dead file does not flood the log)
      else mgr.read_errors_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    j.retry_at = 0;
    j.failures = 0;
    j.first = false;
    const std::uint32_t capf = g.capacity;
    std::uint32_t slot = static_cast<std::uint32_t>(j.fill_end % capf);
    std::uint32_t left = static_cast<std::uint32_t>(n), done = 0;
    while (left != 0) {
      const std::uint32_t run = std::min(left, capf - slot);
      std::memcpy(g.left + slot, stage_l.data() + done, run * sizeof(float));
      if (j.channels == 2) std::memcpy(g.right + slot, stage_r.data() + done, run * sizeof(float));
      done += run;
      left -= run;
      slot = 0;
    }
    j.fill_end += n;
    g.avail.store(ports::stream_pack_avail(j.seg_seen, j.fill_end), std::memory_order_release);
    j.fill_pub.store(j.fill_end, std::memory_order_relaxed);
    g.io_busy.store(0, std::memory_order_seq_cst);
    mgr.reads_.fetch_add(1, std::memory_order_relaxed);
    mgr.frames_read_.fetch_add(n, std::memory_order_relaxed);
    return true;
  }

  void run() noexcept {
    if (mgr.cfg_.priority != nullptr && mgr.cfg_.priority->apply_current_thread(ports::ThreadRole::kIoEdge)) mgr.priority_applied_.fetch_add(1, std::memory_order_relaxed);
    const auto poll = std::chrono::microseconds(std::max<std::uint32_t>(mgr.cfg_.poll_us, 20));
    while (!mgr.stop_.load(std::memory_order_acquire)) {
      if (pass(0xFFFFFFFFu) != 0 || mgr.queue_->nonempty()) continue;  // there was work (or a new stream): look again at once
      std::this_thread::sleep_for(poll);
    }
  }
};

StreamManager::StreamManager(const StreamConfig& cfg) : cfg_(cfg) {}

StreamManager::~StreamManager() {
  stop_.store(true, std::memory_order_release);
  for (auto& w : workers_)
    if (w && w->thread.joinable()) w->thread.join();
}

bool StreamManager::init(Status& st) {
  if (cfg_.ring_count == 0 || cfg_.chunk_frames < 64 || cfg_.chunk_frames > (1u << 20) || cfg_.io_threads > 64) {
    st = Status::kBadConfig;
    return false;
  }
  ring_count_ = cfg_.ring_count;
  ring_frames_ = stream_ring_frames(cfg_);
  const std::uint64_t payload = static_cast<std::uint64_t>(ring_count_) * ring_frames_ * 2u * sizeof(float);
  const std::uint64_t need = head_arena_bytes(static_cast<std::size_t>(payload));
  if (cfg_.budget_bytes != 0 && need > cfg_.budget_bytes) {
    st = Status::kBudgetExceeded;
    return false;
  }
  HeadBuffer::Status hs = HeadBuffer::Status::kOk;
  arena_ = HeadBuffer::allocate(static_cast<std::size_t>(payload), cfg_.memory, hs);
  if (!arena_) {
    st = hs == HeadBuffer::Status::kLockFailed ? Status::kLockFailed : Status::kNoMemory;
    return false;
  }
  buffer_bytes_ = arena_->mapped_bytes();
  rings_.reset(new (std::nothrow) ports::StreamRing[ring_count_]);
  jobs_.reset(new (std::nothrow) JobState[ring_count_]);
  queue_.reset(new (std::nothrow) detail::MpmcQueue(pow2_at_least(static_cast<std::size_t>(ring_count_) * 4 + 16)));
  if (!rings_ || !jobs_ || !queue_) {
    st = Status::kNoMemory;
    return false;
  }
  for (std::uint32_t i = 0; i < ring_count_; ++i) {
    ports::StreamRing& g = rings_[i];
    g.index = i;
    g.capacity = ring_frames_;
    g.left = arena_->floats() + static_cast<std::size_t>(i) * ring_frames_ * 2u;
    g.right = g.left + ring_frames_;
  }
  const std::size_t nworkers = std::max<std::uint32_t>(cfg_.io_threads, 1);
  for (std::size_t i = 0; i < nworkers; ++i) workers_.push_back(std::make_unique<Worker>(*this));
  err_log_.reserve(kErrorLogSize);
  if (cfg_.io_threads != 0) {
    try {
      for (auto& w : workers_) w->thread = std::thread([wp = w.get()] { wp->run(); });
    } catch (...) {
      st = Status::kThreadFailed;
      stop_.store(true, std::memory_order_release);
      for (auto& w : workers_)
        if (w->thread.joinable()) w->thread.join();
      return false;
    }
  }
  st = Status::kOk;
  return true;
}

std::unique_ptr<StreamManager> StreamManager::create(const StreamConfig& cfg, Status* status) {
  std::unique_ptr<StreamManager> m(new StreamManager(cfg));
  Status st = Status::kOk;
  if (!m->init(st)) {
    if (status) *status = st;
    return nullptr;  // the destructor joins whatever started
  }
  if (status) *status = Status::kOk;
  return m;
}

bool StreamManager::buffers_locked() const noexcept { return arena_ && arena_->locked(); }

ports::StreamRing* StreamManager::open(const ports::StreamOpen& rq) noexcept VELORA_NONBLOCKING {
  if (rq.source == nullptr || rq.total_frames == 0 || rq.start >= rq.total_frames || (rq.channels != 1 && rq.channels != 2)) {
    open_failures_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  const std::uint32_t n = ring_count_;
  const std::uint32_t first = hint_.fetch_add(1, std::memory_order_relaxed) % n;
  ports::StreamRing* g = nullptr;
  for (std::uint32_t k = 0; k < n && g == nullptr; ++k) {
    ports::StreamRing& c = rings_[(first + k) % n];
    std::uint32_t s = c.state.load(std::memory_order_acquire);
    if (s == ports::StreamRing::kFree) {
      if (c.state.compare_exchange_strong(s, ports::StreamRing::kReserved, std::memory_order_seq_cst)) g = &c;
    } else if (s == ports::StreamRing::kDraining && c.io_busy.load(std::memory_order_seq_cst) == 0) {
      // Given back and no I/O thread is inside it: claimable. (An I/O thread that sets io_busy right after this check re-checks the state, sees kReserved and backs off.)
      if (c.state.compare_exchange_strong(s, ports::StreamRing::kReserved, std::memory_order_seq_cst)) g = &c;
    }
  }
  if (g == nullptr) {
    open_failures_.fetch_add(1, std::memory_order_relaxed);
    pool_exhausted_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  // Ours (kReserved): describe the stream. A new epoch makes every announcement of the previous life of this ring stale.
  const std::uint32_t epoch = g->epoch.load(std::memory_order_relaxed) + 1;
  g->epoch.store(epoch, std::memory_order_seq_cst);
  g->source = rq.source;
  g->sample = rq.sample;
  g->total_frames = rq.total_frames;
  g->channels = rq.channels;
  g->avail.store(ports::stream_pack_avail(0, 0), std::memory_order_relaxed);  // segment 0 never matches: nothing is available until the producer acknowledges segment 1
  g->read_failed.store(0, std::memory_order_relaxed);
  g->consumed_lo.store(rq.start, std::memory_order_relaxed);
  g->seek_start.store(rq.start, std::memory_order_relaxed);
  g->seek_seq.store(1, std::memory_order_relaxed);
  g->state.store(ports::StreamRing::kActive, std::memory_order_seq_cst);  // publishes the description to an I/O thread that sees kActive
  if (!queue_->push(pack_request(g->index, epoch))) {
    g->state.store(ports::StreamRing::kDraining, std::memory_order_seq_cst);
    open_failures_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  opens_.fetch_add(1, std::memory_order_relaxed);
  return g;
}

void StreamManager::close(ports::StreamRing* ring) noexcept VELORA_NONBLOCKING {
  if (ring == nullptr) return;
  ring->state.store(ports::StreamRing::kDraining, std::memory_order_seq_cst);
  closes_.fetch_add(1, std::memory_order_relaxed);
}

bool StreamManager::fence() noexcept {
  const std::int64_t deadline = now_ns() + static_cast<std::int64_t>(cfg_.fence_timeout_ms) * 1'000'000;
  for (std::uint32_t i = 0; i < ring_count_; ++i) {
    ports::StreamRing& g = rings_[i];
    while (g.state.load(std::memory_order_seq_cst) != ports::StreamRing::kActive && g.io_busy.load(std::memory_order_seq_cst) != 0) {
      if (now_ns() > deadline) {
        fence_timeouts_.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "stream manager: fence timed out after %u ms: an I/O thread is stuck in a read of ring %u; its source and reader are leaked, the ring is never reused\n",
                     cfg_.fence_timeout_ms, i);
        return false;
      }
      std::this_thread::yield();
    }
  }
  return true;
}

std::uint32_t StreamManager::pump(std::uint32_t max_reads) noexcept {
  if (cfg_.io_threads != 0 || workers_.empty()) return 0;
  std::uint32_t total = 0;
  for (;;) {
    const std::uint32_t done = workers_[0]->pass(max_reads - total);
    total += done;
    if (done == 0 || total >= max_reads) break;
  }
  return total;
}

std::uint32_t StreamManager::rings_active() const noexcept {
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < ring_count_; ++i) {
    const std::uint32_t s = rings_[i].state.load(std::memory_order_acquire);
    n += (s == ports::StreamRing::kActive || s == ports::StreamRing::kReserved) ? 1u : 0u;
  }
  return n;
}
std::uint32_t StreamManager::rings_draining() const noexcept {
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < ring_count_; ++i) n += rings_[i].state.load(std::memory_order_acquire) == ports::StreamRing::kDraining ? 1u : 0u;
  return n;
}
std::uint32_t StreamManager::rings_free() const noexcept {
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < ring_count_; ++i) n += rings_[i].state.load(std::memory_order_acquire) == ports::StreamRing::kFree ? 1u : 0u;
  return n;
}

StreamStats StreamManager::stats() const noexcept {
  StreamStats s;
  s.reads = reads_.load(std::memory_order_relaxed);
  s.frames_read = frames_read_.load(std::memory_order_relaxed);
  s.read_errors = read_errors_.load(std::memory_order_relaxed);
  s.open_failures = open_failures_.load(std::memory_order_relaxed);
  s.pool_exhausted = pool_exhausted_.load(std::memory_order_relaxed);
  s.opens = opens_.load(std::memory_order_relaxed);
  s.closes = closes_.load(std::memory_order_relaxed);
  s.discarded = discarded_.load(std::memory_order_relaxed);
  s.priority_applied = priority_applied_.load(std::memory_order_relaxed);
  s.fence_timeouts = fence_timeouts_.load(std::memory_order_relaxed);
  return s;
}

void StreamManager::set_error_handler(std::function<void(const StreamError&)> handler) {
  std::lock_guard<std::mutex> lk(err_mu_);
  err_handler_ = std::move(handler);
}

std::vector<StreamError> StreamManager::recent_errors() const {
  std::lock_guard<std::mutex> lk(err_mu_);
  std::vector<StreamError> out;
  out.reserve(err_log_.size());
  if (err_log_.size() < kErrorLogSize) {
    out = err_log_;
  } else {  // oldest first
    for (std::size_t i = 0; i < kErrorLogSize; ++i) out.push_back(err_log_[(err_next_ + i) % kErrorLogSize]);
  }
  return out;
}

void StreamManager::note_error(const StreamError& e) {
  read_errors_.fetch_add(1, std::memory_order_relaxed);
  std::function<void(const StreamError&)> h;
  {
    std::lock_guard<std::mutex> lk(err_mu_);
    if (err_log_.size() < kErrorLogSize) {
      err_log_.push_back(e);
    } else {
      err_log_[err_next_] = e;
      err_next_ = (err_next_ + 1) % kErrorLogSize;
    }
    h = err_handler_;
  }
  if (h) h(e);
}

}  // namespace velora::host
