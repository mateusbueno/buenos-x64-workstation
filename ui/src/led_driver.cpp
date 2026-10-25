#include "velora/ui/led_driver.hpp"

namespace velora::ui {

void LedDriver::set(uint8_t led_id, uint8_t brightness, uint8_t arg) {
  Led& l = led_[led_id];
  l.known = true;
  l.base = Output{brightness, arg};
  apply(led_id, false);
}

void LedDriver::start_pattern(uint8_t led_id, std::vector<Step> steps) {
  Led& l = led_[led_id];
  l.known = true;
  l.steps = std::move(steps);
  l.pattern_start_ms = now_ms_;
  apply(led_id, false);
}

void LedDriver::blink_twice(uint8_t led_id, uint8_t brightness) {
  start_pattern(led_id, {{brightness, kBlinkStepMs}, {0, kBlinkStepMs}, {brightness, kBlinkStepMs}, {0, kBlinkStepMs}});
}

void LedDriver::dip(uint8_t led_id) { start_pattern(led_id, {{0, kDipMs}}); }

// The output of an LED at `now`: the pattern step it is in, else the base value. A finished pattern is dropped by apply().
LedDriver::Output LedDriver::wanted(const Led& l, uint64_t now, bool& from_pattern) {
  from_pattern = false;
  if (!l.steps.empty()) {
    uint64_t t = l.pattern_start_ms;
    for (const Step& s : l.steps) {
      t += s.ms;
      if (now < t) {
        from_pattern = true;
        return Output{s.brightness, l.base.arg};
      }
    }
  }
  return l.base;
}

void LedDriver::apply(uint8_t led_id, bool force) {
  Led& l = led_[led_id];
  if (!l.known) return;
  if (!l.steps.empty()) {  // drop a finished pattern; its restore step is exempt from the guard
    uint64_t end = l.pattern_start_ms;
    for (const Step& s : l.steps) end += s.ms;
    if (now_ms_ >= end) {
      l.steps.clear();
      l.after_pattern = true;
    }
  }
  bool from_pattern = false;
  const Output want = wanted(l, now_ms_, from_pattern);
  const bool rising = want.brightness > 0 && (!l.ever_sent || l.out.brightness == 0);
  if (!force) {
    if (l.ever_sent && want == l.out) {
      l.dirty = false;
      l.after_pattern = false;
      return;  // nothing changed
    }
    if (l.dirty && now_ms_ < l.next_retry_ms) return;  // backing off after a failed send
    if (!from_pattern && !l.after_pattern && l.ever_sent && rising && l.rose && now_ms_ - l.last_rise_ms < kMinRiseGapMs) {
      ++deferred_;  // the 3 Hz guard (plain changes only): try again at a later tick, the latest wanted value wins
      return;
    }
  }
  const bool ok = send_ ? send_(led_id, want.brightness, want.arg) : false;
  if (ok) {
    l.out = want;
    l.ever_sent = true;
    l.dirty = false;
    l.after_pattern = false;
    ++sent_;
    if (rising && !force) {  // a forced replay of steady state is not a toggle
      l.rose = true;
      l.last_rise_ms = now_ms_;
    }
  } else {
    l.dirty = true;
    l.next_retry_ms = now_ms_ + kRetryMs;
  }
}

void LedDriver::tick(uint64_t now_ms) {
  if (now_ms > now_ms_) now_ms_ = now_ms;
  for (std::size_t i = 0; i < led_.size(); ++i)
    if (led_[i].known) apply(static_cast<uint8_t>(i), false);
}

void LedDriver::resend_all() {
  for (std::size_t i = 0; i < led_.size(); ++i) {
    Led& l = led_[i];
    if (!l.known) continue;
    apply(static_cast<uint8_t>(i), true);
  }
}

std::vector<std::pair<uint8_t, LedDriver::Output>> LedDriver::snapshot() const {
  std::vector<std::pair<uint8_t, Output>> v;
  for (std::size_t i = 0; i < led_.size(); ++i)
    if (led_[i].known) v.emplace_back(static_cast<uint8_t>(i), led_[i].out);
  return v;
}

}  // namespace velora::ui
