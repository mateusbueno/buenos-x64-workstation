// Mac virtual panel (Story 3.10): the mapping file (strict JSON, errors name the entry, nothing partial), keys and MIDI input -> the same MCU frames
// the panel MCU sends -> the same PanelReader output as hand-built frames, and the UMP sink in front of the engine's ingest. Bare main, CHECK style.
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "velora/core/assign_events.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/panel_reader.hpp"
#include "velora/host/virtual_panel_map.hpp"

using namespace velora;
using namespace velora::host;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

namespace {

constexpr std::uint64_t kMs = 1'000'000ull;
namespace ids = mcu_ids::controls;

const char* kGoodMap = R"({
  "version": 1,
  "keys": [
    { "key": "x", "button": "EXIT" },
    { "key": "Up", "control": "VALUE_ENCODER", "delta": 1 },
    { "key": "down", "control": 9, "delta": -2 },
    { "key": "f", "control": "PEDAL_SWITCH", "press": 1, "release": 0 },
    { "key": "q", "control": "WHEEL_PITCH", "press": 4095, "release": 2048 }
  ],
  "midi": [
    { "cc": 21, "control": "FADER_1" },
    { "cc": 22, "control": "FADER_2", "out_min": 0, "out_max": 2047, "invert": true },
    { "cc": 30, "control": "ENCODER_1", "mode": "relative", "step": 2 },
    { "cc": 50, "button": "PART_SCENE_1", "threshold": 64 },
    { "note": 36, "channel": 9, "button": "PART_SCENE_2" },
    { "cc": 1, "control": "WHEEL_MOD" },
    { "pitch_bend": true, "control": "WHEEL_PITCH" }
  ]
})";

// A virtual panel wired to a reader, with a controllable clock.
struct Rig {
  HostEventQueue q;
  KeyboardOnParts parts;
  PanelReader reader{q, parts};
  std::uint64_t now = 100 * 1'000'000'000ull;
  std::vector<std::vector<std::uint8_t>> wires;
  std::vector<std::uint64_t> wire_ns;
  std::vector<PanelButtonEvent> buttons;
  std::unique_ptr<VirtualPanel> vp;
  explicit Rig(const char* json = kGoodMap) {
    parts.set_mask(0b11);
    reader.set_button_sink([this](const PanelButtonEvent& e) { buttons.push_back(e); });
    VirtualPanelMap m;
    std::string err;
    CHECK(VirtualPanelMap::parse(json, m, err));
    vp = std::make_unique<VirtualPanel>(
        std::move(m), [this] { return now; },
        [this](const std::uint8_t* d, std::size_t n, std::uint64_t ns) {
          wires.emplace_back(d, d + n);
          wire_ns.push_back(ns);
          reader.on_bytes(d, n, ns);
        });
  }
  std::vector<StampedUmp> drain() {
    std::vector<StampedUmp> v;
    StampedUmp s;
    while (q.pop(s)) v.push_back(s);
    return v;
  }
};

bool rejects(const char* json, const char* must_contain) {
  VirtualPanelMap m;
  m.keys.push_back({});  // untouched on failure
  std::string err;
  const bool ok = VirtualPanelMap::parse(json, m, err);
  const bool named = err.find(must_contain) != std::string::npos;
  if (ok || !named || m.keys.size() != 1) std::printf("  rejects(%s): ok=%d err=\"%s\"\n", must_contain, ok, err.c_str());
  return !ok && named && m.keys.size() == 1;
}

