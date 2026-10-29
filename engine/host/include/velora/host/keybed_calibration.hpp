// Keybed calibration tables (Story 3.5): data, not code. A table says how many contacts a key has (2 or 3), which contact
// pair measures the velocity, which contact's opening ends the note, and which note each key index plays.
// Switching a 2-contact table for a 3-contact one changes the reader's behaviour with no code change.
// Control / I-O thread code: it allocates (std::vector).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace velora::host {

// Time -> velocity mapping behind the reader's interface. The reader's default is the Story 3.6 chain (velocity_curve.hpp: this table's calibration range, then
// the selected curve, once); LinearVelocityMapper below is the provisional Story 3.5 policy, kept as a test helper.
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

// Travel time range of a keybed model's velocity (or release velocity) measurement: <= fast_ns is the fastest velocity, >= slow_ns the slowest.
// TODO(measure on prototype): the built-in values are placeholders, no real keybed has been measured (Story 3.6 defers the measurement).
struct VelocityRange {
  std::uint64_t fast_ns = 2'000'000;
  std::uint64_t slow_ns = 100'000'000;
};

// Plain description of a keybed (what a JSON file or a built-in provides).
struct KeybedCalibrationSpec {
  std::uint8_t contacts = 3;        // 2 or 3 contacts per key
  std::uint8_t vel_from = 0;        // 0-based contact whose closing starts the velocity measurement
  std::uint8_t vel_to = 2;          // 0-based contact whose closing completes it: Note On is emitted there (vel_from < vel_to < contacts)
  std::uint8_t release_contact = 2; // Note Off when this contact, or a lower one, opens while the key is sounding (vel_to <= release_contact < contacts)
  std::uint8_t first_note = 28;     // key 0 plays this note (E1, the lowest key of the 73-key keybed) ...
  std::uint8_t key_count = 73;      // ... key i plays first_note + i, for i < key_count
  // Story 3.6 calibration data. Attack velocity: travel time of the vel_from -> vel_to pair over this range (log time axis, see velocity_curve.hpp).
  std::uint64_t vel_fast_ns = 2'000'000;      // TODO(measure on prototype)
  std::uint64_t vel_slow_ns = 100'000'000;    // TODO(measure on prototype)
  // Release velocity. When release_pair is set, the release velocity is the time between the opening of release_contact (the first contact to open, which
  // also ends the note) and the opening of rel_to (the next lower contact, rel_to < release_contact), over rel_fast_ns .. rel_slow_ns, linear in the same
  // log axis, with no player curve. The Note Off is therefore emitted when the pair has completed, stamped with the release contact's own time, or after
  // rel_slow_ns at the slowest release velocity if rel_to never opens (a partial release). Without a pair (release_pair = false, the 2-contact rule: the
  // key has no contact below the one that ends the note) the Note Off carries KeybedReaderConfig::release_velocity16 and the reader counts it.
  bool release_pair = false;                  // default off so existing data-driven specs stay valid; three_contact_73() turns it on
  std::uint8_t rel_to = 1;                    // 0-based contact whose opening completes the release measurement
  std::uint64_t rel_fast_ns = 3'000'000;      // TODO(measure on prototype)
  std::uint64_t rel_slow_ns = 80'000'000;     // TODO(measure on prototype)
  bool has_aftertouch = false;                // Story 3.7: whether the keybed has channel or per-key aftertouch before HELLO overrides it
  std::vector<std::uint8_t> key_notes;  // optional explicit key index -> note map (overrides first_note/key_count when not empty)
};

class KeybedCalibration {
 public:
  // Validates the spec (contacts 2 or 3, vel_from < vel_to < contacts, vel_to <= release_contact < contacts (the note must not end
  // before it began), 0 < vel_fast_ns < vel_slow_ns, with a release pair rel_to < release_contact and 0 < rel_fast_ns < rel_slow_ns, every note 0..127, no note listed twice in key_notes, at least one key, at most 256). nullopt when invalid.
  static std::optional<KeybedCalibration> make(const KeybedCalibrationSpec& s);
  // Built-ins for the 73-key keybed. 3 contacts: velocity measured over the full travel (contact 1 -> 3), Note On at
  // contact 3, Note Off when contact 3 opens
  // (emitted once contact 2 has opened too: the release velocity is the 3 -> 2 opening time). 2 contacts: velocity contact 1 -> 2, Note On at contact 2, Note Off when contact 2 opens.
  static KeybedCalibration three_contact_73();
  static KeybedCalibration two_contact_73();

  std::uint8_t contacts() const noexcept { return spec_.contacts; }
  std::uint8_t vel_from() const noexcept { return spec_.vel_from; }
  std::uint8_t vel_to() const noexcept { return spec_.vel_to; }
  std::uint8_t release_contact() const noexcept { return spec_.release_contact; }
  bool release_pair() const noexcept { return spec_.release_pair; }
  bool has_aftertouch() const noexcept { return spec_.has_aftertouch; }
  std::uint8_t rel_to() const noexcept { return spec_.rel_to; }
  VelocityRange velocity_range() const noexcept { return {spec_.vel_fast_ns, spec_.vel_slow_ns}; }
  VelocityRange release_range() const noexcept { return {spec_.rel_fast_ns, spec_.rel_slow_ns}; }
  std::size_t key_count() const noexcept { return notes_.size(); }
  // Note of a key index, or -1 when the key is outside the table.
  int note_of(std::uint32_t key) const noexcept { return key < notes_.size() ? notes_[key] : -1; }

 private:
  KeybedCalibration() = default;
  KeybedCalibrationSpec spec_;
  std::vector<std::uint8_t> notes_;
};

}  // namespace velora::host
