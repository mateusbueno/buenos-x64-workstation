#include "velora/format/instrument_model.hpp"

#include <cmath>

namespace velora::format {

bool has_extension(const Region& r) {
  return r.sw.any() || r.xf != Crossfade{} || r.filter != Filter{};
}

bool has_trigger_extension(const Region& r) {
  return static_cast<std::uint8_t>(r.trigger) > 1 || r.has_end;
}

bool has_amp_extension(const Region& r) { return r.amp != AmpMap{}; }

bool has_cc_extension(const Region& r) { return r.cc != CcMap{}; }

bool has_vg_extension(const Region& r) { return r.vg != VoiceGroup{}; }

bool has_bend_trigger_extension(const Region& r) { return r.bend != PitchBend{} || r.trig != CcTriggers{}; }

bool has_filter_extension(const Region& r) { return r.fmap != FilterMap{}; }


namespace {
// -0.0 == 0.0 as a number but not as bytes: one encoding per value.
bool neg_zero(float v) { return v == 0.f && std::signbit(v); }

Error validate_vg(const VoiceGroup& g) {
  constexpr std::int32_t kLim = 2147483647;
  if (g.group < -kLim || g.group > kLim || g.off_by < -kLim || g.off_by > kLim) return Error::kInvalidArgument;
  if (!g.has_off_by && g.off_by != 0) return Error::kInvalidArgument;
  if (static_cast<std::uint8_t>(g.off_mode) > kMaxOffMode) return Error::kInvalidArgument;
  if (!std::isfinite(g.off_time) || g.off_time < 0.f || g.off_time > 100.f || neg_zero(g.off_time)) return Error::kInvalidArgument;
  if (g.off_mode != OffMode::kTime && g.off_time != 0.f) return Error::kInvalidArgument;  // one encoding per behaviour
  if (g.group_polyphony != 0 && g.group == 0) return Error::kInvalidArgument;  // a group limit needs a group
  if (g.polyphony > kMaxVoiceLimit || g.group_polyphony > kMaxVoiceLimit || g.note_polyphony > kMaxVoiceLimit) return Error::kInvalidArgument;
  return Error::kNone;
}

Error validate_bend_trigger(const PitchBend& b, const CcTriggers& t) {
  if (!std::isfinite(b.up) || !std::isfinite(b.down) || !std::isfinite(b.step) || neg_zero(b.up) || neg_zero(b.down)) return Error::kInvalidArgument;
  if (b.up < -kMaxBendCents || b.up > kMaxBendCents || b.down < -kMaxBendCents || b.down > kMaxBendCents) return Error::kInvalidArgument;
  if (b.step < 1.f || b.step > kMaxBendStep) return Error::kInvalidArgument;
  if (!b.enabled && b != PitchBend{}) return Error::kInvalidArgument;  // one encoding per behaviour: no opcode, no values
  if (t.count > kMaxCcTriggers) return Error::kInvalidArgument;
  if (t.no_note && t.count == 0) return Error::kInvalidArgument;  // a region nothing can start
  for (unsigned i = 0; i < kMaxCcTriggers; ++i) {
    const CcTrigger& e = t.t[i];
    if (i >= t.count) {
      if (e != CcTrigger{}) return Error::kInvalidArgument;  // unused entries are the default (cc 0, 0..127)
      continue;
    }
    if (e.cc > 127 || !std::isfinite(e.lo) || !std::isfinite(e.hi) || e.lo < 0.f || e.hi > 127.f || e.lo > e.hi || neg_zero(e.lo) || neg_zero(e.hi)) return Error::kInvalidArgument;
    if (i > 0 && e.cc <= t.t[i - 1].cc) return Error::kInvalidArgument;  // strictly ascending, so unique
  }
  return Error::kNone;
}

Error validate_filter_tune(const FilterTune& t) {
  auto in = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi && !neg_zero(v); };
  if (!in(t.keytrack, 0.f, 1200.f) || !in(t.veltrack, -12000.f, 12000.f) || t.keycenter > 127) return Error::kInvalidArgument;
  if (t.keytrack == 0.f && t.keycenter != 60) return Error::kInvalidArgument;  // the centre means nothing without a key track: one encoding per behaviour
  if (t.cutoff_count > kMaxFilterMods || t.res_count > kMaxFilterMods) return Error::kInvalidArgument;
  for (int which = 0; which < 2; ++which) {
    const FilterMod* e = which == 0 ? t.cutoff : t.res;
    const unsigned n = which == 0 ? t.cutoff_count : t.res_count;
    const float lo = which == 0 ? -12000.f : 0.f, hi = which == 0 ? 12000.f : 96.f;
    for (unsigned i = 0; i < kMaxFilterMods; ++i) {
      if (i >= n) {
        if (e[i] != FilterMod{}) return Error::kInvalidArgument;  // unused entries are zero
        continue;
      }
      if (!is_cc_mod_source(e[i].cc) || !in(e[i].depth, lo, hi)) return Error::kInvalidArgument;
      if (i > 0 && e[i].cc <= e[i - 1].cc) return Error::kInvalidArgument;  // strictly ascending, so unique
    }
  }
  return Error::kNone;
}

Error validate_filter_map(const Region& r) {
  const FilterMap& m = r.fmap;
  if (validate_filter_tune(m.f1) != Error::kNone || validate_filter_tune(m.f2.tune) != Error::kNone) return Error::kInvalidArgument;
  if (m.f1 != FilterTune{} && r.filter.type == FilterType::kNone) return Error::kInvalidArgument;  // a tuning needs a filter
  const Filter2& f = m.f2;
  if (static_cast<std::uint8_t>(f.type) > 5) return Error::kInvalidArgument;
  if (f.type == FilterType::kNone) return f == Filter2{} ? Error::kNone : Error::kInvalidArgument;
  if (r.filter.type == FilterType::kNone) return Error::kInvalidArgument;  // the second filter follows the first
  if (!std::isfinite(f.cutoff_hz) || !std::isfinite(f.resonance_db) || neg_zero(f.cutoff_hz) || neg_zero(f.resonance_db)) return Error::kInvalidArgument;
  if (f.cutoff_hz < 0.f || f.cutoff_hz > 48000.f || f.resonance_db < 0.f || f.resonance_db > 40.f) return Error::kInvalidArgument;
  return Error::kNone;
}

Error validate_cc(const CcMap& m) {
  if (m.gate_count > kMaxCcGates || m.mod_count > kMaxCcMods) return Error::kInvalidArgument;
  for (unsigned i = 0; i < kMaxCcGates; ++i) {
    const CcGate& g = m.gates[i];
    if (i >= m.gate_count) {
      if (g != CcGate{}) return Error::kInvalidArgument;  // unused entries are the default (cc 0, 0..127)
      continue;
    }
    if (g.cc > 127 || !std::isfinite(g.lo) || !std::isfinite(g.hi) || g.lo < 0.f || g.hi > 127.f || g.hi < 0.f || g.lo > 127.f || g.lo > g.hi) return Error::kInvalidArgument;
    if (neg_zero(g.lo) || neg_zero(g.hi)) return Error::kInvalidArgument;
    if (i > 0 && g.cc <= m.gates[i - 1].cc) return Error::kInvalidArgument;  // strictly ascending, so unique
  }
  for (unsigned i = 0; i < kMaxCcMods; ++i) {
    const CcMod& e = m.mods[i];
    if (i >= m.mod_count) {
      if (e != CcMod{}) return Error::kInvalidArgument;  // unused entries are zero
      continue;
    }
    if (static_cast<std::uint8_t>(e.target) > kMaxCcTarget || !is_cc_mod_source(e.cc) || !std::isfinite(e.depth) || neg_zero(e.depth)) return Error::kInvalidArgument;
    float lo = -100.f, hi = 100.f;
    switch (e.target) {
      case CcTarget::kAmplitude: lo = 0.f; hi = 10000.f; break;
      case CcTarget::kVolume: lo = -144.f; hi = 48.f; break;
      case CcTarget::kPan: lo = -200.f; hi = 200.f; break;
      case CcTarget::kTune: lo = -12000.f; hi = 12000.f; break;
      case CcTarget::kOffset: lo = 0.f; hi = 2147483647.f; break;
      default: break;  // envelope: +-100 s or +-100 percent
    }
    if (e.depth < lo || e.depth > hi) return Error::kInvalidArgument;
    if (i > 0) {
      const CcMod& p = m.mods[i - 1];
      if (static_cast<std::uint8_t>(e.target) < static_cast<std::uint8_t>(p.target) ||
          (e.target == p.target && e.cc <= p.cc))
        return Error::kInvalidArgument;  // ascending by (target, cc), so unique
    }
  }
  return Error::kNone;
}
}  // namespace

