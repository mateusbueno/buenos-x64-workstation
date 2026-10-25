#pragma once
// Story 7.2 / 7.3: per-part Arpeggiator event processor locked to the RT TransportClock.
//
// Modes: Up, Down, UpDown (end notes not repeated when >1 note in the expanded pool), Random (deterministic PRNG seeded per part),
// AsPlayed (key-press order across octaves).
// Octaves: 1..4 (each octave shifts base notes by +12 * oct, keeping notes <= 127).
// Rates (at 960 PPQN): 1/4 (960), 1/8 (480), 1/16 (240), 1/32 (120), 1/4T (640), 1/8T (320), 1/16T (160), 1/32T (80).
// Gate %: 1..200% of the step duration in frames (minimum 1 frame).
// Swing %: 50..75% (50% = straight; delays every odd step of the step pair by the swing amount).
// Latch & Sustain (CC 64): Latch On keeps notes running after key release, replaces latched notes on the first new chord after all
// keys are released, and ends unheld latched notes at the next grid step when switched Off. With Latch Off, sustain pedal down holds
// notes in the arpeggiator pattern until pedal release without deferring arpeggiator-generated step Note Offs in VoiceArbiter.
//
// Tempo changes (internal clock): BlockRenderer reads the tempo parameter once per block, before the block's events run
// (block_renderer.cpp, clock_.set_tempo_tenths), so a tempo parameter event applied inside block K changes the tempo from block K+1; the
// transport position stays continuous. A pending step is (re)evaluated in every block with that block's tempo and fires in the first block
// where its swung tick maps to a block frame < 64 (TransportClock::frame_of_tick). A swung tick that is already past (frame < 0) fires at
// block frame 0 when it lies at most 64 frames (64 x tenths units) before the block start (catch-up window); an older tick is dropped
// (the step is skipped, no note). A tempo change cannot move a pending tick more than half a frame into the past, so the window is a
// safety net there; it is reached when notes start late relative to the grid.
#include <array>
#include <cstddef>
#include <cstdint>

#include "velora/core/event_processor.hpp"
#include "velora/core/rt.hpp"
#include "velora/core/transport_clock.hpp"
#include "velora/core/ump.hpp"

namespace velora::core {

enum class ArpMode : std::uint8_t {
  kUp = 0,
  kDown = 1,
  kUpDown = 2,
  kRandom = 3,
  kAsPlayed = 4,
};

enum class ArpRate : std::uint8_t {
  k1_4 = 0,    // 960 ticks
  k1_8 = 1,    // 480 ticks
  k1_16 = 2,   // 240 ticks
  k1_32 = 3,   // 120 ticks
  k1_4T = 4,   // 640 ticks
  k1_8T = 5,   // 320 ticks
  k1_16T = 6,  // 160 ticks
  k1_32T = 7,  // 80 ticks
};

constexpr std::int64_t arp_rate_ticks(ArpRate rate) noexcept {
  switch (rate) {
    case ArpRate::k1_4: return 960;
    case ArpRate::k1_8: return 480;
    case ArpRate::k1_16: return 240;
    case ArpRate::k1_32: return 120;
    case ArpRate::k1_4T: return 640;
    case ArpRate::k1_8T: return 320;
    case ArpRate::k1_16T: return 160;
    case ArpRate::k1_32T: return 80;
  }
  return 240;
}

struct ArpConfig {
  bool enabled = false;
  ArpMode mode = ArpMode::kUp;
  ArpRate rate = ArpRate::k1_16;
  std::uint8_t octaves = 1;     // 1..4
  float gate_percent = 50.0f;   // 1..200%, default 50%
  float swing_percent = 50.0f;  // 50..75%, default 50% (straight)
  bool latch = false;           // hold notes after key release until replaced or Latch switched Off

  constexpr bool operator==(const ArpConfig& o) const noexcept = default;
};

ArpConfig sanitize_arp_config(const ArpConfig& cfg) noexcept;

class Arpeggiator final : public EventProcessor {
 public:
  static constexpr std::size_t kMaxHeldNotes = 128;
  static constexpr std::size_t kMaxExpandedNotes = kMaxHeldNotes * 4;
  static constexpr std::size_t kMaxSoundingNotes = 16;

  Arpeggiator() noexcept : Arpeggiator(0) {}
  explicit Arpeggiator(std::uint32_t part_index) noexcept;

  void set_part_index(std::uint32_t part_index) noexcept VELORA_NONBLOCKING;
  std::uint32_t part_index() const noexcept VELORA_NONBLOCKING { return part_index_; }

  // Updates the arpeggiator configuration. When `seamless_transition` is true (used when adopting a new scene / Live Set
  // generation while a previous generation was already held), disabling the arpeggiator allows already-sounding step notes
  // to finish their natural gate length rather than cutting them at frame 0 of the new generation.
  void set_config(const ArpConfig& cfg, bool seamless_transition = false) noexcept VELORA_NONBLOCKING;
  const ArpConfig& config() const noexcept VELORA_NONBLOCKING { return config_; }
  bool enabled() const noexcept VELORA_NONBLOCKING { return config_.enabled; }

  // True when an Arp On <-> Off transition has notes to start or stop on the next block, even if enabled() is now false.
  bool has_pending_transition() const noexcept VELORA_NONBLOCKING;

  // Updates the held-key table when Arp is Off and BlockRenderer routes notes directly (so switching Arp On mid-chord knows the held keys).
  void track_passthrough_note(std::uint32_t note, std::uint32_t velocity16, bool on) noexcept VELORA_NONBLOCKING;
  bool is_passthrough_sounding(std::uint32_t note) const noexcept VELORA_NONBLOCKING;
  bool is_held_non_passthrough(std::uint32_t note) const noexcept VELORA_NONBLOCKING;

