#pragma once

#include <cstdint>

#include "velora/core/rt.hpp"

namespace velora::core {

// Engine type IDs. The numeric values of the four reserved IDs are STABLE and must never change
// or be reused (they will appear in saved data). kTestTone is a non-reserved, test-only engine.
enum class EngineTypeId : std::uint32_t {
  kSample = 1,  // reserved
  kOrgan = 2,   // reserved
  kFm = 3,      // reserved
  kVa = 4,      // reserved
  kTestTone = 0xF000,
};

enum class EngineStatus : std::uint8_t {
  kOk = 0,
  kEngineNotAvailable,  // reserved type with no compiled-in engine: silence, never another engine
  kEngineUnknown,       // id outside the reserved set (and not the test engine)
  kPoolExhausted,       // no free voice and no steal victim: newest note-on dropped
  kVoiceNotFound,       // note-off with no matching held voice
  kEventOverflow,       // pending-event buffer full: event dropped (queue keeps draining)
  kCount
};

enum class VoiceState : std::uint8_t { kHeld, kSustained, kReleasing };

// Steal metadata an engine reports per voice (consumed by the VoiceArbiter, Story 1.19).
//   state: kHeld = the key is down; kSustained = the sound outlives the key for a reason the engine
//   knows (e.g. a one-shot sample after note off); kReleasing = fading out on its own or fired by a
//   note off. The pedal is NOT known to the engine: the arbiter defers a pedal-held note off, so the
//   engine still reports kHeld and the arbiter classifies it as pedal-sustained.
struct VoiceInfo {
  std::uint64_t start_time = 0;  // engine frame clock at voice start
  float level = 0.0f;            // current output level estimate
  VoiceState state = VoiceState::kHeld;
  std::uint32_t channel = 0;     // UMP channel the voice was started for
  std::uint32_t note = 0;        // note number the voice was started for
};

class Engine;
struct SampleBank;

// The only way an engine obtains a voice slot once a pool arbiter is attached (AD-7, AD-22).
class VoiceAllocator {
 public:
  static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;
  virtual ~VoiceAllocator() = default;
  // Returns a free slot of `engine` for a voice about to start for (channel, note), stealing a
  // voice (anywhere in the Live Set) when the pool is full; kNoSlot when no slot can be granted.
  // The caller must fill the returned slot before it calls acquire() again.
  virtual std::uint32_t acquire(Engine& engine, std::uint32_t channel, std::uint32_t note) noexcept
      VELORA_NONBLOCKING = 0;
};

// The single static engine contract for Sample, Organ, FM and VA. Engines are compiled in (no
// plugin loading). All voice start/stop calls are made by the core, on the render thread, between
// render segments; render() accumulates (adds) into the stereo buffers.
class Engine {
 public:
  virtual ~Engine() = default;

  virtual EngineTypeId type_id() const noexcept = 0;
  // Cost units one active voice consumes (used by the pool arbiter's budget).
  virtual std::uint32_t cost_units_per_voice() const noexcept = 0;
  virtual std::uint32_t voice_capacity() const noexcept = 0;

