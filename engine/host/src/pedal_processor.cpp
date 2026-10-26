#include "velora/host/pedal_processor.hpp"

#include <algorithm>

namespace velora::host {

PedalRange effective_range(const PedalCal& cal) noexcept {
  const bool usable = cal.max > cal.min && cal.max <= kPedalRawMax && static_cast<unsigned>(cal.max - cal.min) >= kPedalMinSpan;
  if (!usable) return PedalRange{0, kPedalRawMax};
  return PedalRange{cal.min, cal.max};
}

std::uint16_t pedal_deadband(const PedalRange& r) noexcept {
  const unsigned span = static_cast<unsigned>(r.hi - r.lo);
  return static_cast<std::uint16_t>(std::max<unsigned>(kPedalDeadbandMin, span / kPedalDeadbandDivisor));
}

std::uint32_t map_pedal(std::uint16_t raw, const PedalCal& cal) noexcept {
  const PedalRange r = effective_range(cal);
  const std::uint32_t span = static_cast<std::uint32_t>(r.hi - r.lo);
  const std::uint32_t db = pedal_deadband(r);
  const std::uint32_t c = std::clamp<std::uint32_t>(raw, r.lo, r.hi);
  const std::uint32_t u = cal.direction == PedalDirection::kInverted ? r.hi - c : c - r.lo;  // 0 = up, span = fully down
  if (u <= db) return 0;
  if (u >= span - db) return 0xFFFFFFFFu;
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(u - db) * 0xFFFFFFFFull / (span - 2 * db));
}

bool PedalChannel::update(std::uint16_t raw, const PedalCal& cal) noexcept {
  raw = std::min<std::uint16_t>(raw, kPedalRawMax);
  const std::uint32_t v = map_pedal(raw, cal);
  if (!have_) {
    have_ = true;
    raw_ = raw;
    value_ = v;
    return true;
  }
  const unsigned d = raw > raw_ ? raw - raw_ : raw_ - raw;
  if (d < kPedalHysteresis) {
    // A jitter of one count: ignored, unless it reaches or leaves an end state (so the pedal still reads exactly 0 / max when it gets there).
    const bool end_change = v != value_ && (v == 0 || v == 0xFFFFFFFFu || value_ == 0 || value_ == 0xFFFFFFFFu);
    if (!end_change) return false;
  }
  raw_ = raw;
  if (v == value_) return false;
  value_ = v;
  return true;
}

bool PedalChannel::remap(const PedalCal& cal) noexcept {
  if (!have_) return false;
  const std::uint32_t v = map_pedal(raw_, cal);
  if (v == value_) return false;
  value_ = v;
  return true;
}

namespace {
std::uint64_t pack(const PedalCal& c) {
  return static_cast<std::uint64_t>(c.min) | (static_cast<std::uint64_t>(c.max) << 16) | (static_cast<std::uint64_t>(c.direction) << 32);
}
}  // namespace

void PedalSettings::set(PedalId id, const PedalCal& cal) noexcept {
  const std::size_t i = static_cast<std::size_t>(id);
  if (i >= kPedalCount) return;
  const std::uint64_t p = pack(cal);
  if (packed_[i].exchange(p, std::memory_order_acq_rel) != p) version_.fetch_add(1, std::memory_order_acq_rel);
}

PedalCal PedalSettings::get(PedalId id) const noexcept {
  const std::size_t i = static_cast<std::size_t>(id);
  PedalCal c;
  if (i >= kPedalCount) return c;
  const std::uint64_t p = packed_[i].load(std::memory_order_acquire);
  c.min = static_cast<std::uint16_t>(p & 0xFFFFu);
  c.max = static_cast<std::uint16_t>((p >> 16) & 0xFFFFu);
  c.direction = ((p >> 32) & 1u) ? PedalDirection::kInverted : PedalDirection::kNormal;
  return c;
}

void PedalCalibrationProbe::begin(PedalId id) noexcept {
  const std::uint32_t gen = (state_.load(std::memory_order_relaxed) >> 8) + 1;  // only the control thread writes state_
  state_.store((gen << 8) | (static_cast<std::uint32_t>(id) + 1u), std::memory_order_release);
}

void PedalCalibrationProbe::cancel() noexcept { state_.fetch_and(~0xFFu, std::memory_order_acq_rel); }

PedalMeasurement PedalCalibrationProbe::read() const noexcept {
  PedalMeasurement m;
  const std::uint64_t r = result_.load(std::memory_order_acquire);
  const std::uint32_t gen = state_.load(std::memory_order_acquire) >> 8;
  if (r == 0 || static_cast<std::uint16_t>(r >> 48) != static_cast<std::uint16_t>(gen)) return m;  // nothing yet for this run
  m.valid = true;
  m.samples = static_cast<std::uint32_t>((r >> 24) & 0xFFFFFFu);
  m.min = static_cast<std::uint16_t>((r >> 12) & 0xFFFu);
  m.max = static_cast<std::uint16_t>(r & 0xFFFu);
  return m;
}

void PedalCalibrationProbe::observe(PedalId id, std::uint16_t raw) noexcept {
  const std::uint32_t st = state_.load(std::memory_order_acquire);  // one load: the pedal and the run it belongs to cannot disagree
  if ((st & 0xFFu) != static_cast<std::uint32_t>(id) + 1u) return;
  raw = std::min<std::uint16_t>(raw, kPedalRawMax);
  const std::uint32_t g = st >> 8;
  if (g != local_gen_) {
    local_gen_ = g;
    lsamples_ = 0;
    lmin_ = lmax_ = raw;
  }
  lmin_ = std::min(lmin_, raw);
  lmax_ = std::max(lmax_, raw);
  if (lsamples_ < 0xFFFFFFu) ++lsamples_;
  result_.store((static_cast<std::uint64_t>(static_cast<std::uint16_t>(g)) << 48) | (static_cast<std::uint64_t>(lsamples_) << 24) |
                    (static_cast<std::uint64_t>(lmin_) << 12) | lmax_,
                std::memory_order_release);
}

}  // namespace velora::host
