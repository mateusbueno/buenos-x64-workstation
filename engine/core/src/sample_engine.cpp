#include "velora/core/sample_engine.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <span>

namespace velora::core {

namespace {

// --- constants shared with the sfizz reference (see engine/core/README.md for the sources) -----
constexpr float kReleaseThreshold = 1e-4f;        // exponential release hands over to the fade-out below this
constexpr float kTransitionTime = 50e-3f;         // linear fade-out time after the exponential release
constexpr float kFastReleaseTime = 6e-3f;         // minimum exponential time and the "off" time at sample end
constexpr float kDefaultRelease = 0.001f;         // ampeg_release when the region does not carry one (<= 0)
constexpr float kFreeRunningSustain = 0.0032f;    // sustain at or below this releases by itself
constexpr float kVirtuallyZero = 0.001f;
constexpr int kPanSize = 4095;
constexpr float kMonoPanCompensation = 1.4125375446227544f;    // +3 dB, one pan stage
constexpr float kStereoPanCompensation = 1.9952623149688797f;  // +6 dB, pan and position stages

constexpr unsigned kXfadeSmoothingMs = 5;  // sfizz Config::xfadeSmoothing
constexpr float kMaxRatio = 1024.0f, kMinRatio = 1.0f / 4096.0f;
constexpr float kMaxSeconds = 100.0f;
constexpr float kMaxAmpDb = 144.0f;        // Story 13.4: bound on the amp_keytrack total
constexpr float kMaxAmpGain = 1.6e7f;      // about +144 dB: bound on volume x amplitude

bool finite_time(float t) noexcept { return std::isfinite(t) && t >= 0.0f && t <= kMaxSeconds; }

// Story 5.2 fields: keyswitch notes, crossfade ranges and the filter must be well formed (a region that is not is unplayable).
bool extension_sane(const velora::ports::SampleRegionDesc& d) noexcept {
  auto note_ok = [](std::int16_t k) { return k >= -1 && k <= 127; };
  if (!note_ok(d.sw.lokey) || !note_ok(d.sw.hikey) || !note_ok(d.sw.last) || !note_ok(d.sw.down) || !note_ok(d.sw.up) ||
      !note_ok(d.sw.previous) || !note_ok(d.sw.default_key))
    return false;
  if (d.sw.lokey >= 0 && d.sw.hikey >= 0 && d.sw.lokey > d.sw.hikey) return false;  // an inverted keyswitch range
  if (d.xfin_count > velora::ports::kSampleMaxCcCrossfades || d.xfout_count > velora::ports::kSampleMaxCcCrossfades) return false;
  for (unsigned i = 0; i < d.xfin_count; ++i)
    if (d.xfin[i].cc > 127 || d.xfin[i].lo > d.xfin[i].hi || d.xfin[i].hi > 127) return false;
  for (unsigned i = 0; i < d.xfout_count; ++i)
    if (d.xfout[i].cc > 127 || d.xfout[i].lo > d.xfout[i].hi || d.xfout[i].hi > 127) return false;
  if (static_cast<unsigned>(d.filter_type) > static_cast<unsigned>(velora::ports::SampleFilterType::kBpf2p)) return false;
  if (d.filter_type != velora::ports::SampleFilterType::kNone &&
      !(std::isfinite(d.filter_cutoff_hz) && d.filter_cutoff_hz >= 0.0f && std::isfinite(d.filter_resonance_db)))
    return false;
  return true;
}

// Story 13.4 fields: the amp map must be finite and inside the documented ranges (a region that is not is unplayable).
bool amp_sane(const velora::ports::SampleRegionDesc& d) noexcept {
  auto in = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
  if (!in(d.amp_veltrack, -100.0f, 100.0f) || !in(d.amplitude, 0.0f, 10000.0f) || !in(d.amp_keytrack, -96.0f, 12.0f) || d.amp_keycenter > 127) return false;
  for (float t : {d.vel2attack, d.vel2decay, d.vel2release, d.vel2sustain})
    if (!in(t, -100.0f, 100.0f)) return false;
  if (d.vel_point_count > velora::ports::kSampleMaxVelPoints) return false;
  for (unsigned i = 0; i < d.vel_point_count; ++i)
    if (d.vel_points[i].vel > 127 || !in(d.vel_points[i].gain, 0.0f, 1.0f) || (i > 0 && d.vel_points[i].vel <= d.vel_points[i - 1].vel)) return false;
  return true;
}

// Story 13.5 fields: gates and modulation entries must be well formed and inside the documented ranges (a region that is not is unplayable).
bool cc_sane(const velora::ports::SampleRegionDesc& d) noexcept {
  using velora::ports::SampleCcTarget;
  if (d.cc_gate_count > velora::ports::kSampleMaxCcGates || d.cc_mod_count > velora::ports::kSampleMaxCcMods) return false;
  for (unsigned i = 0; i < d.cc_gate_count; ++i) {
    const auto& g = d.cc_gates[i];
    if (g.cc > 127 || !std::isfinite(g.lo) || !std::isfinite(g.hi) || g.lo < 0.0f || g.hi > 127.0f || g.lo > g.hi) return false;
  }
  for (unsigned i = 0; i < d.cc_mod_count; ++i) {
    const auto& m = d.cc_mods[i];
    const bool source = m.cc <= 127 || m.cc == 131 || m.cc == 135 || m.cc == 136 || m.cc == 137 || m.cc == 140 || m.cc == 141;
    if (!source || static_cast<unsigned>(m.target) > static_cast<unsigned>(SampleCcTarget::kEgRelease) || !std::isfinite(m.depth)) return false;
    float lo = -100.0f, hi = 100.0f;
    switch (m.target) {
      case SampleCcTarget::kAmplitude: lo = 0.0f; hi = 10000.0f; break;
      case SampleCcTarget::kVolume: lo = -144.0f; hi = 48.0f; break;
      case SampleCcTarget::kPan: lo = -200.0f; hi = 200.0f; break;
      case SampleCcTarget::kTune: lo = -12000.0f; hi = 12000.0f; break;
      case SampleCcTarget::kOffset: lo = 0.0f; hi = 2147483647.0f; break;
      default: break;
    }
    if (m.depth < lo || m.depth > hi) return false;
  }
  return true;
}

// Story 13.8 fields: the filter tuning and the second filter must be inside the documented ranges and need a first filter (a region that is not is unplayable).
bool filter_tune_default(const velora::ports::SampleFilterTune& t) noexcept {
  return t.keytrack == 0.0f && t.veltrack == 0.0f && t.cutoff_count == 0 && t.res_count == 0;  // keycenter alone changes nothing
}
bool filter_tune_sane(const velora::ports::SampleFilterTune& t) noexcept {
  auto in = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
  if (!in(t.keytrack, 0.0f, 1200.0f) || !in(t.veltrack, -12000.0f, 12000.0f) || t.keycenter > 127) return false;
  if (t.cutoff_count > velora::ports::kSampleMaxFilterMods || t.res_count > velora::ports::kSampleMaxFilterMods) return false;
  for (int which = 0; which < 2; ++which) {
    const velora::ports::SampleFilterMod* e = which == 0 ? t.cutoff : t.res;
    const unsigned n = which == 0 ? t.cutoff_count : t.res_count;
    for (unsigned i = 0; i < n; ++i) {
      const bool source = e[i].cc <= 127 || e[i].cc == 131 || e[i].cc == 135 || e[i].cc == 136 || e[i].cc == 137 || e[i].cc == 140 || e[i].cc == 141;
      if (!source || !in(e[i].depth, which == 0 ? -12000.0f : 0.0f, which == 0 ? 12000.0f : 96.0f)) return false;
    }
  }
  return true;
}
bool filter_map_sane(const velora::ports::SampleRegionDesc& d) noexcept {
  using F = velora::ports::SampleFilterType;
  if (!filter_tune_sane(d.filter_tune) || !filter_tune_sane(d.filter2_tune)) return false;
  if (static_cast<unsigned>(d.filter2_type) > static_cast<unsigned>(F::kBpf2p)) return false;
  if (d.filter_type == F::kNone && !filter_tune_default(d.filter_tune)) return false;  // a tuning needs a filter
  if (d.filter2_type == F::kNone) return filter_tune_default(d.filter2_tune);
  return d.filter_type != F::kNone && std::isfinite(d.filter2_cutoff_hz) && d.filter2_cutoff_hz >= 0.0f && std::isfinite(d.filter2_resonance_db);
}

// Story 13.6 fields: the off time and the limits must be inside the documented ranges (a region that is not is unplayable).
bool voice_group_sane(const velora::ports::SampleRegionDesc& d) noexcept {
  if (static_cast<unsigned>(d.off_mode) > static_cast<unsigned>(velora::ports::SampleOffMode::kTime)) return false;
  if (!std::isfinite(d.off_time) || d.off_time < 0.0f || d.off_time > 100.0f) return false;
  if (!d.has_off_by && d.off_by != 0) return false;
  if (d.group_polyphony != 0 && d.group == 0) return false;
  return d.polyphony <= velora::ports::kSampleMaxVoiceLimit && d.group_polyphony <= velora::ports::kSampleMaxVoiceLimit &&
         d.note_polyphony <= velora::ports::kSampleMaxVoiceLimit;
}

// Story 13.7 fields: the bend range and the controller triggers must be inside the documented ranges (a region that is not is unplayable).
bool bend_trigger_sane(const velora::ports::SampleRegionDesc& d) noexcept {
  auto in = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
  if (!in(d.bend_up, -12000.0f, 12000.0f) || !in(d.bend_down, -12000.0f, 12000.0f) || !in(d.bend_step, 1.0f, 12000.0f)) return false;
  if (d.cc_trigger_count > velora::ports::kSampleMaxCcTriggers) return false;
  for (unsigned i = 0; i < velora::ports::kSampleMaxCcTriggers; ++i) {
    const auto& t = d.cc_triggers[i];
    if (i >= d.cc_trigger_count) {  // unused entries stay at their defaults (the count and the loaded entries agree)
      if (t.cc != 0 || t.lo != 0.0f || t.hi != 127.0f) return false;
      continue;
    }
    if (t.cc > 127 || !in(t.lo, 0.0f, 127.0f) || !in(t.hi, 0.0f, 127.0f) || t.lo > t.hi) return false;
    if (i > 0 && t.cc <= d.cc_triggers[i - 1].cc) return false;  // ascending and unique
  }
  return !(d.no_note && d.cc_trigger_count == 0);  // a region nothing can start
}

// Story 13.9 fields: region EQ and the dedicated LFOs / EGs must be inside the documented ranges (a region that is not is unplayable).
bool mod_sane(const velora::ports::SampleRegionDesc& d) noexcept {
  auto in = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
  const velora::ports::SampleModDesc& m = d.mod;
  const velora::ports::SampleLfoDesc* lfos[3] = {&m.amp, &m.pitch, &m.fil};
  for (int i = 0; i < 3; ++i) {
    const velora::ports::SampleLfoDesc& l = *lfos[i];
    if (!l.on) continue;
    const float dmax = i == 0 ? 10.0f : 1200.0f;
    if (l.wave > 7 || !in(l.depth, -dmax, dmax) || l.depth == 0.0f || !in(l.freq, 0.0f, 100.0f) || !in(l.delay, 0.0f, 30.0f) || !in(l.fade, 0.0f, 30.0f)) return false;
  }
  for (const velora::ports::SampleEgDesc* e : {&m.pitch_eg, &m.fil_eg}) {
    if (!e->on) continue;
    for (float t : {e->delay, e->attack, e->hold, e->decay, e->release})
      if (!in(t, 0.0f, 100.0f)) return false;
    if (!in(e->start, 0.0f, 100.0f) || !in(e->sustain, 0.0f, 100.0f) || !in(e->depth, -12000.0f, 12000.0f) || !in(e->vel2depth, -12000.0f, 12000.0f) || (e->depth == 0.0f && e->vel2depth == 0.0f)) return false;
  }
  if (m.eq_count > velora::ports::kSampleMaxEqBands) return false;
  for (unsigned b = 0; b < m.eq_count; ++b) {
    const velora::ports::SampleEqBand& q = m.eq[b];
    if (q.type < 1 || q.type > 3 || !in(q.freq, 0.0f, 20000.0f) || !in(q.bw, 0.01f, 4.0f) || !in(q.gain, -96.0f, 60.0f) || !in(q.vel2freq, -30000.0f, 30000.0f) || !in(q.vel2gain, -96.0f, 96.0f))
      return false;
    for (int w = 0; w < 3; ++w) {
      const velora::ports::SampleFilterMod* e = w == 0 ? q.freq_mods : w == 1 ? q.bw_mods : q.gain_mods;
      const unsigned n = w == 0 ? q.freq_count : w == 1 ? q.bw_count : q.gain_count;
      const float lim = w == 0 ? 20000.0f : w == 1 ? 4.0f : 96.0f;
      if (n > velora::ports::kSampleMaxFilterMods) return false;
      for (unsigned i = 0; i < n; ++i) {
        const bool source = e[i].cc <= 127 || e[i].cc == 131 || e[i].cc == 135 || e[i].cc == 136 || e[i].cc == 137 || e[i].cc == 140 || e[i].cc == 141;
        if (!source || !in(e[i].depth, -lim, lim)) return false;
      }
    }
  }
  return true;
}
// The pitch factor bound of the pitch LFO and EG (cents at the extreme: |depth| of the LFO, |depth| + |vel2depth| of the EG).
float mod_pitch_cents_max(const velora::ports::SampleRegionDesc& d) noexcept {
  float c = 0.0f;
  if (d.mod.pitch.on) c += std::fabs(d.mod.pitch.depth);
  if (d.mod.pitch_eg.on) c += std::fabs(d.mod.pitch_eg.depth) + std::fabs(d.mod.pitch_eg.vel2depth);
  return c;
}

// The curves a region's entries name must have finite, bounded points (bad_curve[i]: the instrument's definition of curve i is not).
bool cc_curves_ok(const velora::ports::SampleRegionDesc& d, const std::array<bool, 256>& bad_curve) noexcept {
  for (unsigned i = 0; i < d.cc_mod_count && i < velora::ports::kSampleMaxCcMods; ++i)
    if (bad_curve[d.cc_mods[i].curve]) return false;
  for (const velora::ports::SampleFilterTune* t : {&d.filter_tune, &d.filter2_tune}) {  // Story 13.8
    for (unsigned i = 0; i < t->cutoff_count && i < velora::ports::kSampleMaxFilterMods; ++i)
      if (bad_curve[t->cutoff[i].curve]) return false;
    for (unsigned i = 0; i < t->res_count && i < velora::ports::kSampleMaxFilterMods; ++i)
      if (bad_curve[t->res[i].curve]) return false;
  }
  for (unsigned b = 0; b < d.mod.eq_count && b < velora::ports::kSampleMaxEqBands; ++b) {  // Story 13.9
    const velora::ports::SampleEqBand& q = d.mod.eq[b];
    for (unsigned i = 0; i < q.freq_count && i < velora::ports::kSampleMaxFilterMods; ++i) if (bad_curve[q.freq_mods[i].curve]) return false;
    for (unsigned i = 0; i < q.bw_count && i < velora::ports::kSampleMaxFilterMods; ++i) if (bad_curve[q.bw_mods[i].curve]) return false;
    for (unsigned i = 0; i < q.gain_count && i < velora::ports::kSampleMaxFilterMods; ++i) if (bad_curve[q.gain_mods[i].curve]) return false;
  }
  return true;
}

// The engine's own starting value of a controller (sfizz: CC7 100, CC10 centre, CC11 127, the rest 0).
float engine_cc_default(unsigned cc) noexcept { return cc == 7 ? 100.0f : cc == 10 ? 63.5f : cc == 11 ? 127.0f : 0.0f; }

// Story 3.7 fields: aftertouch modulation depths must be finite and inside documented ranges (cutoff requires a first filter).
bool aftertouch_sane(const velora::ports::SampleRegionDesc& d) noexcept {
  auto in = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
  if (!in(d.volume_aftertouch_db, -144.0f, 48.0f) || !in(d.cutoff_aftertouch_cents, -12000.0f, 12000.0f)) return false;
  if (d.cutoff_aftertouch_cents != 0.0f && d.filter_type == velora::ports::SampleFilterType::kNone) return false;
  return true;
}

// Everything the audio thread later turns into counts, rates and gains must be finite and bounded.
bool region_params_sane(const velora::ports::SampleRegionDesc& d) noexcept {
  return extension_sane(d) && amp_sane(d) && cc_sane(d) && filter_map_sane(d) && mod_sane(d) && aftertouch_sane(d) && voice_group_sane(d) && bend_trigger_sane(d) && finite_time(d.ampeg_delay) && finite_time(d.ampeg_attack) && finite_time(d.ampeg_hold) &&
         finite_time(d.ampeg_decay) && finite_time(d.ampeg_release) && std::isfinite(d.ampeg_start) &&
         std::isfinite(d.ampeg_sustain) && std::isfinite(d.lorand) && std::isfinite(d.hirand) &&
         d.lorand < d.hirand && d.seq_length >= 1 && d.seq_position >= 1 && d.seq_position <= d.seq_length &&
         std::isfinite(d.tune_cents) && std::isfinite(d.volume_db) && d.volume_db >= -144.0f &&
         d.volume_db <= 48.0f && std::isfinite(d.pan);
}

constexpr double kHalfPi = 1.57079632679489661923;

// sfizz maps a SFZ time to samples/rates in single precision; so do we.
std::int32_t seconds_to_samples(float t, float sr) noexcept {
  return t <= 0.0f ? 0 : static_cast<std::int32_t>(t * sr);
}
float seconds_to_lin_rate(float t, float sr) noexcept { return t <= 0.0f ? 1.0f : 1.0f / (sr * t); }
float seconds_to_exp_rate(float t, float sr) noexcept {
  if (t <= 0.0f) return 0.0f;
  t = std::max(kFastReleaseTime, t);
  return std::exp(-9.0f / (t * sr));
}

float pan_lookup(const std::array<float, 4096>& table, float p) noexcept {
  const long idx = static_cast<long>(std::lround(p * static_cast<float>(kPanSize - 1)));
  return table[static_cast<std::size_t>(std::clamp(idx, 0L, static_cast<long>(kPanSize)))];
}

// Story 13.5: sfizz Curve::lerpFill on a 128-entry table whose set[] entries are given (the ends always are).
void curve_lerp_fill(std::array<float, 128>& t, const std::array<bool, 128>& set) noexcept {
  int left = 0, right = 1;
  while (right < 128) {
    for (; right < 128 && !set[static_cast<std::size_t>(right)]; ++right) {}
    const int length = right - left;
    if (length > 1) {
      const float mu = (t[static_cast<std::size_t>(right)] - t[static_cast<std::size_t>(left)]) / static_cast<float>(length);
      const float start = t[static_cast<std::size_t>(left)];
      for (int i = 0; i < length; ++i) t[static_cast<std::size_t>(left + i)] = start + mu * static_cast<float>(i);
    }
    left = right++;
  }
}

// The curve table for SFZ curve index `index`: the instrument's own <curve> definition, else sfizz's predefined curves 0..6, else the linear default.
std::array<float, 128> build_cc_curve(std::uint8_t index, std::span<const velora::ports::SampleCurve> defs) noexcept {
  std::array<float, 128> t{};
  std::array<bool, 128> set{};
  auto bipolar = [&](float v1, float v2) {
    t[0] = v1;
    t[127] = v2;
    set[0] = set[127] = true;
    curve_lerp_fill(t, set);
  };
  for (const auto& c : defs) {
    if (c.index != index) continue;
    t[0] = 0.0f; set[0] = true;  // the ends default to (0, 0) and (127, 1)
    t[127] = 1.0f; set[127] = true;
    for (const auto& pt : c.points) {
      if (pt.x > 127) continue;
      t[pt.x] = pt.y;
      set[pt.x] = true;
    }
    curve_lerp_fill(t, set);
    return t;
  }
  switch (index) {
    case 1: bipolar(-1.0f, 1.0f); break;
    case 2: bipolar(1.0f, 0.0f); break;
    case 3: bipolar(1.0f, -1.0f); break;
    case 4:
      for (int i = 0; i < 128; ++i) { const double x = i / 127.0; t[static_cast<std::size_t>(i)] = static_cast<float>(x * x); }
      break;
    case 5:
      t[0] = 0.0f; t[127] = 1.0f;
      for (int i = 1; i < 127; ++i) t[static_cast<std::size_t>(i)] = static_cast<float>(std::sqrt(i / 127.0));
      break;
    case 6:
      t[0] = 1.0f; t[127] = 0.0f;
      for (int i = 1; i < 127; ++i) t[static_cast<std::size_t>(i)] = static_cast<float>(std::sqrt(1.0 - i / 127.0));
      break;
    default: bipolar(0.0f, 1.0f); break;  // 0 and every unknown index: the linear default
  }
  return t;
}

// sfizz pan stage gains (Voice::panStageMono / panStageStereo) for a pan in percent.
void pan_gains(const std::array<float, 4096>& table, float pan, bool stereo, float& chan_l, float& chan_r) noexcept {
  const float p = std::clamp((pan / 100.0f + 1.0f) * 0.5f, 0.0f, 1.0f);
  const float pl = pan_lookup(table, p), pr = pan_lookup(table, 1.0f - p);
  if (stereo) {
    const float pos = pan_lookup(table, 0.5f);
    chan_l = pl * pos * kStereoPanCompensation;
    chan_r = pr * pos * kStereoPanCompensation;
  } else {
    chan_l = pl * kMonoPanCompensation;
    chan_r = pr * kMonoPanCompensation;
  }
}

inline void hermite_weights(float c, float w[4]) noexcept {
  for (int i = -1; i < 3; ++i) {
    const float x = std::fabs(static_cast<float>(i) - c);
    const float x2 = x * x, x3 = x2 * x;
    const float q = 2.5f * x2;
    const float p1 = 1.0f - q + 1.5f * x3;
    const float p2 = 2.0f - 4.0f * x + q - 0.5f * x3;
    float y = 0.0f;
    y = (x < 2.0f) ? p2 : y;
    y = (x < 1.0f) ? p1 : y;
    w[i + 1] = y;
  }
}

}  // namespace

SampleEngine::SampleEngine(const Config& cfg) : cfg_(cfg) {
  if (cfg_.max_voices == 0) cfg_.max_voices = 1;
  if (cfg_.sample_rate == 0) cfg_.sample_rate = 48000;
  capacity_ = cfg_.max_voices;
  voices_.resize(capacity_);
  for (int i = 0; i < kPanSize; ++i)
    pan_table_[static_cast<std::size_t>(i)] = static_cast<float>(std::cos(i * (kHalfPi / (kPanSize - 1))));
  for (std::size_t i = kPanSize; i < pan_table_.size(); ++i) pan_table_[i] = pan_table_[kPanSize - 1];
  tr_release_rate_off_ = seconds_to_exp_rate(kFastReleaseTime, static_cast<float>(cfg_.sample_rate));
  inv_sr_ = 1.0f / static_cast<float>(cfg_.sample_rate);
  filt_smooth_step_ = 1.0 - std::exp(-1000.0 / static_cast<double>(cfg_.sample_rate));
  set_seed(cfg_.seed);
  // CcState defaults are sfizz's (MidiState / Synth::resetDefaultCCValues): CC7 volume 100, CC10 pan centre, CC11 expression 127.
  for (auto& part : cc_) {
    part.fill(0.0f);
    part[7] = 100.0f;
    part[10] = 63.5f;
    part[11] = 127.0f;
  }
  // The CC crossfade smoother is sfizz's `Smoother` (= LinearSmoother) with xfadeSmoothing = 5 ms: int(5e-3 * fs) frames.
  xf_frames_ = static_cast<std::int32_t>(1e-3f * static_cast<float>(kXfadeSmoothingMs) * static_cast<float>(cfg_.sample_rate));
}

void SampleEngine::set_seed(std::uint64_t seed) noexcept {
  // splitmix64 scramble so that nearby seeds give unrelated streams; never zero for xorshift.
  std::uint64_t z = seed + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  rng_ = z ? z : 0x2545F4914F6CDD1Dull;
}

float SampleEngine::next_random() noexcept {
  // xorshift64*; the top 24 bits give a float uniformly distributed in [0, 1).
  std::uint64_t x = rng_;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rng_ = x;
  const std::uint64_t r = x * 0x2545F4914F6CDD1Dull;
  return static_cast<float>(r >> 40) * (1.0f / 16777216.0f);
}

std::shared_ptr<SampleBank> SampleEngine::prepare(const velora::ports::SampleSource& src, std::shared_ptr<const void> owner) const {
  static std::atomic<std::uint64_t> next_bank_id{1};
  auto bank = std::make_shared<SampleBank>();
  bank->id = next_bank_id.fetch_add(1, std::memory_order_relaxed);
  bank->owner = std::move(owner);
  std::vector<SampleBankRegion>& regions = bank->regions;
  const velora::ports::InstrumentView view = src.instrument();
  const float sr = static_cast<float>(cfg_.sample_rate);
  regions.reserve(view.regions.size());
  std::array<bool, 256> bad_curve{};
  for (const auto& c : view.curves)
    for (const auto& pt : c.points)
      if (!std::isfinite(pt.y) || std::fabs(pt.y) > 1.0e6f || pt.x > 127) bad_curve[c.index] = true;
  for (const auto& cd : view.cc_defaults)  // Story 13.5: the controller values a part starts with when it binds this bank
    if (cd.cc <= 127 && std::isfinite(cd.value) && cd.value >= 0.0f && cd.value <= 127.0f) bank->cc_defaults.push_back(cd);
  for (const auto& d : view.regions) {
    RegionRt r;
    r.d = d;
    if (d.sample_index < view.samples.size()) {
      const velora::ports::SamplePcm& pcm = view.samples[d.sample_index];
      r.frames = pcm.frames;
      r.left = pcm.left;
      r.right = pcm.right;
      r.has_tail = pcm.total_frames > pcm.frames;
      r.total_frames = r.has_tail ? pcm.total_frames : pcm.frames;
      if (d.has_end && d.end > 0)  // Story 13.3: exclusive end, clamped to the stored length of the sample
        r.end_cut = static_cast<std::int64_t>(std::min<std::uint64_t>(d.end, r.total_frames));
      if (r.has_tail) {  // Story 1.17: the tail's reader and the resident loop segments travel with the region (borrowed; the bank's owner keeps them alive)
        r.tail_source = pcm.tail_source;
        r.tail_sample = pcm.tail_sample;
        r.segs = pcm.segments;
        r.seg_count = pcm.segment_count;
      }
      r.damage = pcm.damaged_blocks;
      r.damage_block_frames = pcm.damaged_block_frames;
      r.damage_block_count = pcm.damaged_block_count;
      if (r.damage != nullptr && (r.damage_block_frames == 0 || r.damage_block_count == 0)) r.damage = nullptr;  // a map that cannot be indexed is no map
      if (r.damage != nullptr && (r.damage_block_frames & (r.damage_block_frames - 1)) == 0) {
        r.damage_shift = 0;
        while ((std::uint32_t{1} << r.damage_shift) < r.damage_block_frames) ++r.damage_shift;
      }
      r.stereo = pcm.channels == 2;
      r.speed = pcm.sample_rate == 0 ? 0.0f
                                     : static_cast<float>(static_cast<double>(pcm.sample_rate) /
                                                          static_cast<double>(cfg_.sample_rate));
      r.playable = (pcm.channels == 1 || pcm.channels == 2) && pcm.frames > 0 && pcm.left != nullptr &&
                   (pcm.channels == 1 || pcm.right != nullptr) && pcm.sample_rate != 0 &&
                   d.offset < pcm.frames && (r.end_cut < 0 || static_cast<std::int64_t>(d.offset) < r.end_cut) && d.key_lo <= d.key_hi && d.vel_lo <= d.vel_hi &&
                   region_params_sane(d) && cc_curves_ok(d, bad_curve);
      if (r.playable) {
        // Pitch ratio at the extreme keys of the range must be finite and sane.
        const float spd = r.speed;
        const float base = d.tune_cents + 100.0f * static_cast<float>(d.transpose);
        const int khi = d.no_note ? d.key_center : d.key_hi, klo = d.no_note ? d.key_center : d.key_lo;  // a controller-only region plays at its key centre only
        const float up = 100.0f * static_cast<float>(khi - static_cast<int>(d.key_center)) + base;
        const float dn = 100.0f * static_cast<float>(klo - static_cast<int>(d.key_center)) + base;
        const float rmax = std::pow(2.0f, up / 1200.0f) * spd, rmin = std::pow(2.0f, dn / 1200.0f) * spd;
        // Story 13.7: the wheel can add up to bend_up / bend_down cents on top.
        const float bmax = d.bend_enabled && bend_trigger_sane(d) ? std::pow(2.0f, std::max({0.0f, d.bend_up, d.bend_down}) / 1200.0f) : 1.0f;
        const float bmin = d.bend_enabled && bend_trigger_sane(d) ? std::pow(2.0f, std::min({0.0f, d.bend_up, d.bend_down}) / 1200.0f) : 1.0f;
        // Story 13.9: the pitch LFO and EG can add up to mod_pitch_cents_max in either direction.
        const float mc = mod_sane(d) ? mod_pitch_cents_max(d) : 0.0f;
        const float mmax = std::pow(2.0f, mc / 1200.0f), mmin = std::pow(2.0f, -mc / 1200.0f);
        r.playable = std::isfinite(rmax) && std::isfinite(rmin) && rmax * bmax * mmax <= kMaxRatio && rmin * bmin * mmin >= kMinRatio;
      }
    }
    r.disabled = d.has_end && d.end == 0;  // sfizz: end=0 switches the region off
    if (!r.playable && !r.disabled) ++bank->unplayable_regions;

    r.uses_sequence = d.seq_length != 1 || d.seq_position != 1;

    // Loops: only the two looping modes, with the range inside the sample (otherwise no loop, as sfizz).
    if (d.loop_mode == velora::ports::SampleLoopMode::kLoopContinuous ||
        d.loop_mode == velora::ports::SampleLoopMode::kLoopSustain) {
      std::uint64_t loop_frames = r.has_tail ? r.total_frames : r.frames;  // a loop may lie beyond the head; the voice then underruns at the head end
      if (r.end_cut >= 0) loop_frames = std::min<std::uint64_t>(loop_frames, static_cast<std::uint64_t>(r.end_cut));  // a loop must lie inside the played part
      std::int64_t ls = 0, le = static_cast<std::int64_t>(loop_frames) - 1;
      if (d.has_loop_points) {
        ls = static_cast<std::int64_t>(std::min<std::uint64_t>(d.loop_start, INT64_MAX));
        le = static_cast<std::int64_t>(std::min<std::uint64_t>(d.loop_end, INT64_MAX));
      }
      if (loop_frames > 0 && ls <= le && le < static_cast<std::int64_t>(loop_frames)) {
        r.loops = true;
        r.loop_start = ls;
        r.loop_end = le;
        r.loop_size = le + 1 - ls;
        if (r.has_tail) {
          // Where the stream re-seeks on a wrap: the first frame after the resident run (head, then the segments) that starts at the frame before the loop start (the left tap).
          std::int64_t c = std::max<std::int64_t>(ls - 1, 0);
          for (int it = 0; it < 8; ++it) {
            bool moved = false;
            if (c < static_cast<std::int64_t>(r.frames)) {
              c = static_cast<std::int64_t>(r.frames);
              moved = true;
            }
            for (std::uint32_t k = 0; k < r.seg_count; ++k) {
              const auto& sg = r.segs[k];
              if (sg.left != nullptr && c >= static_cast<std::int64_t>(sg.start) && c < static_cast<std::int64_t>(sg.start + sg.frames)) {
                c = static_cast<std::int64_t>(sg.start + sg.frames);
                moved = true;
              }
            }
            if (!moved) break;
          }
          r.loop_seek = c;
        }
      }
    }

    r.base_cents = d.tune_cents + 100.0f * static_cast<float>(d.transpose);
    r.vol_gain = std::pow(10.0f, d.volume_db * 0.05f);
    // Story 13.4: the note-on amplitude map. A region with none of it keeps the old law and the old numbers (shaped = false).
    if (amp_sane(d)) {
      if (d.amplitude != 100.0f) r.vol_gain *= d.amplitude / 100.0f;
      r.vol_gain = std::clamp(r.vol_gain, 0.0f, kMaxAmpGain);  // finite whatever amplitude x volume fold to
      r.vel_shaped = d.amp_veltrack != 100.0f || d.vel_point_count != 0;
      r.amp_veltrack = d.amp_veltrack / 100.0f;
      r.key_gain_db = d.amp_keytrack;
      r.key_center = d.amp_keycenter;
      r.vel2 = d.vel2attack != 0.0f || d.vel2decay != 0.0f || d.vel2sustain != 0.0f || d.vel2release != 0.0f;
      if (d.vel_point_count != 0) {  // sfizz Curve::buildFromVelcurvePoints (linear): ends (0, 0) and (127, 1) unless given, gaps filled by a ramp
        std::array<float, 128> t{};
        std::array<bool, 128> set{};
        t[0] = 0.0f; set[0] = true;
        t[127] = 1.0f; set[127] = true;
        for (unsigned i = 0; i < d.vel_point_count; ++i) {
          t[d.vel_points[i].vel] = d.vel_points[i].gain;
          set[d.vel_points[i].vel] = true;
        }
        int left = 0, right = 1;
        while (right < 128) {
          for (; right < 128 && !set[static_cast<std::size_t>(right)]; ++right) {}
          const int length = right - left;
          if (length > 1) {
            const float mu = (t[static_cast<std::size_t>(right)] - t[static_cast<std::size_t>(left)]) / static_cast<float>(length);
            const float start = t[static_cast<std::size_t>(left)];
            for (int i = 0; i < length; ++i) t[static_cast<std::size_t>(left + i)] = start + mu * static_cast<float>(i);
          }
          left = right++;
        }
        r.vel_table = static_cast<std::int32_t>(bank->vel_tables.size());
        bank->vel_tables.push_back(t);
      }
    }
    // Story 13.5: CC gates and modulation entries. A region without them keeps every flag false (and the bank its empty tables).
    if (cc_sane(d)) {
      r.cc_gate = d.cc_gate_count != 0;
      r.cc_mod = d.cc_mod_count != 0;
      for (unsigned i = 0; i < d.cc_mod_count; ++i) {
        const auto& m = d.cc_mods[i];
        if (m.target >= velora::ports::SampleCcTarget::kEgDelay) r.eg_cc = true;
        if (m.cc == 135 || m.cc == 136) bank->uses_cc_random = true;
        if (bank->cc_curve_slot[m.curve] < 0) {
          bank->cc_curve_slot[m.curve] = static_cast<std::int16_t>(bank->cc_curves.size());
          bank->cc_curves.push_back(build_cc_curve(m.curve, view.curves));
        }
      }
    }
    // Story 13.8: the curve tables and the random pseudo-CCs of the filter entries (the same bank tables as the Story 13.5 entries).
    if (filter_map_sane(d)) {
      for (const velora::ports::SampleFilterTune* t : {&d.filter_tune, &d.filter2_tune})
        for (int which = 0; which < 2; ++which) {
          const velora::ports::SampleFilterMod* e = which == 0 ? t->cutoff : t->res;
          for (unsigned i = 0; i < (which == 0 ? t->cutoff_count : t->res_count); ++i) {
            if (e[i].cc == 135 || e[i].cc == 136) bank->uses_cc_random = true;
            if (bank->cc_curve_slot[e[i].curve] < 0) {
              bank->cc_curve_slot[e[i].curve] = static_cast<std::int16_t>(bank->cc_curves.size());
              bank->cc_curves.push_back(build_cc_curve(e[i].curve, view.curves));
            }
          }
        }
    }
    // Story 13.6: choke groups and polyphony limits. A region with none keeps the bank's flag clear.
    if (voice_group_sane(d)) {
      if (d.off_mode == velora::ports::SampleOffMode::kTime) r.off_rate = seconds_to_exp_rate(std::max(d.off_time, kFastReleaseTime), sr);  // the 6 ms floor first: off_time=0 is a fast cut, not an instant one
      if (d.has_off_by || d.polyphony != 0 || d.group_polyphony != 0 || d.note_polyphony != 0) bank->uses_voice_groups = true;
    }
    // Story 13.7: pitch bend range and controller triggers. A region with neither keeps every flag false and the bank its flags clear.
    if (bend_trigger_sane(d)) {
      if (d.bend_enabled) {
        r.bends = true;
        r.bend_smooth_frames = static_cast<std::int32_t>(1e-3f * static_cast<float>(d.bend_smooth_ms) * sr);  // sfizz LinearSmoother::setSmoothing
        r.bend_fac_max = std::pow(2.0f, std::max({0.0f, d.bend_up, d.bend_down}) / 1200.0f);
        bank->uses_bend = true;
      }
      if (d.cc_trigger_count != 0) {
        r.cc_trigger = true;
        bank->uses_cc_triggers = true;
      }
      r.no_note = d.no_note;
    }
    pan_gains(pan_table_, d.pan, r.stereo, r.chan_l, r.chan_r);

    r.env_delay = seconds_to_samples(d.ampeg_delay, sr);
    r.env_hold = seconds_to_samples(d.ampeg_hold, sr);
    r.env_start = std::clamp(d.ampeg_start / 100.0f, 0.0f, 1.0f);
    r.env_attack_step = seconds_to_lin_rate(d.ampeg_attack, sr);
    r.env_decay_rate = seconds_to_exp_rate(d.ampeg_decay, sr);
    r.env_sustain = std::clamp(d.ampeg_sustain / 100.0f, 0.0f, 1.0f);
    r.env_release_rate = seconds_to_exp_rate(d.ampeg_release > 0.0f ? d.ampeg_release : kDefaultRelease, sr);
    r.env_free_running = r.env_sustain <= kFreeRunningSustain;

    // Story 5.2: keyswitch gating, CC crossfades, filter.
    r.has_keyswitch = d.sw.last >= 0 || d.sw.down >= 0 || d.sw.up >= 0 || d.sw.previous >= 0;
    r.has_xfade = d.xfin_count != 0 || d.xfout_count != 0;
    if (d.filter_type != velora::ports::SampleFilterType::kNone && extension_sane(d)) {
      r.filter = make_sample_filter(d.filter_type, d.filter_cutoff_hz, d.filter_resonance_db, static_cast<double>(cfg_.sample_rate));
      r.filtered = r.filter.kind != SampleFilterCoeffs::Kind::kNone;
      if (r.filtered && filter_map_sane(d)) {  // Story 13.8
        r.filter_dyn = !filter_tune_default(d.filter_tune);
        if (d.filter2_type != velora::ports::SampleFilterType::kNone) {
          r.filter2_on = true;
          r.filter2_dyn = !filter_tune_default(d.filter2_tune);
          r.filter2 = make_sample_filter(d.filter2_type, d.filter2_cutoff_hz, d.filter2_resonance_db, static_cast<double>(cfg_.sample_rate));
          r.filter2_on = r.filter2.kind != SampleFilterCoeffs::Kind::kNone;
        }
      }
    }
    // Story 13.9: region EQ and the dedicated LFOs / EGs. A region with none keeps every flag false (and takes the old render paths, bit for bit).
    if (mod_sane(d)) {
      const velora::ports::SampleModDesc& m = d.mod;
      r.lfo_a = m.amp.on;
      r.lfo_p = m.pitch.on;
      r.lfo_f = m.fil.on && r.filtered;
      r.eg_p = m.pitch_eg.on;
      r.eg_f = m.fil_eg.on && r.filtered;
      if (aftertouch_sane(d)) {
        r.vol_at = d.volume_aftertouch_db != 0.0f;
        r.cut_at = d.cutoff_aftertouch_cents != 0.0f && r.filtered;
      }
      r.modded = r.lfo_a || r.lfo_p || r.lfo_f || r.eg_p || r.eg_f || r.vol_at || r.cut_at;
      r.cutmod = r.lfo_f || r.eg_f || r.cut_at;
      bank->uses_aftertouch = bank->uses_aftertouch || r.vol_at || r.cut_at;
      r.mod_fac_max = std::pow(2.0f, mod_pitch_cents_max(d) / 1200.0f);
      for (unsigned b = 0; b < m.eq_count; ++b) {
        const velora::ports::SampleEqBand& q = m.eq[b];
        if (q.gain != 0.0f || q.vel2gain != 0.0f || q.gain_count != 0) r.eq_on = true;
        for (int w = 0; w < 3; ++w) {
          const velora::ports::SampleFilterMod* e = w == 0 ? q.freq_mods : w == 1 ? q.bw_mods : q.gain_mods;
          for (unsigned i = 0; i < (w == 0 ? q.freq_count : w == 1 ? q.bw_count : q.gain_count); ++i) {
            if (e[i].cc == 135 || e[i].cc == 136) bank->uses_cc_random = true;
            if (bank->cc_curve_slot[e[i].curve] < 0) {
              bank->cc_curve_slot[e[i].curve] = static_cast<std::int16_t>(bank->cc_curves.size());
              bank->cc_curves.push_back(build_cc_curve(e[i].curve, view.curves));
            }
          }
        }
      }
    }
    if (extension_sane(d)) {
      if (d.sw.last >= 0) bank->switch_key[static_cast<std::size_t>(d.sw.last)] = 1;
      if (d.sw.lokey >= 0 || d.sw.hikey >= 0) {  // the keyswitch range: a missing end is the end of the keyboard (SFZ defaults 0 / 127)
        const int lo = d.sw.lokey >= 0 ? d.sw.lokey : 0, hi = d.sw.hikey >= 0 ? d.sw.hikey : 127;
        for (int k = lo; k <= hi; ++k) bank->switch_key[static_cast<std::size_t>(k)] = 1;
        bank->uses_keyswitches = true;
      }
      if (d.sw.default_key >= 0) bank->default_switch = d.sw.default_key;
    }
    bank->uses_keyswitches = bank->uses_keyswitches || r.has_keyswitch;
    bank->uses_filters = bank->uses_filters || r.filtered;
    bank->uses_second_filter = bank->uses_second_filter || (r.filter2_on && r.playable && !r.disabled);  // Story 13.12
    bank->uses_crossfades = bank->uses_crossfades || r.has_xfade;
    regions.push_back(r);
  }

  // Per-note lookup, regions in table order (a note's regions are visited in that order).
  std::array<std::uint32_t, 128> counts{};
  for (const auto& r : regions) {
    if (r.disabled || r.no_note) continue;  // Story 13.7: a controller-only region is in no note's list
    for (unsigned n = r.d.key_lo; n <= std::min<unsigned>(r.d.key_hi, 127); ++n) ++counts[n];
  }
  bank->note_begin[0] = 0;
  for (unsigned n = 0; n < 128; ++n) bank->note_begin[n + 1] = bank->note_begin[n] + counts[n];
  bank->note_list.assign(bank->note_begin[128], 0);
  std::array<std::uint32_t, 128> fill{};
  for (std::uint32_t i = 0; i < regions.size(); ++i) {
    if (regions[i].disabled || regions[i].no_note) continue;
    for (unsigned n = regions[i].d.key_lo; n <= std::min<unsigned>(regions[i].d.key_hi, 127); ++n)
      bank->note_list[bank->note_begin[n] + fill[n]++] = i;
  }

  // Story 13.7: the per-controller lists of triggered regions, in region order. A disabled (end=0) or unplayable region is skipped at run time like for a note.
  {
    std::array<std::uint32_t, 128> cc_counts{};
    for (const auto& r : regions)
      if (r.cc_trigger && !r.disabled)
        for (unsigned i = 0; i < r.d.cc_trigger_count; ++i) ++cc_counts[r.d.cc_triggers[i].cc];
    bank->cct_begin[0] = 0;
    for (unsigned c = 0; c < 128; ++c) bank->cct_begin[c + 1] = bank->cct_begin[c] + cc_counts[c];
    bank->cct_list.assign(bank->cct_begin[128], 0);
    std::array<std::uint32_t, 128> cc_fill{};
    for (std::uint32_t ri = 0; ri < regions.size(); ++ri)
      if (regions[ri].cc_trigger && !regions[ri].disabled)
        for (unsigned i = 0; i < regions[ri].d.cc_trigger_count; ++i) {
          const unsigned c = regions[ri].d.cc_triggers[i].cc;
          bank->cct_list[bank->cct_begin[c] + cc_fill[c]++] = ri;
        }
  }

  bank->seq_counter.assign(regions.size(), 0);
  bank->seq_switched.assign(regions.size(), 0);
  for (std::size_t i = 0; i < regions.size(); ++i) bank->seq_switched[i] = regions[i].uses_sequence ? 0 : 1;
  return bank;
}

bool SampleEngine::set_instrument(const velora::ports::SampleSource& src) {
  clear_instrument();
  default_bank_ = prepare(src);
  unplayable_regions_ = default_bank_->unplayable_regions;
  for (std::uint32_t ch = 0; ch < 16; ++ch) {
    reset_part_keys(ch);
    seed_part_cc(ch);
  }
  return true;
}

void SampleEngine::clear_instrument() {
  for (auto& v : voices_) {
    release_stream(v);
    v = Voice{};
  }
  default_bank_.reset();
  last_vel7_.fill(0);
  last_bank_.fill(0);
  unplayable_regions_ = 0;
  skipped_unplayable_ = 0;
  undeclared_extended_ = 0;
  undeclared_second_ = 0;
  for (std::uint32_t ch = 0; ch < 16; ++ch) reset_part_keys(ch);
}

// --- Story 5.2: keyswitch state, controllers ----------------------------------------------------------------------------------

void SampleEngine::reset_part_keys(std::uint32_t channel) noexcept {
  channel &= 15u;
  KeyState k;
  const SampleBank* bank = bank_for(channel);
  k.last = bank ? bank->default_switch : static_cast<std::int16_t>(-1);
  ks_[channel] = k;
}

void SampleEngine::set_part_sample_bank(std::uint32_t part, SampleBank* bank) noexcept {
  if (part >= part_bank_.size()) return;
  const std::uint64_t id = bank ? bank->id : 0;
  part_bank_[part] = bank;
  if (part_bound_[part] && part_bank_id_[part] == id) return;  // the same instrument again (by token, not by address): nothing changes
  part_bank_id_[part] = id;
  part_bound_[part] = true;
  reset_part_keys(part);
  seed_part_cc(part);
}

// Story 13.5: set_cc / set_hdcc. Seeds win at a bind, except over a controller that received a real message since the previous bind (cc_touched_): a held pedal or a
// wheel is not overwritten. A controller the previous bank seeded and this one does not name goes back to the engine's own value (cc_seeded_).
void SampleEngine::seed_part_cc(std::uint32_t channel) noexcept {
  channel &= 15u;
  const SampleBank* bank = bank_for(channel);
  auto& seeded = cc_seeded_[channel];
  auto& touched = cc_touched_[channel];
  auto bit = [](const std::array<std::uint64_t, 2>& m, unsigned cc) { return ((m[cc >> 6] >> (cc & 63u)) & 1u) != 0; };
  std::array<std::uint64_t, 2> now{};
  if (bank)
    for (const auto& cd : bank->cc_defaults) {
      const unsigned cc = cd.cc & 127u;
      if (bit(touched, cc)) continue;
      cc_[channel][cc] = cd.value;
      now[cc >> 6] |= std::uint64_t{1} << (cc & 63u);
    }
  for (unsigned cc = 0; cc < 128; ++cc)
    if (bit(seeded, cc) && !bit(now, cc) && !bit(touched, cc)) cc_[channel][cc] = engine_cc_default(cc);
  seeded = now;
  touched = {};
}

void SampleEngine::clear_part_banks() noexcept {
  for (std::uint32_t p = 0; p < part_bank_.size(); ++p) {
    const bool was = part_bound_[p];
    part_bank_[p] = nullptr;
    part_bank_id_[p] = 0;
    part_bound_[p] = false;
    if (was) reset_part_keys(p);
  }
}

void SampleEngine::reset_play_state() noexcept {
  cc_started_ = {};
  bend_.fill(0.0f);  // a stale wheel must not bend the first voices of the next set (the block loop replays the keyboard's wheel after a switch)
  aftertouch_.fill(0.0f);
  for (auto& p : poly_aftertouch_) p.fill(0.0f);
  for (std::uint32_t ch = 0; ch < 16; ++ch) {
    reset_part_keys(ch);
    part_engaged_[ch].clear();  // the new Live Set's stored part_attack / part_release arrive right after and engage again where they differ from the default
  }
}

void SampleEngine::set_part_param(std::uint32_t part, std::uint32_t param_id, float value) noexcept { put_part_param(part, param_id, value, false); }
void SampleEngine::set_part_param_edit(std::uint32_t part, std::uint32_t param_id, float value) noexcept { put_part_param(part, param_id, value, true); }

void SampleEngine::put_part_param(std::uint32_t part, std::uint32_t param_id, float value, bool edit) noexcept {
  if (part >= 16 || (param_id != params::kPartAttack && param_id != params::kPartRelease) || !std::isfinite(value)) return;
  if (edit) part_params::engage(part_engaged_[part], param_id - params::kPartAttack);
  else if (!part_params::should_apply(part_engaged_[part], param_id - params::kPartAttack, param_id, value)) return;
  const params::ParamDef* d = params::find(param_id);
  const float ms = d ? std::clamp(value, static_cast<float>(d->min), static_cast<float>(d->max)) : value;
  const float sr = static_cast<float>(cfg_.sample_rate);
  if (param_id == params::kPartAttack) part_attack_step_[part] = seconds_to_lin_rate(ms * 0.001f, sr);
  else part_release_rate_[part] = seconds_to_exp_rate(ms * 0.001f, sr);
}

bool SampleEngine::set_part_cc_replay(std::uint32_t part, std::uint32_t cc, std::uint32_t value32) noexcept {
  return put_part_cc(part, cc, value32, false);
}

bool SampleEngine::set_part_cc(std::uint32_t part, std::uint32_t cc, std::uint32_t value32) noexcept { return put_part_cc(part, cc, value32, true); }

bool SampleEngine::put_part_cc(std::uint32_t part, std::uint32_t cc, std::uint32_t value32, bool fire) noexcept {
  if (cc > 127) return false;
  // 32-bit value -> CC units. A 7-bit MIDI 1.0 value v arrives as the bit repetition of v, which is v / 127 of full scale to
  // better than 1e-8: values within 1e-6 of a whole CC step snap to it, so a controller sitting exactly on a crossfade
  // boundary behaves as in sfizz (which sees the integer).
  double x = static_cast<double>(value32) * (127.0 / 4294967295.0);
  const double nearest = std::floor(x + 0.5);
  if (std::fabs(x - nearest) < 1e-6) x = nearest;
  const float before = cc_[part & 15u][cc];
  cc_[part & 15u][cc] = static_cast<float>(x);
  cc_touched_[part & 15u][cc >> 6] |= std::uint64_t{1} << (cc & 63u);
  // Story 13.7: the controller may move into the trigger range of a region (sfizz Synth::ccDispatch, which runs before the value is stored, so a repeat of the same
  // value starts nothing). The gates of the region see the new value (Layer::updateCCState runs first).
  SampleBank* bank = bank_for(part);
  if (fire && bank != nullptr && bank->uses_cc_triggers && static_cast<float>(x) != before) cc_trigger(*bank, part & 15u, cc, static_cast<float>(x));
  return true;
}

// --- Story 13.7: pitch bend and controller-started voices ------------------------------------------------------------------------

bool SampleEngine::set_part_pitch_bend(std::uint32_t part, std::uint32_t value32) noexcept {
  // A 14-bit MIDI 1.0 bend v arrives as v << 18 (a bit-repeating upscale differs from it by less than 1.3e-4 of the travel). sfizz normalises v - 8192 by 8191 and clamps to
  // +-1 (v = 0 and 1 are both -1): dividing by 2^31 - 2^18 and clamping reproduces it, and a full-resolution MIDI 2.0 value reaches +-1 at the same points.
  const double x = (static_cast<double>(value32) - 2147483648.0) / (2147483648.0 - 262144.0);
  bend_[part & 15u] = static_cast<float>(std::clamp(x, -1.0, 1.0));
  const SampleBank* bank = bank_for(part);
  return bank != nullptr && bank->uses_bend;
}

bool SampleEngine::set_part_aftertouch(std::uint32_t part, std::uint32_t value32) noexcept {
  const double x = static_cast<double>(value32) * (1.0 / 4294967295.0);
  aftertouch_[part & 15u] = static_cast<float>(std::clamp(x, 0.0, 1.0));
  const SampleBank* bank = bank_for(part);
  return bank != nullptr && bank->uses_aftertouch;
}

bool SampleEngine::set_part_poly_aftertouch(std::uint32_t part, std::uint32_t note, std::uint32_t value32) noexcept {
  if (note > 127) return false;
  const double x = static_cast<double>(value32) * (1.0 / 4294967295.0);
  poly_aftertouch_[part & 15u][note & 127u] = static_cast<float>(std::clamp(x, 0.0, 1.0));
  const SampleBank* bank = bank_for(part);
  return bank != nullptr && bank->uses_aftertouch;
}

float SampleEngine::bend_cents(const SampleBankRegion& r, float wheel) noexcept {
  // sfizz Region::getBendInCents, then the quantiser of linearEnvelope(..., step): trunc(cents / step) * step.
  const float c = wheel > 0.0f ? wheel * r.d.bend_up : -wheel * r.d.bend_down;
  return r.d.bend_step > 1.0f ? std::trunc(c / r.d.bend_step) * r.d.bend_step : c;
}

// sfizz Layer::registerCC for every region listening to `cc`: the random number first, then the trigger range (inclusive), then the round robin counter, then the
// layer's switches (keyswitch, controller gates). The voice starts as a note on at the region's pitch_keycenter would for pitch, with no velocity law (sfizz skips
// noteGain for a controller trigger) and the controller's value as its velocity.
void SampleEngine::cc_trigger(SampleBank& bank, std::uint32_t part, std::uint32_t cc, float value) noexcept {
  const std::uint32_t b = bank.cct_begin[cc], e = bank.cct_begin[cc + 1];
  if (b == e) return;
  const float rnd = next_random();  // one draw per controller event that reaches a triggered region: a bank without them never moves the random sequence
  ++event_seq_;                     // the voices of this event are sisters
  const KeyState& ks = ks_[part];
  const std::uint8_t vel7 = static_cast<std::uint8_t>(std::clamp(std::lround(value), 0l, 127l));  // the trigger value as the 7-bit velocity (CC 64 = 127 -> 127)
  bool pseudo_done = false;
  for (std::uint32_t i = b; i < e; ++i) {
    const std::uint32_t ri = bank.cct_list[i];
    const RegionRt& r = bank.regions[ri];
    if (r.d.trigger != velora::ports::SampleTrigger::kAttack) continue;  // a controller voice is an attack (the importer refuses the other modes)
    if (!(rnd >= r.d.lorand && rnd < r.d.hirand)) continue;
    bool in_range = false;
    for (unsigned k = 0; k < r.d.cc_trigger_count; ++k)
      if (r.d.cc_triggers[k].cc == cc && value >= r.d.cc_triggers[k].lo && value <= r.d.cc_triggers[k].hi) in_range = true;
    if (!in_range) continue;
    bank.seq_switched[ri] = (bank.seq_counter[ri]++ % r.d.seq_length) == r.d.seq_position - 1 ? 1 : 0;
    if (r.has_keyswitch && !region_switched(r, ks)) continue;
    if (r.cc_gate && !cc_gate_open(r, part)) continue;
    if (!bank.seq_switched[ri]) continue;
    if (!r.playable) {
      ++skipped_unplayable_;
      continue;
    }
    const std::uint32_t note = r.d.key_center;
    if (!pseudo_done) {
      pseudo_.vel = pseudo_.vel_eg = static_cast<float>(vel7) / 127.0f;  // pseudo-CC 131 is the trigger value for a controller voice
      pseudo_.alt = ks.alt;
      pseudo_.keydelta = ks.keydelta;
      pseudo_.uni = pseudo_.bi = 0.0f;
      if (bank.uses_cc_random) {
        pseudo_.uni = next_random();
        pseudo_.bi = next_random() * 2.0f - 1.0f;
      }
      pseudo_done = true;
    }
    if (bank.uses_voice_groups) apply_voice_limits(bank, ri, part, note, vel7, true);
    if (start_region(bank, ri, part, note, vel7, false, true)) cc_started_[note >> 6] |= std::uint64_t{1} << (note & 63u);
  }
}

bool SampleEngine::key_released(std::uint32_t channel, std::uint32_t note) noexcept {
  if (note > 127) return false;
  channel &= 15u;
  KeyState& ks = ks_[channel];
  const bool was_held = ks.is_held(static_cast<int>(note));
  ks.set_held(static_cast<int>(note), false);
  if (!was_held) return false;  // a key up for a key that was not down (or a repeat) fires nothing

  // Story 13.3, trigger=release_key: fires at the physical key up, whatever the sustain pedal does (trigger=release is started by stop_voice_impl, which the
  // block loop defers to the pedal up). Same gates as a release region: the note-on velocity, round robin, keyswitch, an attack voice still alive.
  SampleBank* bank = bank_for(channel);
  const std::uint64_t started_on = last_bank_[channel * 128u + note];
  if (!bank || (started_on && started_on != bank->id)) return false;
  ++event_seq_;  // Story 13.6
  bool started = false;
  bool drawn = false;
  float rnd = 0.0f;  // drawn at the first candidate only, so a bank without release_key regions never moves the random sequence
  const std::uint8_t vel7 = last_vel7_[channel * 128u + note];
  for (std::uint32_t i = bank->note_begin[note]; i < bank->note_begin[note + 1]; ++i) {
    const std::uint32_t ri = bank->note_list[i];
    const RegionRt& r = bank->regions[ri];
    if (r.d.trigger != velora::ports::SampleTrigger::kReleaseKey) continue;
    if (r.has_keyswitch && !region_switched(r, ks)) continue;
    if (r.cc_gate && !cc_gate_open(r, channel)) continue;
    if (!bank->seq_switched[ri]) continue;
    if (vel7 < r.d.vel_lo || vel7 > r.d.vel_hi) continue;
    if (!drawn) {
      rnd = next_random();
      drawn = true;
      note_pseudo(bank, channel, note, vel7, false);  // Story 13.5: once per key up
    }
    if (!(rnd >= r.d.lorand && rnd < r.d.hirand)) continue;
    if (!playing_attack_voice(r, channel, true)) continue;
    if (!r.playable) {
      ++skipped_unplayable_;
      continue;
    }
    if (bank->uses_voice_groups) apply_voice_limits(*bank, ri, channel, note, vel7);  // Story 13.6
    if (start_region(*bank, ri, channel, note, vel7, true)) started = true;  // a full voice pool drops the release sample, as it would a note
  }
  return started;
}

bool SampleEngine::region_switched(const RegionRt& r, const KeyState& ks) const noexcept {
  const auto& sw = r.d.sw;
  if (sw.last >= 0 && ks.last != sw.last) return false;
  if (sw.down >= 0 && !ks.is_held(sw.down)) return false;
  if (sw.up >= 0 && ks.is_held(sw.up)) return false;
  if (sw.previous >= 0 && ks.prev != sw.previous) return false;
  return true;
}

// The product of the region's CC crossfade gains at the part's current controller values (sfizz Voice::applyCrossfades).
float SampleEngine::crossfade_target(const RegionRt& r, std::uint32_t channel) const noexcept {
  const auto& cc = cc_[channel & 15u];
  float g = 1.0f;
  for (unsigned i = 0; i < r.d.xfin_count; ++i) g *= sample_xf_in_gain(r.d.xfin[i].lo, r.d.xfin[i].hi, cc[r.d.xfin[i].cc], r.d.xf_curve);
  for (unsigned i = 0; i < r.d.xfout_count; ++i) g *= sample_xf_out_gain(r.d.xfout[i].lo, r.d.xfout[i].hi, cc[r.d.xfout[i].cc], r.d.xf_curve);
  return g;
}

// --- Story 13.5: CC gates, pseudo-CCs and CC modulation, all read once when a voice starts ---------------------------------------

// sfizz Layer::ccSwitched_: every locc/hicc condition holds at the part's controller values (both ends inclusive; the importer already widened hicc by the
// fill-gap, so a plain comparison is exact).
bool SampleEngine::cc_gate_open(const RegionRt& r, std::uint32_t channel) const noexcept {
  const auto& cc = cc_[channel & 15u];
  for (unsigned i = 0; i < r.d.cc_gate_count; ++i) {
    const auto& g = r.d.cc_gates[i];
    const float x = cc[g.cc & 127u];
    if (!(x >= g.lo && x <= g.hi)) return false;
  }
  return true;
}

// The pseudo-CCs of the event that is starting voices. Called before the region loop of a note on, a note off (trigger=release) and a key up (release_key).
// A note on advances the part's alternate and key delta, as sfizz's MidiState::noteOnEvent does for every note on (with or without a bank). The two random
// numbers (135, 136) are drawn only when the bank has a region that reads them, so a bank without such an entry leaves the random sequence untouched.
void SampleEngine::note_pseudo(const SampleBank* bank, std::uint32_t channel, std::uint32_t note, std::uint8_t vel7, bool note_on) noexcept {
  const KeyState& ks = ks_[channel & 15u];
  pseudo_.vel = note_on ? static_cast<float>(vel7) / 127.0f : 0.0f;  // sfizz: the velocity controller is the trigger's only for a note on (0 for a release voice)
  pseudo_.vel_eg = note_on ? static_cast<float>(vel7) / 127.0f : static_cast<float>(last_vel7_[(channel & 15u) * 128u + (note & 127u)]) / 127.0f;  // a release voice: the released note's own velocity
  pseudo_.alt = ks.alt;
  pseudo_.keydelta = ks.keydelta;
  pseudo_.uni = 0.0f;
  pseudo_.bi = 0.0f;
  if (bank != nullptr && bank->uses_cc_random) {
    pseudo_.uni = next_random();
    pseudo_.bi = next_random() * 2.0f - 1.0f;
  }
}

// The controller value of a modulation entry: normalized (CC / 127) for a real controller and 131, 135, 136, 137; the raw semitone difference for 140 / 141.
float SampleEngine::cc_norm(unsigned cc, std::uint32_t channel, bool eg) const noexcept {
  if (cc <= 127) return cc_[channel & 15u][cc] / 127.0f;
  switch (cc) {
    case 131: return eg ? pseudo_.vel_eg : pseudo_.vel;
    case 135: return pseudo_.uni;
    case 136: return pseudo_.bi;
    case 137: return pseudo_.alt;
    case 140: return pseudo_.keydelta;
    case 141: return std::fabs(pseudo_.keydelta);
    default: return 0.0f;
  }
}

// sfizz Curve::evalNormalized: linear between the table's entries, the input clamped to 0..127 CC units.
float SampleEngine::cc_curve(const SampleBank& bank, std::uint8_t curve, float norm) const noexcept {
  const std::int16_t slot = bank.cc_curve_slot[curve];
  if (slot < 0) return norm;  // unreachable for a prepared bank (every used curve has a table); the linear value is the safe answer
  const std::array<float, 128>& t = bank.cc_curves[static_cast<std::size_t>(slot)];
  const float x = std::isfinite(norm) ? std::clamp(norm * 127.0f, 0.0f, 127.0f) : 0.0f;
  const int i1 = static_cast<int>(x);
  const int i2 = std::min(127, i1 + 1);
  const float mu = x - static_cast<float>(i1);
  return t[static_cast<std::size_t>(i1)] + mu * (t[static_cast<std::size_t>(i2)] - t[static_cast<std::size_t>(i1)]);
}

// One entry's source value for the modulation matrix targets (amplitude, volume, pan, tune): sfizz's ControllerSource transforms the controller by the curve, except
// the key deltas, which pass through raw. For the envelope (eg = true) sfizz's EGDescription curves every value, the key deltas included.
float SampleEngine::cc_mod_value(const SampleBank& bank, const velora::ports::SampleCcMod& m, std::uint32_t channel, bool eg) const noexcept {
  const float x = cc_norm(m.cc, channel, eg);
  if (!eg && (m.cc == 140 || m.cc == 141)) return x;
  return cc_curve(bank, m.curve, x);
}

// Story 13.8: sfizz FilterHolder::setup plus the constant CC modulation of that instant. The base cutoff is scaled by the key track and the velocity track in cents and
// clamped to 0..20000 Hz, then scaled by the CC entries (sum of depth x curve(controller), cents) and clamped again; the resonance adds the CC entries (dB). All in float,
// as sfizz. Called when a voice starts, on the render thread: plain arithmetic and std::pow / std::sin / std::cos (in make_sample_filter), no allocation or lock.
// Story 13.9 split the arithmetic in two (filter_parts, then the scaling by the entries) so a cutoff that an LFO or an EG moves can recompute it per control interval.
float SampleEngine::mod_entries(const SampleBank& bank, const velora::ports::SampleFilterMod* e, unsigned n, std::uint32_t channel) const noexcept {
  float sum = 0.0f;
  for (unsigned i = 0; i < n; ++i) {
    const float x = cc_norm(e[i].cc, channel, false);
    sum += e[i].depth * ((e[i].cc == 140 || e[i].cc == 141) ? x : cc_curve(bank, e[i].curve, x));
  }
  return sum;
}

SampleEngine::FilterParts SampleEngine::filter_parts(const SampleBank& bank, float cutoff_hz, float resonance_db, const velora::ports::SampleFilterTune& tune,
                                                     std::uint32_t channel, std::uint32_t note, std::uint8_t vel7) const noexcept {
  auto factor = [](float cents) { return std::pow(2.0f, cents / 1200.0f); };
  constexpr float kMaxCut = static_cast<float>(kSampleFilterMaxCutoffHz);
  FilterParts p;
  float base = cutoff_hz;
  base *= factor(tune.keytrack * static_cast<float>(static_cast<int>(note) - static_cast<int>(tune.keycenter)));
  base *= factor(tune.veltrack * (static_cast<float>(vel7) / 127.0f));
  p.base = std::isfinite(base) ? std::clamp(base, 0.0f, kMaxCut) : kMaxCut;
  p.has_cc = tune.cutoff_count != 0;
  if (p.has_cc) p.cc_cents = mod_entries(bank, tune.cutoff, tune.cutoff_count, channel);
  float res = resonance_db;
  for (unsigned i = 0; i < tune.res_count; ++i) {
    const velora::ports::SampleFilterMod& m = tune.res[i];
    const float x = cc_norm(m.cc, channel, false);
    res += m.depth * ((m.cc == 140 || m.cc == 141) ? x : cc_curve(bank, m.curve, x));
  }
  p.res = res;
  return p;
}

SampleFilterCoeffs SampleEngine::voice_filter(const SampleBank& bank, velora::ports::SampleFilterType type, float cutoff_hz, float resonance_db,
                                              const velora::ports::SampleFilterTune& tune, std::uint32_t channel, std::uint32_t note, std::uint8_t vel7) const noexcept {
  constexpr float kMaxCut = static_cast<float>(kSampleFilterMaxCutoffHz);
  const FilterParts p = filter_parts(bank, cutoff_hz, resonance_db, tune, channel, note, vel7);
  float cut = p.base;
  if (p.has_cc) cut = std::clamp(p.base * std::pow(2.0f, std::clamp(p.cc_cents, -48000.0f, 48000.0f) / 1200.0f), 0.0f, kMaxCut);
  return make_sample_filter(type, static_cast<double>(cut), static_cast<double>(p.res), static_cast<double>(cfg_.sample_rate));
}

// Story 13.9: one frame of a dedicated LFO, sfizz LFO.cpp in its SFZ v1 form. Delay frames output 0 and do not advance the phase; the output is the wave at the phase before
// the increment (phase 0 at the start); the fade-in gain runs from 0 to 1 over `fade` seconds from the first frame after the delay.
float SampleEngine::lfo_next(LfoState& s, const velora::ports::SampleLfoDesc& d) const noexcept {
  if (s.delay_left > 0) {
    --s.delay_left;
    return 0.0f;
  }
  const float p = s.phase;
  float y;
  switch (d.wave) {
    case 0: y = -4.0f * p + 2.0f; y = (p < 0.25f) ? (4.0f * p) : y; y = (p > 0.75f) ? (4.0f * p - 4.0f) : y; break;  // triangle
    case 2: y = (p < 0.75f) ? 1.0f : 0.0f; break;
    case 3: y = (p < 0.5f) ? 1.0f : 0.0f; break;
    case 4: y = (p < 0.25f) ? 1.0f : 0.0f; break;
    case 5: y = (p < 0.125f) ? 1.0f : 0.0f; break;
    case 6: y = 2.0f * p - 1.0f; break;
    case 7: y = 1.0f - 2.0f * p; break;
    default: {  // sine (sfizz's parabolic approximation)
      const float x = p + p - 1.0f;
      y = -4.0f * x * (1.0f - std::fabs(x));
      break;
    }
  }
  float np = p + inv_sr_ * d.freq;  // ratio (1) x samplePeriod x frequency
  np = np - static_cast<float>(static_cast<int>(np));
  np += np < 0.0f ? 1.0f : 0.0f;
  s.phase = np;
  if (s.fade_pos < 1.0f) {
    y *= s.fade_pos;
    s.fade_pos = std::min(1.0f, s.fade_pos + s.fade_step);
  }
  return y;
}

// Story 13.9: the pitch / filter EGs follow the amplitude EG into release (key up, pedal, choke, steal release, sample end), as sfizz releases all three together. An EG that is
// still in its delay when that happens never sounded: it goes idle (done, level 0), like an amplitude EG released before its delay (env_release).
void SampleEngine::release_mod_egs(Voice& v, const RegionRt& r) const noexcept {
  ModVoice& md = v.md;
  if (md.released || v.env.s < EnvState::kRelease) return;
  md.released = true;
  for (int i = 0; i < 2; ++i) {
    if (!(i == 0 ? r.eg_p : r.eg_f)) continue;
    Env& e = md.eg[i];
    if (e.s == EnvState::kDelay && e.delay_left > 0) {
      e.s = EnvState::kDone;
      e.v = 0.0f;
    } else if (e.s < EnvState::kRelease) {
      e.s = EnvState::kRelease;
    }
  }
}

// Story 13.9: sets up the modulation state of a voice that is starting (called from start_region on the render thread; no allocation or lock): the three LFOs (delay in
// frames, fade step), the two EGs (the amplitude EG's ADSR with the descriptor's times, sfizz EGDescription defaults) and their depth at this velocity, the base of a
// modulated cutoff, and the EQ bands (frequency, bandwidth and gain at this velocity and controller state, sfizz EQHolder::setup + the constant CC entries).
void SampleEngine::init_mod(Voice& v, const SampleBank& bank, const RegionRt& r, std::uint32_t channel, std::uint32_t note, std::uint8_t vel7) const noexcept {
  namespace P = velora::ports;
  const P::SampleModDesc& m = r.d.mod;
  const float sr = static_cast<float>(cfg_.sample_rate);
  const float vf = static_cast<float>(vel7) / 127.0f;
  ModVoice& md = v.md;
  const bool lfo_on[3] = {r.lfo_a, r.lfo_p, r.lfo_f};
  const P::SampleLfoDesc* ld[3] = {&m.amp, &m.pitch, &m.fil};
  for (int i = 0; i < 3; ++i) {
    if (!lfo_on[i]) continue;
    LfoState& s = md.lfo[i];
    s.phase = 0.0f;
    s.delay_left = ld[i]->delay > 0.0f ? static_cast<std::int32_t>(std::ceil(sr * ld[i]->delay)) : 0;
    s.fade_pos = ld[i]->fade > 0.0f ? 0.0f : 1.0f;
    s.fade_step = ld[i]->fade > 0.0f ? (1.0f / sr) / ld[i]->fade : 0.0f;
  }
  const bool eg_on[2] = {r.eg_p, r.eg_f};
  const P::SampleEgDesc* gd[2] = {&m.pitch_eg, &m.fil_eg};
  for (int i = 0; i < 2; ++i) {
    if (!eg_on[i]) continue;
    const P::SampleEgDesc& d = *gd[i];
    Env& e = md.eg[i];
    e = Env{};
    e.s = EnvState::kDelay;
    e.start = std::clamp(d.start / 100.0f, 0.0f, 1.0f);
    e.v = e.start;
    e.delay_left = seconds_to_samples(d.delay, sr);
    e.hold_left = seconds_to_samples(d.hold, sr);
    e.attack_step = seconds_to_lin_rate(d.attack, sr);
    e.decay_rate = seconds_to_exp_rate(d.decay, sr);
    e.sustain = std::clamp(d.sustain / 100.0f, 0.0f, 1.0f);
    e.release_rate = seconds_to_exp_rate(d.release, sr);
    e.free_running = e.sustain <= kFreeRunningSustain;
    md.eg_depth[i] = std::clamp(d.depth + vf * d.vel2depth, -24000.0f, 24000.0f);
  }
  if (r.cutmod) {
    const FilterParts p = filter_parts(bank, r.d.filter_cutoff_hz, r.d.filter_resonance_db, r.d.filter_tune, channel, note, vel7);
    md.cut_base = p.base;
    md.cut_cc_cents = p.has_cc ? std::clamp(p.cc_cents, -48000.0f, 48000.0f) : 0.0f;
    md.cut_res = p.res;
  }
  for (unsigned b = 0; b < m.eq_count; ++b) {
    const P::SampleEqBand& q = m.eq[b];
    const float freq = q.freq + vf * q.vel2freq + mod_entries(bank, q.freq_mods, q.freq_count, channel);
    const float bw = q.bw + mod_entries(bank, q.bw_mods, q.bw_count, channel);
    const float gain = q.gain + vf * q.vel2gain + mod_entries(bank, q.gain_mods, q.gain_count, channel);
    if (!(gain != 0.0f)) continue;  // a band at 0 dB (or a NaN) is transparent
    md.eq[b].k = make_sample_eq(q.type, static_cast<double>(freq), static_cast<double>(bw), static_cast<double>(gain), static_cast<double>(cfg_.sample_rate));
    md.eq[b].on = md.eq[b].k.kind != SampleFilterCoeffs::Kind::kNone;
  }
}

std::uint32_t SampleEngine::active_voices() const noexcept {
  std::uint32_t n = 0;
  for (const auto& v : voices_) n += v.active ? 1 : 0;
  return n;
}

// --- amplitude envelope: a sample-by-sample port of sfizz's ADSREnvelope --------------------------

void SampleEngine::init_env(Env& e, const SampleBank& bank, const RegionRt& r, std::uint32_t channel, std::uint8_t vel7) const noexcept {
  e.s = EnvState::kDelay;
  e.start = r.env_start;
  e.v = r.env_start;
  e.delay_left = r.env_delay;
  e.hold_left = r.env_hold;
  e.transition_delta = 0.0f;
  e.release_rate = r.env_release_rate;
  e.attack_step = part_engaged_[channel & 15u].test(0) ? part_attack_step_[channel & 15u] : r.env_attack_step;
  e.decay_rate = r.env_decay_rate;
  e.sustain = r.env_sustain;
  e.free_running = r.env_free_running;
  if (r.vel2) {  // Story 13.4: sfizz EGDescription::get*: value + velocity * vel2*, evaluated once at note-on (velocity = vel7 / 127)
    const float sr = static_cast<float>(cfg_.sample_rate);
    const float vf = static_cast<float>(vel7) / 127.0f;
    const velora::ports::SampleRegionDesc& d = r.d;
    if (!part_engaged_[channel & 15u].test(0)) e.attack_step = seconds_to_lin_rate(d.ampeg_attack + vf * d.vel2attack, sr);
    e.decay_rate = seconds_to_exp_rate(d.ampeg_decay + vf * d.vel2decay, sr);
    e.release_rate = seconds_to_exp_rate((d.ampeg_release > 0.0f ? d.ampeg_release : kDefaultRelease) + vf * d.vel2release, sr);
    e.sustain = std::clamp(d.ampeg_sustain / 100.0f + vf * (d.vel2sustain / 100.0f), 0.0f, 1.0f);
    e.free_running = e.sustain <= kFreeRunningSustain;
  }
  if (r.eg_cc) {  // Story 13.5: sfizz EGDescription::get*: (value + velocity * vel2*) + sum of curve(CC) * modifier, evaluated once at note-on (no ampeg_dynamic)
    const float sr = static_cast<float>(cfg_.sample_rate);
    const float vf = static_cast<float>(vel7) / 127.0f;
    const velora::ports::SampleRegionDesc& d = r.d;
    using T = velora::ports::SampleCcTarget;
    auto add = [&](T target, float base, float scale) {
      for (unsigned i = 0; i < d.cc_mod_count; ++i)
        if (d.cc_mods[i].target == target) base += cc_mod_value(bank, d.cc_mods[i], channel, true) * (d.cc_mods[i].depth * scale);
      return base;
    };
    auto secs = [](float t) { return std::clamp(t, 0.0f, kMaxSeconds); };
    const float delay = secs(add(T::kEgDelay, d.ampeg_delay, 1.0f));
    const float attack = secs(add(T::kEgAttack, d.ampeg_attack + vf * d.vel2attack, 1.0f));
    const float hold = secs(add(T::kEgHold, d.ampeg_hold, 1.0f));
    const float decay = secs(add(T::kEgDecay, d.ampeg_decay + vf * d.vel2decay, 1.0f));
    const float release = secs(add(T::kEgRelease, (d.ampeg_release > 0.0f ? d.ampeg_release : kDefaultRelease) + vf * d.vel2release, 1.0f));
    e.start = std::clamp(add(T::kEgStart, d.ampeg_start / 100.0f, 0.01f), 0.0f, 1.0f);
    e.v = e.start;
    e.sustain = std::clamp(add(T::kEgSustain, d.ampeg_sustain / 100.0f + vf * (d.vel2sustain / 100.0f), 0.01f), 0.0f, 1.0f);
    e.delay_left = seconds_to_samples(delay, sr);
    e.hold_left = seconds_to_samples(hold, sr);
    if (!part_engaged_[channel & 15u].test(0)) e.attack_step = seconds_to_lin_rate(attack, sr);
    e.decay_rate = seconds_to_exp_rate(decay, sr);
    e.release_rate = seconds_to_exp_rate(release, sr);
    e.free_running = e.sustain <= kFreeRunningSustain;
  }
}

float SampleEngine::env_next(Env& e, const RegionRt&, bool mod, bool block_end) const noexcept {
  const float sr = static_cast<float>(cfg_.sample_rate);
  for (;;) {
    switch (e.s) {
      case EnvState::kDelay:
        if (e.delay_left-- > 0) {
          e.v = e.start;
          return e.v;
        }
        e.s = EnvState::kAttack;
        continue;
      case EnvState::kAttack:
        e.v += e.attack_step;
        if (e.v < 1.0f) return e.v;
        e.v = 1.0f;
        e.s = EnvState::kHold;
        continue;
      case EnvState::kHold:
        if (e.hold_left-- > 0) return e.v;
        e.s = EnvState::kDecay;
        continue;
      case EnvState::kDecay: {
        e.v *= e.decay_rate;
        if (mod) {  // sfizz ADSREnvelope: decay until the level is at or below the sustain, then exactly the sustain (no ramp); a block ending inside the threshold switches early
          if (e.v > e.sustain) {
            if (block_end && e.v <= e.sustain + kVirtuallyZero) {
              e.s = EnvState::kSustain;
              e.transition_delta = (e.sustain - e.v) / (sr * kTransitionTime);
            }
            return e.v;
          }
          e.v = e.sustain;
          e.s = EnvState::kSustain;
          e.transition_delta = 0.0f;
          continue;
        }
        if (e.v > e.sustain) {
          const float out = e.v;
          if (e.v <= e.sustain + kVirtuallyZero) {
            e.s = EnvState::kSustain;
            e.transition_delta = (e.sustain - e.v) / (sr * kTransitionTime);
          }
          return out;
        }
        e.v = std::max(e.sustain, e.v);
        e.s = EnvState::kSustain;
        e.transition_delta = (e.sustain - e.v) / (sr * kTransitionTime);
        continue;
      }
      case EnvState::kSustain:
        if (e.free_running) {  // a (near) zero sustain level releases by itself
          e.s = EnvState::kRelease;
          continue;
        }
        if (e.v > e.sustain) e.v += e.transition_delta;
        return e.v;
      case EnvState::kRelease: {
        const float nv = e.v * e.release_rate;
        if (nv > kReleaseThreshold) {
          e.v = nv;
          return nv;
        }
        // Exponential tail exhausted: linear fade-out from the last emitted value.
        e.s = EnvState::kFadeout;
        e.transition_delta = -std::max(kReleaseThreshold, e.v) / (sr * kTransitionTime);
        continue;
      }
      case EnvState::kFadeout: {
        const float nv = e.v + e.transition_delta;
        if (nv > 0.0f) {
          e.v = nv;
          return nv;
        }
        e.s = EnvState::kDone;
        e.v = 0.0f;
        continue;
      }
      case EnvState::kDone:
      default:
        e.v = 0.0f;
        return 0.0f;
    }
  }
}

bool SampleEngine::env_release(Env& e, const RegionRt&) const noexcept {
  if (e.s == EnvState::kDelay && e.delay_left > 0) return false;  // released before it ever sounded
  if (e.s < EnvState::kRelease) e.s = EnvState::kRelease;
  return true;
}

void SampleEngine::env_fast_release(Env& e) const noexcept {
  e.release_rate = tr_release_rate_off_;
  if (e.s < EnvState::kRelease) e.s = EnvState::kRelease;
}

// --- voices ---------------------------------------------------------------------------------------

bool SampleEngine::start_region(const SampleBank& bank, std::uint32_t region, std::uint32_t channel, std::uint32_t note,
                                std::uint8_t vel7, bool release_trigger, bool cc_voice) noexcept {
  const std::uint32_t slot = acquire_slot(channel, note);
  if (slot == VoiceAllocator::kNoSlot || slot >= capacity_) return false;
  Voice* v = &voices_[slot];
  const RegionRt& r = bank.regions[region];

  // Pitch: key tracking (100 cents per key) from pitch_keycenter, tune, transpose; single precision.
  float cents = 100.0f * (static_cast<float>(note) - static_cast<float>(r.d.key_center));
  cents += r.base_cents;
  const float ratio = std::pow(2.0f, cents / 1200.0f) * r.speed;

  // Amplitude velocity tracking at the SFZ default amp_veltrack=100: gain = velocity squared.
  const float vf = static_cast<float>(vel7) / 127.0f;
  float vgain = 1.0f - 1.0f * (1.0f - vf * vf);
  if (r.vel_shaped) {
    // Story 13.4, sfizz velocityCurve(): g = curve(velocity) (a point table, linear between entries) or velocity squared; the gain is
    // 1 - |veltrack| * (1 - g), or |veltrack| * (1 - g) for a negative veltrack. Evaluated here, once, at note-on.
    float g = vf * vf;
    if (r.vel_table >= 0) {
      const std::array<float, 128>& t = bank.vel_tables[static_cast<std::size_t>(r.vel_table)];
      const float x = std::clamp(vf * 127.0f, 0.0f, 127.0f);
      const int i1 = static_cast<int>(x);
      const int i2 = std::min(127, i1 + 1);
      const float mu = x - static_cast<float>(i1);
      g = t[static_cast<std::size_t>(i1)] + mu * (t[static_cast<std::size_t>(i2)] - t[static_cast<std::size_t>(i1)]);
    }
    const float inv = std::fabs(r.amp_veltrack) * (1.0f - g);
    vgain = r.amp_veltrack < 0.0f ? inv : 1.0f - inv;
  }
  if (cc_voice) vgain = 1.0f;  // Story 13.7: sfizz skips noteGain (velocity law, amp_keytrack) for a controller trigger
  float base = vgain * r.vol_gain;
  if (r.key_gain_db != 0.0f && !cc_voice) {  // amp_keytrack: dB per key from amp_keycenter, the total bounded so the gain stays finite
    const float db = std::clamp(r.key_gain_db * static_cast<float>(static_cast<int>(note) - static_cast<int>(r.key_center)), -kMaxAmpDb, kMaxAmpDb);
    base *= std::pow(10.0f, db * 0.05f);
  }

  // Story 13.5: CC modulation of amplitude, volume, pan, tune and offset, read once here (the envelope entries are read in init_env). A region without entries
  // skips all of it, so its numbers are exactly the ones above.
  float chan_l = r.chan_l, chan_r = r.chan_r;
  double start_pos = static_cast<double>(r.d.offset);
  float cc_ratio = ratio;
  if (r.cc_mod) {
    using T = velora::ports::SampleCcTarget;
    const velora::ports::SampleRegionDesc& d = r.d;
    float amp = 1.0f, db = 0.0f, pan_add = 0.0f, cents_add = 0.0f;
    bool has_amp = false, has_db = false, has_pan = false, has_tune = false;
    std::int64_t off_add = 0;
    bool has_off = false;
    for (unsigned i = 0; i < d.cc_mod_count; ++i) {
      const velora::ports::SampleCcMod& m = d.cc_mods[i];
      switch (m.target) {
        case T::kAmplitude: amp *= (m.depth / 100.0f) * cc_mod_value(bank, m, channel, false); has_amp = true; break;  // multiplicative: a depth of 0 silences, as sfizz
        case T::kVolume: db += m.depth * cc_mod_value(bank, m, channel, false); has_db = true; break;
        case T::kPan: pan_add += m.depth * cc_mod_value(bank, m, channel, false); has_pan = true; break;
        case T::kTune: cents_add += m.depth * cc_mod_value(bank, m, channel, false); has_tune = true; break;
        case T::kOffset:  // sfizz sampleOffset(): += int64(depth * controller), no curve
          off_add += static_cast<std::int64_t>(m.depth * cc_norm(m.cc, channel, false));
          has_off = true;
          break;
        default: break;
      }
    }
    if (has_amp) base *= std::clamp(amp, 0.0f, kMaxAmpGain);
    if (has_db) base *= std::pow(10.0f, std::clamp(db, -kMaxAmpDb, kMaxAmpDb) * 0.05f);
    if (base > kMaxAmpGain) base = kMaxAmpGain;
    if (has_pan) pan_gains(pan_table_, std::clamp(d.pan + pan_add, -1.0e4f, 1.0e4f), r.stereo, chan_l, chan_r);
    if (has_tune) cc_ratio = std::clamp(std::pow(2.0f, (cents + std::clamp(cents_add, -12000.0f, 12000.0f)) / 1200.0f) * r.speed, kMinRatio, kMaxRatio);
    if (has_off) {  // clamped to the resident part of the sample (a streamed tail is never a start position)
      const std::int64_t last = static_cast<std::int64_t>(r.end_cut >= 0 ? std::min<std::uint64_t>(r.frames, static_cast<std::uint64_t>(r.end_cut)) : r.frames) - 1;
      start_pos = static_cast<double>(std::clamp<std::int64_t>(static_cast<std::int64_t>(d.offset) + off_add, 0, std::max<std::int64_t>(last, 0)));
    }
  }

  release_stream(*v);  // a free slot holds no ring (every end gives it back); defence in depth, so a ring can never leak through a reused slot
  *v = Voice{};
  v->active = true;
  v->release_trigger = release_trigger;
  v->cc_voice = cc_voice;
  v->channel = channel;
  v->note = note;
  v->region = region;
  v->bank = &bank;
  v->vel7 = vel7;
  v->start_time = clock_;
  v->order = ++voice_order_;  // Story 13.6
  v->event = event_seq_;
  v->pos = start_pos;
  v->ratio = cc_ratio;
  v->gain_l = base * chan_l;
  v->gain_r = base * chan_r;
  v->base_gain = base;
  if (r.bends) {  // Story 13.7: sfizz resets the bend smoother to the wheel's current position when a voice starts: no ramp
    v->bend_tgt = v->bend_cur = v->bend_prev = bend_cents(r, bend_[channel & 15u]);
    v->bend_fac = v->bend_cur == 0.0f ? 1.0f : std::pow(2.0f, v->bend_cur / 1200.0f);
  }
  init_env(v->env, bank, r, channel, vel7);
  if (r.has_xfade) {  // sfizz resetCrossfades(): the smoother starts at the current gain, so a new note does not ramp
    v->xf_tgt = crossfade_target(r, channel);
    v->xf_cur = v->xf_tgt;
    v->xf_step = 0.0f;
  }
  if (r.filtered) {  // Story 13.8: the voice owns its coefficients; a region without tuning hands over its prepared ones
    const velora::ports::SampleRegionDesc& fd = r.d;
    v->fk = r.filter_dyn ? voice_filter(bank, fd.filter_type, fd.filter_cutoff_hz, fd.filter_resonance_db, fd.filter_tune, channel, note, vel7) : r.filter;
    if (r.filter2_on)
      v->fk2 = r.filter2_dyn ? voice_filter(bank, fd.filter2_type, fd.filter2_cutoff_hz, fd.filter2_resonance_db, fd.filter2_tune, channel, note, vel7) : r.filter2;
  }
  if (r.modded || r.eq_on) {  // Story 13.9
    init_mod(*v, bank, r, channel, note, vel7);
    if (!(cfg_.extended_voices && cfg_.modulated_voices)) ++undeclared_mod_;
  }
  if ((r.filtered || r.has_xfade || r.modded || r.eq_on) && !cfg_.extended_voices) ++undeclared_extended_;  // the modulation declaration sits on top of the extended one
  if (r.filter2_on && !second_filter_declared()) ++undeclared_second_;
  if (stream_ != nullptr && r.has_tail && r.tail_source != nullptr) {
    // Story 1.17: register the stream (non-blocking). The ring starts at the head end: frames below it are read from the head. No ring = the Story 1.16 placeholder.
    velora::ports::StreamOpen req;
    req.source = r.tail_source;
    req.sample = r.tail_sample;
    req.total_frames = r.total_frames;
    req.channels = r.stereo ? 2 : 1;
    req.start = r.frames;
    if (velora::ports::StreamRing* ring = stream_->open(req)) {
      v->ring = ring;
      v->streamed = true;
      v->seg = 1;
      v->seg_start = r.frames;
    } else {
      stream_open_failures_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return true;
}

// --- choke groups and polyphony limits (Story 13.6) ---------------------------------------------------

// sfizz Voice::Impl::off: the release time of the cut (fast = 6 ms, time = off_time, normal = the voice's own), then the release itself. A voice released before its
// envelope delay elapsed never sounds (dropped, as a note off does). The sustain pedal is not consulted: a cut is not a key release, so it never waits for the pedal.
void SampleEngine::off_voice(Voice& v) noexcept {
  const RegionRt& r = v.bank->regions[v.region];
  if (r.d.off_mode == velora::ports::SampleOffMode::kFast) v.env.release_rate = tr_release_rate_off_;
  else if (r.d.off_mode == velora::ports::SampleOffMode::kTime) v.env.release_rate = r.off_rate;
  else if (part_engaged_[v.channel & 15u].test(1)) v.env.release_rate = part_release_rate_[v.channel & 15u];  // normal: the part's release knob replaces the region's
  v.offed = true;
  if (!env_release(v.env, r)) end_voice(v);
}

// sfizz SisterVoiceRing::offAllSisters: the voices that one note on (or note off) started are released together.
void SampleEngine::off_sisters(std::uint64_t event) noexcept {
  for (auto& v : voices_)
    if (v.active && !v.stolen && !v.offed && v.event == event) off_voice(v);
}

void SampleEngine::apply_voice_limits(const SampleBank& bank, std::uint32_t ri, std::uint32_t channel, std::uint32_t note, std::uint8_t vel7, bool from_cc) noexcept {
  const velora::ports::SampleRegionDesc& d = bank.regions[ri].d;
  // A voice counts for the limits while it is alive on this bank and part and has not been cut already (sfizz offedOrFree). One that was merely released by its
  // note off and is still ringing counts, and can be cut.
  auto live = [&](const Voice& v) { return v.active && !v.stolen && !v.offed && v.bank == &bank && v.channel == channel; };

  // Synth::checkOffGroups: every voice whose region has off_by == this region's group is cut, except a voice of the same group and the same note whose own gates still
  // hold (the other layers of the very note on). Voices fired by a note off (trigger=release / release_key) are never cut.
  for (auto& v : voices_) {
    if (!live(v) || v.release_trigger) continue;
    const RegionRt& vr = bank.regions[v.region];
    if (!vr.d.has_off_by || vr.d.off_by != d.group) continue;
    if (vr.d.group == d.group && !v.cc_voice && !from_cc && v.note == note && (!vr.cc_gate || cc_gate_open(vr, channel))) continue;
    off_voice(v);
  }

  // VoiceManager::checkNotePolyphony: the voices of the group started by this note. With note_selfmask on only those that are not louder are candidates, quietest
  // first (then oldest); off: oldest first. Each cut takes its sister voices along, and the count is reduced for every candidate visited, as sfizz.
  if (d.note_polyphony != 0) {
    unsigned counter = 0;
    for (auto& v : voices_) {
      v.mark = false;
      if (!live(v) || v.cc_voice != from_cc || v.note != note || bank.regions[v.region].d.group != d.group) continue;
      ++counter;
      v.mark = d.note_selfmask ? v.vel7 <= vel7 : true;
    }
    const unsigned target = d.note_polyphony - 1u;
    while (counter > target) {
      Voice* pick = nullptr;
      for (auto& v : voices_) {
        if (!v.mark) continue;
        if (pick == nullptr || (d.note_selfmask ? (v.vel7 < pick->vel7 || (v.vel7 == pick->vel7 && v.order < pick->order)) : v.order < pick->order)) pick = &v;
      }
      if (pick == nullptr) break;
      pick->mark = false;
      if (!pick->offed) off_sisters(pick->event);
      --counter;
    }
    for (auto& v : voices_) v.mark = false;
  }

  // checkRegionPolyphony and checkGroupPolyphony: at the limit, the oldest voice (and its sisters) goes.
  auto oldest_at_limit = [&](unsigned limit, auto&& counts) -> Voice* {
    unsigned n = 0;
    Voice* oldest = nullptr;
    for (auto& v : voices_) {
      if (!live(v) || !counts(v)) continue;
      ++n;
      if (oldest == nullptr || v.order < oldest->order) oldest = &v;
    }
    return n >= limit ? oldest : nullptr;
  };
  if (d.polyphony != 0)
    if (Voice* victim = oldest_at_limit(d.polyphony, [&](const Voice& v) { return v.region == ri; })) off_sisters(victim->event);
  if (d.group_polyphony != 0)
    if (Voice* victim = oldest_at_limit(d.group_polyphony, [&](const Voice& v) { return bank.regions[v.region].d.group == d.group; })) off_sisters(victim->event);
}

bool SampleEngine::playing_attack_voice(const RegionRt& r, std::uint32_t channel, bool match_velocity) const noexcept {
  // sfizz rule: a release region only fires while an attack voice of a compatible note is alive.
  for (const auto& v : voices_) {
    if (v.active && !v.stolen && !v.release_trigger && !v.cc_voice && v.channel == channel && v.note >= r.d.key_lo && v.note <= r.d.key_hi &&
        (!match_velocity || (v.vel7 >= r.d.vel_lo && v.vel7 <= r.d.vel_hi)))
      return true;
  }
  return false;
}

EngineStatus SampleEngine::start_voice(std::uint32_t channel, std::uint32_t note,
                                       std::uint32_t velocity16) noexcept {
  channel &= 15u;
  SampleBank* bank = bank_for(channel);
  if (note > 127) return EngineStatus::kOk;
  KeyState& ks = ks_[channel];
  const bool others_held = ks.others_held(static_cast<int>(note));  // Story 13.3: before this key goes down (first / legato)
  ks.set_held(static_cast<int>(note), true);  // the key state follows the keyboard even when the part has no (or an empty) instrument
  poly_aftertouch_[channel][note] = 0.0f;     // Story 3.7: a new strike starts with zero poly pressure until an aftertouch event arrives for it
  const std::uint32_t v16 = std::min<std::uint32_t>(velocity16, 65535u);
  // 7-bit velocity, as MIDI 1.0 and SFZ define it (the UMP value is rounded to the nearest step).
  std::uint8_t vel7 = static_cast<std::uint8_t>((v16 * 127u + 32767u) / 65535u);
  if (vel7 == 0) vel7 = 1;  // a note on is never velocity 0
  // Story 13.5, sfizz MidiState::noteOnEvent (every note on, bank or not): the key delta against the previous note on, the alternate (0, 1, 0, ...), the velocity.
  ks.keydelta = ks.prev >= 0 ? static_cast<float>(static_cast<int>(note) - static_cast<int>(ks.prev)) : 0.0f;
  ks.alt = ks.alt_next;
  ks.alt_next = ks.alt_next == 0.0f ? 1.0f : 0.0f;
  if (!bank || bank->regions.empty()) {
    ks.prev = static_cast<std::int16_t>(note);
    return EngineStatus::kOk;
  }
  last_vel7_[(channel & 15u) * 128u + note] = vel7;
  last_bank_[(channel & 15u) * 128u + note] = bank->id;
  ++event_seq_;  // Story 13.6: the voices of this note on are sisters
  const float rnd = next_random();
  note_pseudo(bank, channel, note, vel7, true);  // after rnd: a bank without a 135 / 136 entry draws nothing here

  // Keyswitch state first (sfizz order): the key goes down, and a keyswitch key becomes the part's last switch, before any region
  // is looked at, so a region mapped to the keyswitch key itself already follows it.
  if (bank->switch_key[note]) ks.last = static_cast<std::int16_t>(note);

  bool exhausted = false;
  for (std::uint32_t i = bank->note_begin[note]; i < bank->note_begin[note + 1]; ++i) {
    const std::uint32_t ri = bank->note_list[i];
    const RegionRt& r = bank->regions[ri];
    // Round robin: every region whose key range holds the note advances its own counter.
    bank->seq_switched[ri] = (bank->seq_counter[ri]++ % r.d.seq_length) == r.d.seq_position - 1 ? 1 : 0;
    using Trig = velora::ports::SampleTrigger;
    if (r.d.trigger == Trig::kFirst ? others_held : r.d.trigger == Trig::kLegato ? !others_held : r.d.trigger != Trig::kAttack) continue;
    if (r.has_keyswitch && !region_switched(r, ks)) continue;
    if (r.cc_gate && !cc_gate_open(r, channel)) continue;  // Story 13.5: locc / hicc at the controller values of this note on
    if (!bank->seq_switched[ri]) continue;
    if (vel7 < r.d.vel_lo || vel7 > r.d.vel_hi) continue;
    if (!(rnd >= r.d.lorand && rnd < r.d.hirand)) continue;
    if (!r.playable) {
      ++skipped_unplayable_;
      continue;
    }
    if (bank->uses_voice_groups) apply_voice_limits(*bank, ri, channel, note, vel7);  // Story 13.6
    if (!start_region(*bank, ri, channel, note, vel7, false)) exhausted = true;
  }
  ks.prev = static_cast<std::int16_t>(note);  // sw_previous: the note on before the next one
  return exhausted ? EngineStatus::kPoolExhausted : EngineStatus::kOk;
}

EngineStatus SampleEngine::stop_voice(std::uint32_t channel, std::uint32_t note) noexcept { return stop_voice_impl(channel, note, 0); }

EngineStatus SampleEngine::release_voice(std::uint32_t channel, std::uint32_t note, std::uint32_t release_velocity16) noexcept {
  return stop_voice_impl(channel, note, release_velocity16);
}

EngineStatus SampleEngine::stop_voice_impl(std::uint32_t channel, std::uint32_t note, std::uint32_t release_velocity16) noexcept {
  if (note > 127) return EngineStatus::kVoiceNotFound;
  channel &= 15u;
  // Only a physical note off clears a held key (key_released()): this call also comes from the sustain pedal's deferred release, when the key
  // may be down again.
  bool found = false;
  for (auto& v : voices_) {
    if (!v.active || v.stolen || v.release_trigger || v.cc_voice || v.channel != channel || v.note != note || v.key_up) continue;
    const RegionRt& r = v.bank->regions[v.region];
    if (v.env.s >= EnvState::kRelease) continue;  // already releasing
    found = true;
    if (r.d.loop_mode == velora::ports::SampleLoopMode::kOneShot) {
      v.key_up = true;  // one_shot ignores the note off
      continue;
    }
    if (!env_release(v.env, r)) {
      end_voice(v);
      continue;
    }
    if (part_engaged_[channel].test(1)) v.env.release_rate = part_release_rate_[channel];  // part_release replaces the region's release time
    if (const float scale = half_pedal_scale(cc_[channel][64]); scale < 1.0f) {
      v.env.release_rate = std::pow(v.env.release_rate, scale);  // Story 3.11: half-pedal damping (see half_pedal_scale)
    }
  }

  // Release-trigger regions: the note-on velocity, a fresh random draw.
  ++event_seq_;  // Story 13.6
  const float rnd = next_random();
  std::uint8_t vel7 = last_vel7_[(channel & 15u) * 128u + note];
  const bool by_release_velocity = cfg_.release_velocity_triggers && release_velocity16 != 0;
  if (by_release_velocity) {  // Story 3.6: the measured release velocity (same 16 -> 7 bit rounding as the note on)
    const std::uint32_t rv = std::min<std::uint32_t>(release_velocity16, 65535u);
    vel7 = static_cast<std::uint8_t>((rv * 127u + 32767u) / 65535u);
    if (vel7 == 0) vel7 = 1;
  }
  bool exhausted = false;
  SampleBank* bank = bank_for(channel);
  const std::uint64_t started_on = last_bank_[(channel & 15u) * 128u + note];
  if (started_on && (!bank || started_on != bank->id)) bank = nullptr;  // rebound while held: no release-trigger lookup on a bank the note never used
  bool pseudo_done = false;  // Story 13.5: the pseudo-CC randoms are drawn lazily, once a release region passes its gates, so a note off without one consumes none
  for (std::uint32_t i = 0; bank && i < bank->note_begin[note + 1] - bank->note_begin[note]; ++i) {
    const std::uint32_t ri = bank->note_list[bank->note_begin[note] + i];
    const RegionRt& r = bank->regions[ri];
    if (r.d.trigger != velora::ports::SampleTrigger::kRelease) continue;
    if (r.has_keyswitch && !region_switched(r, ks_[channel])) continue;
    if (r.cc_gate && !cc_gate_open(r, channel)) continue;
    if (!bank->seq_switched[ri]) continue;
    if (vel7 < r.d.vel_lo || vel7 > r.d.vel_hi) continue;
    if (!(rnd >= r.d.lorand && rnd < r.d.hirand)) continue;
    if (!playing_attack_voice(r, channel, !by_release_velocity)) continue;
    if (!pseudo_done) {
      note_pseudo(bank, channel, note, vel7, false);
      pseudo_done = true;
    }  // by release velocity: the layer is chosen by it, the attack voice only has to be alive
    if (!r.playable) {
      ++skipped_unplayable_;
      continue;
    }
    if (bank->uses_voice_groups) apply_voice_limits(*bank, ri, channel, note, vel7);  // Story 13.6
    if (start_region(*bank, ri, channel, note, vel7, true)) found = true;
    else exhausted = true;
  }
  if (exhausted) return EngineStatus::kPoolExhausted;
  return found ? EngineStatus::kOk : EngineStatus::kVoiceNotFound;
}

void SampleEngine::set_param(std::uint32_t param_id, float value) noexcept {
  if (!std::isfinite(value)) return;
  if (param_id == kParamGain) gain_ = std::clamp(value, 0.0f, kMaxGain);
  else if (param_id == kParamSeed && value >= 0.0f && value < 9.0e18f) set_seed(static_cast<std::uint64_t>(value));
}

bool SampleEngine::parallel_applicable(std::uint32_t frames) const noexcept {
  return pool_ != nullptr && frames != 0 && frames <= par_max_frames_;
}

void SampleEngine::render(float* left, float* right, std::uint32_t frames) noexcept {
  if (parallel_applicable(frames)) {
    float* l[1] = {left};
    float* r[1] = {right};
    render_parallel(l, r, frames, false);
  } else {
    for (auto& v : voices_)
      if (v.active) render_voice(v, left, right, frames);
  }
  clock_ += frames;
}

void SampleEngine::render_split(float* const* left, float* const* right, std::uint32_t frames) noexcept {
  if (parallel_applicable(frames)) {
    render_parallel(left, right, frames, true);
  } else {
    for (auto& v : voices_)
      if (v.active) render_voice(v, left[v.channel & (kSplitChannels - 1)], right[v.channel & (kSplitChannels - 1)], frames);
  }
  clock_ += frames;
}

void SampleEngine::set_parallel_voice_pool(WorkerPool* pool, std::uint32_t max_frames) {
  pool_ = nullptr;  // never render with a half-built scratch
  par_max_frames_ = 0;
  par_stride_ = 0;
  scratch_base_ = nullptr;
  scratch_.clear();
  scratch_.shrink_to_fit();
  if (pool == nullptr || max_frames == 0) return;
  // Every (partition, channel, bus) buffer starts on a cache line (buses of different partitions never share a line): the stride is rounded up to
  // 16 floats and the base is aligned inside a vector over-allocated by one line.
  par_stride_ = (max_frames + 15u) & ~15u;
  scratch_.assign(static_cast<std::size_t>(WorkerPool::kPartitions) * kSplitChannels * 2 * par_stride_ + 16u, 0.0f);
  const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(scratch_.data());
  scratch_base_ = scratch_.data() + (((64u - (addr & 63u)) & 63u) / sizeof(float));
  par_max_frames_ = max_frames;
  pool_ = pool;
}

void SampleEngine::partition_task(void* self, std::uint32_t partition) noexcept {
  static_cast<SampleEngine*>(self)->render_partition(partition);
}

// One partition's work: the voices of slots partition, partition + 3, ... rendered into this partition's own scratch buses.
//
// Shared-state audit (Story 1.18; render_partition runs on any pool thread, concurrently for the three partitions):
//  * Written: only the partition's own Voice objects (every Voice field, including `active = false` when a voice ends or its steal fade completes),
//    its own scratch buses, and its own partition_mask_[partition] element. Voices of different partitions are different slots (slot % 3).
//  * integrity_events_: std::atomic, fetch_add relaxed. The only engine-wide write.
//  * Read only during the phase (nothing writes them until the pool barrier has passed): cfg_, gain_, xf_frames_, tr_release_rate_off_,
//    pan_table_, cc_ (crossfade_target), clock_ (advanced after the barrier in render()/render_split()), par_frames_/par_split_ (published by the
//    pool's release/acquire), the banks' regions and PCM (const), and the damage flags (std::atomic, relaxed loads).
//  * Never touched here: the arbiter / VoiceAllocator (a freed voice only clears its `active` flag; the arbiter reads voice_info() in the serial phase,
//    after the barrier), the RNG (rng_, drawn only by start_voice/stop_voice), round-robin counters, keyswitch state (ks_), last_vel7_/last_bank_,
//    the skipped / undeclared counters. start_voice, stop_voice, begin_steal_fade, key_released and set_param all run in the serial phase.
//  * No end-of-voice notification exists, so nothing has to be deferred to the serial phase: voice end is observed through `active` there.
// No race was found. Adding any new engine-wide write to render_voice requires deferring it past the barrier.
void SampleEngine::render_partition(std::uint32_t partition) noexcept {
  const std::uint32_t frames = par_frames_;
  std::uint32_t mask = 0;
  for (std::uint32_t slot = partition; slot < voices_.size(); slot += WorkerPool::kPartitions) {
    const Voice& v = voices_[slot];
    if (v.active) mask |= std::uint32_t{1} << (par_split_ ? (v.channel & (kSplitChannels - 1)) : 0u);
  }
  partition_mask_[partition] = mask;
  for (std::uint32_t c = 0; c < kSplitChannels; ++c) {
    if (!(mask & (std::uint32_t{1} << c))) continue;
    std::fill_n(scratch(partition, 0, c), frames, 0.0f);
    std::fill_n(scratch(partition, 1, c), frames, 0.0f);
  }
  for (std::uint32_t slot = partition; slot < voices_.size(); slot += WorkerPool::kPartitions) {
    Voice& v = voices_[slot];
    if (!v.active) continue;
    const std::uint32_t c = par_split_ ? (v.channel & (kSplitChannels - 1)) : 0u;
    render_voice(v, scratch(partition, 0, c), scratch(partition, 1, c), frames);
  }
}

void SampleEngine::render_parallel(float* const* left, float* const* right, std::uint32_t frames, bool split) noexcept {
  bool any = false;
  for (const auto& v : voices_)
    if (v.active) {
      any = true;
      break;
    }
  if (!any) return;  // nothing to render: no need to wake the workers
  par_frames_ = frames;
  par_split_ = split;
  pool_->run(&SampleEngine::partition_task, this);  // fork, render the three partitions, barrier
  // Summation in the fixed order partition 0, 1, 2 (then ascending part), independent of the executing threads.
  for (std::uint32_t p = 0; p < WorkerPool::kPartitions; ++p) {
    for (std::uint32_t c = 0; c < kSplitChannels; ++c) {
      if (!(partition_mask_[p] & (std::uint32_t{1} << c))) continue;
      const float* sl = scratch(p, 0, c);
      const float* sr = scratch(p, 1, c);
      float* ol = split ? left[c] : left[0];
      float* orr = split ? right[c] : right[0];
      for (std::uint32_t f = 0; f < frames; ++f) {
        ol[f] += sl[f];
        orr[f] += sr[f];
      }
    }
  }
}

namespace {

// Story 1.17: where the frames of a streamed sample are: the resident head, the resident loop segments, or the ring buffer of the stream.
struct StreamView {
  const float* hl = nullptr;
  const float* hr = nullptr;
  std::int64_t head_n = 0;
  const velora::ports::SamplePcmSegment* segs = nullptr;
  std::uint32_t nsegs = 0;
  const float* rl = nullptr;  // the ring (nullptr: the voice no longer has one)
  const float* rr = nullptr;
  std::uint32_t cap = 1;
  std::uint64_t seg_start = 0, hi = 0;  // the ring holds frames [seg_start, hi) of the voice's segment (hi: a snapshot of the producer's filled_end)
  std::int64_t total = 0;
  bool stereo = false;
};

constexpr std::size_t kNoSlot = ~std::size_t{0};

// Frame `i` (left, right; mono repeats left in `r`). Outside [0, total) the sample is zero, as a fully resident render reads it. false = the frame is not available yet.
// `slot` is the ring slot of the PREVIOUS frame when the caller reads consecutive frames (kNoSlot otherwise): the slot of a ring frame is then that plus one with a wrap compare,
// instead of a 64-bit modulo per tap.
inline bool stream_frame(const StreamView& sv, std::int64_t i, float& l, float& r, std::size_t& slot) noexcept {
  if (i < 0 || i >= sv.total) {
    l = 0.0f;
    r = 0.0f;
    slot = kNoSlot;
    return true;
  }
  if (i < sv.head_n) {
    l = sv.hl[i];
    r = sv.stereo ? sv.hr[i] : l;
    slot = kNoSlot;
    return true;
  }
  for (std::uint32_t k = 0; k < sv.nsegs; ++k) {
    const auto& sg = sv.segs[k];
    if (sg.left != nullptr && i >= static_cast<std::int64_t>(sg.start) && i < static_cast<std::int64_t>(sg.start + sg.frames)) {
      const std::int64_t o = i - static_cast<std::int64_t>(sg.start);
      l = sg.left[o];
      r = sv.stereo && sg.right != nullptr ? sg.right[o] : l;
      slot = kNoSlot;
      return true;
    }
  }
  const auto u = static_cast<std::uint64_t>(i);
  if (sv.rl != nullptr && u >= sv.seg_start && u < sv.hi) {
    if (slot == kNoSlot) slot = static_cast<std::size_t>(u % sv.cap);
    else if (++slot == sv.cap) slot = 0;
    l = sv.rl[slot];
    r = sv.stereo ? sv.rr[slot] : l;
    return true;
  }
  slot = kNoSlot;
  return false;
}

}  // namespace

void SampleEngine::render_voice(Voice& v, float* left, float* right, std::uint32_t frames) noexcept {
  const RegionRt& r = v.bank->regions[v.region];
  const bool linear = cfg_.interpolation == SampleInterpolation::kLinear;
  const bool streamed = v.streamed;  // Story 1.17: the sample is `total_frames` long and the head is only its first part
  const std::int64_t nframes = static_cast<std::int64_t>(streamed ? r.total_frames : r.frames);
  // Story 13.3: `end` (exclusive) makes the played part [0, eff_n); without it eff_n == nframes and nothing below changes.
  const std::int64_t eff_n = (r.end_cut >= 0 && r.end_cut < nframes) ? r.end_cut : nframes;
  const std::int64_t end_idx = eff_n - 2;  // sfizz: positions at or past this index end the sample
  const bool head_cut = r.end_cut >= 0 && r.end_cut <= static_cast<std::int64_t>(r.frames);  // the sample ends inside the resident head: not an underrun
  const bool sustain_loop = r.d.loop_mode == velora::ports::SampleLoopMode::kLoopSustain;
  const float gl = v.gain_l * gain_, gr = v.gain_r * gain_;
  const bool has_xf = r.has_xfade, filtered = r.filtered, filter2 = r.filter2_on;
  const bool modded = r.modded, has_eq = r.eq_on, cutmod = r.cutmod;  // Story 13.9
  ModVoice& md = v.md;
  const float sr_f = static_cast<float>(cfg_.sample_rate);
  if (has_xf) {  // the controllers as they are for this render segment: a new target restarts the linear ramp (sfizz LinearSmoother)
    const float want = crossfade_target(r, v.channel);
    if (want != v.xf_tgt) {
      v.xf_tgt = want;
      v.xf_step = (want - v.xf_cur) / static_cast<float>(std::max<std::int32_t>(16, xf_frames_));
      if (v.xf_step == 0.0f || !std::isfinite(v.xf_step)) v.xf_cur = want;  // a step that rounds to nothing would never arrive: snap
    }
  }
  const float cl = r.chan_l * gain_, cr = r.chan_r * gain_;
  // Story 13.7: the pitch wheel as it is for this render segment. A new target restarts a linear ramp of max(16, smooth frames) frames (sfizz LinearSmoother); below
  // 2 smooth frames the voice jumps at once. sfizz reads frame i after stepping its position by the pitch of frame i, so the first smoother output belongs to the step INTO
  // the first frame of the segment. During the loop below the state is cur = out_f at the start of frame f (the step into f used it) and prev = out_{f-1}: the smoother
  // moves to out_{f+1} in iteration f and the step into f + 1 uses it. A new target found at the segment start moves the smoother from prev (the previous iteration already
  // moved cur by the old target's step), and the position, which was stepped with the old rate, is corrected by the difference. A region without bend opcodes skips all of
  // it, and its position step below is exactly v.ratio.
  const bool bends = r.bends;
  if (bends) {
    const float want = bend_cents(r, bend_[v.channel & 15u]);
    if (want != v.bend_tgt) {
      const float old_fac = v.bend_fac;
      v.bend_tgt = want;
      if (r.bend_smooth_frames < 2) {
        v.bend_cur = want;
        v.bend_step = 0.0f;
      } else {
        v.bend_step = (want - v.bend_prev) / static_cast<float>(std::max<std::int32_t>(16, r.bend_smooth_frames));
        if (v.bend_step == 0.0f || !std::isfinite(v.bend_step)) {  // a step that rounds to nothing would never arrive: snap
          v.bend_cur = want;
        } else {
          const float nxt = want > v.bend_prev ? std::min(want, v.bend_prev + v.bend_step) : std::max(want, v.bend_prev + v.bend_step);
          v.bend_cur = nxt == v.bend_prev ? want : nxt;
        }
      }
      v.bend_fac = std::pow(2.0f, v.bend_cur / 1200.0f);
      if (v.bend_live) v.pos += static_cast<double>(v.ratio) * (static_cast<double>(v.bend_fac) - static_cast<double>(old_fac));  // only a voice that has stepped had a step to correct
    }
  }
  float ratio_hi = bends ? v.ratio * r.bend_fac_max : v.ratio;  // the fastest the voice can read (stream window, integrity look-ahead)
  if (modded) ratio_hi *= r.mod_fac_max;                       // Story 13.9: and the pitch LFO / EG on top

  // Story 1.17: this block's view of the stream. The producer's progress is read ONCE (acquire) per block, and the lowest frame the voice can still read is published (release) so
  // the producer never overwrites it: positions only move forward within a segment, and the voice reads at most idx - 1 - ceil(ratio) (the oracle hold) below its start.
  StreamView sv;
  if (streamed) {
    sv.hl = r.left;
    sv.hr = r.right;
    sv.head_n = static_cast<std::int64_t>(r.frames);
    sv.segs = r.segs;
    sv.nsegs = r.seg_count;
    sv.total = nframes;
    sv.stereo = r.stereo;
    sv.seg_start = v.seg_start;
    if (v.ring != nullptr) {
      sv.rl = v.ring->left;
      sv.rr = v.ring->right;
      sv.cap = v.ring->capacity;
      const std::uint64_t a = v.ring->avail.load(std::memory_order_acquire);
      sv.hi = static_cast<std::uint32_t>(a >> 48) == v.seg ? (a & velora::ports::kStreamFrameMask) : 0;
      const std::int64_t i0 = static_cast<std::int64_t>(std::floor(v.pos));
      const std::int64_t lo = std::max<std::int64_t>(static_cast<std::int64_t>(v.seg_start), i0 - 2 - static_cast<std::int64_t>(std::ceil(ratio_hi)));
      v.lo_pub = static_cast<std::uint64_t>(lo);
      v.ring->consumed_lo.store(v.lo_pub, std::memory_order_release);
    }
  }
  // Reads the producer's progress again (the data may have arrived since the block began). True when it moved.
  const auto refresh_stream = [&]() noexcept {
    if (v.ring == nullptr) return false;
    const std::uint64_t a = v.ring->avail.load(std::memory_order_acquire);
    const std::uint64_t nh = static_cast<std::uint32_t>(a >> 48) == v.seg ? (a & velora::ports::kStreamFrameMask) : 0;
    const bool moved = nh != sv.hi;
    sv.hi = nh;
    return moved;
  };

  const std::uint64_t oracle_n = cfg_.oracle_block_frames;
  // Story 6.5: how far ahead of the playhead (in sample frames) the integrity fade looks, so the fade has finished when a damaged block is reached.
  const std::int64_t integ_look = r.damage != nullptr ? static_cast<std::int64_t>(std::ceil(static_cast<double>(ratio_hi) * kIntegrityFadeFrames)) + 1 : 0;
  for (std::uint32_t f = 0; f < frames; ++f) {
    float fade = 1.0f;
    if (v.stolen) {  // steal ramp: gain fade_left/fade_total, strictly falling; 0 frees the voice
      if (v.fade_left == 0) {
        end_voice(v);
        return;
      }
      fade = static_cast<float>(v.fade_left) / static_cast<float>(v.fade_total);
      --v.fade_left;
    }
    bool stall_now = false;
    if (oracle_n != 0 && !v.stalled) {
      const std::uint64_t abs = clock_ + f;
      if (abs % oracle_n == 0 && abs / oracle_n > v.start_time / oracle_n) {
        stall_now = true;
        v.pos = std::max(0.0, v.pos - static_cast<double>(v.ratio) * (bends ? static_cast<double>(v.bend_fac) : 1.0) * static_cast<double>(v.prev_pitch_mul));  // repeat the previous frame's position once (Story 13.7: a bent voice's last step)
        v.stalled = true;
      }
    }
    // Story 13.9: this frame's LFO / EG values. The EGs are released when the amplitude EG is (key up, pedal, choke, sample end), as sfizz releases all three together.
    float amp_mod = 1.0f, pitch_mul = 1.0f;
    if (modded) {
      const velora::ports::SampleModDesc& mdesc = r.d.mod;
      release_mod_egs(v, r);
      float cents = 0.0f, cut_cents = 0.0f;
      const float at_pressure = (r.vol_at || r.cut_at)
                                    ? std::clamp(aftertouch_[v.channel & 15u] + poly_aftertouch_[v.channel & 15u][v.note & 127u], 0.0f, 1.0f)
                                    : 0.0f;
      if (r.lfo_a || r.vol_at) {
        const float db = (r.lfo_a ? mdesc.amp.depth * lfo_next(md.lfo[0], mdesc.amp) : 0.0f) +
                         (r.vol_at ? r.d.volume_aftertouch_db * at_pressure : 0.0f);
        amp_mod = std::pow(10.0f, db * 0.05f);
      }
      if (r.lfo_p) cents += mdesc.pitch.depth * lfo_next(md.lfo[1], mdesc.pitch);
      const bool blk_end = oracle_n != 0 && (clock_ + f + 1) % oracle_n == 0;
      if (r.eg_p) cents += md.eg_depth[0] * env_next(md.eg[0], r, true, blk_end);
      if (r.lfo_f) cut_cents += mdesc.fil.depth * lfo_next(md.lfo[2], mdesc.fil);
      if (r.eg_f) cut_cents += md.eg_depth[1] * env_next(md.eg[1], r, true, blk_end);
      if (r.cut_at) cut_cents += r.d.cutoff_aftertouch_cents * at_pressure;
      if (cents != 0.0f) pitch_mul = std::pow(2.0f, cents / 1200.0f);
      if (md.age > 0 && !stall_now && pitch_mul != v.prev_pitch_mul)  // sfizz steps INTO frame f with frame f's pitch: the step added after frame f - 1 used its own factor, so correct it
        v.pos += static_cast<double>(v.ratio) * (bends ? static_cast<double>(v.bend_fac) : 1.0) * (static_cast<double>(pitch_mul) - static_cast<double>(v.prev_pitch_mul));
      if (cutmod) {  // sfizz FilterHolder::process + Filter::processModulated: the cutoff of the interval's first frame, coefficients smoothed per frame
        if ((md.age & 15u) == 0) {
          const float total = std::clamp(md.cut_cc_cents + cut_cents, -48000.0f, 48000.0f);
          const float cut = std::clamp(md.cut_base * std::pow(2.0f, total / 1200.0f), 0.0f, static_cast<float>(kSampleFilterMaxCutoffHz));
          md.ftgt = make_sample_filter(r.d.filter_type, static_cast<double>(cut), static_cast<double>(md.cut_res), static_cast<double>(sr_f));
          if (md.age == 0) md.fcur = md.ftgt;
        }
        sample_filter_smooth(md.fcur, md.ftgt, filt_smooth_step_);
      }
    }

    // Loop wrap: continuous loops always, sustain loops until the envelope is released.
    const bool looping = r.loops && !v.release_trigger && !v.ended && (!sustain_loop || v.env.s < EnvState::kRelease);
    std::int64_t idx = static_cast<std::int64_t>(std::floor(v.pos));
    if (looping) {
      if (idx > r.loop_end) {  // same as subtracting the loop size repeatedly, in bounded time
        v.pos = static_cast<double>(r.loop_start) +
                std::fmod(v.pos - static_cast<double>(r.loop_start), static_cast<double>(r.loop_size));
        idx = static_cast<std::int64_t>(std::floor(v.pos));
        if (streamed && v.ring != nullptr && r.loop_seek >= 0 && r.loop_seek < nframes) {
          // Story 1.17: the wrap sends the voice back to resident memory (the head and the loop segments); the stream re-seeks to the frame after that resident run. A ring
          // still at its segment start (nothing consumed, nothing skipped) already is where the seek would put it.
          const auto s0 = static_cast<std::uint64_t>(r.loop_seek);
          if (!(v.lo_pub <= v.seg_start && v.seg_start == s0)) {
            const std::uint32_t ns = velora::ports::stream_next_segment(v.seg);
            v.ring->consumed_lo.store(s0, std::memory_order_relaxed);
            v.ring->seek_start.store(s0, std::memory_order_relaxed);
            v.ring->seek_seq.store(ns, std::memory_order_release);  // publishes the two stores above to the producer
            v.seg = ns;
            v.seg_start = s0;
            v.lo_pub = s0;
            sv.seg_start = s0;
            sv.hi = 0;
          }
        }
      }
    } else if (idx >= end_idx && !v.ended) {
      // Sample exhausted: a 6 ms release starts on this very frame (sfizz turns the voice off at
      // the frame where the position reaches the end) while the last frame is held below.
      v.ended = true;
      env_fast_release(v.env);
      if (r.has_tail && !streamed && !head_cut) underrun_events_.fetch_add(1, std::memory_order_relaxed);  // Story 1.16: the head of a sample with a tail ran out (a streamed voice reads on)
    }
    if (looping && r.has_tail && !streamed && !head_cut && idx >= end_idx) {  // a loop that reaches beyond the resident head: same ending, same count (once ended, the voice no longer loops)
      v.ended = true;
      env_fast_release(v.env);
      underrun_events_.fetch_add(1, std::memory_order_relaxed);
    }

    const float env = env_next(v.env, r);
    if (modded) release_mod_egs(v, r);  // an amplitude release that began inside this frame (sample end, fast release) reaches the modulation EGs at once
    if (v.env.s == EnvState::kDone) {
      end_voice(v);
      return;
    }

    float sl, sr_;
    if (streamed) {
      // Story 1.17: the same arithmetic as the resident path below on frames that come from the head, the loop segments or the ring. The frames the interpolation window needs
      // must all be there; otherwise the voice is STARVED: it never waits, it holds its last output under a short fade (silence), counts one underrun per episode and goes on
      // (the position advances). When the data arrives it fades back in. With the data on time the fade gain is exactly 1 and nothing differs from the resident render.
      bool ok = true;
      if (idx >= end_idx && !looping) {  // past the end: hold the last frame (read once, then the ring is given back)
        if (v.hold_valid) {
          sl = v.hold_l;
          sr_ = v.hold_r;
        } else {
          float a = 0.0f, b = 0.0f;
          std::size_t slot = kNoSlot;
          ok = stream_frame(sv, eff_n - 1, a, b, slot) || (refresh_stream() && stream_frame(sv, eff_n - 1, a, b, slot));
          sl = a;
          sr_ = r.stereo ? b : a;
          if (ok) {
            v.hold_l = sl;
            v.hold_r = sr_;
            v.hold_valid = true;
            release_stream(v);
            sv.rl = nullptr;
          }
        }
      } else {
        const float c = static_cast<float>(v.pos - static_cast<double>(idx));
        float tl[4], tr[4];
        const auto gather = [&]() noexcept {
          if (idx >= 1 && idx + 2 < sv.head_n) {  // all four taps in the head: the common case at the start of a note
            for (int k = 0; k < 4; ++k) {
              tl[k] = sv.hl[idx - 1 + k];
              tr[k] = sv.stereo ? sv.hr[idx - 1 + k] : tl[k];
            }
            return true;
          }
          std::size_t slot = kNoSlot;
          for (int k = 0; k < 4; ++k)
            if (!stream_frame(sv, idx - 1 + k, tl[k], tr[k], slot)) return false;
          return true;
        };
        ok = gather() || (refresh_stream() && gather());
        if (ok) {
          if (linear) {
            const float a = 1.0f - c;
            sl = tl[1] * a + tl[2] * c;
            sr_ = r.stereo ? tr[1] * a + tr[2] * c : sl;
          } else {
            float w[4];
            hermite_weights(c, w);
            sl = w[0] * tl[0] + w[1] * tl[1] + w[2] * tl[2] + w[3] * tl[3];
            sr_ = r.stereo ? w[0] * tr[0] + w[1] * tr[1] + w[2] * tr[2] + w[3] * tr[3] : sl;
          }
        } else {
          sl = 0.0f;
          sr_ = 0.0f;
        }
      }
      constexpr float kSgStep = 1.0f / static_cast<float>(kStreamFadeFrames);
      if (ok) {
        if (v.starved) {
          // Re-entry: the output was the held sample under the fade; crossfade from it (not from the pre-fade value) to the real samples over kStreamFadeFrames, so a starve of any
          // length, even a few frames, ends without a step. (Only after a starve: with the data on time none of this runs and nothing is multiplied.)
          v.starved = false;
          v.rx_l = v.last_l * v.sgain;
          v.rx_r = v.last_r * v.sgain;
          v.rx_n = 1;
          v.sgain = 1.0f;
        }
        if (v.rx_n != 0) {
          const float t = static_cast<float>(v.rx_n) * kSgStep;
          sl = v.rx_l * (1.0f - t) + sl * t;
          sr_ = v.rx_r * (1.0f - t) + sr_ * t;
          v.rx_n = v.rx_n >= kStreamFadeFrames ? 0u : v.rx_n + 1u;
        }
        v.last_l = sl;
        v.last_r = sr_;
      } else {
        if (!v.starved) {  // one underrun per episode (a stream that flaps starves, recovers and starves again counts each episode)
          v.starved = true;
          v.rx_n = 0;
          underrun_events_.fetch_add(1, std::memory_order_relaxed);
        }
        v.sgain = std::max(0.0f, v.sgain - kSgStep);
        sl = v.last_l * v.sgain;
        sr_ = v.last_r * v.sgain;
      }
    } else if (idx >= end_idx && (!looping || r.has_tail)) {  // past the end (or, for a looping voice with a tail, past the resident head): hold the last frame
      sl = r.left[eff_n - 1];
      sr_ = r.stereo ? r.right[eff_n - 1] : sl;
    } else {
      const float c = static_cast<float>(v.pos - static_cast<double>(idx));
      if (linear) {
        const float a = 1.0f - c;
        const float l0 = (idx >= 0 && idx < nframes) ? r.left[idx] : 0.0f, l1 = (idx >= -1 && idx + 1 < nframes) ? r.left[idx + 1] : 0.0f;
        sl = l0 * a + l1 * c;
        if (r.stereo) {
          const float r0 = (idx >= 0 && idx < nframes) ? r.right[idx] : 0.0f, r1 = (idx >= -1 && idx + 1 < nframes) ? r.right[idx + 1] : 0.0f;
          sr_ = r0 * a + r1 * c;
        } else {
          sr_ = sl;
        }
      } else {
        float w[4];
        hermite_weights(c, w);
        if (idx >= 1 && idx + 2 < nframes) {
          const float* p = r.left + idx;
          sl = w[0] * p[-1] + w[1] * p[0] + w[2] * p[1] + w[3] * p[2];
          if (r.stereo) {
            const float* q = r.right + idx;
            sr_ = w[0] * q[-1] + w[1] * q[0] + w[2] * q[1] + w[3] * q[2];
          } else {
            sr_ = sl;
          }
        } else {
          auto tap = [&](const float* d, std::int64_t i) { return (i < 0 || i >= nframes) ? 0.0f : d[i]; };
          sl = w[0] * tap(r.left, idx - 1) + w[1] * tap(r.left, idx) + w[2] * tap(r.left, idx + 1) +
               w[3] * tap(r.left, idx + 2);
          if (r.stereo)
            sr_ = w[0] * tap(r.right, idx - 1) + w[1] * tap(r.right, idx) + w[2] * tap(r.right, idx + 1) +
                  w[3] * tap(r.right, idx + 2);
          else
            sr_ = sl;
        }
      }
    }

    if (r.damage != nullptr) {  // Story 6.5: a protected sample. Fade out before a damaged block is reached, back in after it; count each entry once.
      const auto block_of = [&r](std::int64_t i) noexcept { return r.damage_shift >= 0 ? (i >> r.damage_shift) : i / static_cast<std::int64_t>(r.damage_block_frames); };
      const auto damaged_block = [&r](std::int64_t b) noexcept {
        return b >= 0 && b < static_cast<std::int64_t>(r.damage_block_count) && r.damage[b].load(std::memory_order_relaxed) != 0;
      };
      // Frames from the read position to the first damaged block in the look-ahead window [idx, idx + integ_look] (0 = the playhead is inside one); integ_look + 1 =
      // none. Walked block by block, bounded by look / block size + 1 steps; the window wraps through the loop point of a looping voice.
      const auto first_damaged_start = [&](std::int64_t from, std::int64_t to, std::int64_t& start) noexcept {
        for (std::int64_t b = block_of(from) + 1; b <= block_of(to); ++b)
          if (damaged_block(b)) {
            start = r.damage_shift >= 0 ? (b << r.damage_shift) : b * static_cast<std::int64_t>(r.damage_block_frames);
            return true;
          }
        return false;
      };
      std::int64_t dist = integ_look + 1;
      if (idx >= 0 && damaged_block(block_of(idx))) {
        dist = 0;
      } else {
        const std::int64_t from = idx < 0 ? 0 : idx;
        const std::int64_t hi = idx + integ_look;
        std::int64_t start = 0;
        if (first_damaged_start(from, hi, start)) dist = std::max<std::int64_t>(0, start - idx);
        if (looping && hi > r.loop_end) {
          const std::int64_t wrapped = r.loop_start + (hi - r.loop_start) % r.loop_size;
          if (damaged_block(block_of(r.loop_start))) dist = std::min(dist, std::max<std::int64_t>(0, r.loop_end + 1 - idx));
          else if (first_damaged_start(r.loop_start, wrapped, start)) dist = std::min(dist, std::max<std::int64_t>(0, (r.loop_end + 1 - idx) + (start - r.loop_start)));
        }
      }
      const bool bad = dist <= integ_look;
      // The gain the distance allows: a voice that is already inside the window (it started there, or the flag was set late) may not play the damaged block at
      // full gain, so the gain is capped at dist / look -- 0 when the block is reached.
      const float limit = bad ? std::min(1.0f, static_cast<float>(dist) / static_cast<float>(std::max<std::int64_t>(integ_look, 1))) : 1.0f;
      if (!v.integ_seen) {  // first look: start at the allowed gain (a voice that starts inside a damaged block starts silent)
        v.integ_seen = true;
        v.integ_gain = limit;
      }
      if (bad != v.integ_bad) {
        v.integ_bad = bad;
        if (bad) integrity_events_.fetch_add(1, std::memory_order_relaxed);
      }
      constexpr float kStep = 1.0f / static_cast<float>(kIntegrityFadeFrames);
      v.integ_gain = bad ? std::min(limit, std::max(0.0f, v.integ_gain - kStep)) : std::min(1.0f, v.integ_gain + kStep);
      sl *= v.integ_gain;
      sr_ *= v.integ_gain;
    }

    if (!has_xf && !filtered && !modded && !has_eq) {  // the plain voice: exactly the Story 1.15 arithmetic (bit-exact bypass)
      if (v.stolen) {
        left[f] += sl * env * gl * fade;
        right[f] += sr_ * env * gr * fade;
      } else {
        left[f] += sl * env * gl;
        right[f] += sr_ * env * gr;
      }
    } else {
      // CC crossfade gain: a linear ramp to the target the controllers ask for, 5 ms long (sfizz's LinearSmoother).
      float xf = 1.0f;
      if (has_xf) {
        if (v.xf_tgt > v.xf_cur) v.xf_cur = std::min(v.xf_tgt, v.xf_cur + v.xf_step);
        else if (v.xf_tgt < v.xf_cur) v.xf_cur = std::max(v.xf_tgt, v.xf_cur + v.xf_step);
        xf = v.xf_cur;
      }
      float ol, orr;
      if (filtered || has_eq) {  // amplitude stage -> filter -> EQ bands -> pan (mono) / pan -> filter -> EQ (stereo; the same linear result)
        float a = env * xf * v.base_gain;
        if (modded) a *= amp_mod;  // Story 13.9: the amplitude LFO (dB)
        float yl, yr;
        if (filtered) {
          if (cutmod) {  // Story 13.9: a cutoff that an LFO / EG moves runs on the smoothed coefficients, with sfizz's per-term coefficient timing
            yl = static_cast<float>(sample_filter_tick_smooth(md.fcur, md.sml, static_cast<double>(sl * a)));
            yr = r.stereo ? static_cast<float>(sample_filter_tick_smooth(md.fcur, md.smr, static_cast<double>(sr_ * a))) : yl;
          } else {
            yl = static_cast<float>(sample_filter_tick(v.fk, v.fl, static_cast<double>(sl * a)));
            yr = r.stereo ? static_cast<float>(sample_filter_tick(v.fk, v.fr, static_cast<double>(sr_ * a))) : yl;
          }
          if (filter2) {  // Story 13.8: the second filter in series; a float between the stages, as sfizz's buffers
            yl = static_cast<float>(sample_filter_tick(v.fk2, v.fl2, static_cast<double>(yl)));
            yr = r.stereo ? static_cast<float>(sample_filter_tick(v.fk2, v.fr2, static_cast<double>(yr))) : yl;
          }
        } else {
          yl = sl * a;
          yr = r.stereo ? sr_ * a : yl;
        }
        if (has_eq) {  // Story 13.9: the EQ bands follow the filters (sfizz filterStage), a float between the stages
          for (unsigned b = 0; b < velora::ports::kSampleMaxEqBands; ++b) {
            EqBandState& q = md.eq[b];
            if (!q.on) continue;
            yl = static_cast<float>(sample_filter_tick(q.k, q.l, static_cast<double>(yl)));
            yr = r.stereo ? static_cast<float>(sample_filter_tick(q.k, q.r, static_cast<double>(yr))) : yl;
          }
        }
        ol = yl * cl;
        orr = yr * cr;
      } else {
        float a = env * xf;
        if (modded) a *= amp_mod;
        ol = sl * a * gl;
        orr = sr_ * a * gr;
      }
      if (v.stolen) {
        ol *= fade;
        orr *= fade;
      }
      left[f] += ol;
      right[f] += orr;
    }
    if (bends) {  // Story 13.7: advance the ramp one frame, then read at the bent rate
      v.bend_live = true;
      v.bend_prev = v.bend_cur;
      if (v.bend_tgt != v.bend_cur) {
        const float nxt = v.bend_tgt > v.bend_cur ? std::min(v.bend_tgt, v.bend_cur + v.bend_step) : std::max(v.bend_tgt, v.bend_cur + v.bend_step);
        v.bend_cur = nxt == v.bend_cur ? v.bend_tgt : nxt;  // a step too small to move the float would never arrive: snap
        v.bend_fac = std::pow(2.0f, v.bend_cur / 1200.0f);
      }
      v.pos += static_cast<double>(v.ratio) * static_cast<double>(v.bend_fac) * static_cast<double>(pitch_mul);
    } else {
      v.pos += static_cast<double>(v.ratio) * static_cast<double>(pitch_mul);  // Story 13.9: pitch_mul is exactly 1 for a region without a pitch LFO / EG
    }
    v.prev_pitch_mul = pitch_mul;
    if (modded) ++md.age;
  }
}

bool SampleEngine::voice_info(std::uint32_t slot, VoiceInfo& out) const noexcept {
  if (slot >= capacity_ || !voices_[slot].active) return false;
  const Voice& v = voices_[slot];
  out.start_time = v.start_time;
  out.level = v.env.v * std::max(v.gain_l, v.gain_r) * gain_;
  if (v.stolen || v.release_trigger || v.cc_voice || v.env.s >= EnvState::kRelease) out.state = VoiceState::kReleasing;
  else if (v.key_up) out.state = VoiceState::kSustained;
  else out.state = VoiceState::kHeld;
  out.channel = v.channel;
  out.note = v.note;
  return true;
}

bool SampleEngine::begin_steal_fade(std::uint32_t slot, std::uint32_t ramp_frames) noexcept {
  if (slot >= capacity_ || !voices_[slot].active || voices_[slot].stolen) return false;
  Voice& v = voices_[slot];
  v.stolen = true;
  v.fade_total = ramp_frames == 0 ? 1 : ramp_frames;
  v.fade_left = v.fade_total;
  return true;
}

}  // namespace velora::core
