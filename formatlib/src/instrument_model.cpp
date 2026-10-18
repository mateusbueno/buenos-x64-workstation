#include "velora/format/instrument_model.hpp"

#include <cmath>

namespace velora::format {

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
  if (static_cast<std::uint8_t>(r.loop_mode) > 3 || static_cast<std::uint8_t>(r.trigger) > 1)
    return Error::kInvalidArgument;
  const AmpEnvelope& e = r.ampeg;
  for (float t : {e.delay, e.attack, e.hold, e.decay, e.release})
    if (!finite(t) || t < 0.f || t > 100.f) return Error::kInvalidArgument;
  for (float p : {e.start, e.sustain})
    if (!finite(p) || p < 0.f || p > 100.f) return Error::kInvalidArgument;
  if (r.offset >= frames) return Error::kInvalidArgument;
  if (r.has_loop_points) {
    if (r.loop_start > r.loop_end || r.loop_end >= frames) return Error::kInvalidArgument;
  } else if (r.loop_start != 0 || r.loop_end != 0) {
    return Error::kInvalidArgument;
  }
  return Error::kNone;
}

}  // namespace velora::format
