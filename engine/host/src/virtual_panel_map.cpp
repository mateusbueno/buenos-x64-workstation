#include "velora/host/virtual_panel_map.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>

#include "velora/format/json_lite.hpp"
#include "velora/host/mcu_ids.hpp"
#include "velora/host/midi_ingest.hpp"

namespace velora::host {

namespace {

using format::JsonValue;

std::string lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

const mcu_ids::IdInfo* find_in(const mcu_ids::IdInfo* table, std::size_t n, std::uint8_t id) {
  for (std::size_t i = 0; i < n; ++i)
    if (table[i].id == id) return &table[i];
  return nullptr;
}
const mcu_ids::IdInfo* control_info(std::uint8_t id) { return find_in(mcu_ids::kControls, mcu_ids::kControlsCount, id); }
const mcu_ids::IdInfo* button_info(std::uint8_t id) { return find_in(mcu_ids::kButtons, mcu_ids::kButtonsCount, id); }

// A name or a number to an ID of one table; nullptr (and `why`) when it names none.
const mcu_ids::IdInfo* resolve_id(const JsonValue& v, const mcu_ids::IdInfo* table, std::size_t n, const char* what, std::string& why) {
  if (v.is_string()) {
    for (std::size_t i = 0; i < n; ++i)
      if (v.string == table[i].name) return &table[i];
    why = std::string("unknown ") + what + " \"" + v.string + "\"";
    return nullptr;
  }
  if (v.is_number() && v.number >= 0 && v.number <= 255 && v.number == std::floor(v.number)) {
    if (const auto* i = find_in(table, n, static_cast<std::uint8_t>(v.number))) return i;
    why = std::string("unknown ") + what + " id " + std::to_string(static_cast<int>(v.number));
    return nullptr;
  }
  why = std::string(what) + " must be a name or an id number";
  return nullptr;
}

bool as_int(const JsonValue& v, long lo, long hi, long& out) {
  if (!v.is_number() || v.number != std::floor(v.number) || v.number < static_cast<double>(lo) || v.number > static_cast<double>(hi)) return false;
  out = static_cast<long>(v.number);
  return true;
}

bool known_key(const std::string& k, std::initializer_list<const char*> allowed) {
  for (const char* a : allowed)
    if (k == a) return true;
  return false;
}

std::string entry_label(const char* array, std::size_t i, const std::string& detail) {
  return std::string(array) + "[" + std::to_string(i) + "]" + (detail.empty() ? "" : " (" + detail + ")");
}

bool parse_key_entry(const JsonValue& e, std::size_t idx, VirtualPanelMap::KeyEntry& out, std::string& err) {
  std::string label = entry_label("keys", idx, "");
  if (!e.is_object()) {
    err = label + ": must be an object";
    return false;
  }
  for (const std::string& k : e.keys)
    if (!known_key(k, {"key", "button", "control", "delta", "press", "release"})) {
      err = label + ": unknown member \"" + k + "\"";
      return false;
    }
  const JsonValue* key = e.find("key");
  if (!key || !key->is_string() || normalize_key_name(key->string).empty()) {
    err = label + ": \"key\" must name a key";
    return false;
  }
  out.key = normalize_key_name(key->string);
  label = entry_label("keys", idx, "key \"" + key->string + "\"");
  const JsonValue* button = e.find("button");
  const JsonValue* control = e.find("control");
  if ((button != nullptr) == (control != nullptr)) {
    err = label + ": exactly one of \"button\" and \"control\" is required";
    return false;
  }
  std::string why;
  if (button) {
    if (e.find("delta") || e.find("press") || e.find("release")) {
      err = label + ": a button takes no \"delta\", \"press\" or \"release\"";
      return false;
    }
    const auto* info = resolve_id(*button, mcu_ids::kButtons, mcu_ids::kButtonsCount, "button", why);
    if (!info) {
      err = label + ": " + why;
      return false;
    }
    out.is_button = true;
    out.id = info->id;
    return true;
  }
  const auto* info = resolve_id(*control, mcu_ids::kControls, mcu_ids::kControlsCount, "control", why);
  if (!info) {
    err = label + ": " + why;
    return false;
  }
  out.id = info->id;
  const std::string kind = info->kind;
  const JsonValue* delta = e.find("delta");
  const JsonValue* press = e.find("press");
  const JsonValue* release = e.find("release");
  if (kind == "encoder") {
    long d = 0;
    if (!delta || press || release || !as_int(*delta, -127, 127, d) || d == 0) {
      err = label + ": an encoder needs a non-zero \"delta\" within -127..127 (and no \"press\" / \"release\")";
      return false;
    }
    out.has_delta = true;
    out.delta = static_cast<std::int16_t>(d);
    return true;
  }
  long v = 0;
  if (delta || !press || !as_int(*press, info->min, info->max, v)) {
    err = label + ": " + info->name + " needs \"press\" within " + std::to_string(info->min) + ".." + std::to_string(info->max) + " (and no \"delta\")";
    return false;
  }
  out.press_value = static_cast<std::uint16_t>(v);
  if (release) {
    long r = 0;
    if (!as_int(*release, info->min, info->max, r)) {
      err = label + ": \"release\" must be within " + std::to_string(info->min) + ".." + std::to_string(info->max);
      return false;
    }
    out.release_value = static_cast<std::uint16_t>(r);
  }
  return true;
}

bool parse_midi_entry(const JsonValue& e, std::size_t idx, VirtualPanelMap::MidiEntry& out, std::string& err) {
  std::string label = entry_label("midi", idx, "");
  if (!e.is_object()) {
    err = label + ": must be an object";
    return false;
  }
  for (const std::string& k : e.keys)
    if (!known_key(k, {"cc", "note", "pitch_bend", "channel", "button", "control", "mode", "step", "threshold", "out_min", "out_max", "invert"})) {
      err = label + ": unknown member \"" + k + "\"";
      return false;
    }
  const JsonValue* cc = e.find("cc");
  const JsonValue* note = e.find("note");
  const JsonValue* pb = e.find("pitch_bend");
  if ((cc != nullptr) + (note != nullptr) + (pb != nullptr) != 1) {
    err = label + ": exactly one of \"cc\", \"note\" and \"pitch_bend\" is required";
    return false;
  }
  long n = 0;
  if (cc) {
    if (!as_int(*cc, 0, 127, n)) {
      err = label + ": \"cc\" must be 0..127";
      return false;
    }
    out.source = VirtualPanelMap::MidiSource::kCc;
    out.number = static_cast<int>(n);
    label = entry_label("midi", idx, "cc " + std::to_string(n));
  } else if (note) {
    if (!as_int(*note, 0, 127, n)) {
      err = label + ": \"note\" must be 0..127";
      return false;
    }
    out.source = VirtualPanelMap::MidiSource::kNote;
    out.number = static_cast<int>(n);
    label = entry_label("midi", idx, "note " + std::to_string(n));
  } else {
    if (pb->type != JsonValue::Type::kBool || !pb->boolean) {
      err = label + ": \"pitch_bend\" must be true";
      return false;
    }
    out.source = VirtualPanelMap::MidiSource::kPitchBend;
    label = entry_label("midi", idx, "pitch bend");
  }
  if (const JsonValue* ch = e.find("channel")) {
    long c = 0;
    if (!as_int(*ch, 0, 15, c)) {
      err = label + ": \"channel\" must be 0..15";
      return false;
    }
    out.channel = static_cast<int>(c);
  }
  const JsonValue* button = e.find("button");
  const JsonValue* control = e.find("control");
  if ((button != nullptr) == (control != nullptr)) {
    err = label + ": exactly one of \"button\" and \"control\" is required";
    return false;
  }
  std::string why;
  if (button) {
    if (out.source == VirtualPanelMap::MidiSource::kPitchBend) {
      err = label + ": a pitch bend maps to a control, not a button";
      return false;
    }
    for (const char* bad : {"mode", "step", "out_min", "out_max", "invert"})
      if (e.find(bad)) {
        err = label + ": a button takes no \"" + bad + "\"";
        return false;
      }
    const auto* info = resolve_id(*button, mcu_ids::kButtons, mcu_ids::kButtonsCount, "button", why);
    if (!info) {
      err = label + ": " + why;
      return false;
    }
    out.is_button = true;
    out.id = info->id;
    if (const JsonValue* th = e.find("threshold")) {
      long t = 0;
      if (out.source != VirtualPanelMap::MidiSource::kCc || !as_int(*th, 1, 127, t)) {
        err = label + ": \"threshold\" is for a cc button and must be 1..127";
        return false;
      }
      out.threshold = static_cast<int>(t);
    }
    return true;
  }
  if (out.source == VirtualPanelMap::MidiSource::kNote) {
    err = label + ": a note maps to a button";
    return false;
  }
  if (e.find("threshold")) {
    err = label + ": a control takes no \"threshold\"";
    return false;
  }
  const auto* info = resolve_id(*control, mcu_ids::kControls, mcu_ids::kControlsCount, "control", why);
  if (!info) {
    err = label + ": " + why;
    return false;
  }
  out.id = info->id;
  const bool encoder = std::string(info->kind) == "encoder";
  std::string mode = "absolute";
  if (const JsonValue* m = e.find("mode")) {
    if (!m->is_string() || (m->string != "absolute" && m->string != "relative")) {
      err = label + ": \"mode\" must be \"absolute\" or \"relative\"";
      return false;
    }
    mode = m->string;
  }
  out.relative = mode == "relative";
  if (encoder != out.relative) {
    err = label + ": " + info->name + (encoder ? " is an encoder and needs \"mode\": \"relative\"" : " is not an encoder and takes the absolute mode");
    return false;
  }
  if (out.relative) {
    if (out.source != VirtualPanelMap::MidiSource::kCc) {
      err = label + ": relative mode needs a cc";
      return false;
    }
    for (const char* bad : {"out_min", "out_max", "invert"})
      if (e.find(bad)) {
        err = label + ": relative mode takes no \"" + bad + "\"";
        return false;
      }
    long s = 1;
    if (const JsonValue* st = e.find("step"))
      if (!as_int(*st, 1, 127, s)) {
        err = label + ": \"step\" must be 1..127";
        return false;
      }
    out.step = static_cast<int>(s);
    return true;
  }
  if (e.find("step")) {
    err = label + ": absolute mode takes no \"step\"";
    return false;
  }
  out.out_min = info->min;
  out.out_max = info->max;
  for (const char* which : {"out_min", "out_max"}) {
    if (const JsonValue* v = e.find(which)) {
      long x = 0;
      if (!as_int(*v, info->min, info->max, x)) {
        err = label + ": \"" + which + "\" must be within " + std::to_string(info->min) + ".." + std::to_string(info->max);
        return false;
      }
      (std::string(which) == "out_min" ? out.out_min : out.out_max) = static_cast<std::int32_t>(x);
    }
  }
  if (const JsonValue* inv = e.find("invert")) {
    if (inv->type != JsonValue::Type::kBool) {
      err = label + ": \"invert\" must be true or false";
      return false;
    }
    out.invert = inv->boolean;
  }
  return true;
}

}  // namespace

std::string normalize_key_name(std::string_view name) {
  std::string n = lower(name);
  if (n == "esc") n = "escape";
  else if (n == "enter") n = "return";
  else if (n == "spacebar") n = "space";
  else if (n == "pgup") n = "pageup";
  else if (n == "pgdn" || n == "pagedn") n = "pagedown";
  static const char* const kNamed[] = {"escape", "tab", "return", "space", "up", "down", "left", "right", "pageup", "pagedown"};
  for (const char* k : kNamed)
    if (n == k) return n;
  if (n.size() == 1 && std::isprint(static_cast<unsigned char>(n[0])) && n[0] != ' ') return n;
  return {};
}

std::string key_name_from_keycode(std::int32_t keycode) {
  switch (keycode) {
    case 27: return "escape";
    case 9: return "tab";
    case 13: return "return";
    case 32: return "space";
    case 1073741906: return "up";
    case 1073741905: return "down";
    case 1073741904: return "left";
    case 1073741903: return "right";
    case 1073741899: return "pageup";
    case 1073741902: return "pagedown";
    default: break;
  }
  if (keycode > 32 && keycode < 127) return normalize_key_name(std::string(1, static_cast<char>(keycode)));
  return {};
}

bool VirtualPanelMap::parse(std::string_view json, VirtualPanelMap& out, std::string& err) {
  JsonValue root;
  std::string jerr;
  if (!format::parse_json(json, root, &jerr)) {
    err = "malformed JSON: " + jerr;
    return false;
  }
  if (!root.is_object()) {
    err = "the mapping must be a JSON object";
    return false;
  }
  for (const std::string& k : root.keys)
    if (!known_key(k, {"version", "keys", "midi", "comment"})) {
      err = "unknown member \"" + k + "\"";
      return false;
    }
  const JsonValue* ver = root.find("version");
  long v = 0;
  if (!ver || !as_int(*ver, 1, 1, v)) {
    err = "\"version\" must be 1";
    return false;
  }
  VirtualPanelMap m;
  if (const JsonValue* keys = root.find("keys")) {
    if (!keys->is_array()) {
      err = "\"keys\" must be an array";
      return false;
    }
    for (std::size_t i = 0; i < keys->array.size(); ++i) {
      KeyEntry e;
      if (!parse_key_entry(keys->array[i], i, e, err)) return false;
      for (std::size_t j = 0; j < m.keys.size(); ++j)
        if (m.keys[j].key == e.key) {
          err = entry_label("keys", i, "key \"" + e.key + "\"") + ": the key is already mapped by keys[" + std::to_string(j) + "]";
          return false;
        }
      m.keys.push_back(std::move(e));
    }
  }
  if (const JsonValue* midi = root.find("midi")) {
    if (!midi->is_array()) {
      err = "\"midi\" must be an array";
      return false;
    }
    for (std::size_t i = 0; i < midi->array.size(); ++i) {
      MidiEntry e;
      if (!parse_midi_entry(midi->array[i], i, e, err)) return false;
      for (std::size_t j = 0; j < m.midi.size(); ++j) {
        const MidiEntry& o = m.midi[j];
        const bool same_channel = o.channel == e.channel || o.channel < 0 || e.channel < 0;
        if (o.source == e.source && o.number == e.number && same_channel) {
          err = entry_label("midi", i, "") + ": the input is already mapped by midi[" + std::to_string(j) + "]";
          return false;
        }
      }
      m.midi.push_back(e);
    }
  }
  out = std::move(m);
  return true;
}

bool VirtualPanelMap::load_file(const std::string& path, VirtualPanelMap& out, std::string& err) {
  std::string text, rerr;
  if (!format::read_text_file(path, text, &rerr)) {
    err = path + ": " + rerr;
    return false;
  }
  if (!parse(text, out, err)) {
    err = path + ": " + err;
    return false;
  }
  return true;
}

const VirtualPanelMap::KeyEntry* VirtualPanelMap::find_key(std::string_view name) const {
  const std::string n = normalize_key_name(name);
  for (const KeyEntry& k : keys)
    if (k.key == n) return &k;
  return nullptr;
}

namespace {
const VirtualPanelMap::MidiEntry* find_midi(const std::vector<VirtualPanelMap::MidiEntry>& v, VirtualPanelMap::MidiSource s, int channel, int number) {
  for (const auto& e : v)
    if (e.source == s && e.number == number && (e.channel < 0 || e.channel == channel)) return &e;
  return nullptr;
}
}  // namespace

const VirtualPanelMap::MidiEntry* VirtualPanelMap::find_cc(int channel, int cc) const { return find_midi(midi, MidiSource::kCc, channel, cc); }
const VirtualPanelMap::MidiEntry* VirtualPanelMap::find_note(int channel, int note) const { return find_midi(midi, MidiSource::kNote, channel, note); }
const VirtualPanelMap::MidiEntry* VirtualPanelMap::find_pitch_bend(int channel) const { return find_midi(midi, MidiSource::kPitchBend, channel, 0); }

// ---- VirtualPanel -------------------------------------------------------------------------------------------------

VirtualPanel::VirtualPanel(VirtualPanelMap map, NowFn now_ns, FrameSink sink, double tick_hz)
    : map_(std::move(map)), now_(std::move(now_ns)), sink_(std::move(sink)), tick_hz_((tick_hz > 0.0 && std::isfinite(tick_hz)) ? tick_hz : 1'000'000.0) {
  button_state_.assign(map_.midi.size(), 0);
  key_down_.assign(map_.keys.size(), 0);
}

void VirtualPanel::set_map(VirtualPanelMap map) {
  std::lock_guard<std::mutex> lk(mu_);
  map_ = std::move(map);
  button_state_.assign(map_.midi.size(), 0);
  key_down_.assign(map_.keys.size(), 0);
}

std::uint64_t VirtualPanel::stamp(std::uint64_t at_ns) const { return at_ns ? at_ns : (now_ ? now_() : 0); }

void VirtualPanel::send(const mcu::Message& m, std::uint64_t at_ns) {
  const auto tick = static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<double>(at_ns) * tick_hz_ / 1e9));
  const std::vector<std::uint8_t> wire = writer_.write(m, tick);
  if (wire.empty()) return;  // a value the frame protocol refuses never leaves the panel
  ++c_.frames;
  if (sink_) sink_(wire.data(), wire.size(), at_ns);
}

