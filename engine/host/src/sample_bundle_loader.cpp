#include "velora/host/sample_bundle_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace velora::host {

namespace fmt = velora::format;
namespace ports = velora::ports;

const char* to_string(LoadStatus s) {
  switch (s) {
    case LoadStatus::kOk: return "ok";
    case LoadStatus::kInstrumentNotFound: return "instrument not found";
    case LoadStatus::kInstrumentIndex: return "instrument index out of range";
    case LoadStatus::kBundleInconsistent: return "instrument samples outside the bundle";
    case LoadStatus::kCancelled: return "cancelled";
    case LoadStatus::kContentIntegrity: return "content_integrity";
    case LoadStatus::kWrongContentKey: return "wrong_content_key";
    case LoadStatus::kSignatureInvalid: return "signature_invalid";
    case LoadStatus::kUntrustedKey: return "untrusted_key";
    case LoadStatus::kKeyUnavailable: return "key_unavailable";
    case LoadStatus::kProtectedUnsupported: return "protected_unsupported";
    case LoadStatus::kProtectedDowngrade: return "protected_downgrade";
    case LoadStatus::kHeadLockFailed: return "head_lock_failed";
    case LoadStatus::kHeadProtectedUnsupported: return "head_protected_unsupported";
    case LoadStatus::kStreamRequiresHeads: return "stream_requires_heads";
    case LoadStatus::kStreamProtectedUnsupported: return "stream_protected_unsupported";
    case LoadStatus::kStreamNeedsReader: return "stream_needs_reader";
  }
  return "?";
}

LoadStatus load_status_from(fmt::Error e) {
  switch (e) {
    case fmt::Error::kBlockHashMismatch:
    case fmt::Error::kCorruptBlock:
    case fmt::Error::kPcmHashMismatch:
      return LoadStatus::kContentIntegrity;
    case fmt::Error::kWrongContentKey: return LoadStatus::kWrongContentKey;
    case fmt::Error::kSignatureInvalid:
    case fmt::Error::kHashListMismatch:
    case fmt::Error::kCorruptIndex:
    case fmt::Error::kCorruptManifest:
    case fmt::Error::kCorruptHeader:
      return LoadStatus::kSignatureInvalid;
    case fmt::Error::kUntrustedKey: return LoadStatus::kUntrustedKey;
    default: return LoadStatus::kBundleInconsistent;
  }
}

bool BundleInstrument::mark_block_damaged(std::size_t sample, std::uint64_t block) const noexcept {
  if (sample >= damage_.size() || block >= damage_count_[sample]) return false;
  return damage_[sample][block].exchange(1, std::memory_order_relaxed) == 0;
}
bool BundleInstrument::block_damaged(std::size_t sample, std::uint64_t block) const noexcept {
  return sample < damage_.size() && block < damage_count_[sample] && damage_[sample][block].load(std::memory_order_relaxed) != 0;
}
std::uint64_t BundleInstrument::damaged_blocks() const noexcept {
  std::uint64_t n = 0;
  for (std::size_t s = 0; s < damage_.size(); ++s)
    for (std::uint32_t b = 0; b < damage_count_[s]; ++b) n += damage_[s][b].load(std::memory_order_relaxed) != 0 ? 1u : 0u;
  return n;
}

namespace {
// Adapts the unprotected reader to the loader's source interface.
class PlainSource final : public InstrumentSource {
 public:
  explicit PlainSource(const fmt::BundleReader& r) : r_(r) {}
  std::span<const fmt::InstrumentInfo> instruments() const override { return r_.instruments(); }
  std::span<const fmt::SampleInfo> samples() const override { return r_.samples(); }
  fmt::Error read_frames(std::size_t sample, std::uint64_t first, std::uint64_t count, std::span<std::int32_t> out) const override {
    return r_.read_frames(sample, first, count, out);
  }
  bool set_no_cache() const override { return r_.set_no_cache(); }

 private:
  const fmt::BundleReader& r_;
};
}  // namespace

