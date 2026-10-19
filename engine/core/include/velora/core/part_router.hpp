#pragma once
// PartRouter (Story 4.3): the input-side routing of a Live Set. It turns the keyboard's note events into per-part note events, in the serial
// phase of the block loop, and remembers where every key went so that the note off reaches exactly the parts that got the note on.
//
// Keyboard channel convention. In a Live Set generation (Snapshot::live_set) every UMP note on / note off, on ANY channel 0..15, is a
// KEYBOARD event: the channel only says which keyboard source sent it, and it is NOT a part address. The router fans it out by zone to
// the parts, whose events then travel the existing channel == part path (the part's UMP channel is its index 0..7: engines, arbiter,
// mixer and the parameter events are addressed that way, unchanged). Outside a Live Set generation the router is idle and a note on
// channel c plays part / engine c exactly as before (all pre-4.3 renders are bit-identical). Parameter events keep addressing a part
// by their channel (channel == part); only note on / note off / sustain (CC 64) follow the keyboard path.
//
// Zones. A part sounds a key only when its keyboard flag is on, its state is kReady and low_key <= key <= high_key, the key being the one
// AS PLAYED. Octave and transpose move the pitch handed to the part (key + 12 * octave + transpose) and never the zone test, so
// overlapping zones layer and adjacent zones split. A pitch outside 0..127 drops that part's note (counted in dropped_out_of_range()).
//
// Note-off consistency. Per (source channel, key) the router records, for each part that received the note on, the pitch it sounded
// and the engine type it went to. The note off releases exactly those, with that pitch and engine, whatever the zones, keyboard flags,
// octave, transpose or even the Live Set have become since. A part with the keyboard off gets no new notes but still gets the note off
// of a key it received before. A note on for a key that is still tracked first releases the earlier note of any part whose pitch or
// engine would now differ, so nothing is orphaned. Keys (or keyboard sources) that end up on the same (part, pitch) are counted: the part's
// note off goes out when the last of them lets go, so one key's release never cuts a note another key still holds.
//
// Sustain (CC 64): pedal down reaches the parts with the keyboard on; pedal up reaches every part (so a part switched off while the
// pedal held its notes still lets them go). Pitch bend is not consumed by any engine and stays an ignored event. Since Story 5.2 the other
// controllers (mod wheel, expression, ...) are read by the sample engine: the block loop fans them out to the parts that sound with the
// keyboard on (BlockRenderer::apply_controller, not the router); one that no engine reads stays an ignored event.
//
// Real-time: configure(), note_on() and note_off() never allocate, lock or block.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "velora/core/params_gen.hpp"
#include "velora/core/rt.hpp"
#include "velora/core/snapshot.hpp"

namespace velora::core {

class PartRouter {
 public:
  static constexpr std::uint32_t kParts = params::kPartCount;
  static constexpr std::uint8_t kNoPitch = 0xFF;

  // One part's slice of a fan-out result: the note to hand to part `part` (its UMP channel).
  struct Target {
    std::uint8_t part = 0;
    std::uint8_t pitch = 0;
    std::uint32_t engine = 0;  // the full engine type id the note went to
    bool release_first = false;  // an earlier note of this part for the same key (different pitch / engine) must be released first
    std::uint8_t old_pitch = 0;
    std::uint32_t old_engine = 0;
  };
  struct Fanout {
    std::array<Target, kParts> t{};
    std::uint32_t count = 0;
  };

  // Adopts the part model of a Live Set generation (copies the plain fields; keeps no pointer into the snapshot). Called by the block
  // loop when a generation is adopted. active() becomes `s.live_set`.
  void configure(const Snapshot& s) noexcept VELORA_NONBLOCKING {
    active_ = s.live_set;
    for (std::uint32_t p = 0; p < kParts; ++p) {
      const PartCfg& c = s.parts[p];
      Part& r = part_[p];
      r.low = c.low_key;
      r.high = c.high_key;
      r.shift = 12 * static_cast<int>(c.octave) + static_cast<int>(c.transpose);
      r.keyboard_on = c.keyboard_on != 0;
      r.sounds = c.state == PartState::kReady && c.engine_type != 0;
      r.engine = c.engine_type;
      // Story 4.5: the instrument a key sounded on is part of its identity. When a part's bank (or engine) changes, as it does at a Live Set
      // switch, keys struck from now on get a new tag, so they never share a reference count with a key still held on the old bank.
      const auto bank = reinterpret_cast<std::uintptr_t>(c.sample_bank.get());
      if (configured_ && (bank != r.bank || c.engine_type != r.tag_engine)) r.tag = static_cast<std::uint8_t>((r.tag + 1) & (kTags - 1));
      r.bank = bank;
      r.tag_engine = c.engine_type;
    }
    configured_ = true;
  }
  bool active() const noexcept { return active_; }

