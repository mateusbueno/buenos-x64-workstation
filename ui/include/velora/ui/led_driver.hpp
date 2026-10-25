#pragma once

// Story 3.12 (FR111, UX-DR50/81): the ui side of the panel LEDs. It keeps the full LED state (the steady "base" value of every LED it has been given), turns
// it into discrete LED commands (ws.v1 SetLed via the send hook; the engine writes the LED_SET frames, ui never touches the panel link) and generates the timed
// patterns itself, in time:
//   blink twice  2 x (250 ms on, 250 ms off), then the base value       (e.g. DRAWBARS pressed with no organ part)
//   recall dip   one 200 ms dip to off, then the base value             (a scene recalled from the part/scene buttons)
// No LED rises (off -> on) more often than 3 Hz (the guard covers plain state changes; pattern steps are exempt: they are <= 3 Hz by construction): a rising edge closer than kMinRiseGapMs to the previous one waits (the latest wanted value wins, so
// quick on/off flicker collapses). Falling edges are never delayed. Time is driven by tick(now_ms) so everything is deterministic under test.
// Full state resend (ui restart, engine reconnect): resend_all() sends every known LED's current output in id order; a send that fails (no connection) keeps
// the last delivered value and is retried from tick() every kRetryMs. Single thread (the UI thread). Never involves the RT plane: a LED command is only a control message.

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace velora::ui {

class LedDriver {
 public:
  static constexpr uint32_t kBlinkStepMs = 250;
  static constexpr uint32_t kDipMs = 200;
  static constexpr uint32_t kMinRiseGapMs = 334;  // ceil(1000 / 3): at most 3 on-edges per second (plain state changes; patterns are <= 3 Hz by construction)
  static constexpr uint32_t kRetryMs = 250;       // a failed send is retried from tick() after this long
  static constexpr uint8_t kOn = 255;

  // Returns false when the command could not be delivered (not connected).
  using SendFn = std::function<bool(uint8_t led_id, uint8_t brightness, uint8_t arg)>;

  struct Output {
    uint8_t brightness{0};
    uint8_t arg{0};
    friend bool operator==(const Output&, const Output&) = default;
  };

  void set_send(SendFn fn) { send_ = std::move(fn); }

  // The steady state of an LED. Sent at once unless it would be a rising edge inside the 3 Hz guard (then at the first tick that allows it).
  void set(uint8_t led_id, uint8_t brightness, uint8_t arg = 0);
  // Pattern overlays: they replace the output for their duration, then the base value is restored. Starting one while another runs restarts it.
  void blink_twice(uint8_t led_id, uint8_t brightness = kOn);
  void dip(uint8_t led_id);

  // Advances time (absolute ms, monotonic) and applies due pattern steps / deferred edges.
  void tick(uint64_t now_ms);
  uint64_t now_ms() const noexcept { return now_ms_; }

  // Sends every known LED's current output (ascending id), ignoring the 3 Hz guard (a replay of steady state, not a toggle).
  void resend_all();

  // What the driver currently outputs for an LED (what was last sent, or is due to be), and whether it has a base value.
  Output output(uint8_t led_id) const { return led_[led_id].out; }
  Output base(uint8_t led_id) const { return led_[led_id].base; }
  bool known(uint8_t led_id) const { return led_[led_id].known; }
  bool pattern_active(uint8_t led_id) const { return !led_[led_id].steps.empty(); }
  // Every LED with a base value, ascending id, with its current output (what an on-screen LED view shows).
  std::vector<std::pair<uint8_t, Output>> snapshot() const;

  uint64_t sent_count() const noexcept { return sent_; }
  uint64_t deferred_count() const noexcept { return deferred_; }

 private:
  struct Step {
    uint8_t brightness;
    uint32_t ms;
  };
  struct Led {
    bool known{false};
    bool dirty{false};      // the last send failed: retried from tick() every kRetryMs, or by resend_all()
    bool after_pattern{false};  // a pattern just ended: its restore step is exempt from the guard
    uint64_t next_retry_ms{0};
    bool ever_sent{false};
    bool rose{false};       // last_rise_ms is valid
    Output base{};
    Output out{};           // the output last DELIVERED
    std::vector<Step> steps;
    uint64_t pattern_start_ms{0};
    uint64_t last_rise_ms{0};
  };

  void start_pattern(uint8_t led_id, std::vector<Step> steps);
  Output wanted(const Led& l, uint64_t now, bool& from_pattern);
  void apply(uint8_t led_id, bool force);

  std::array<Led, 256> led_{};
  SendFn send_;
  uint64_t now_ms_{0};
  uint64_t sent_{0}, deferred_{0};
};

}  // namespace velora::ui
