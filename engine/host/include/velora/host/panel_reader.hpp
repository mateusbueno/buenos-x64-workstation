// Panel reader core (Story 3.10, FR5 / FR43 / AR16 / AR10 / AR41): input-edge-thread logic with no hardware I/O.
//
//   panel MCU bytes -> StreamDecoder -> CONTROL / BUTTON frames -> device time (drift-tracked, the keybed reader's TimeSyncLink)
//     CONTROL pitch / mod wheel -> pitch bend / CC 1 UMP for EVERY keyboard-on part   -> HostEventQueue (SPSC) -> RT plane
//     CONTROL fader / encoder / pedal -> raw panel control event (+ the encoder step event for encoders 1..8) -> HostEventQueue
//     BUTTON -> PanelButtonEvent (id, pressed, device ns) -> the button sink (ws.v1 PanelEvent push); nothing goes to the RT plane
//
// The reader touches the RT plane only through the HostEventQueue (StampedUmp with host_ns, CLOCK_MONOTONIC_RAW ns). Every time it needs "now"
// the caller passes it in, so the whole reader is deterministic under test. Single thread (the panel input thread); allocates (frame decode),
// never runs on the RT thread. Binding faders / encoders / pedals to parameters is Epic 4, pedal semantics Story 3.11.
//
// LEDs (Story 3.12): the reader is the ONLY writer of the panel link. set_led() (any thread: the control thread, from a ws.v1 SetLed) validates the LED id
// against the ID map, keeps the full LED state and sends one LED_SET frame through the TX hook; every HELLO of the panel MCU replays the stored state, so a
// restarted MCU lights what the SoC owns. All writes to the link (time sync requests included) are serialised by one mutex. Never touches the RT plane.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "velora/core/snapshot.hpp"
#include "velora/core/ump.hpp"
#include "velora/host/drift_clock.hpp"
#include "velora/host/mcu_frame.hpp"
#include "velora/host/midi_ingest.hpp"

namespace velora::host {

// The parts (UMP channels 0..15) whose keyboard is on. The control thread updates it, the panel input thread reads it: one atomic word.
class KeyboardOnParts {
 public:
  void set_mask(std::uint16_t mask) noexcept { mask_.store(mask, std::memory_order_release); }
  std::uint16_t mask() const noexcept { return mask_.load(std::memory_order_acquire); }
  // Takes the parts of a published generation that sound with the keyboard (the PartRouter rule: ready, with an engine, keyboard on).
  void update_from(const core::Snapshot& s) noexcept {
    std::uint16_t m = 0;
    for (std::uint32_t p = 0; p < core::params::kPartCount && p < 16; ++p)
      if (s.parts[p].keyboard_on != 0 && s.parts[p].state == core::PartState::kReady && s.parts[p].engine_type != 0) m = static_cast<std::uint16_t>(m | (1u << p));
    set_mask(m);
  }

 private:
  std::atomic<std::uint16_t> mask_{0};
};

struct PanelButtonEvent {
  std::uint8_t id = 0;
  bool pressed = false;
  std::uint64_t device_ns = 0;
  friend bool operator==(const PanelButtonEvent&, const PanelButtonEvent&) = default;
};

struct PanelReaderConfig {
  std::uint32_t group = 0;                      // UMP group (the core serves group 0)
  double tick_hz = 1'000'000.0;                 // nominal MCU tick rate
  std::uint64_t sync_interval_ns = 1'000'000'000;
  std::uint64_t sync_fast_interval_ns = 100'000'000;
  std::size_t sync_fast_samples = 16;
  std::uint64_t sync_timeout_ns = 500'000'000;
};

struct PanelReaderCounters {
  std::uint64_t frames = 0, crc_errors = 0, framing_errors = 0, seq_gaps = 0, seq_lost = 0, version_rejects = 0, rejected_frames = 0;
  std::uint64_t ignored_frames = 0;                    // frames of other types (key contacts, aftertouch, ...)
  std::uint64_t control_frames = 0, button_frames = 0;
  std::uint64_t pitch_bends = 0, modulations = 0;      // wheel frames that reached at least one part (UMP count is per part)
  std::uint64_t wheel_no_parts = 0;                    // wheel frames while no part had its keyboard on: nothing emitted
  std::uint64_t raw_controls = 0, encoder_steps = 0;   // raw panel control events / encoder step events published
  std::uint64_t unknown_control = 0;                   // CONTROL frames naming an ID the map does not have: dropped
  std::uint64_t unknown_button = 0;                    // BUTTON frames naming an ID the map does not have: dropped
  std::uint64_t wheel_replays = 0;                     // wheel UMPs re-sent: shed earlier, a part joined the keyboard, or the panel MCU restarted
  std::uint64_t wheel_centres = 0;                     // centre / zero sent to parts that left the keyboard
  std::uint64_t buttons_delivered = 0, buttons_unsunk = 0;  // BUTTON frames given to the sink / with no sink set
  std::uint64_t sync_requests = 0, sync_replies = 0, missed_syncs = 0, sync_stray = 0, sync_rejected = 0, unsynced_events = 0;
  std::uint64_t clock_clamps = 0;                      // converted times raised to keep events in order
  std::uint64_t queue_full = 0, controllers_dropped = 0;
  std::uint64_t mcu_resets = 0;
  std::uint64_t leds_sent = 0, leds_replayed = 0;      // LED_SET frames written for set_led() / replayed after a panel HELLO
  std::uint64_t leds_unlinked = 0, leds_rejected = 0;  // LED states stored with no TX hook / set_led() calls with an unknown id
};

// One LED of the stored state (MCU-ID-MAP.md section 8): brightness 0..255, arg = colour index or ring fill.
struct LedValue {
  std::uint8_t brightness = 0;
  std::uint8_t arg = 0;
  friend bool operator==(const LedValue&, const LedValue&) = default;
};

class PanelReader {
 public:
  using TxFn = TimeSyncLink::TxFn;
  using ButtonSink = std::function<void(const PanelButtonEvent&)>;
  // `parts` is not owned and must outlive the reader. `cfg` is sanitised (tick_hz <= 0 or NaN becomes 1 MHz, group clamped to 0..15).
  PanelReader(HostEventQueue& out, const KeyboardOnParts& parts, PanelReaderConfig cfg = {});
  PanelReader(const PanelReader&) = delete;
  PanelReader& operator=(const PanelReader&) = delete;
  PanelReader(PanelReader&&) = delete;
  PanelReader& operator=(PanelReader&&) = delete;