ports::SampleRegionDesc to_port_region(const fmt::Region& r) {
  ports::SampleRegionDesc d;
  d.sample_index = r.sample;
  d.key_lo = r.key_lo;
  d.key_hi = r.key_hi;
  d.key_center = r.key_center;
  d.vel_lo = r.vel_lo;
  d.vel_hi = r.vel_hi;
  d.transpose = r.transpose;
  d.tune_cents = r.tune_cents;
  d.volume_db = r.volume_db;
  d.pan = r.pan;
  d.offset = r.offset;
  d.seq_length = r.seq_length;
  d.seq_position = r.seq_position;
  d.lorand = r.lorand;
  d.hirand = r.hirand;
  switch (r.loop_mode) {
    case fmt::LoopMode::kNoLoop: d.loop_mode = ports::SampleLoopMode::kNoLoop; break;
    case fmt::LoopMode::kOneShot: d.loop_mode = ports::SampleLoopMode::kOneShot; break;
    case fmt::LoopMode::kLoopContinuous: d.loop_mode = ports::SampleLoopMode::kLoopContinuous; break;
    case fmt::LoopMode::kLoopSustain: d.loop_mode = ports::SampleLoopMode::kLoopSustain; break;
  }
  d.has_loop_points = r.has_loop_points;
  d.loop_start = r.loop_start;
  d.loop_end = r.loop_end;
  d.ampeg_delay = r.ampeg.delay;
  d.ampeg_start = r.ampeg.start;
  d.ampeg_attack = r.ampeg.attack;
  d.ampeg_hold = r.ampeg.hold;
  d.ampeg_decay = r.ampeg.decay;
  d.ampeg_sustain = r.ampeg.sustain;
  d.ampeg_release = r.ampeg.release;
  switch (r.trigger) {
    case fmt::TriggerMode::kAttack: d.trigger = ports::SampleTrigger::kAttack; break;
    case fmt::TriggerMode::kRelease: d.trigger = ports::SampleTrigger::kRelease; break;
    case fmt::TriggerMode::kFirst: d.trigger = ports::SampleTrigger::kFirst; break;
    case fmt::TriggerMode::kLegato: d.trigger = ports::SampleTrigger::kLegato; break;
    case fmt::TriggerMode::kReleaseKey: d.trigger = ports::SampleTrigger::kReleaseKey; break;
  }
  d.has_end = r.has_end;
  d.end = r.end;
  // Story 13.4: the amp map travels unchanged.
  d.amp_veltrack = r.amp.veltrack;
  d.amplitude = r.amp.amplitude;
  d.amp_keytrack = r.amp.keytrack;
  d.amp_keycenter = r.amp.keycenter;
  d.vel2attack = r.amp.vel2attack;
  d.vel2decay = r.amp.vel2decay;
  d.vel2sustain = r.amp.vel2sustain;
  d.vel2release = r.amp.vel2release;
  // More points than the port holds (formatlib bounds them at the same 32) is not truncated: the count is passed on and the engine makes the region
  // unplayable (counted) instead of playing a different curve.
  d.vel_point_count = r.amp.curve_count;
  for (unsigned i = 0; i < fmt::kMaxVelPoints && i < ports::kSampleMaxVelPoints; ++i) d.vel_points[i] = {r.amp.curve[i].vel, r.amp.curve[i].gain};
  // Story 5.2: the Story 5.1 extension (keyswitches, CC crossfades, filter) travels unchanged.
  d.sw = {r.sw.lokey, r.sw.hikey, r.sw.last, r.sw.down, r.sw.up, r.sw.previous, r.sw.default_key};
  // More controllers than the port holds (formatlib bounds them at the same 4, so this is defence in depth) is not truncated: the count is
  // passed on as it is and the engine's validation makes the region unplayable (counted) instead of playing a different crossfade.
  d.xfin_count = r.xf.in_count;
  d.xfout_count = r.xf.out_count;
  d.xf_curve = r.xf.curve == fmt::XfCurve::kGain ? ports::SampleXfCurve::kGain : ports::SampleXfCurve::kPower;
  for (unsigned i = 0; i < std::min<unsigned>(d.xfin_count, ports::kSampleMaxCcCrossfades); ++i) d.xfin[i] = {r.xf.in[i].cc, r.xf.in[i].lo, r.xf.in[i].hi};
  for (unsigned i = 0; i < std::min<unsigned>(d.xfout_count, ports::kSampleMaxCcCrossfades); ++i) d.xfout[i] = {r.xf.out[i].cc, r.xf.out[i].lo, r.xf.out[i].hi};
  switch (r.filter.type) {
    case fmt::FilterType::kNone: d.filter_type = ports::SampleFilterType::kNone; break;
    case fmt::FilterType::kLpf1p: d.filter_type = ports::SampleFilterType::kLpf1p; break;
    case fmt::FilterType::kLpf2p: d.filter_type = ports::SampleFilterType::kLpf2p; break;
    case fmt::FilterType::kHpf1p: d.filter_type = ports::SampleFilterType::kHpf1p; break;
    case fmt::FilterType::kHpf2p: d.filter_type = ports::SampleFilterType::kHpf2p; break;
    case fmt::FilterType::kBpf2p: d.filter_type = ports::SampleFilterType::kBpf2p; break;
  }
  d.filter_cutoff_hz = r.filter.cutoff_hz;
  d.filter_resonance_db = r.filter.resonance_db;
  // Story 13.5: CC gates and modulation entries travel unchanged (formatlib bounds them at the port's 8 and 16; a count beyond the port is passed on and makes the
  // region unplayable in the engine, never a different region).
  d.cc_gate_count = r.cc.gate_count;
  d.cc_mod_count = r.cc.mod_count;
  for (unsigned i = 0; i < std::min<unsigned>(r.cc.gate_count, ports::kSampleMaxCcGates); ++i) d.cc_gates[i] = {r.cc.gates[i].cc, r.cc.gates[i].lo, r.cc.gates[i].hi};
  for (unsigned i = 0; i < std::min<unsigned>(r.cc.mod_count, ports::kSampleMaxCcMods); ++i)
    d.cc_mods[i] = {static_cast<ports::SampleCcTarget>(static_cast<std::uint8_t>(r.cc.mods[i].target)), r.cc.mods[i].cc, r.cc.mods[i].curve, r.cc.mods[i].depth};
  // Story 13.6: choke groups and polyphony limits travel unchanged.
  static_assert(ports::kSampleMaxVoiceLimit == fmt::kMaxVoiceLimit);
  static_assert(static_cast<int>(ports::SampleOffMode::kFast) == static_cast<int>(fmt::OffMode::kFast) && static_cast<int>(ports::SampleOffMode::kNormal) == static_cast<int>(fmt::OffMode::kNormal) &&
                static_cast<int>(ports::SampleOffMode::kTime) == static_cast<int>(fmt::OffMode::kTime));
  d.group = r.vg.group;
  d.has_off_by = r.vg.has_off_by;
  d.off_by = r.vg.off_by;
  d.off_mode = static_cast<ports::SampleOffMode>(static_cast<std::uint8_t>(r.vg.off_mode));
  d.off_time = r.vg.off_time;
  d.polyphony = r.vg.polyphony;
  d.group_polyphony = r.vg.group_polyphony;
  d.note_polyphony = r.vg.note_polyphony;
  d.note_selfmask = r.vg.note_selfmask;
  return d;
}