VirtualPanelCounters VirtualPanel::counters() const {
  std::lock_guard<std::mutex> lk(mu_);
  return c_;
}

void VirtualPanel::start() {
  std::lock_guard<std::mutex> lk(mu_);
  send(mcu::Hello{0x0100, mcu::kProtocolVersion, 2, 0}, stamp(0));
}

bool VirtualPanel::on_key(std::string_view key_name, bool pressed, std::uint64_t at_ns) {
  std::lock_guard<std::mutex> lk(mu_);
  const std::string n = normalize_key_name(key_name);
  std::size_t idx = 0;
  for (; idx < map_.keys.size(); ++idx)
    if (map_.keys[idx].key == n) break;
  if (idx == map_.keys.size()) {
    ++c_.unmapped;
    return false;
  }
  const VirtualPanelMap::KeyEntry& e = map_.keys[idx];
  ++c_.key_events;
  const std::uint64_t t = stamp(at_ns);
  if (e.is_button) {
    if (pressed == (key_down_[idx] != 0)) return true;  // key auto-repeat of a held key: no new button edge
    key_down_[idx] = pressed;
    send(mcu::Button{e.id, pressed}, t);
  } else if (e.has_delta) {
    if (pressed) send(mcu::make_delta(e.id, e.delta), t);  // repeats of a held key are further steps
  } else {
    if (pressed == (key_down_[idx] != 0)) return true;
    key_down_[idx] = pressed;
    if (pressed) send(mcu::Control{e.id, e.press_value}, t);
    else if (e.release_value) send(mcu::Control{e.id, *e.release_value}, t);
  }
  return true;
}