  virtual EngineStatus start_voice(std::uint32_t channel, std::uint32_t note,
                                   std::uint32_t velocity16) noexcept VELORA_NONBLOCKING = 0;
  // Begins release of every held voice for (channel, note).
  virtual EngineStatus stop_voice(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING = 0;
  // Sound parameters are addressed by param_id.
  virtual void set_param(std::uint32_t param_id, float value) noexcept VELORA_NONBLOCKING = 0;
  // Adds `frames` frames of output into left/right.
  virtual void render(float* left, float* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING = 0;

  // Per-part render (Story 2.7). Same as render(), but the output of the voices of UMP channel c is added into left[c]/right[c]
  // (c = 0..kSplitChannels-1). Every pointer must be valid. The arithmetic per sample is the one of render(), so a render() call
  // equals a render_split() call with all pointers equal. Pure virtual: an engine must say where its voices go. In the routing
  // topology UMP channels 8..15 ALIAS parts 0..7 (the Mixer points channel c and c + 8 at the same part bus); parts are 0..7.
  static constexpr std::uint32_t kSplitChannels = 16;
  virtual void render_split(float* const* left, float* const* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING = 0;

  // Steal metadata. Returns false when the slot is inactive.
  virtual bool voice_info(std::uint32_t slot, VoiceInfo& out) const noexcept VELORA_NONBLOCKING = 0;

  // Called by the arbiter when it steals `slot`: the voice must fade to silence with a monotone
  // (never rising) gain ramp over `ramp_frames` rendered frames, then free itself. It stays active
  // (voice_info() true) until silent, ignores later note offs, and keeps its slot meanwhile.
  // False when the slot is inactive or already fading.
  virtual bool begin_steal_fade(std::uint32_t slot, std::uint32_t ramp_frames) noexcept VELORA_NONBLOCKING = 0;

  // Phase 2 hook (Story 2.2). Called once per engine per block, before any voice renders and before
  // render() is called for the block's segments, with the block length. An engine with a shared
  // generator (the tonewheel organ) produces the block's shared per-part state here; voices then only
  // read it. Default: nothing to do.
  virtual void begin_block(std::uint32_t frames) noexcept VELORA_NONBLOCKING { (void)frames; }

  // A performance parameter of `part` (registry param id, already clamped to its range) changed. Called
  // by the block loop at the event's frame, between render segments, so the engine can start a ramp at
  // exactly that in-block offset. Also called for initial values on a generation change. Default: ignored.
  virtual void set_part_param(std::uint32_t part, std::uint32_t param_id, float value) noexcept VELORA_NONBLOCKING {
    (void)part;
    (void)param_id;
    (void)value;
  }

  // Key contact events (Story 2.5, contact_events.hpp). An engine that can follow a multi-contact keybed (the tonewheel organ)
  // returns true from supports_contacts() and handles set_contact(): contact `contact` (1-based) of key (channel, note) changed
  // to `closed`, at the in-block frame the block loop calls it for (between render segments). Malformed sequences are the
  // engine's to ignore (and count); the return value is a typed status for real failures only (kPoolExhausted,
  // kEngineNotAvailable), kOk otherwise. Other engines never see contact events: the block loop counts them as unsupported and
  // those engines keep starting on Note On.
  // `effect` reports what the event did to the key: a voice started (the block loop then marks the key down in the arbiter and
  // binds the snapshot generation), the key's last contact opened (the block loop marks it up), or nothing.
  enum class ContactEffect : std::uint8_t { kNone, kStarted, kReleased };
  virtual bool supports_contacts() const noexcept { return false; }
  virtual EngineStatus set_contact(std::uint32_t channel, std::uint32_t note, std::uint32_t contact, bool closed,
                                   ContactEffect& effect) noexcept VELORA_NONBLOCKING {
    (void)channel;
    (void)note;
    (void)contact;
    (void)closed;
    effect = ContactEffect::kNone;
    return EngineStatus::kEngineNotAvailable;
  }

  // Per-part instrument (Story 4.3): the block loop hands a part's prepared sample bank (or nullptr = silent) to the engine at a
  // generation change, between render segments. Only the sample engine uses it; the default ignores it.
  virtual void set_part_sample_bank(std::uint32_t part, SampleBank* bank) noexcept VELORA_NONBLOCKING {
    (void)part;
    (void)bank;
  }

  // Forgets every per-part binding made with set_part_sample_bank() (the block loop leaves a Live Set generation).
  virtual void clear_part_sample_banks() noexcept VELORA_NONBLOCKING {}

  // True for an engine whose voices the arbiter must never choose as steal victims (the tonewheel organ).
  // Its cost is reserved ahead of every engine floor through LiveSetConfig::organ_rotary_cost.
  virtual bool steal_protected() const noexcept { return false; }

  // Setup-time: attach the pool arbiter. Without one (standalone use in tests) the engine hands out
  // its first free slot and reports kPoolExhausted when none is left.
  void set_voice_allocator(VoiceAllocator* allocator) noexcept { allocator_ = allocator; }

 protected:
  // Slot for a voice that is about to start; VoiceAllocator::kNoSlot when none can be had.
  std::uint32_t acquire_slot(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING {
    if (allocator_) return allocator_->acquire(*this, channel, note);
    VoiceInfo vi;
    const std::uint32_t n = voice_capacity();
    for (std::uint32_t i = 0; i < n; ++i)
      if (!voice_info(i, vi)) return i;
    return VoiceAllocator::kNoSlot;
  }

 private:
  VoiceAllocator* allocator_ = nullptr;
};

}  // namespace velora::core
