#include "velora/host/keybed_calibration.hpp"

namespace velora::host {

std::optional<KeybedCalibration> KeybedCalibration::make(const KeybedCalibrationSpec& s) {
  if (s.contacts != 2 && s.contacts != 3) return std::nullopt;
  if (!(s.vel_from < s.vel_to) || s.vel_to >= s.contacts || s.release_contact >= s.contacts ||
      s.release_contact < s.vel_to)
    return std::nullopt;
  KeybedCalibration c;
  c.spec_ = s;
  if (!s.key_notes.empty()) {
    if (s.key_notes.size() > 256) return std::nullopt;
    c.notes_ = s.key_notes;
  } else {
    if (s.key_count == 0) return std::nullopt;
    for (unsigned i = 0; i < s.key_count; ++i) c.notes_.push_back(static_cast<std::uint8_t>(s.first_note + i));
    if (static_cast<unsigned>(s.first_note) + s.key_count - 1u > 127u) return std::nullopt;
  }
  bool seen[128] = {};
  for (std::uint8_t n : c.notes_) {
    if (n > 127 || seen[n]) return std::nullopt;  // out of range, or two keys on one note
    seen[n] = true;
  }
  c.spec_.key_notes.clear();
  return c;
}

KeybedCalibration KeybedCalibration::three_contact_73() {
  KeybedCalibrationSpec s;  // defaults describe the 3-contact 73-key keybed
  return *make(s);
}

KeybedCalibration KeybedCalibration::two_contact_73() {
  KeybedCalibrationSpec s;
  s.contacts = 2;
  s.vel_from = 0;
  s.vel_to = 1;
  s.release_contact = 1;
  return *make(s);
}

}  // namespace velora::host