bool pcm_to_float(const fmt::PcmFormat& f, std::span<const std::int32_t> in, std::uint64_t frames,
                  std::vector<float>& out) {
  const std::size_t ch = f.channels;
  if ((ch != 1 && ch != 2) || in.size() != static_cast<std::size_t>(frames) * ch) return false;
  if (f.is_float ? f.bits_per_sample != 32 : (f.bits_per_sample < 1 || f.bits_per_sample > 32)) return false;
  const float scale = f.is_float ? 1.0f : static_cast<float>(1.0 / static_cast<double>(std::int64_t{1} << (f.bits_per_sample - 1)));
  out.resize(in.size());
  float* planes[2] = {out.data(), ch == 2 ? out.data() + frames : nullptr};
  for (std::uint64_t i = 0; i < frames; ++i)
    for (std::size_t c = 0; c < ch; ++c) {
      const std::int32_t raw = in[static_cast<std::size_t>(i) * ch + c];
      const float v = f.is_float ? fmt::bits_to_float(raw) : static_cast<float>(raw) * scale;
      if (!std::isfinite(v)) return false;
      planes[c][i] = v;
    }
  return true;
}

namespace {

// Story 1.17: the reader of the tails. Decodes frames [start, start + n) of a sample with the SAME decoder the resident loader uses (InstrumentSource::read_frames, then
// pcm_to_float), so the floats the I/O threads put in a ring are bit for bit those of a fully resident render. Blocking; called by I/O edge threads only (it allocates, in
// per-thread scratch buffers that grow once). The Linux io_uring + O_DIRECT implementation of ports::BlockSource replaces this class on the board (TODO(board)): the port is
// the seam, nothing above it changes.
class BundleBlockSource final : public ports::BlockSource {
 public:
  struct Sample {
    fmt::PcmFormat format;
    std::uint64_t frames = 0;
  };
  BundleBlockSource(std::shared_ptr<const InstrumentSource> src, std::size_t first_sample, std::vector<Sample> samples, ports::StreamFence* fence)
      : src_(std::move(src)), first_(first_sample), samples_(std::move(samples)), fence_(fence) {}
  // Called by ~BundleInstrument before anything the reads use goes away: stops new reads and waits (StreamFence, with its timeout) for the in-flight ones. false = an I/O thread
  // is stuck inside a read: the caller must leak this object and the reader.
  bool quiesce() noexcept {
    closed_.store(true, std::memory_order_release);
    return fence_ == nullptr || fence_->fence();
  }

