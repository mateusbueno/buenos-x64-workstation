// Mac virtual panel (Story 3.10, FR43 / AR41): a mapping file (JSON data) turns keyboard keys and a USB MIDI control surface's faders, knobs and
// buttons into the SAME MCU frames the panel MCU would send, which go to the same PanelReader. Epic 4 panel behaviour can be developed with no hardware.
//
// Mapping file (version 1), every member below is strict (an unknown member is an error naming its entry):
//   { "version": 1,
//     "keys": [ { "key": "x", "button": "EXIT" },                                   // press -> BUTTON pressed, release -> BUTTON released
//               { "key": "up", "control": "VALUE_ENCODER", "delta": 1 },            // encoder step on every press
//               { "key": "f", "control": "PEDAL_SWITCH", "press": 1, "release": 0 } // absolute value on press / on release (release optional)
//             ],
//     "midi": [ { "cc": 21, "control": "FADER_1" },                                  // absolute: 0..1 of the CC range -> out_min..out_max (control range default)
//               { "cc": 30, "control": "ENCODER_1", "mode": "relative", "step": 1 }, // 7-bit two's complement steps
//               { "cc": 50, "button": "PART_SCENE_1", "threshold": 64 },             // pressed while the value is >= threshold
//               { "note": 36, "button": "PART_SCENE_2" },                            // note on pressed, note off released
//               { "pitch_bend": true, "control": "WHEEL_PITCH" } ],
//     // optional on every midi entry: "channel" 0..15 (default: any), "out_min" / "out_max", "invert"
//   }
// Controls and buttons are named (MCU-ID-MAP.md names) or numbered. A malformed file, an unknown ID, a wrong kind of target, or a duplicate input is
// rejected as a whole with an error naming the entry: nothing of it is applied.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "velora/host/mcu_frame.hpp"
#include "velora/ports/midi_input.hpp"

namespace velora::host {

struct VirtualPanelMap {
  struct KeyEntry {
    std::string key;                  // normalised key name
    bool is_button = false;
    std::uint8_t id = 0;
    bool has_delta = false;
    std::int16_t delta = 0;
    std::uint16_t press_value = 0;
    std::optional<std::uint16_t> release_value;
  };
  enum class MidiSource { kCc, kNote, kPitchBend };
  struct MidiEntry {
    MidiSource source = MidiSource::kCc;
    int number = 0;                   // CC or note number
    int channel = -1;                 // -1 = any
    bool is_button = false;
    std::uint8_t id = 0;
    bool relative = false;
    int step = 1;
    int threshold = 64;
    std::int32_t out_min = 0, out_max = 0;
    bool invert = false;
  };
  std::vector<KeyEntry> keys;
  std::vector<MidiEntry> midi;

  // Parses and validates `json`. On failure returns false with `err` naming the entry and leaves `out` untouched.
  static bool parse(std::string_view json, VirtualPanelMap& out, std::string& err);
  static bool load_file(const std::string& path, VirtualPanelMap& out, std::string& err);

  const KeyEntry* find_key(std::string_view name) const;
  const MidiEntry* find_cc(int channel, int cc) const;
  const MidiEntry* find_note(int channel, int note) const;
  const MidiEntry* find_pitch_bend(int channel) const;
};

// Lower-cases a key name and resolves the aliases ("esc", "escape", "enter", "return", ...). Empty for an unusable name.
std::string normalize_key_name(std::string_view name);
// The key name of an SDL-style keycode (printable ASCII or the special keys of ui/src/virtual_panel.cpp); empty when unnamed.
std::string key_name_from_keycode(std::int32_t keycode);

struct VirtualPanelCounters {
  std::uint64_t frames = 0, unmapped = 0, key_events = 0, midi_events = 0;
};

// Turns mapped input into MCU frames (HELLO first, then CONTROL / BUTTON) and hands the wire bytes to `sink` with the host time they were built.
// Frame ticks come from the injected clock (tick = ns * tick_hz / 1e9), so tests are deterministic. Thread-safe by an internal lock; note that the
// sink (the PanelReader) must still see ONE thread at a time, and the reader's event queue is single-producer.
class VirtualPanel {
 public:
  using NowFn = std::function<std::uint64_t()>;
  using FrameSink = std::function<void(const std::uint8_t* data, std::size_t len, std::uint64_t rx_ns)>;
  VirtualPanel(VirtualPanelMap map, NowFn now_ns, FrameSink sink, double tick_hz = 1'000'000.0);