void test_map_parsing() {
  VirtualPanelMap m;
  std::string err;
  CHECK(VirtualPanelMap::parse(kGoodMap, m, err));
  CHECK(m.keys.size() == 5 && m.midi.size() == 7);
  CHECK(m.find_key("UP") && m.find_key("up")->id == ids::VALUE_ENCODER && m.find_key("up")->delta == 1);
  CHECK(m.find_key("DOWN") && m.find_key("down")->delta == -2);
  CHECK(m.find_key("x")->is_button && m.find_key("x")->id == mcu_ids::buttons::EXIT);
  CHECK(!m.find_key("z"));
  CHECK(m.find_cc(3, 21) && m.find_cc(0, 21)->id == ids::FADER_1 && !m.find_cc(0, 20));
  CHECK(m.find_note(9, 36) && !m.find_note(0, 36));
  CHECK(m.find_pitch_bend(4)->id == ids::WHEEL_PITCH);
  CHECK(normalize_key_name("ESC") == "escape" && normalize_key_name("Enter") == "return" && normalize_key_name("ab").empty() && normalize_key_name(" ").empty());
  CHECK(key_name_from_keycode(27) == "escape" && key_name_from_keycode('X') == "x" && key_name_from_keycode(1073741906) == "up" && key_name_from_keycode(5).empty());

  // the shipped default mapping parses
  VirtualPanelMap d;
  CHECK(VirtualPanelMap::load_file(VELORA_VIRTUAL_PANEL_MAP, d, err));
  CHECK(d.find_key("escape") && d.find_cc(0, 21) && d.find_pitch_bend(0));
  // every key of the existing ui mapping is in it
  for (const char* k : {"x", "m", "w", "v", "up", "down", "left", "right", "]", "[", "+", "=", "-", "pageup", "pagedown", ",", ".", "1", "2", "f", "tab", "return", "space", "escape"}) CHECK(d.find_key(k));

  // malformed or invalid: rejected whole, the error names the entry, nothing partial
  CHECK(rejects("{ not json", "malformed JSON"));
  CHECK(rejects("[]", "JSON object"));
  CHECK(rejects(R"({"version":2})", "version"));
  CHECK(rejects(R"({"version":1,"keys":[{"key":"x","button":"EXIT"},{"key":"y","button":"EXITT"}]})", "keys[1]"));
  CHECK(rejects(R"({"version":1,"keys":[{"key":"y","button":"EXITT"}]})", "unknown button \"EXITT\""));
  CHECK(rejects(R"({"version":1,"keys":[{"key":"y","control":"FADER_99","press":1}]})", "unknown control \"FADER_99\""));
  CHECK(rejects(R"({"version":1,"keys":[{"key":"y","button":99}]})", "unknown button id 99"));
  CHECK(rejects(R"({"version":1,"midi":[{"cc":1,"control":"WHEEL_MOD"},{"cc":2,"control":"NOPE"}]})", "midi[1]"));
  CHECK(rejects(R"({"version":1,"midi":[{"cc":1,"control":"WHEEL_MOD"},{"cc":1,"control":"FADER_1"}]})", "already mapped by midi[0]"));
  CHECK(rejects(R"({"version":1,"keys":[{"key":"x","button":"EXIT"},{"key":"X","button":"MENU"}]})", "already mapped by keys[0]"));
  CHECK(rejects(R"({"version":1,"keys":[{"key":"x","button":"EXIT","delta":1}]})", "keys[0]"));
  CHECK(rejects(R"({"version":1,"keys":[{"key":"x","button":"EXIT","bogus":1}]})", "unknown member \"bogus\""));
  CHECK(rejects(R"({"version":1,"keys":[{"key":"x","control":"FADER_1","press":4096}]})", "within 0..4095"));
  CHECK(rejects(R"({"version":1,"keys":[{"key":"x","control":"ENCODER_1","press":1}]})", "encoder"));
  CHECK(rejects(R"({"version":1,"midi":[{"cc":1,"control":"ENCODER_1"}]})", "relative"));
  CHECK(rejects(R"({"version":1,"midi":[{"cc":1,"control":"FADER_1","mode":"relative"}]})", "absolute"));
  CHECK(rejects(R"({"version":1,"midi":[{"cc":200,"control":"FADER_1"}]})", "0..127"));
  CHECK(rejects(R"({"version":1,"midi":[{"note":1,"control":"FADER_1"}]})", "button"));
  CHECK(rejects(R"({"version":1,"midi":[{"cc":3,"channel":16,"control":"FADER_1"}]})", "channel"));
  CHECK(rejects(R"({"version":1,"keys":[{"button":"EXIT"}]})", "keys[0]"));
  VirtualPanelMap bad;
  CHECK(!VirtualPanelMap::load_file("/nonexistent/virtual-panel.json", bad, err) && err.find("/nonexistent") != std::string::npos);
}