Error validate_cc_setup(const InstrumentCcSetup& s) {
  for (std::size_t i = 0; i < s.defaults.size(); ++i) {
    const CcDefault& d = s.defaults[i];
    if (d.cc > 127 || !std::isfinite(d.value) || d.value < 0.f || d.value > 127.f || neg_zero(d.value)) return Error::kInvalidArgument;
    if (i > 0 && d.cc <= s.defaults[i - 1].cc) return Error::kInvalidArgument;
  }
  for (std::size_t i = 0; i < s.curves.size(); ++i) {
    const CurveDef& c = s.curves[i];
    if (c.points.size() > kMaxCurvePoints) return Error::kInvalidArgument;
    if (i > 0 && c.index <= s.curves[i - 1].index) return Error::kInvalidArgument;
    for (std::size_t k = 0; k < c.points.size(); ++k) {
      const CurvePoint& p = c.points[k];
      if (p.x > 127 || !std::isfinite(p.y) || neg_zero(p.y)) return Error::kInvalidArgument;
      if (k > 0 && p.x <= c.points[k - 1].x) return Error::kInvalidArgument;
    }
  }
  return Error::kNone;
}

namespace {
Error validate_amp(const AmpMap& a) {
  auto in = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
  for (float v : {a.veltrack, a.amplitude, a.keytrack, a.vel2attack, a.vel2decay, a.vel2sustain, a.vel2release})
    if (v == 0.f && std::signbit(v)) return Error::kInvalidArgument;  // -0.0 == 0.0 as a number but not as bytes: one encoding only
  for (unsigned i = 0; i < a.curve_count && i < kMaxVelPoints; ++i)
    if (a.curve[i].gain == 0.f && std::signbit(a.curve[i].gain)) return Error::kInvalidArgument;
  if (!in(a.veltrack, -100.f, 100.f) || !in(a.amplitude, 0.f, 10000.f) || !in(a.keytrack, -96.f, 12.f)) return Error::kInvalidArgument;
  if (a.keycenter > 127) return Error::kInvalidArgument;
  for (float t : {a.vel2attack, a.vel2decay, a.vel2release, a.vel2sustain})
    if (!in(t, -100.f, 100.f)) return Error::kInvalidArgument;
  if (a.curve_count > kMaxVelPoints) return Error::kInvalidArgument;
  for (unsigned i = 0; i < kMaxVelPoints; ++i) {
    if (i >= a.curve_count) {
      if (a.curve[i] != VelPoint{}) return Error::kInvalidArgument;  // unused entries are zero
      continue;
    }
    if (a.curve[i].vel > 127 || !in(a.curve[i].gain, 0.f, 1.f)) return Error::kInvalidArgument;
    if (i > 0 && a.curve[i].vel <= a.curve[i - 1].vel) return Error::kInvalidArgument;  // strictly ascending, so unique
  }
  return Error::kNone;
}

bool note_ok(std::int16_t v) { return v >= -1 && v <= 127; }

Error validate_extension(const Region& r) {
  const Keyswitch& k = r.sw;
  for (std::int16_t v : {k.lokey, k.hikey, k.last, k.down, k.up, k.previous, k.default_key})
    if (!note_ok(v)) return Error::kInvalidArgument;
  if (k.lokey >= 0 && k.hikey >= 0 && k.lokey > k.hikey) return Error::kInvalidArgument;

  const Crossfade& x = r.xf;
  if (static_cast<std::uint8_t>(x.curve) > 1) return Error::kInvalidArgument;
  if (x.in_count > kMaxCcCrossfades || x.out_count > kMaxCcCrossfades) return Error::kInvalidArgument;
  auto check = [](const CcRange* e, unsigned n) {
    for (unsigned i = 0; i < kMaxCcCrossfades; ++i) {
      if (i >= n) {
        if (e[i] != CcRange{}) return false;  // unused entries are zero
        continue;
      }
      if (e[i].cc > 127 || e[i].lo > e[i].hi || e[i].hi > 127) return false;
      if (i > 0 && e[i].cc <= e[i - 1].cc) return false;  // strictly ascending, so unique
    }
    return true;
  };
  if (!check(x.in, x.in_count) || !check(x.out, x.out_count)) return Error::kInvalidArgument;

  const Filter& f = r.filter;
  if (static_cast<std::uint8_t>(f.type) > 5) return Error::kInvalidArgument;
  if (!std::isfinite(f.cutoff_hz) || !std::isfinite(f.resonance_db)) return Error::kInvalidArgument;
  if (f.type == FilterType::kNone) {
    if (f.cutoff_hz != 0.f || f.resonance_db != 0.f) return Error::kInvalidArgument;
  } else if (f.cutoff_hz < 0.f || f.cutoff_hz > 48000.f || f.resonance_db < 0.f || f.resonance_db > 40.f) {
    return Error::kInvalidArgument;
  }
  return Error::kNone;
}
}  // namespace