  ports::BlockReadStatus read(std::uint32_t sample, std::uint64_t start, std::uint32_t frames, float* left, float* right) const noexcept override {
    if (closed_.load(std::memory_order_acquire)) return ports::BlockReadStatus::kClosed;
    if (sample >= samples_.size()) return ports::BlockReadStatus::kOutOfRange;
    const Sample& s = samples_[sample];
    if (start > s.frames || frames > s.frames - start || left == nullptr) return ports::BlockReadStatus::kOutOfRange;
    if (frames == 0) return ports::BlockReadStatus::kOk;
    const std::size_t ch = s.format.channels;
    if (ch == 2 && right == nullptr) return ports::BlockReadStatus::kOutOfRange;
    try {
      thread_local std::vector<std::int32_t> raw;
      thread_local std::vector<float> planar;
      raw.resize(static_cast<std::size_t>(frames) * ch);
      const fmt::Error e = src_->read_frames(first_ + sample, start, frames, raw);
      if (e != fmt::Error::kNone) {
        switch (e) {
          case fmt::Error::kIo: return ports::BlockReadStatus::kIoError;
          case fmt::Error::kTruncated: return ports::BlockReadStatus::kShortRead;
          case fmt::Error::kOutOfRange: return ports::BlockReadStatus::kOutOfRange;
          default: return ports::BlockReadStatus::kDecodeError;
        }
      }
      if (!pcm_to_float(s.format, raw, frames, planar)) return ports::BlockReadStatus::kDecodeError;
      std::memcpy(left, planar.data(), static_cast<std::size_t>(frames) * sizeof(float));
      if (ch == 2) std::memcpy(right, planar.data() + frames, static_cast<std::size_t>(frames) * sizeof(float));
      return ports::BlockReadStatus::kOk;
    } catch (...) {
      return ports::BlockReadStatus::kIoError;  // no memory for the scratch: a failed read, never an exception into the I/O thread
    }
  }

 private:
  std::shared_ptr<const InstrumentSource> src_;
  std::size_t first_;
  std::vector<Sample> samples_;
  ports::StreamFence* fence_;
  std::atomic<bool> closed_{false};
};

}  // namespace

namespace {
thread_local bool t_fence_failed = false;
}

BundleInstrument::~BundleInstrument() {
  // Stream mode: no I/O thread may be inside a read of this instrument's tails when the reader (the lease / context) and the memory go. fence() timed out (a stuck read): leak
  // the block source (and the reader reference it holds) and tell the owner's deleter to leak the lease too (take_fence_failed); never free what a stuck thread may still use.
  if (block_source_ && !static_cast<BundleBlockSource*>(block_source_.get())->quiesce()) {
    t_fence_failed = true;
    (void)block_source_.release();
    if (context_) (void)new std::shared_ptr<const void>(context_);
  }
}

bool BundleInstrument::take_fence_failed() noexcept {
  const bool f = t_fence_failed;
  t_fence_failed = false;
  return f;
}