  void set_tx(TxFn tx) {
    std::lock_guard<std::mutex> lk(link_mu_);
    have_tx_ = static_cast<bool>(tx);
    link_.set_tx(std::move(tx));
  }
  // Where BUTTON frames go (the ws.v1 PanelEvent push). Called on the input thread, so it must not block.
  void set_button_sink(ButtonSink sink) { sink_ = std::move(sink); }

  // Bytes from the MCU link, received at host time `rx_ns` (CLOCK_MONOTONIC_RAW ns).
  void on_bytes(const std::uint8_t* data, std::size_t len, std::uint64_t rx_ns);
  // Periodic housekeeping: sends TIME_SYNC requests, counts missed syncs.
  void poll(std::uint64_t now_ns);

  // Story 3.12: sets one panel LED. Thread-safe. Returns false (nothing stored, nothing sent) for an id the ID map has no LED for. The state is stored even with
  // no TX hook yet (replayed on HELLO). `now_ns` stamps the frame (0 = the last time the reader saw).
  bool set_led(std::uint8_t id, std::uint8_t brightness, std::uint8_t arg, std::uint64_t now_ns = 0);
  // The stored state of one LED (nullopt: the id is not an LED, or it was never set). Thread-safe.
  std::optional<LedValue> led(std::uint8_t id) const;
  static bool is_led_id(std::uint8_t id) noexcept;

  PanelReaderCounters counters() const;
  const DriftClock& clock() const noexcept { return link_.clock(); }
  const mcu::Hello* hello() const noexcept { return have_hello_ ? &hello_ : nullptr; }

  // The 12-bit wheel / pitch values as UMP values (exposed for tests and the virtual panel's documentation).
  static std::uint32_t pitch_bend_value32(std::uint16_t wheel12) noexcept;  // 2048 -> 0x80000000, 0 -> 0, 4095 -> 0xFFFFFFFF
  static std::uint32_t scale12_to_32(std::uint16_t v12) noexcept;           // 0..4095 -> 0..0xFFFFFFFF

 private:
  void handle(const mcu::Frame& f, std::uint64_t rx_ns);
  void on_control(const mcu::Control& c, std::uint64_t t);
  void on_button(const mcu::Button& b, std::uint64_t t);
  void on_hello(const mcu::Hello& h, std::uint32_t tick, std::uint64_t rx_ns);
  bool emit(const core::Ump& u, std::uint64_t t, bool is_controller = true);
  // Wheels: the current value is kept so that a UMP shed near a full queue, a part that joins the keyboard and a panel restart never leave a stale bend.
  void emit_wheel(bool pitch, std::uint16_t channels, std::uint32_t value32, std::uint64_t t);
  void sync_parts_and_flush();
  std::uint64_t order(std::uint64_t t);

  HostEventQueue& out_;
  const KeyboardOnParts& parts_;
  PanelReaderConfig cfg_;
  mcu::StreamDecoder decoder_;
  TimeSyncLink link_;
  ButtonSink sink_;
  PanelReaderCounters c_;
  std::uint64_t last_out_ns_ = 0;
  bool have_pitch_ = false, have_mod_ = false;
  std::uint32_t pitch32_ = 0x80000000u, mod32_ = 0;
  std::uint16_t pending_pitch_ = 0, pending_mod_ = 0;  // channels whose last wheel UMP was shed
  std::uint16_t last_mask_ = 0;                        // the keyboard-on parts at the last frame / poll
  mcu::Hello hello_{};
  bool have_hello_ = false;
  std::vector<mcu::Frame> frames_;  // scratch

  void send_led_locked(std::uint8_t id, const LedValue& v, std::uint64_t now_ns, bool replay);
  mutable std::mutex link_mu_;                      // serialises every write to the panel link and guards the LED state
  std::array<LedValue, 256> leds_{};
  std::array<bool, 256> led_set_{};
  std::uint64_t last_now_ns_ = 0;
  std::uint64_t leds_sent_ = 0, leds_replayed_ = 0, leds_unlinked_ = 0, leds_rejected_ = 0;
  bool have_tx_ = false;
};

}  // namespace velora::host
