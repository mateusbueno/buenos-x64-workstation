// Keybed calibration tables (Story 3.5): data, not code. A table says how many contacts a key has (2 or 3), which contact
// pair measures the velocity, which contact's opening ends the note, and which note each key index plays.
// Switching a 2-contact table for a 3-contact one changes the reader's behaviour with no code change.
// Control / I-O thread code: it allocates (std::vector).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace velora::host {

// Provisional time -> velocity mapping (Story 3.6 replaces the policy with the real velocity curve, behind this interface).
class VelocityMapper {
 public:
  virtual ~VelocityMapper() = default;
  // `dt_ns`: time between the closing of the velocity pair's first contact and its second (the key travel time).
  virtual std::uint16_t velocity16(std::uint64_t dt_ns) const noexcept = 0;
};

// Linear in time: dt <= fast_ns -> 65535, dt >= slow_ns -> min_velocity, linear in between. No curve.
class LinearVelocityMapper final : public VelocityMapper {
 public:
  explicit LinearVelocityMapper(std::uint64_t fast_ns = 2'000'000, std::uint64_t slow_ns = 100'000'000,
                                std::uint16_t min_velocity = 512) noexcept
      : fast_ns_(fast_ns), slow_ns_(slow_ns > fast_ns ? slow_ns : fast_ns + 1), min_(min_velocity) {}
  std::uint16_t velocity16(std::uint64_t dt_ns) const noexcept override {
    if (dt_ns <= fast_ns_) return 0xFFFF;
    if (dt_ns >= slow_ns_) return min_;
    const double f = static_cast<double>(dt_ns - fast_ns_) / static_cast<double>(slow_ns_ - fast_ns_);
    const double v = 65535.0 - f * (65535.0 - static_cast<double>(min_));
    return static_cast<std::uint16_t>(v + 0.5);
  }
  std::uint64_t slow_ns() const noexcept { return slow_ns_; }

 private:
  std::uint64_t fast_ns_, slow_ns_;
  std::uint16_t min_;
};

// Plain description of a keybed (what a JSON file or a built-in provides).
struct KeybedCalibrationSpec {
  std::uint8_t contacts = 3;        // 2 or 3 contacts per key
  std::uint8_t vel_from = 0;        // 0-based contact whose closing starts the velocity measurement
  std::uint8_t vel_to = 2;          // 0-based contact whose closing completes it: Note On is emitted there (vel_from < vel_to < contacts)
  std::uint8_t release_contact = 2; // Note Off when this contact, or a lower one, opens while the key is sounding (vel_to <= release_contact < contacts)
  std::uint8_t first_note = 28;     // key 0 plays this note (E1, the lowest key of the 73-key keybed) ...
  std::uint8_t key_count = 73;      // ... key i plays first_note + i, for i < key_count
  std::vector<std::uint8_t> key_notes;  // optional explicit key index -> note map (overrides first_note/key_count when not empty)
};

class KeybedCalibration {
 public:
  // Validates the spec (contacts 2 or 3, vel_from < vel_to < contacts, vel_to <= release_contact < contacts (the note must not end
  // before it began), every note 0..127, no note listed twice in key_notes, at least one key, at most 256). nullopt when invalid.
  static std::optional<KeybedCalibration> make(const KeybedCalibrationSpec& s);
  // Built-ins for the 73-key keybed. 3 contacts: velocity measured over the full travel (contact 1 -> 3), Note On at
  // contact 3, Note Off when contact 3 opens. 2 contacts: velocity contact 1 -> 2, Note On at contact 2, Note Off when contact 2 opens.
  static KeybedCalibration three_contact_73();
  static KeybedCalibration two_contact_73();

  std::uint8_t contacts() const noexcept { return spec_.contacts; }
  std::uint8_t vel_from() const noexcept { return spec_.vel_from; }
  std::uint8_t vel_to() const noexcept { return spec_.vel_to; }
  std::uint8_t release_contact() const noexcept { return spec_.release_contact; }
  std::size_t key_count() const noexcept { return notes_.size(); }
  // Note of a key index, or -1 when the key is outside the table.
  int note_of(std::uint32_t key) const noexcept { return key < notes_.size() ? notes_[key] : -1; }

 private:
  KeybedCalibration() = default;
  KeybedCalibrationSpec spec_;
  std::vector<std::uint8_t> notes_;
};

}  // namespace velora::host