struct BundleInstrumentLoader {
  static LoadResult load(const InstrumentSource& bundle, std::size_t index, const LoadHooks* hooks, const SampleLoadOptions& options) {
    LoadResult res;
    const auto instruments = bundle.instruments();
    if (index >= instruments.size()) return res;
    const fmt::InstrumentInfo& info = instruments[index];
    if (info.first_sample > bundle.samples().size() || info.sample_count > bundle.samples().size() - info.first_sample) {
      res.status = LoadStatus::kBundleInconsistent;
      return res;
    }

    const bool head_mode = options.head.enabled;
    const bool stream_mode = options.stream.enabled;
    if (stream_mode && !head_mode) {  // streaming continues a head: there is no heads-off streaming
      res.status = LoadStatus::kStreamRequiresHeads;
      return res;
    }
    if (head_mode && bundle.protected_source()) {  // heads (and so streaming) are for unprotected bundles only (deferred): refuse, typed
      res.status = stream_mode ? LoadStatus::kStreamProtectedUnsupported : LoadStatus::kHeadProtectedUnsupported;
      return res;
    }
    if (stream_mode && !options.stream.reader) {  // no reader that outlives this call (the BundleReader overload supplies one): refuse instead of keeping a dangling reference
      res.status = LoadStatus::kStreamNeedsReader;
      return res;
    }
    std::vector<HeadLength> plan;
    if (head_mode) plan = plan_heads(instruments, bundle.samples(), index, options.head.sizing);
    std::vector<std::vector<LoopSegment>> seg_plan;
    if (stream_mode) seg_plan = plan_loop_segments(instruments, bundle.samples(), index, options.head.sizing, plan);

    auto out = std::make_unique<BundleInstrument>();
    out->head_mode_ = head_mode;
    std::vector<std::size_t> head_offset;  // floats, per sample, inside the arena
    std::vector<std::vector<std::size_t>> seg_offset(stream_mode ? info.sample_count : 0);  // floats, per sample and loop segment, inside the arena
    if (head_mode) {
      // One arena for the whole instrument: heads (and in stream mode the resident loop segments) at 64 byte aligned offsets, the arena page rounded and mlocked once (the budget counts exactly this).
      head_offset.assign(info.sample_count, 0);
      std::size_t payload = 0;
      for (std::size_t s = 0; s < plan.size(); ++s) {
        head_offset[s] = payload / sizeof(float);
        const std::size_t sch = bundle.samples()[info.first_sample + s].format.channels;
        payload += head_align(static_cast<std::size_t>(plan[s].frames) * sch * sizeof(float));
        if (stream_mode && s < seg_plan.size())
          for (const LoopSegment& sg : seg_plan[s]) {
            seg_offset[s].push_back(payload / sizeof(float));
            payload += head_align(static_cast<std::size_t>(sg.frames) * sch * sizeof(float));
          }
      }
      if (payload != 0) {
        HeadBuffer::Status hs = HeadBuffer::Status::kOk;
        out->head_arena_ = HeadBuffer::allocate(head_arena_bytes(payload), options.head.memory, hs);
        if (!out->head_arena_) {
          res.status = hs == HeadBuffer::Status::kLockFailed ? LoadStatus::kHeadLockFailed : LoadStatus::kBundleInconsistent;  // no memory: the heads are not resident
          return res;
        }
        out->heads_locked_ = out->head_arena_->locked();
        out->resident_bytes_ = out->head_arena_->mapped_bytes();
      }
    }
    out->regions_.reserve(info.regions.size());
    for (const fmt::Region& r : info.regions) out->regions_.push_back(to_port_region(r));
    for (const fmt::CcDefault& cd : info.cc_setup.defaults) out->cc_defaults_.push_back({cd.cc, cd.value});
    out->curve_points_.resize(info.cc_setup.curves.size());
    out->curves_.reserve(info.cc_setup.curves.size());
    for (std::size_t k = 0; k < info.cc_setup.curves.size(); ++k) {
      for (const fmt::CurvePoint& pt : info.cc_setup.curves[k].points) out->curve_points_[k].push_back({pt.x, pt.y});
      out->curves_.push_back({info.cc_setup.curves[k].index, std::span<const ports::SampleCurvePoint>(out->curve_points_[k])});
    }
    out->pcm_.resize(info.sample_count);
    out->data_.resize(info.sample_count);
    if (stream_mode) {
      // The reader of the tails (StreamLoadOptions::reader; the BundleReader overload makes one that lives as long as the instrument).
      std::shared_ptr<const InstrumentSource> reader = options.stream.reader;
      std::vector<BundleBlockSource::Sample> descs(info.sample_count);
      for (std::size_t s = 0; s < info.sample_count; ++s) descs[s] = {bundle.samples()[info.first_sample + s].format, bundle.samples()[info.first_sample + s].frame_count};
      if (options.stream.no_cache) out->stream_no_cache_ = reader->set_no_cache();
      out->block_source_ = std::make_unique<BundleBlockSource>(std::move(reader), info.first_sample, std::move(descs), options.stream.fence);
      out->segments_.resize(info.sample_count);
    }
    if (bundle.protected_source()) {  // Story 6.5: every sample gets a block-damage map the I/O edge threads can set and the audio thread reads
      out->damage_.resize(info.sample_count);
      out->damage_count_.assign(info.sample_count, 0);
      for (std::size_t s = 0; s < info.sample_count; ++s) {
        const std::uint32_t blocks = bundle.samples()[info.first_sample + s].block_count;
        out->damage_count_[s] = blocks;
        if (blocks != 0) out->damage_[s] = std::make_unique<std::atomic<std::uint8_t>[]>(blocks);  // value-initialised: all zero
      }
    }

    const auto samples = bundle.samples();
    std::vector<std::int32_t> tmp;
    // The hooks run between chunks; false = stop. Everything resident so far dies with `out` (RAII), so a cancelled load leaves nothing.
    auto proceed = [&](std::uint64_t bytes) {
      if (!hooks) return true;
      if (hooks->cancel && hooks->cancel->load(std::memory_order_acquire)) return false;
      return !(hooks->progress && bytes != 0 && !hooks->progress(bytes));
    };
    for (std::size_t s = 0; s < info.sample_count; ++s) {
      if (!proceed(0)) {
        res.status = LoadStatus::kCancelled;
        return res;
      }
      const fmt::SampleInfo& si = samples[info.first_sample + s];
      ports::SamplePcm& pcm = out->pcm_[s];  // stays empty (frames == 0) on failure
      const std::size_t ch = si.format.channels;
      const std::uint64_t total_frames = si.frame_count;
      const std::uint64_t frames = head_mode && s < plan.size() ? plan[s].frames : total_frames;  // the frames made resident: the head, or the whole sample
      if (frames == 0 || total_frames == 0 || (ch != 1 && ch != 2) || total_frames > (std::uint64_t{1} << 31)) {
        ++out->failed_samples_;  // empty or unusable: the engine skips and counts its regions
        continue;
      }
      try {
        tmp.assign(static_cast<std::size_t>(frames) * ch, 0);
        bool read_ok = true;
        for (std::uint64_t at = 0; at < frames && read_ok;) {  // chunked, so a cancel is seen within one chunk
          const std::uint64_t n = std::min<std::uint64_t>(kLoadChunkFrames, frames - at);
          const fmt::Error rerr = bundle.read_frames(info.first_sample + s, at, n, std::span<std::int32_t>(tmp).subspan(static_cast<std::size_t>(at) * ch, static_cast<std::size_t>(n) * ch));
          read_ok = rerr == fmt::Error::kNone;
          if (!read_ok && out->first_error_ == fmt::Error::kNone) out->first_error_ = rerr;
          if (!read_ok && bundle.protected_source() && load_status_from(rerr) != LoadStatus::kBundleInconsistent) {
            // Story 6.5: a protected instrument with a block that fails its hash or does not decode is REFUSED with the typed cause; whatever was made resident
            // dies with `out`, and the cause is not recoverable by playing the other samples.
            res.status = load_status_from(rerr);
            return res;
          }
          at += n;
          if (read_ok && !proceed(n * ch * sizeof(float))) {
            out->data_[s] = std::vector<float>();
            res.status = LoadStatus::kCancelled;
            return res;
          }
        }
        if (!read_ok || !pcm_to_float(si.format, tmp, frames, out->data_[s])) {
          out->data_[s].clear();
          ++out->failed_samples_;
          continue;
        }
      } catch (const std::bad_alloc&) {
        out->data_[s] = std::vector<float>();
        ++out->failed_samples_;
        continue;
      }
      std::vector<float>& buf = out->data_[s];
      float* left = buf.data();
      float* right = ch == 2 ? buf.data() + frames : nullptr;
      if (head_mode) {
        // The head goes into the arena (planar, left then right) on this (loading) thread.
        float* base = out->head_arena_->floats() + head_offset[s];
        std::memcpy(base, buf.data(), buf.size() * sizeof(float));
        left = base;
        right = ch == 2 ? base + frames : nullptr;
        out->data_[s] = std::vector<float>();  // the temporary decode buffer is released; the head lives in locked memory
        if (frames < total_frames) ++out->tail_samples_;
      }
      if (stream_mode && s < seg_plan.size() && !seg_plan[s].empty()) {
        // The resident loop segments (stream mode): read like the head, in chunks so a cancel is seen, into the arena on this (loading) thread.
        try {
          std::vector<std::int32_t> raw;
          std::vector<float> planar;
          bool seg_ok = true;
          for (std::size_t k = 0; k < seg_plan[s].size() && seg_ok; ++k) {
            const LoopSegment& sg = seg_plan[s][k];
            float* base = out->head_arena_->floats() + seg_offset[s][k];
            for (std::uint64_t at = 0; at < sg.frames && seg_ok; at += kLoadChunkFrames) {
              const std::uint64_t n = std::min<std::uint64_t>(kLoadChunkFrames, sg.frames - at);
              raw.resize(static_cast<std::size_t>(n) * ch);
              const fmt::Error rerr = bundle.read_frames(info.first_sample + s, sg.start + at, n, raw);
              seg_ok = rerr == fmt::Error::kNone && pcm_to_float(si.format, raw, n, planar);
              if (!seg_ok && out->first_error_ == fmt::Error::kNone) out->first_error_ = rerr;
              if (!seg_ok) break;
              std::memcpy(base + at, planar.data(), static_cast<std::size_t>(n) * sizeof(float));
              if (ch == 2) std::memcpy(base + sg.frames + at, planar.data() + n, static_cast<std::size_t>(n) * sizeof(float));
              if (!proceed(n * ch * sizeof(float))) {
                res.status = LoadStatus::kCancelled;
                return res;
              }
            }
            if (seg_ok) {
              ports::SamplePcmSegment ps;
              ps.start = sg.start;
              ps.frames = sg.frames;
              ps.left = base;
              ps.right = ch == 2 ? base + sg.frames : nullptr;
              out->segments_[s].push_back(ps);
              ++out->loop_segments_;
            }
          }
          if (!seg_ok) {
            out->segments_[s].clear();
            ++out->failed_samples_;
            continue;
          }
        } catch (const std::bad_alloc&) {
          out->segments_[s].clear();
          ++out->failed_samples_;
          continue;
        }
      }
      pcm.sample_rate = si.format.sample_rate;
      pcm.channels = static_cast<std::uint8_t>(ch);
      pcm.frames = frames;
      pcm.total_frames = head_mode ? total_frames : 0;
      pcm.has_tail = head_mode && frames < total_frames;
      pcm.left = left;
      pcm.right = right;
      if (stream_mode && pcm.has_tail) {  // Story 1.17: the tail's reader and the resident loop segments travel with the sample
        pcm.tail_source = out->block_source_.get();
        pcm.tail_sample = static_cast<std::uint32_t>(s);
        pcm.segments = out->segments_[s].empty() ? nullptr : out->segments_[s].data();
        pcm.segment_count = static_cast<std::uint32_t>(out->segments_[s].size());
      }
      if (!out->damage_.empty() && out->damage_count_[s] != 0) {
        pcm.damaged_blocks = out->damage_[s].get();
        pcm.damaged_block_frames = fmt::kBlockFrames;
        pcm.damaged_block_count = out->damage_count_[s];
      }
      if (!head_mode) out->resident_bytes_ += static_cast<std::size_t>(frames) * ch * sizeof(float);
    }
    res.status = LoadStatus::kOk;
    res.instrument = std::move(out);
    return res;
  }
};