  // A key went down on keyboard `src` (0..15): which parts play it, at which pitch. Records the key.
  void note_on(std::uint32_t src, std::uint32_t key, Fanout& out) noexcept VELORA_NONBLOCKING {
    out.count = 0;
    key &= 127u;
    Track* tr = &track_[(src & 15u) * 128 + key][0];
    for (std::uint32_t p = 0; p < kParts; ++p) {
      const Part& r = part_[p];
      if (!r.sounds || !r.keyboard_on || key < r.low || key > r.high) continue;
      const int pitch = static_cast<int>(key) + r.shift;
      if (pitch < 0 || pitch > 127) {
        dropped_out_of_range_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      Target& t = out.t[out.count++];
      t.part = static_cast<std::uint8_t>(p);
      t.pitch = static_cast<std::uint8_t>(pitch);
      t.engine = r.engine;
      t.release_first = false;
      t.old_pitch = tr[p].pitch;
      t.old_engine = tr[p].engine;
      const bool tracked = tr[p].pitch != kNoPitch;
      if (!tracked || tr[p].pitch != t.pitch || tr[p].engine != r.engine || tr[p].tag != r.tag) {
        if (tracked) {  // the earlier note of this key on this part lets go of its pitch; the voice is released when no other key holds it
          const std::uint16_t left = --ref(p, tr[p].pitch, tr[p].engine, tr[p].tag);
          t.release_first = left == 0;
        }
        ++ref(p, t.pitch, r.engine, r.tag);
      }
      tr[p].pitch = t.pitch;
      tr[p].engine = r.engine;
      tr[p].tag = r.tag;
      fanned_.fetch_add(1, std::memory_order_relaxed);
    }
    if (out.count == 0) unzoned_keys_.fetch_add(1, std::memory_order_relaxed);
  }

  // The key came up on keyboard `src`: the parts (and pitches) that got its note on, then forgotten.
  void note_off(std::uint32_t src, std::uint32_t key, Fanout& out) noexcept VELORA_NONBLOCKING {
    out.count = 0;
    Track* tr = &track_[(src & 15u) * 128 + (key & 127u)][0];
    bool had = false;
    for (std::uint32_t p = 0; p < kParts; ++p) {
      if (tr[p].pitch == kNoPitch) continue;
      had = true;
      // Several keys (or keyboard sources) can hold the same (part, pitch): the part's note off goes out when the last of them lets go.
      const std::uint16_t left = --ref(p, tr[p].pitch, tr[p].engine, tr[p].tag);
      if (left == 0) {
        Target& t = out.t[out.count++];
        t.part = static_cast<std::uint8_t>(p);
        t.pitch = tr[p].pitch;
        t.engine = tr[p].engine;
      }
      tr[p] = Track{};
    }
    if (!had) orphan_note_offs_.fetch_add(1, std::memory_order_relaxed);
  }

  // Every note that is still down, released: calls f(part, pitch, engine) once per (part, pitch) with the last holder and forgets all keys.
  // Used when the block loop leaves a Live Set generation.
  template <class F>
  void release_all(F&& f) noexcept VELORA_NONBLOCKING {
    for (auto& k : track_)
      for (std::uint32_t p = 0; p < kParts; ++p) {
        if (k[p].pitch == kNoPitch) continue;
        if (--ref(p, k[p].pitch, k[p].engine, k[p].tag) == 0) f(p, static_cast<std::uint32_t>(k[p].pitch), k[p].engine);
        k[p] = Track{};
      }
  }
  // Pedal down goes to the parts that listen to the keyboard; pedal up to every part.
  bool pedal_down_target(std::uint32_t part) const noexcept VELORA_NONBLOCKING {
    return part < kParts && part_[part].sounds && part_[part].keyboard_on;
  }

  // Counters (relaxed atomics, any thread).
  std::uint64_t fanned_notes() const noexcept { return fanned_.load(std::memory_order_relaxed); }
  std::uint64_t dropped_out_of_range() const noexcept { return dropped_out_of_range_.load(std::memory_order_relaxed); }
  // Note ons that no part took (outside every zone, keyboard off everywhere, all parts silent).
  std::uint64_t unzoned_keys() const noexcept { return unzoned_keys_.load(std::memory_order_relaxed); }
  // Note offs of keys the router had no record of (never fanned, or already released): benign.
  std::uint64_t orphan_note_offs() const noexcept { return orphan_note_offs_.load(std::memory_order_relaxed); }
  // Keys that currently hold at least one part (introspection for tests; scans the table: RT thread or after rendering).
  std::uint32_t held_keys() const noexcept {
    std::uint32_t n = 0;
    for (const auto& k : track_)
      for (const Track& t : k)
        if (t.pitch != kNoPitch) {
          ++n;
          break;
        }
    return n;
  }

 private:
  struct Part {
    std::uint8_t low = 0, high = 127;
    int shift = 0;
    bool keyboard_on = true;
    bool sounds = false;
    std::uint32_t engine = 0;
    std::uintptr_t bank = 0;
    std::uint32_t tag_engine = 0;
    std::uint8_t tag = 0;
  };
  struct Track {
    std::uint8_t pitch = kNoPitch;
    std::uint32_t engine = 0;
    std::uint8_t tag = 0;
  };
  bool configured_ = false;
  static constexpr std::uint32_t kTags = 4;  // bank generations told apart per part; a key held across 4 bank changes could alias an older tag
  bool active_ = false;
  std::array<Part, kParts> part_{};
  std::array<std::array<Track, kParts>, 16 * 128> track_{};
  // Keys holding each (part, pitch, engine): the engine is part of the identity, since a voice on another engine is another voice.
  static constexpr std::uint32_t kEngineSlots = 5;  // Sample, Organ, FM, VA, anything else
  static constexpr std::uint32_t engine_slot(std::uint32_t id) noexcept { return id >= 1 && id <= 4 ? id - 1 : 4; }
  std::uint16_t& ref(std::uint32_t part, std::uint32_t pitch, std::uint32_t engine, std::uint32_t tag) noexcept {
    return refs_[part][pitch][engine_slot(engine)][tag & (kTags - 1)];
  }
  std::array<std::array<std::array<std::array<std::uint16_t, kTags>, kEngineSlots>, 128>, kParts> refs_{};
  std::atomic<std::uint64_t> fanned_{0}, dropped_out_of_range_{0}, unzoned_keys_{0}, orphan_note_offs_{0};
};

}  // namespace velora::core