namespace {
std::uint16_t scale_out(const VirtualPanelMap::MidiEntry& e, double frac) {
  if (e.invert) frac = 1.0 - frac;
  frac = std::clamp(frac, 0.0, 1.0);
  const double v = static_cast<double>(e.out_min) + frac * static_cast<double>(e.out_max - e.out_min);
  return static_cast<std::uint16_t>(std::clamp<long>(std::lround(v), 0, 65535));
}
}  // namespace

bool VirtualPanel::on_midi_cc(int channel, int cc, std::uint32_t value32, std::uint64_t at_ns) {
  std::lock_guard<std::mutex> lk(mu_);
  const VirtualPanelMap::MidiEntry* e = map_.find_cc(channel, cc);
  if (!e) {
    ++c_.unmapped;
    return false;
  }
  ++c_.midi_events;
  const std::uint64_t t = stamp(at_ns);
  const std::size_t idx = static_cast<std::size_t>(e - map_.midi.data());
  if (e->is_button) {
    const bool down = (value32 >> 25) >= static_cast<std::uint32_t>(e->threshold);  // the 7-bit value of the 32-bit CC
    if (down != (button_state_[idx] != 0)) {
      button_state_[idx] = down;
      send(mcu::Button{e->id, down}, t);
    }
  } else if (e->relative) {
    int v7 = static_cast<int>(value32 >> 25);
    if (v7 >= 64) v7 -= 128;  // 7-bit two's complement
    if (v7 != 0) send(mcu::make_delta(e->id, static_cast<std::int16_t>(v7 * e->step)), t);
  } else {
    send(mcu::Control{e->id, scale_out(*e, static_cast<double>(value32) / 4294967295.0)}, t);
  }
  return true;
}