LoadResult load_instrument_by_index(const fmt::BundleReader& bundle, std::size_t index, const LoadHooks* hooks) {
  const PlainSource src(bundle);
  return BundleInstrumentLoader::load(src, index, hooks, SampleLoadOptions{});
}

LoadResult load_instrument_by_index(const InstrumentSource& source, std::size_t index, const LoadHooks* hooks) {
  return BundleInstrumentLoader::load(source, index, hooks, SampleLoadOptions{});
}

LoadResult load_instrument_by_index(const fmt::BundleReader& bundle, std::size_t index, const LoadHooks* hooks, const SampleLoadOptions& options) {
  if (options.stream.enabled && !options.stream.reader) {
    // The I/O threads read through an adapter that must live as long as the instrument (not this call's stack): an owning adapter over the caller's reader, which the caller
    // keeps alive for the instrument's life.
    SampleLoadOptions opt = options;
    opt.stream.reader = std::make_shared<PlainSource>(bundle);
    const PlainSource src(bundle);
    return BundleInstrumentLoader::load(src, index, hooks, opt);
  }
  const PlainSource src(bundle);
  return BundleInstrumentLoader::load(src, index, hooks, options);
}

LoadResult load_instrument_by_index(const InstrumentSource& source, std::size_t index, const LoadHooks* hooks, const SampleLoadOptions& options) {
  return BundleInstrumentLoader::load(source, index, hooks, options);
}

