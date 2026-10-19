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
  static LoadResult load(const fmt::BundleReader& bundle, std::size_t index) {
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
    for (std::size_t s = 0; s < info.sample_count; ++s) {
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
        if (bundle.read_frames(info.first_sample + s, 0, frames, tmp) != fmt::Error::kNone ||
            !pcm_to_float(si.format, tmp, frames, out->data_[s])) {
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

LoadResult load_instrument_by_index(const fmt::BundleReader& bundle, std::size_t index) {
  return BundleInstrumentLoader::load(bundle, index);
}

LoadResult load_instrument(const fmt::BundleReader& bundle, std::string_view key) {
  const auto instruments = bundle.instruments();
  for (std::size_t i = 0; i < instruments.size(); ++i)
    if (instruments[i].key == key) return BundleInstrumentLoader::load(bundle, i);
  LoadResult res;
  res.status = LoadStatus::kInstrumentNotFound;
  return res;
}

}  // namespace velora::host