  // Replaces the mapping as a whole (nothing partial: the caller parsed it already). Held keys are forgotten.
  void set_map(VirtualPanelMap map);
  // Sends the HELLO a panel MCU sends at boot.
  void start();

  // Each returns true when the input was mapped (and produced a frame). `at_ns` 0 = take the injected clock.
  bool on_key(std::string_view key_name, bool pressed, std::uint64_t at_ns = 0);
  bool on_midi_cc(int channel, int cc, std::uint32_t value32, std::uint64_t at_ns = 0);
  bool on_midi_note(int channel, int note, bool on, std::uint64_t at_ns = 0);
  bool on_midi_pitch_bend(int channel, std::uint32_t value32, std::uint64_t at_ns = 0);

  VirtualPanelCounters counters() const;

 private:
  void send(const mcu::Message& m, std::uint64_t at_ns);
  std::uint64_t stamp(std::uint64_t at_ns) const;

  mutable std::mutex mu_;
  VirtualPanelMap map_;
  NowFn now_;
  FrameSink sink_;
  double tick_hz_;
  mcu::FrameWriter writer_;
  VirtualPanelCounters c_;
  std::vector<std::uint8_t> button_state_;  // per MIDI entry that maps a CC to a button: last pressed state
  std::vector<std::uint8_t> key_down_;      // per key entry: held
};

// The LED side of the Mac virtual panel (Story 3.12): the bytes the panel reader writes to its TX hook are what a panel MCU would receive. This is the
// simulated MCU's receiver: it decodes LED_SET frames into the LED state an on-screen LED view shows, one entry per LED id of the ID map (the other frames, the
// reader's TIME_SYNC requests, are ignored). Thread-safe.
struct VirtualLedCounters {
  std::uint64_t frames = 0, led_frames = 0, other_frames = 0;
};
class VirtualPanelLeds {
 public:
  using ChangeFn = std::function<void(std::uint8_t led_id, std::uint8_t brightness, std::uint8_t arg)>;
  // Feeds wire bytes from the panel reader's TX hook.
  void on_bytes(const std::uint8_t* data, std::size_t len);
  // The LED's last state (0 / 0 for an LED never set).
  mcu::LedSet led(std::uint8_t id) const;
  // Every LED that was set, ascending by id.
  std::vector<mcu::LedSet> snapshot() const;
  // Called (on the feeding thread, outside the lock) for every LED_SET frame. Set before feeding.
  void set_on_change(ChangeFn fn) { on_change_ = std::move(fn); }
  VirtualLedCounters counters() const;

 private:
  mutable std::mutex mu_;
  mcu::StreamDecoder decoder_;
  std::array<mcu::LedSet, 256> leds_{};
  std::array<bool, 256> set_{};
  VirtualLedCounters c_;
  ChangeFn on_change_;
};

// A UMP sink in front of the engine's MidiIngest: packets the map names (a CC, a note, a pitch bend) become panel frames and are consumed; every other
// packet goes on to `downstream` unchanged. Runs on the MIDI adapter thread, like the ingest it wraps.
class VirtualPanelUmpSink final : public ports::UmpSink {
 public:
  VirtualPanelUmpSink(VirtualPanel& panel, ports::UmpSink& downstream) noexcept : panel_(panel), downstream_(downstream) {}
  // Called with the host time at the start of every incoming UMP batch, on the thread that feeds the panel reader: engine-host points it at PanelReader::poll()
  // (the reader's state is single-threaded, so poll() must run here and not on the control tick). Set before MIDI runs.
  void set_tick(std::function<void(std::uint64_t host_ns)> tick) { tick_ = std::move(tick); }
  void on_ump(const std::uint32_t* words, std::size_t count, std::uint64_t host_ns) noexcept override {
    on_ump_port(ports::MidiPortKind::kUsb, words, count, host_ns);
  }
  void on_ump_port(ports::MidiPortKind port, const std::uint32_t* words, std::size_t count, std::uint64_t host_ns) noexcept override;

 private:
  VirtualPanel& panel_;
  ports::UmpSink& downstream_;
  std::function<void(std::uint64_t)> tick_;
};

}  // namespace velora::host