std::uint64_t instrument_head_bytes(const fmt::BundleReader& bundle, std::size_t index) {
  return instrument_head_bytes(bundle.instruments(), bundle.samples(), index);
}

std::uint64_t instrument_head_bytes(std::span<const fmt::InstrumentInfo> instruments, std::span<const fmt::SampleInfo> samples, std::size_t index) {
  if (index >= instruments.size()) return 0;
  const fmt::InstrumentInfo& info = instruments[index];
  if (info.first_sample > samples.size() || info.sample_count > samples.size() - info.first_sample) return 0;
  std::uint64_t bytes = 0;
  for (std::size_t s = 0; s < info.sample_count; ++s) {
    const fmt::SampleInfo& si = samples[info.first_sample + s];
    const std::size_t ch = si.format.channels;
    if (si.frame_count == 0 || (ch != 1 && ch != 2) || si.frame_count > (std::uint64_t{1} << 31)) continue;  // the loader leaves these empty
    bytes += si.frame_count * ch * sizeof(float);
  }
  return bytes;
}

std::vector<HeadLength> plan_heads(std::span<const fmt::InstrumentInfo> instruments, std::span<const fmt::SampleInfo> samples, std::size_t index, const HeadSizing& sizing) {
  std::vector<HeadLength> plan;
  if (index >= instruments.size()) return plan;
  const fmt::InstrumentInfo& info = instruments[index];
  if (info.first_sample > samples.size() || info.sample_count > samples.size() - info.first_sample) return plan;
  // Per sample: the highest pitch ratio and the largest start offset over the regions that use it (a sample no region uses keeps ratio 1, offset 0).
  std::vector<double> ratio(info.sample_count, 0.0);
  std::vector<std::uint64_t> offset(info.sample_count, 0);
  std::vector<std::uint64_t> loop_need(info.sample_count, 0);  // frames a loop with explicit points needs resident, so wrapping never meets the head end
  for (const fmt::Region& r : info.regions) {
    if (r.sample >= info.sample_count) continue;
    if ((r.loop_mode == fmt::LoopMode::kLoopContinuous || r.loop_mode == fmt::LoopMode::kLoopSustain) && r.has_loop_points)
      loop_need[r.sample] = std::max<std::uint64_t>(loop_need[r.sample], r.loop_end + 1 + sizing.guard_frames);
    ratio[r.sample] = std::max(ratio[r.sample], region_max_pitch_ratio(r.key_hi, r.key_center, r.transpose, r.tune_cents));
    offset[r.sample] = std::max<std::uint64_t>(offset[r.sample], r.offset);
  }
  plan.resize(info.sample_count);
  for (std::size_t s = 0; s < info.sample_count; ++s) {
    const fmt::SampleInfo& si = samples[info.first_sample + s];
    const std::size_t ch = si.format.channels;
    if (si.frame_count == 0 || (ch != 1 && ch != 2) || si.frame_count > (std::uint64_t{1} << 31)) continue;  // the loader leaves these empty
    plan[s] = head_length(sizing, si.format.sample_rate, ratio[s] > 0.0 ? ratio[s] : 1.0, si.frame_count, offset[s]);
    // A loop that lies inside the head but ends in its last guard frames would hit the end-of-head check (its interpolation taps run past the head) before it
    // wraps: cover its end plus the guard. A loop that reaches past the natural head is left alone: it really needs the tail (stream underrun, Story 1.17).
    if (loop_need[s] > plan[s].frames && loop_need[s] - sizing.guard_frames <= plan[s].frames) {
      plan[s].frames = std::min<std::uint64_t>(loop_need[s], si.frame_count);
      plan[s].has_tail = plan[s].frames < si.frame_count;
    }
  }
  return plan;
}