bool VirtualPanel::on_midi_note(int channel, int note, bool on, std::uint64_t at_ns) {
  std::lock_guard<std::mutex> lk(mu_);
  const VirtualPanelMap::MidiEntry* e = map_.find_note(channel, note);
  if (!e) {
    ++c_.unmapped;
    return false;
  }
  ++c_.midi_events;
  const std::size_t idx = static_cast<std::size_t>(e - map_.midi.data());
  if (on != (button_state_[idx] != 0)) {
    button_state_[idx] = on;
    send(mcu::Button{e->id, on}, stamp(at_ns));
  }
  return true;
}

bool VirtualPanel::on_midi_pitch_bend(int channel, std::uint32_t value32, std::uint64_t at_ns) {
  std::lock_guard<std::mutex> lk(mu_);
  const VirtualPanelMap::MidiEntry* e = map_.find_pitch_bend(channel);
  if (!e) {
    ++c_.unmapped;
    return false;
  }
  ++c_.midi_events;
  send(mcu::Control{e->id, scale_out(*e, static_cast<double>(value32) / 4294967295.0)}, stamp(at_ns));
  return true;
}

// ---- VirtualPanelUmpSink ------------------------------------------------------------------------------------------

void VirtualPanelUmpSink::on_ump_port(ports::MidiPortKind port, const std::uint32_t* words, std::size_t count, std::uint64_t host_ns) noexcept {
  std::size_t i = 0;
  while (i < count) {
    const std::uint32_t mt = words[i] >> 28;
    std::size_t n = ump_packet_words(mt);
    if (n < 1) n = 1;  // always make progress
    if (i + n > count) n = count - i;  // a truncated packet goes on as it is
    const std::uint32_t w0 = words[i];
    const std::uint32_t status = (w0 >> 20) & 0xF;
    const int ch = static_cast<int>((w0 >> 16) & 0xF);
    bool taken = false;
    try {
      if (mt == 0x2 && n == 1) {  // MIDI 1.0 channel voice
        const int d1 = static_cast<int>((w0 >> 8) & 0x7F), d2 = static_cast<int>(w0 & 0x7F);
        if (status == 0xB) taken = panel_.on_midi_cc(ch, d1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(d2) * 0xFFFFFFFFull / 127u), host_ns);
        else if (status == 0x9) taken = panel_.on_midi_note(ch, d1, d2 != 0, host_ns);
        else if (status == 0x8) taken = panel_.on_midi_note(ch, d1, false, host_ns);
        else if (status == 0xE) {
          const std::uint32_t v14 = static_cast<std::uint32_t>((d2 << 7) | d1);  // 14 bit, bit-repeated up to 32
          taken = panel_.on_midi_pitch_bend(ch, (v14 << 18) | (v14 << 4) | (v14 >> 10), host_ns);
        }
      } else if (mt == 0x4 && n == 2) {  // MIDI 2.0 channel voice
        const std::uint32_t w1 = words[i + 1];
        const int d1 = static_cast<int>((w0 >> 8) & 0x7F);
        if (status == 0xB) taken = panel_.on_midi_cc(ch, d1, w1, host_ns);
        else if (status == 0x9) taken = panel_.on_midi_note(ch, d1, true, host_ns);
        else if (status == 0x8) taken = panel_.on_midi_note(ch, d1, false, host_ns);
        else if (status == 0xE) taken = panel_.on_midi_pitch_bend(ch, w1, host_ns);
      }
    } catch (...) {  // a sink callback threw: the packet is passed on rather than lost
      taken = false;
    }
    if (!taken) downstream_.on_ump_port(port, words + i, n, host_ns);
    i += n;
  }
}