// The virtual panel's frames and the reader's output are the same as hand-built frames fed to a second reader.
void test_same_frames_same_output() {
  Rig r;
  r.vp->start();
  r.now += 10 * kMs;
  CHECK(r.vp->on_key("up", true) && r.vp->on_key("up", false));
  r.now += 10 * kMs;
  CHECK(r.vp->on_key("X", true));
  r.now += 50 * kMs;
  CHECK(r.vp->on_key("x", false));
  r.now += 5 * kMs;
  CHECK(r.vp->on_key("q", true));
  r.now += 5 * kMs;
  CHECK(r.vp->on_key("q", false));
  r.now += 5 * kMs;
  CHECK(r.vp->on_midi_cc(0, 21, 0xFFFFFFFFu));
  CHECK(r.vp->on_midi_cc(7, 22, 0xFFFFFFFFu));  // inverted, out 0..2047
  CHECK(r.vp->on_midi_cc(0, 30, 3u << 25));      // +3 steps * 2
  CHECK(r.vp->on_midi_cc(0, 30, 125u << 25));    // -3 steps * 2
  CHECK(r.vp->on_midi_cc(0, 50, 127u << 25) && r.vp->on_midi_cc(0, 50, 0));
  CHECK(r.vp->on_midi_note(9, 36, true) && r.vp->on_midi_note(9, 36, false));
  CHECK(!r.vp->on_midi_note(0, 36, true));       // wrong channel: not mapped
  CHECK(r.vp->on_midi_pitch_bend(2, 0x80000000u));
  CHECK(r.vp->on_midi_cc(0, 1, 0xFFFFFFFFu));
  CHECK(!r.vp->on_key("z", true) && !r.vp->on_midi_cc(0, 99, 5));
  const std::vector<StampedUmp> got = r.drain();

  // the reader the hardware would feed, with hand-built frames of the same ticks
  HostEventQueue q2;
  KeyboardOnParts p2;
  p2.set_mask(0b11);
  PanelReader ref(q2, p2);
  std::vector<PanelButtonEvent> ref_buttons;
  ref.set_button_sink([&](const PanelButtonEvent& e) { ref_buttons.push_back(e); });
  mcu::FrameWriter w;
  auto hand = [&](const mcu::Message& m, std::uint64_t ns) {
    const auto wire = w.write(m, static_cast<std::uint32_t>(ns / 1000));
    ref.on_bytes(wire.data(), wire.size(), ns);
  };
  std::vector<std::uint64_t> ns = r.wire_ns;  // the instants the virtual panel built its frames at (hello first)
  std::size_t i = 0;
  hand(mcu::Hello{0x0100, mcu::kProtocolVersion, 2, 0}, ns[i++]);
  hand(mcu::make_delta(ids::VALUE_ENCODER, 1), ns[i++]);
  hand(mcu::Button{mcu_ids::buttons::EXIT, true}, ns[i++]);
  hand(mcu::Button{mcu_ids::buttons::EXIT, false}, ns[i++]);
  hand(mcu::Control{ids::WHEEL_PITCH, 4095}, ns[i++]);
  hand(mcu::Control{ids::WHEEL_PITCH, 2048}, ns[i++]);
  hand(mcu::Control{ids::FADER_1, 4095}, ns[i++]);
  hand(mcu::Control{ids::FADER_2, 0}, ns[i++]);
  hand(mcu::make_delta(ids::ENCODER_1, 6), ns[i++]);
  hand(mcu::make_delta(ids::ENCODER_1, -6), ns[i++]);
  hand(mcu::Button{mcu_ids::buttons::PART_SCENE_1, true}, ns[i++]);
  hand(mcu::Button{mcu_ids::buttons::PART_SCENE_1, false}, ns[i++]);
  hand(mcu::Button{mcu_ids::buttons::PART_SCENE_2, true}, ns[i++]);
  hand(mcu::Button{mcu_ids::buttons::PART_SCENE_2, false}, ns[i++]);
  hand(mcu::Control{ids::WHEEL_PITCH, 2048}, ns[i++]);
  hand(mcu::Control{ids::WHEEL_MOD, 4095}, ns[i++]);
  CHECK(i == ns.size());
  std::vector<StampedUmp> want;
  StampedUmp s;
  while (q2.pop(s)) want.push_back(s);
  CHECK(!want.empty() && got.size() == want.size());
  for (std::size_t k = 0; k < got.size() && k < want.size(); ++k)
    CHECK(got[k].ump.word0 == want[k].ump.word0 && got[k].ump.word1 == want[k].ump.word1 && got[k].host_ns == want[k].host_ns);
  CHECK(r.buttons == ref_buttons && r.buttons.size() == 6);
  CHECK(r.reader.counters().crc_errors == 0 && r.reader.counters().seq_gaps == 0 && r.reader.counters().rejected_frames == 0);
  CHECK(r.vp->counters().unmapped == 3 && r.vp->counters().frames == ns.size());
  // the frames really are MCU frames: they decode with the plain decoder and carry the injected clock's ticks
  mcu::StreamDecoder dec;
  std::vector<mcu::Frame> frames;
  for (const auto& wi : r.wires) dec.feed(wi.data(), wi.size(), &frames);
  CHECK(frames.size() == r.wires.size() && dec.counters().crc_errors == 0);
  if (!frames.empty()) CHECK(frames[1].tick - frames[0].tick == 10000u);  // 10 ms at 1 MHz
}