Error validate_region(const Region& r, std::uint64_t frames) {
  auto finite = [](float f) { return std::isfinite(f); };
  if (r.key_lo > r.key_hi || r.key_hi > 127 || r.key_center > 127) return Error::kInvalidArgument;
  if (r.vel_lo > r.vel_hi || r.vel_hi > 127) return Error::kInvalidArgument;
  if (r.seq_length < 1 || r.seq_length > 255 || r.seq_position < 1 || r.seq_position > r.seq_length)
    return Error::kInvalidArgument;
  if (!finite(r.tune_cents) || !finite(r.volume_db) || !finite(r.pan) || !finite(r.lorand) ||
      !finite(r.hirand))
    return Error::kInvalidArgument;
  if (r.pan < -100.f || r.pan > 100.f) return Error::kInvalidArgument;
  // Same ranges the SFZ importer enforces, so a crafted manifest cannot carry more.
  if (r.transpose < -127 || r.transpose > 127) return Error::kInvalidArgument;
  if (r.tune_cents < -1200.f || r.tune_cents > 1200.f) return Error::kInvalidArgument;
  if (r.volume_db < -144.f || r.volume_db > 48.f) return Error::kInvalidArgument;
  if (r.lorand < 0.f || r.hirand > 1.f || r.lorand > r.hirand) return Error::kInvalidArgument;
  if (static_cast<std::uint8_t>(r.loop_mode) > 3 || static_cast<std::uint8_t>(r.trigger) > kMaxTriggerMode)
    return Error::kInvalidArgument;
  const AmpEnvelope& e = r.ampeg;
  for (float t : {e.delay, e.attack, e.hold, e.decay, e.release})
    if (!finite(t) || t < 0.f || t > 100.f) return Error::kInvalidArgument;
  for (float p : {e.start, e.sustain})
    if (!finite(p) || p < 0.f || p > 100.f) return Error::kInvalidArgument;
  if (!r.has_end && r.end != 0) return Error::kInvalidArgument;
  if (r.end > (std::uint64_t(1) << 40)) return Error::kInvalidArgument;
  if (validate_extension(r) != Error::kNone) return Error::kInvalidArgument;
  if (validate_amp(r.amp) != Error::kNone) return Error::kInvalidArgument;
  if (validate_cc(r.cc) != Error::kNone) return Error::kInvalidArgument;
  if (validate_vg(r.vg) != Error::kNone) return Error::kInvalidArgument;
  if (validate_bend_trigger(r.bend, r.trig) != Error::kNone) return Error::kInvalidArgument;
  if (validate_filter_map(r) != Error::kNone) return Error::kInvalidArgument;
  if (r.offset >= frames) return Error::kInvalidArgument;
  if (r.has_loop_points) {
    if (r.loop_start > r.loop_end || r.loop_end >= frames) return Error::kInvalidArgument;
  } else if (r.loop_start != 0 || r.loop_end != 0) {
    return Error::kInvalidArgument;
  }
  return Error::kNone;
}

}  // namespace velora::format
