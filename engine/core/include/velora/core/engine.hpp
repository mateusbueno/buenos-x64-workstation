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
class WorkerPool;
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
  // Story 3.6: the physical note off with the MIDI 2.0 16-bit release velocity of the Note Off UMP (0 = none was measured). An engine that does not use
  // the release velocity keeps this default, which is stop_voice(); the sample engine uses it to pick / scale release-trigger regions. No curve is applied here.
  virtual EngineStatus release_voice(std::uint32_t channel, std::uint32_t note, std::uint32_t release_velocity16) noexcept VELORA_NONBLOCKING {
    (void)release_velocity16;
    return stop_voice(channel, note);
  }
  // Sound parameters are addressed by param_id.
  virtual void set_param(std::uint32_t param_id, float value) noexcept VELORA_NONBLOCKING = 0;
  // Adds `frames` frames of output into left/right.
  virtual void render(float* left, float* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING = 0;

  // Per-part render (Story 2.7). Same as render(), but the output of the voices of UMP channel c is added into left[c]/right[c]
  // (c = 0..kSplitChannels-1). Every pointer must be valid. The arithmetic per sample is the one of render(), so a render() call
  // equals a render_split() call with all pointers equal. Pure virtual: an engine must say where its voices go. In the routing
  // topology UMP channel c IS part c (16 parts, Story 4.1; before it, channels 8..15 aliased parts 0..7 on the Mixer's part buses).
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

  // The same parameter, but as a live EDIT (a UMP parameter event from a knob / ws.v1 / MIDI CC, an encoder step) instead of an initial or re-synced value
  // (generation change, part remap). An engine whose base patch is not the registry default uses the difference (part_params.hpp, "engagement"): an edit always
  // takes effect, an initial value only when it differs from the registry default. Default: the same as set_part_param().
  virtual void set_part_param_edit(std::uint32_t part, std::uint32_t param_id, float value) noexcept VELORA_NONBLOCKING {
    set_part_param(part, param_id, value);
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

  // Controllers (Story 5.2). A MIDI 2.0 control change (CC number 0..127, 32-bit value 0 = 0.0 .. 0xFFFFFFFF = 1.0) addressed to `part`
  // (the part's UMP channel), delivered by the block loop at the event's frame between render segments. Returns true when the engine
  // reads controllers (only the sample engine: CC crossfades); the block loop counts the event as ignored when no engine did. The
  // sustain pedal (CC 64) is not delivered here: the arbiter handles it.
  virtual bool set_part_cc(std::uint32_t part, std::uint32_t cc, std::uint32_t value32) noexcept VELORA_NONBLOCKING {
    (void)part;
    (void)cc;
    (void)value32;
    return false;
  }

  // Pitch bend (Story 3.10): MIDI 2.0 pitch bend addressed to `part`, value32 unsigned with 0x80000000 = centre, delivered at the event's frame between
  // render segments. Returns true when the engine follows it; the block loop counts the event as ignored when no engine did. Default: not followed.
  virtual bool set_part_pitch_bend(std::uint32_t part, std::uint32_t value32) noexcept VELORA_NONBLOCKING {
    (void)part;
    (void)value32;
    return false;
  }

  // The key (channel, note) physically came up (Story 5.2). Called at the note off even when the sustain pedal defers the engine's
  // stop_voice() to the pedal up, so that engines that follow held keys (sw_down / sw_up keyswitches) see the release at once.
  // Idempotent for the engine: stop_voice() of the same key afterwards changes nothing more. Default: nothing.
  // Returns true when the engine started a voice because of the key up (the sample engine's trigger=release_key): the block loop then binds the
  // generation to the key. Default: false.
  virtual bool key_released(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING {
    (void)channel;
    (void)note;
    return false;
  }
  // Every key of `channel` is up (all sound off / all notes off, Story 13.3): engines that track held keys forget them. Default: nothing.
  virtual void all_keys_released(std::uint32_t channel) noexcept VELORA_NONBLOCKING { (void)channel; }

  // A Live Set switch takes effect (Story 5.2): drop the per-part play state that belongs to the previous Live Set (the sample engine
  // returns every part to its instrument's sw_default keyswitch and forgets held keys). Sounding voices are not touched. Default: nothing.
  virtual void reset_play_state() noexcept VELORA_NONBLOCKING {}

  // True for an engine whose voices the arbiter must never choose as steal victims (the tonewheel organ).
  // Its cost is reserved ahead of every engine floor through LiveSetConfig::organ_rotary_cost.
  virtual bool steal_protected() const noexcept { return false; }
  // Story 6.5: how many times a voice of this engine met a damaged block of a protected library (a voice faded to silence for it). Monotone, written by the
  // audio thread, readable from any thread; 0 for an engine that plays no protected content.
  virtual std::uint64_t content_integrity_events() const noexcept { return 0; }
  // Story 1.16: how many times a voice of this engine reached the end of the resident head of a sample that has a tail (a stream underrun placeholder until
  // Story 1.17 streams the tail). Monotone, written by the audio thread, readable anywhere; 0 for an engine that plays no heads.
  virtual std::uint64_t stream_underrun_events() const noexcept { return 0; }

  // Parallel voice render (Story 1.18). Setup-time, never concurrent with render: hands the engine the block loop's worker pool (nullptr = serial,
  // the default) and the longest `frames` count a single render()/render_split() call will carry. An engine whose voices can render in parallel
  // (only the sample engine) allocates its per-worker scratch here; the others ignore it and stay on the calling thread. Default: ignored.
  virtual void set_parallel_voice_pool(WorkerPool* pool, std::uint32_t max_frames) {
    (void)pool;
    (void)max_frames;
  }

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