void test_scaling_and_edges() {
  Rig r;
  auto last_raw = [&] {
    const auto v = r.drain();
    return v.empty() ? -1 : static_cast<int>(core::panel_control_value16(v.back().ump));
  };
  r.vp->on_midi_cc(0, 21, 0);
  CHECK(last_raw() == 0);
  r.vp->on_midi_cc(0, 21, 0xFFFFFFFFu);
  CHECK(last_raw() == 4095);
  r.vp->on_midi_cc(0, 21, 0x80000000u);
  CHECK(last_raw() == 2048);
  r.vp->on_midi_cc(0, 22, 0);  // inverted, out_max 2047
  CHECK(last_raw() == 2047);
  r.vp->on_midi_pitch_bend(0, 0x80000000u);
  r.vp->on_midi_pitch_bend(0, 0);
  const auto pb = r.drain();
  CHECK(pb.size() == 4);  // two bends, each a wheel frame to two keyboard parts
  if (pb.size() == 4) CHECK(pb[0].ump.pitch_bend_value32() == 0x80000000u && pb[2].ump.pitch_bend_value32() == 0);
  // a held key repeating does not repeat the button edge; a button CC below the threshold is a release
  r.buttons.clear();
  r.vp->on_key("x", true);
  r.vp->on_key("x", true);
  r.vp->on_key("x", false);
  r.vp->on_key("x", false);
  CHECK(r.buttons.size() == 2 && r.buttons[0].pressed && !r.buttons[1].pressed);
  r.buttons.clear();
  r.vp->on_midi_cc(0, 50, 63u << 25);
  r.vp->on_midi_cc(0, 50, 64u << 25);
  r.vp->on_midi_cc(0, 50, 100u << 25);
  r.vp->on_midi_cc(0, 50, 10u << 25);
  CHECK(r.buttons.size() == 2 && r.buttons[0].pressed && !r.buttons[1].pressed);
  // a zero relative step is nothing
  r.drain();
  r.vp->on_midi_cc(0, 30, 0);
  r.vp->on_midi_cc(0, 30, 64u << 25);  // 64 -> -64 steps * 2, clamped to -127
  const auto enc = r.drain();
  CHECK(enc.size() == 2 && static_cast<std::int16_t>(core::panel_control_value16(enc[0].ump)) == -127);
  // set_map replaces the mapping as a whole
  VirtualPanelMap m2;
  std::string err;
  CHECK(VirtualPanelMap::parse(R"({"version":1,"keys":[{"key":"z","button":"MENU"}]})", m2, err));
  r.vp->set_map(std::move(m2));
  CHECK(!r.vp->on_key("x", true) && r.vp->on_key("z", true) && !r.vp->on_midi_cc(0, 21, 5));
}