std::vector<std::vector<LoopSegment>> plan_loop_segments(std::span<const fmt::InstrumentInfo> instruments, std::span<const fmt::SampleInfo> samples, std::size_t index, const HeadSizing& sizing,
                                                         const std::vector<HeadLength>& plan) {
  std::vector<std::vector<LoopSegment>> out;
  if (index >= instruments.size()) return out;
  const fmt::InstrumentInfo& info = instruments[index];
  if (info.first_sample > samples.size() || info.sample_count > samples.size() - info.first_sample) return out;
  out.resize(info.sample_count);
  for (const fmt::Region& r : info.regions) {
    if (r.sample >= info.sample_count || r.sample >= plan.size()) continue;
    if (r.loop_mode != fmt::LoopMode::kLoopContinuous && r.loop_mode != fmt::LoopMode::kLoopSustain) continue;
    const std::size_t s = r.sample;
    const fmt::SampleInfo& si = samples[info.first_sample + s];
    const std::uint64_t total = si.frame_count, head = plan[s].frames;
    if (total == 0 || !plan[s].has_tail) continue;
    // The loop as the engine reads it: explicit points, else the whole sample; a range outside the sample is no loop.
    const std::uint64_t ls = r.has_loop_points ? r.loop_start : 0, le = r.has_loop_points ? r.loop_end : total - 1;
    if (ls > le || le >= total) continue;
    if (le + 1 + sizing.guard_frames <= head) continue;  // the loop (and its interpolation taps) lies inside the head: a wrap never leaves resident memory
    const std::uint64_t lo = ls > 0 ? ls - 1 : 0;  // the left interpolation tap of the first frame after a wrap
    const double ratio = region_max_pitch_ratio(r.key_hi, r.key_center, r.transpose, r.tune_cents);
    const std::uint64_t need_end = head_length(sizing, si.format.sample_rate, ratio, total, lo).frames;
    const std::uint64_t s0 = std::max(lo, head);
    if (need_end <= s0) continue;  // the head already covers the latency after the wrap
    out[s].push_back({s0, need_end - s0});
  }
  for (auto& v : out) {  // merge overlapping and adjoining segments (sorted by start)
    std::sort(v.begin(), v.end(), [](const LoopSegment& a, const LoopSegment& b) { return a.start < b.start; });
    std::vector<LoopSegment> m;
    for (const LoopSegment& g : v) {
      if (!m.empty() && g.start <= m.back().start + m.back().frames) m.back().frames = std::max(m.back().frames, g.start + g.frames - m.back().start);
      else m.push_back(g);
    }
    v = std::move(m);
  }
  return out;
}

std::uint64_t instrument_head_bytes(std::span<const fmt::InstrumentInfo> instruments, std::span<const fmt::SampleInfo> samples, std::size_t index, const HeadOptions& head) {
  if (!head.enabled) return instrument_head_bytes(instruments, samples, index);
  const std::vector<HeadLength> plan = plan_heads(instruments, samples, index, head.sizing);
  if (plan.empty()) return 0;
  const fmt::InstrumentInfo& info = instruments[index];
  std::size_t payload = 0;
  for (std::size_t s = 0; s < plan.size(); ++s) payload += head_align(static_cast<std::size_t>(plan[s].frames) * samples[info.first_sample + s].format.channels * sizeof(float));
  if (head.loop_segments) {  // Story 1.17: the resident loop segments live in the same arena
    const auto segs = plan_loop_segments(instruments, samples, index, head.sizing, plan);
    for (std::size_t s = 0; s < segs.size(); ++s)
      for (const LoopSegment& sg : segs[s]) payload += head_align(static_cast<std::size_t>(sg.frames) * samples[info.first_sample + s].format.channels * sizeof(float));
  }
  return head_arena_bytes(payload);
}

LoadResult load_instrument(const fmt::BundleReader& bundle, std::string_view key) {
  const auto instruments = bundle.instruments();
  for (std::size_t i = 0; i < instruments.size(); ++i)
    if (instruments[i].key == key) return load_instrument_by_index(bundle, i, nullptr);
  LoadResult res;
  res.status = LoadStatus::kInstrumentNotFound;
  return res;
}

}  // namespace velora::host