void VirtualPanelLeds::on_bytes(const std::uint8_t* data, std::size_t len) {
  std::vector<mcu::Frame> frames;
  std::vector<mcu::LedSet> changed;
  {
    std::lock_guard<std::mutex> lk(mu_);
    decoder_.feed(data, len, &frames, nullptr);
    for (const mcu::Frame& f : frames) {
      ++c_.frames;
      const auto msg = mcu::parse_message(f);
      const auto* l = msg ? std::get_if<mcu::LedSet>(&*msg) : nullptr;
      if (!l) {
        ++c_.other_frames;
        continue;
      }
      ++c_.led_frames;
      leds_[l->id] = *l;
      set_[l->id] = true;
      changed.push_back(*l);
    }
  }
  if (on_change_)
    for (const mcu::LedSet& l : changed) on_change_(l.id, l.brightness, l.arg);
}

mcu::LedSet VirtualPanelLeds::led(std::uint8_t id) const {
  std::lock_guard<std::mutex> lk(mu_);
  return set_[id] ? leds_[id] : mcu::LedSet{id, 0, 0};
}

std::vector<mcu::LedSet> VirtualPanelLeds::snapshot() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<mcu::LedSet> out;
  for (std::size_t i = 0; i < set_.size(); ++i)
    if (set_[i]) out.push_back(leds_[i]);
  return out;
}

VirtualLedCounters VirtualPanelLeds::counters() const {
  std::lock_guard<std::mutex> lk(mu_);
  return c_;
}

}  // namespace velora::host