// The sink in front of the engine's ingest: mapped packets become panel frames, everything else reaches the ingest unchanged.
void test_ump_sink() {
  Rig r;
  HostEventQueue iq;
  MidiIngest ingest(iq);
  VirtualPanelUmpSink sink(*r.vp, ingest);
  auto m1 = [](std::uint32_t status, std::uint32_t ch, std::uint32_t d1, std::uint32_t d2) {  // MIDI 1.0 channel voice, one word
    return (0x2u << 28) | (status << 20) | (ch << 16) | (d1 << 8) | d2;
  };
  auto m2 = [](std::uint32_t status, std::uint32_t ch, std::uint32_t d1) { return (0x4u << 28) | (status << 20) | (ch << 16) | (d1 << 8); };
  auto send = [&](std::initializer_list<std::uint32_t> words, std::uint64_t ns) { sink.on_ump(words.begin(), words.size(), ns); };
  const std::uint64_t t = r.now;
  send({m1(0xB, 0, 21, 100)}, t);                                  // the fader
  send({m2(0xB, 0, 21), 0xFFFFFFFFu}, t + kMs);                    // the same fader as a MIDI 2.0 CC
  send({m1(0xB, 0, 22, 0), m1(0x9, 0, 75, 80)}, t + 2 * kMs);      // CC 22 (mapped), then a note on (not mapped) in ONE call
  send({m1(0x9, 0, 36, 127)}, t + 3 * kMs);                        // note 36 on channel 0: the map names channel 9 only -> the ingest
  send({m1(0x9, 9, 36, 127), m1(0x8, 9, 36, 0)}, t + 4 * kMs);     // channel 9 note 36 on and off: button pressed / released
  send({m1(0xE, 0, 0x00, 0x40)}, t + 5 * kMs);                     // pitch bend, centre
  send({m1(0xB, 0, 1, 127)}, t + 6 * kMs);                         // CC 1: the mod wheel
  send({m2(0x9, 0, 0x40), 0x80000000u}, t + 7 * kMs);              // a MIDI 2.0 note on -> the ingest
  const auto panel = r.drain();
  std::size_t notes = 0;
  StampedUmp u;
  while (iq.pop(u)) notes += u.ump.is_note_on();
  CHECK(notes == 3);  // note 75, note 36 on channel 0 and the MIDI 2.0 note: nothing else leaked through
  CHECK(r.buttons.size() == 2 && r.buttons[0].id == mcu_ids::buttons::PART_SCENE_2 && r.buttons[0].pressed && !r.buttons[1].pressed);
  std::size_t raw = 0, bends = 0, ccs = 0;  // 3 faders, one pitch bend and one mod wheel frame (2 keyboard parts each)
  for (const StampedUmp& s : panel) {
    raw += s.ump.is_assignable_controller();
    bends += s.ump.is_pitch_bend();
    ccs += s.ump.is_control_change();
  }
  CHECK(raw == 3 && bends == 2 && ccs == 2);
  CHECK(r.vp->counters().midi_events == 7);
  if (bends == 2) {
    for (const StampedUmp& s : panel)
      if (s.ump.is_pitch_bend()) CHECK(s.ump.pitch_bend_value32() > 0x7FF00000u && s.ump.pitch_bend_value32() < 0x80100000u);
  }
}

}  // namespace

int main() {
  test_map_parsing();
  test_same_frames_same_output();
  test_scaling_and_edges();
  test_ump_sink();
  if (g_fail) std::printf("virtual_panel_map_test: %d failure(s)\n", g_fail);
  else std::printf("virtual_panel_map_test: ok\n");
  return g_fail ? 1 : 0;
}