  // Emits Note Off at frame 0 into `out` for any currently sounding arpeggiated notes (used when leaving a Live Set).
  void flush_sounding(EventBuffer& out) noexcept VELORA_NONBLOCKING;

  void reset() noexcept VELORA_NONBLOCKING override;
  void process_input(const Ump& ump, std::uint32_t frame, const TransportClock& clock,
                     EventBuffer& out) noexcept VELORA_NONBLOCKING override;
  void process_block(const EventBuffer& in, const TransportClock& clock, std::uint32_t frames,
                     EventBuffer& out) noexcept VELORA_NONBLOCKING override;

  std::size_t held_note_count() const noexcept VELORA_NONBLOCKING { return held_count_; }
  std::size_t physically_held_note_count() const noexcept VELORA_NONBLOCKING;
  bool sustain_pedal_down() const noexcept VELORA_NONBLOCKING { return sustain_pedal_down_; }
  void set_sustain_pedal(bool down) noexcept VELORA_NONBLOCKING { sustain_pedal_down_ = down; }

  // Deterministic per-part PRNG (splitmix64 seed + xorshift64* step), exposed so tests can verify exact random note sequences.
  static constexpr std::uint64_t initial_rng_state(std::uint32_t part_index) noexcept {
    std::uint64_t x = 0x9E3779B97F4A7C15ull * (static_cast<std::uint64_t>(part_index) + 1ull);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x = x ^ (x >> 31);
    return x ? x : 0x9E3779B97F4A7C15ull;
  }

  static constexpr std::uint64_t step_rng(std::uint64_t& state) noexcept {
    std::uint64_t x = state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state = x;
    return x * 0x2545F4914F6CDD1Dull;
  }

  // Returns the swung musical tick for a straight grid tick `grid_tick` (a multiple of `step_ticks`).
  // Even steps ((grid_tick / step_ticks) % 2 == 0) return `grid_tick`; odd steps return
  // `(grid_tick - step_ticks) + llround(2.0 * step_ticks * (swing_percent / 100.0))`.
  static std::int64_t swung_step_tick(std::int64_t grid_tick, std::int64_t step_ticks,
                                      float swing_percent) noexcept VELORA_NONBLOCKING;

  // Smallest multiple of `step_ticks` (>= 0) whose swung TransportClock::frame_of_tick is >= `frame`.
  static std::int64_t first_grid_tick_from_frame(const TransportClock& clock, std::uint32_t frame,
                                                 std::int64_t step_ticks,
                                                 float swing_percent = 50.0f) noexcept VELORA_NONBLOCKING;

 private:
  struct HeldNote {
    std::uint8_t note = 0;
    std::uint16_t velocity16 = 0;
    std::uint64_t press_order = 0;
    std::uint64_t first_press_order = 0;
    bool passthrough_sounding = false;
    bool physically_held = true;
  };

  struct SoundingNote {
    std::uint8_t note = 0;
    std::uint64_t off_frame = 0;  // absolute frame (clock.frames() + in_block_frame)
    bool active = false;
  };

  struct ExpandedNote {
    std::uint8_t note = 0;
    std::uint16_t velocity16 = 0;
  };

  bool hold_notes_active() const noexcept VELORA_NONBLOCKING { return config_.latch || sustain_pedal_down_; }
  bool has_active_sounding() const noexcept VELORA_NONBLOCKING;
  void add_or_update_held(std::uint8_t note, std::uint16_t velocity16, bool passthrough_sounding) noexcept VELORA_NONBLOCKING;
  void remove_held(std::uint8_t note) noexcept VELORA_NONBLOCKING;
  void mark_note_released(std::uint8_t note) noexcept VELORA_NONBLOCKING;
  void remove_unheld_notes() noexcept VELORA_NONBLOCKING;
  void handle_sustain_cc(std::uint32_t value32, std::uint32_t frame, EventBuffer& out) noexcept VELORA_NONBLOCKING;
  void emit_all_sounding_offs(std::uint32_t frame, EventBuffer& out) noexcept VELORA_NONBLOCKING;
  void emit_due_sounding_offs(std::uint64_t abs_frame, std::uint32_t frame, EventBuffer& out) noexcept VELORA_NONBLOCKING;
  void apply_mode_transition(std::uint32_t frame, const TransportClock& clock, EventBuffer& out) noexcept VELORA_NONBLOCKING;
  std::size_t build_expanded_pool(std::array<ExpandedNote, kMaxExpandedNotes>& pool) const noexcept VELORA_NONBLOCKING;
  void trigger_step(std::uint32_t frame, const TransportClock& clock, EventBuffer& out) noexcept VELORA_NONBLOCKING;

  std::uint32_t part_index_ = 0;
  ArpConfig config_{};
  bool rate_changed_ = false;
  bool latch_off_pending_ = false;
  bool transition_start_passthrough_ = false;
  bool sustain_pedal_down_ = false;
  std::array<HeldNote, kMaxHeldNotes> held_{};
  std::size_t held_count_ = 0;
  std::uint64_t press_counter_ = 0;
  std::array<SoundingNote, kMaxSoundingNotes> sounding_{};
  bool pattern_running_ = false;
  std::uint64_t step_index_ = 0;
  std::int64_t next_grid_tick_ = 0;
  std::uint64_t rng_state_ = initial_rng_state(0);
};

}  // namespace velora::core
