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
  }
  return "?";
}

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
  d.trigger = r.trigger == fmt::TriggerMode::kRelease ? ports::SampleTrigger::kRelease : ports::SampleTrigger::kAttack;
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

struct BundleInstrumentLoader {
  static LoadResult load(const fmt::BundleReader& bundle, std::size_t index, const LoadHooks* hooks) {
    LoadResult res;
    const auto instruments = bundle.instruments();
    if (index >= instruments.size()) return res;
    const fmt::InstrumentInfo& info = instruments[index];
    if (info.first_sample > bundle.samples().size() || info.sample_count > bundle.samples().size() - info.first_sample) {
      res.status = LoadStatus::kBundleInconsistent;
      return res;
    }

    auto out = std::make_unique<BundleInstrument>();
    out->regions_.reserve(info.regions.size());
    for (const fmt::Region& r : info.regions) out->regions_.push_back(to_port_region(r));
    out->pcm_.resize(info.sample_count);
    out->data_.resize(info.sample_count);

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
      const std::uint64_t frames = si.frame_count;
      if (frames == 0 || (ch != 1 && ch != 2) || frames > (std::uint64_t{1} << 31)) {
        ++out->failed_samples_;  // empty or unusable: the engine skips and counts its regions
        continue;
      }
      try {
        tmp.assign(static_cast<std::size_t>(frames) * ch, 0);
        bool read_ok = true;
        for (std::uint64_t at = 0; at < frames && read_ok;) {  // chunked, so a cancel is seen within one chunk
          const std::uint64_t n = std::min<std::uint64_t>(kLoadChunkFrames, frames - at);
          read_ok = bundle.read_frames(info.first_sample + s, at, n, std::span<std::int32_t>(tmp).subspan(static_cast<std::size_t>(at) * ch, static_cast<std::size_t>(n) * ch)) ==
                    fmt::Error::kNone;
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
      pcm.sample_rate = si.format.sample_rate;
      pcm.channels = static_cast<std::uint8_t>(ch);
      pcm.frames = frames;
      pcm.left = left;
      pcm.right = right;
      out->resident_bytes_ += buf.size() * sizeof(float);
    }
    res.status = LoadStatus::kOk;
    res.instrument = std::move(out);
    return res;
  }
};

LoadResult load_instrument_by_index(const fmt::BundleReader& bundle, std::size_t index, const LoadHooks* hooks) {
  return BundleInstrumentLoader::load(bundle, index, hooks);
}

std::uint64_t instrument_head_bytes(const fmt::BundleReader& bundle, std::size_t index) {
  const auto instruments = bundle.instruments();
  if (index >= instruments.size()) return 0;
  const fmt::InstrumentInfo& info = instruments[index];
  const auto samples = bundle.samples();
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

LoadResult load_instrument(const fmt::BundleReader& bundle, std::string_view key) {
  const auto instruments = bundle.instruments();
  for (std::size_t i = 0; i < instruments.size(); ++i)
    if (instruments[i].key == key) return BundleInstrumentLoader::load(bundle, i, nullptr);
  LoadResult res;
  res.status = LoadStatus::kInstrumentNotFound;
  return res;
}

}  // namespace velora::host
