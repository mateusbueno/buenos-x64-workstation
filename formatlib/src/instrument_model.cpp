#include "velora/format/instrument_model.hpp"

#include <cmath>

namespace velora::format {

bool has_extension(const Region& r) {
  return r.sw.any() || r.xf != Crossfade{} || r.filter != Filter{};
}

bool has_trigger_extension(const Region& r) {
  return static_cast<std::uint8_t>(r.trigger) > 1 || r.has_end;
}

namespace {
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
  if (r.offset >= frames) return Error::kInvalidArgument;
  if (r.has_loop_points) {
    if (r.loop_start > r.loop_end || r.loop_end >= frames) return Error::kInvalidArgument;
  } else if (r.loop_start != 0 || r.loop_end != 0) {
    return Error::kInvalidArgument;
  }
  return Error::kNone;
}

}  // namespace velora::format
