#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "velora/core/arena.hpp"
#include "velora/core/arpeggiator.hpp"
#include "velora/core/clock_follower.hpp"
#include "velora/core/engine.hpp"
#include "velora/core/engine_registry.hpp"
#include "velora/core/event_processor.hpp"
#include "velora/core/mixer.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/part_router.hpp"
#include "velora/core/snapshot.hpp"
#include "velora/core/spsc_queue.hpp"
#include "velora/core/test_tone_engine.hpp"
#include "velora/core/transport_clock.hpp"
#include "velora/core/worker_pool.hpp"
#include "velora/core/ump.hpp"
#include "velora/core/voice_arbiter.hpp"

namespace velora::core {

constexpr std::uint32_t kBlockFrames = 64;
constexpr std::uint32_t kSampleRate = 48000;

// Single-threaded serial block renderer: 64-frame stereo float blocks at 48 kHz.
//
// Event frame semantics: submit() takes a TimedUmp whose target_frame is relative to the start of
// the NEXT block rendered. Events with target_frame < 64 apply at exactly that frame. Events with
// target_frame >= 64 are DEFERRED: carried to following blocks with 64 subtracted per block, so
// they apply at the correct absolute position (not rejected). Within a block events apply in frame
// order, ties in arrival order.
//
// Sustain pedal (CC 64, value >= half range = down): a note off while the pedal is down (CC 64 value >= 64, i.e. >= 0x80000000 in 32 bits) is deferred to
// the pedal-up event; the arbiter treats such voices as pedal-sustained (protected from stealing).
struct EmittedNoteEvent {
  std::uint64_t frame = 0;       // absolute frame since start/reset (clock().frames() + in_block_frame)
  std::uint8_t part = 0;         // part / channel index
  std::uint8_t note = 0;         // MIDI note 0..127
  std::uint16_t velocity16 = 0;  // 16-bit velocity
  bool on = false;               // true = NoteOn, false = NoteOff
};

using NoteObserver = void (*)(void* ctx, const EmittedNoteEvent& ev) noexcept;

class BlockRenderer {
 public:
  static constexpr std::size_t kQueueCapacity = 1024;
  static constexpr std::size_t kPendingCapacity = 256;

  explicit BlockRenderer(std::uint32_t test_tone_voices = 32);
  ~BlockRenderer();
  BlockRenderer(const BlockRenderer&) = delete;
  BlockRenderer& operator=(const BlockRenderer&) = delete;

  // Producer side (any single producer thread). UMP is the only accepted input type.
  // False when the queue is full; events already queued are unaffected.
  bool submit(const TimedUmp& ev) noexcept VELORA_NONBLOCKING { return queue_.push(ev); }

  // Setup-time: choose the engine type that serves a UMP channel (default kTestTone).
  // The engine is also handed the current value of every performance parameter of that part (so drawbars
  // set before the routing existed are not lost).
  void set_channel_engine(std::uint32_t channel, std::uint32_t engine_type_id) noexcept {
    channel_engine_[channel & 0xF] = engine_type_id;
    sync_part_params(channel & 0xF);
  }

  // Setup-time: bind another compiled-in engine (e.g. the SampleEngine) next to the always-bound test
  // tone, then route channels to it with set_channel_engine(). False when the registry is full or the
  // type is already bound. The engine must outlive the renderer and be configured before audio runs;
  // every bound engine is rendered each block (idle engines add nothing).
  // The engine joins the voice pool arbiter as well (call before arbiter().reserve()).
  bool bind_engine(Engine& engine) {
    if (!registry_.bind(engine)) return false;
    if (arbiter_.bind(engine)) {
      if (voice_pool_active_) engine.set_parallel_voice_pool(&voice_pool_, kBlockFrames);
      // channels mapped before the bind receive the current performance values
      for (std::uint32_t ch = 0; ch < 16; ++ch)
        if (channel_engine_[ch] == static_cast<std::uint32_t>(engine.type_id())) sync_part_params(ch);
      return true;
    }
    registry_.unbind_last();  // keep registry and arbiter in step
    return false;
  }

  // Story 1.18, setup-time (never while process_block runs): the parallel voice render. Each block still runs its events serially; the voices of
  // the engines that support it (the sample engine) are then rendered in 3 fixed partitions on a lock-free worker pool, a barrier, and a
  // summation in fixed partition order (see SampleEngine::set_parallel_voice_pool). `threads`: 0 = off (the default: the exact serial render),
  // 1 = the three partitions run inline on the audio thread, 3 = one worker per partition (other values are clamped, see
  // WorkerPool::StartResult). 1 and 3 give bit-identical output. `affinity` (nullable) pins worker p to cpus[p] (nullable: cpu p), once at start.
  // Applies to the engines bound now and later. SETUP-TIME ONLY: never call it while process_block() runs (asserted in debug builds); the bound
  // engines must outlive the renderer while it is on (the destructor detaches the pool from them).
  WorkerPool::StartResult set_parallel_voice_render(std::uint32_t threads, ports::ThreadAffinity* affinity = nullptr,
                                                    const std::uint32_t* cpus = nullptr);
  std::uint32_t parallel_voice_threads() const noexcept { return voice_pool_active_ ? voice_pool_.threads() : 0u; }

  // The voice pool arbiter (Story 1.19): the sole allocator and stealer. reserve() it with the Live Set
  // configuration; until then voices are first-free and a full pool reports kPoolExhausted.
  VoiceArbiter& arbiter() noexcept { return arbiter_; }
  const VoiceArbiter& arbiter() const noexcept { return arbiter_; }

  // Render one block for an adapter with a single stereo output. left/right each hold kBlockFrames floats and are overwritten.
  // The Aux bus is FOLDED INTO Main here (Main + Aux, so a part assigned to Aux is still heard); blocks in which Aux carried
  // signal are counted in aux_folded_blocks(). Adapters with an Aux output use the OutputBuses overload.
  void process_block(float* left, float* right) noexcept VELORA_NONBLOCKING;
  // Render one block to the Main bus and the Aux bus (Story 2.7). All four buffers hold kBlockFrames floats and are overwritten.
  // Aux must be given completely or not at all (not at all = the Aux bus is discarded); with only one of aux_left / aux_right
  // set, or without Main, nothing is rendered, Main is silenced and the block is counted in rejected_output_blocks().
  void process_block(const OutputBuses& out) noexcept VELORA_NONBLOCKING;
  std::uint64_t aux_folded_blocks() const noexcept { return aux_folded_.load(std::memory_order_relaxed); }
  std::uint64_t rejected_output_blocks() const noexcept { return rejected_outputs_.load(std::memory_order_relaxed); }

  // --- routing (Story 2.7) --------------------------------------------------------------------------------
  // The fixed routing topology after the voices: per-part insert chains, sends, Main/Aux, line-in and the master section.
  Mixer& mixer() noexcept { return mixer_; }
  const Mixer& mixer() const noexcept { return mixer_; }
  // Setup-time: the line-in source (not owned, must outlive the renderer's use; nullptr = silent). Mixed into the master only.
  void set_line_in(ports::LineInSource* source) noexcept { mixer_.set_line_in(source); }

  // contact_k events (Story 2.5, contact_events.hpp) are neither notes nor ignored events: contact_events_routed() counts those
  // delivered to an engine that follows contacts (the organ; the engine counts the malformed ones it refuses),
  // contact_events_dropped() those with no taker (an engine without contact support, a note field above 127, no engine).
  std::uint64_t contact_events_routed() const noexcept { return contact_routed_.load(std::memory_order_relaxed); }
  std::uint64_t contact_events_dropped() const noexcept { return contact_dropped_.load(std::memory_order_relaxed); }

  // Count of occurrences of each typed status since construction (kOk counts successes).
  std::uint64_t status_count(EngineStatus s) const noexcept { return status_counts_[static_cast<std::size_t>(s)]; }
  EngineStatus last_status() const noexcept { return last_status_; }
  std::uint64_t ignored_events() const noexcept { return ignored_; }
  // Note offs for a note whose voices the arbiter had stolen (nothing left to release): benign, kept out
  // of status_count(). dropped_deferred_releases(): pedal-up releases whose engine was no longer there.
  std::uint64_t stolen_note_offs() const noexcept { return stolen_note_offs_; }
  std::uint64_t dropped_deferred_releases() const noexcept { return dropped_deferred_; }

  // --- control-plane state (Story 2.1) ------------------------------------------------------------
  // The control thread publishes structural Snapshots through snapshots() and reads the RT-owned live
  // performance values from last_values(); both are lock-free. The renderer adopts the newest published
  // generation at the start of each block and keeps it referenced for every note that started on it
  // until that note's voices are gone (AD-23), so the control thread can tell when a generation is free.
  SnapshotExchange& snapshots() noexcept { return exchange_; }
  const LastValueTable& last_values() const noexcept { return last_; }
  // Story 1.21: the host render path records xruns, render times and event latencies here (atomics only).
  Diagnostics& diagnostics_rt() noexcept { return last_.diagnostics_rt(); }
  const Snapshot* held_snapshot() const noexcept { return held_; }  // RT thread only
  // Performance parameter UMPs applied to / refused by the last-value table (unknown id, structural
  // class, part out of range). Relaxed counters, readable from any thread.
  std::uint64_t param_events_applied() const noexcept { return param_applied_.load(std::memory_order_relaxed); }
  std::uint64_t param_events_rejected() const noexcept { return param_rejected_.load(std::memory_order_relaxed); }
  // Story 3.10: raw panel control events (faders, VALUE encoder, pedals; assign_events.hpp) that reached the RT plane; binding them is Epic 4.
  std::uint64_t panel_control_events() const noexcept { return panel_controls_.load(std::memory_order_relaxed); }
  // The last raw value of panel control `control_id` (0..255), as the frame carried it; 0 before the first event.
  std::uint16_t panel_control_value(std::uint32_t control_id) const noexcept { return panel_value_[control_id & 0xFFu].load(std::memory_order_relaxed); }

  // The RT plane's single tempo value in bpm (Story 2.11): the live value of the performance parameter `tempo_bpm` (master scope, 20..300, default 120), clamped.
  // Today it is set by an ordinary parameter event (ws.v1 SetPerformanceParam, a UMP, the offline renderer's options): the internal tempo is the only
  // source. Epic 7's external clock follower will write the same slot, so every consumer (the Mixer's delays read it once per block, at the start of
  // the block, so a change takes effect at the next block) keeps this one API. Any thread.
  float tempo_bpm() const noexcept;

  // The RT plane's single transport clock (Story 7.1): reads tempo_bpm() once per block (before the Mixer's begin_block) and advances at block
  // end. Position is the block start; query ticks_at / frame_of_tick for frames inside the block. RT thread only (or while not rendering).
  const TransportClock& clock() const noexcept { return clock_; }

  // --- External MIDI Clock Follower (Story 7.6) -----------------------------------------------------------------
  void set_clock_source(ClockSource source) noexcept VELORA_NONBLOCKING {
    clock_source_.store(source, std::memory_order_release);
    if (source != ClockSource::kInternal) {
      follower_.reset();
    }
  }
  ClockSource clock_source() const noexcept VELORA_NONBLOCKING {
    return clock_source_.load(std::memory_order_acquire);
  }
  ClockFollower& clock_follower() noexcept VELORA_NONBLOCKING { return follower_; }
  const ClockFollower& clock_follower() const noexcept VELORA_NONBLOCKING { return follower_; }

  // --- Event-processor framework and arpeggiator (Story 7.2, AR17) ------------------------------------------------
  // Each part (0..7) has an EventProcessor slot in the serial phase that consumes the part's UMP input and emits UMP with exact
  // frame positions before voice allocation. By default the slot runs the part's built-in Arpeggiator (configured via Snapshot::parts[p].arp
  // or set_part_arp()); a custom processor (e.g. a test processor or a future sequencer) may be installed with set_event_processor().
  void set_event_processor(std::uint32_t part, EventProcessor* processor) noexcept VELORA_NONBLOCKING {
    if (part < params::kPartCount) custom_processors_[part] = processor;
  }
  EventProcessor* event_processor(std::uint32_t part) noexcept VELORA_NONBLOCKING {
    if (part >= params::kPartCount) return nullptr;
    return custom_processors_[part] ? custom_processors_[part] : &arps_[part];
  }
  const EventProcessor* event_processor(std::uint32_t part) const noexcept VELORA_NONBLOCKING {
    if (part >= params::kPartCount) return nullptr;
    return custom_processors_[part] ? custom_processors_[part] : &arps_[part];
  }
  Arpeggiator& arp(std::uint32_t part) noexcept VELORA_NONBLOCKING { return arps_[part & (params::kPartCount - 1)]; }
  const Arpeggiator& arp(std::uint32_t part) const noexcept VELORA_NONBLOCKING { return arps_[part & (params::kPartCount - 1)]; }
  void set_part_arp(std::uint32_t part, const ArpConfig& cfg) noexcept VELORA_NONBLOCKING {
    if (part < params::kPartCount) arps_[part].set_config(cfg);
  }

  // Optional test / offline-render observation hook called on every part note applied to voice allocation.
  void set_note_observer(NoteObserver fn, void* ctx = nullptr) noexcept {
    note_observer_ = fn;
    note_observer_ctx_ = ctx;
  }

  // --- ASSIGN encoders (Story 4.10) ---------------------------------------------------------------------------------
  // The encoder mode flag, the selected part and the counters of the encoder steps (assign_events.hpp): set by the reserved control events
  // (make_encoder_mode_event, make_select_part_event) and read from any thread. In ASSIGN mode an EncoderStep event changes the parameter mapped to
  // its slot in the current generation's ASSIGN map (Snapshot::assign); PART and ALL do nothing yet (counted).
  const EncoderState& encoder_state() const noexcept { return last_.encoder(); }

  // --- Live Set routing (Story 4.3) ----------------------------------------------------------------------------
  // In a generation built from a Live Set (Snapshot::live_set) the keyboard's note on / note off / sustain are fanned out to the parts
  // by zone (PartRouter, see part_router.hpp for the keyboard-channel convention and the note-off rules) and each part is mapped to the
  // engine and instrument of its PartCfg at the generation change. The counters live in router().
  const PartRouter& router() const noexcept { return router_; }
  // --- controllers (Story 5.2) ---------------------------------------------------------------------------------------
  // A UMP control change other than the sustain pedal (CC 64) reaches the sample engine's per-part CcState (CC crossfades). Outside a
  // Live Set generation the channel is the part (like every part event). In a Live Set generation the channel names the keyboard
  // source and the controller is fanned out to every part that sounds with the keyboard on, exactly as the sustain pedal reaches the
  // parts: the parts' CcState follows the keyboard's controllers. Registry-mapped CCs (70-94: drawbars, vibrato, ...) arrive as
  // parameter events (the host maps them before the queue, unchanged) and keep that meaning; additionally the CcState of the
  // addressed part follows them (a parameter event carries the CC's normalised value), so a sample instrument that crossfades on one
  // of those controller numbers still sees it. A control change no engine reads is an ignored event (ignored_events()). Only genuine MIDI control changes feed that state: the event
  // made by cc_to_param_event carries kMidiCcOriginBit and is routed exactly like an unmapped CC (every part that listens in a Live Set, whatever the
  // scope of the parameter and even when the parameter event itself is refused); a parameter event from the UI / ws.v1 does not.
  // Parts a Live Set generation left silent (missing library, engine not available) receive no notes; notes that no part took are
  // counted in router().unzoned_keys().

  bool ready() const noexcept { return bound_; }
  const EngineRegistry& registry() const noexcept { return registry_; }
  TestToneEngine& test_tone() noexcept { return tone_; }

 private:
  void apply(const Ump& ump) noexcept VELORA_NONBLOCKING;
  void apply_contact(const Ump& ump) noexcept VELORA_NONBLOCKING;
  // One note on / off for engine `type_id` on UMP channel `channel` (= the part in a Live Set generation): arbiter bookkeeping, engine
  // call, status counting. The body of the pre-4.3 note path. When `from_arp` is true, Note Offs bypass sustain-pedal deferral in
  // VoiceArbiter so arpeggiator-generated step gates always end on schedule.
  void apply_part_note(std::uint32_t channel, std::uint32_t note, std::uint32_t velocity16, bool on, std::uint32_t type_id,
                       bool from_arp = false) noexcept VELORA_NONBLOCKING;
  void apply_keyboard_note(const Ump& ump) noexcept VELORA_NONBLOCKING;
  void apply_keyboard_sustain(bool down) noexcept VELORA_NONBLOCKING;
  // Controllers other than the pedal (Story 5.2): UMP control change -> the part's engine (CcState of the sample engine).
  void apply_controller(const Ump& ump) noexcept VELORA_NONBLOCKING;
  // The raw controller path: `channel` is the part (outside a Live Set) or the keyboard source (in one, fanned out to every part that
  // listens). `count_ignored`: a control change nobody read is an ignored event (not for the copy of a mapped CC that also was a parameter).
  void route_cc(std::uint32_t channel, std::uint32_t cc, std::uint32_t value32, bool count_ignored) noexcept VELORA_NONBLOCKING;
  // Pitch bend (Story 3.10): like a controller. In a Live Set generation the keyboard's bend goes to every part that sounds with the keyboard on; otherwise to the channel's part.
  void route_pitch_bend(std::uint32_t channel, std::uint32_t value32) noexcept VELORA_NONBLOCKING;
  bool deliver_pitch_bend(std::uint32_t part, std::uint32_t value32) noexcept VELORA_NONBLOCKING;
  bool deliver_cc(std::uint32_t part, std::uint32_t cc, std::uint32_t value32) noexcept VELORA_NONBLOCKING;
  // Gives the keyboard's last controller values to the parts that listen and did not have them (all = a seamless switch: every listening part).
  void replay_keyboard_ccs(const Snapshot& s, bool all, const std::array<bool, params::kPartCount>& remapped) noexcept VELORA_NONBLOCKING;
  void leave_live_set(std::array<bool, params::kPartCount>& remapped) noexcept VELORA_NONBLOCKING;
  void adopt_live_set_parts(const Snapshot& s, std::array<bool, params::kPartCount>& remapped) noexcept VELORA_NONBLOCKING;
  void record(EngineStatus s) noexcept VELORA_NONBLOCKING;
  void drain_queue() noexcept VELORA_NONBLOCKING;
  void render_segment(float* l, float* r, std::uint32_t from, std::uint32_t to, bool routed) noexcept VELORA_NONBLOCKING;

  void release_deferred(std::uint32_t channel) noexcept VELORA_NONBLOCKING;
  void adopt_generation() noexcept VELORA_NONBLOCKING;
  void forward_part_param(const params::ParamDef& d, std::uint32_t part, float value) noexcept VELORA_NONBLOCKING;
  void sync_part_params(std::uint32_t channel) noexcept VELORA_NONBLOCKING;
  void apply_param(const ParamEvent& ev) noexcept VELORA_NONBLOCKING;
  void apply_reserved_event(std::uint32_t id, const Ump& ump) noexcept VELORA_NONBLOCKING;
  void apply_encoder_step(std::uint32_t encoder, std::int32_t delta) noexcept VELORA_NONBLOCKING;
  void bind_generation(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING;
  void reap_generation_refs() noexcept VELORA_NONBLOCKING;

  void detach_voice_pool() noexcept;
  std::atomic<bool> in_block_{false};
  Arena arena_;
  WorkerPool voice_pool_;  // Story 1.18: joined in the destructor; engines only borrow it
  bool voice_pool_active_ = false;
  TestToneEngine tone_;
  EngineRegistry registry_;
  VoiceArbiter arbiter_;
  SpscQueue<TimedUmp, kQueueCapacity> queue_;
  std::array<TimedUmp, kPendingCapacity> pending_{};  // sorted by target_frame, stable
  std::size_t pending_count_ = 0;
  std::array<std::uint32_t, 16> channel_engine_{};
  std::array<std::uint32_t, 16> legacy_engine_{};  // channel_engine_ as it was before the first Live Set generation (restored when one is left)
  bool in_live_set_ = false;
  std::array<std::uint64_t, static_cast<std::size_t>(EngineStatus::kCount)> status_counts_{};
  EngineStatus last_status_ = EngineStatus::kOk;
  std::uint64_t ignored_ = 0;
  std::uint64_t stolen_note_offs_ = 0;
  std::uint64_t dropped_deferred_ = 0;
  bool bound_ = false;

  PartRouter router_;
  SnapshotExchange exchange_;
  Mixer mixer_{exchange_};
  TransportClock clock_;
  std::atomic<ClockSource> clock_source_{ClockSource::kInternal};
  ClockFollower follower_;
  LastValueTable last_;
  const Snapshot* held_ = nullptr;
  std::array<float, params::kPerfSlotCount> applied_init_{};  // RT-owned: perf_init last applied
  bool applied_valid_ = false;
  std::uint64_t applied_program_generation_ = 0;  // RT-owned: the program_generation last adopted (Story 4.4)
  std::uint64_t applied_reservation_epoch_ = 0;   // RT-owned: the Snapshot::reservation_epoch last committed to the arbiter (Story 4.5)
  std::array<std::uint32_t, 128> keyboard_cc_{};  // RT-owned: the last value of every controller the keyboard sent in a Live Set generation (32 bit)
  std::array<bool, 128> keyboard_cc_seen_{};
  std::uint32_t keyboard_pitch_bend_ = Ump::kPitchBendCentre32;  // RT-owned: the keyboard's last pitch bend in a Live Set generation (replayed like the controllers)
  bool keyboard_pitch_seen_ = false;
  std::array<bool, params::kPartCount> replay_listening_{};    // RT-owned: parts that listened to the keyboard at the last replay decision
  std::array<std::uint64_t, params::kPartCount> replay_bank_{};  // ... and the SampleBank::id they had
  // Story 8.8: physical input activity (group 0 note on/off, CC 64), published once per block through LastValueTable::store_input_activity().
  std::array<std::uint8_t, 16 * 128> input_held_{};
  std::uint32_t input_held_count_ = 0;
  bool input_pedal_down_ = false;
  bool keyboard_pedal_down_ = false;              // RT-owned: the last keyboard sustain (CC 64) state seen in a Live Set generation
  // Per (channel, note): bit i set = this key holds one reference on ledger slot i (a generation).
  std::array<std::uint32_t, 16 * 128> gen_mask_{};
  std::array<std::uint8_t, 16 * 128> present_{};  // scratch for reap_generation_refs()
  std::uint32_t ref_keys_ = 0;                     // keys with a non-zero mask
  std::atomic<std::uint64_t> param_applied_{0}, param_rejected_{0};
  std::atomic<std::uint64_t> panel_controls_{0};
  std::array<std::atomic<std::uint16_t>, 256> panel_value_{};
  std::atomic<std::uint64_t> aux_folded_{0}, rejected_outputs_{0};
  std::array<float, 2 * 64> fold_{};  // scratch: Aux bus of the single-output overload (64 = kBlockFrames)
  std::atomic<std::uint64_t> contact_routed_{0}, contact_dropped_{0};

  struct DirectPartNote {
    std::size_t seq = 0;
    std::uint32_t frame = 0;
    std::uint32_t engine = 0;
    std::uint16_t velocity16 = 0;
    std::uint8_t part = 0;
    std::uint8_t note = 0;
    bool on = false;
  };
  static constexpr std::size_t kDirectPartNotesCapacity = kPendingCapacity * 2;
  std::array<Arpeggiator, params::kPartCount> arps_{};
  std::array<EventProcessor*, params::kPartCount> custom_processors_{};
  std::array<EventBuffer, params::kPartCount> part_in_{};
  std::array<EventBuffer, params::kPartCount> part_out_{};
  std::array<DirectPartNote, kDirectPartNotesCapacity> direct_part_notes_{};
  std::size_t direct_part_notes_count_ = 0;
  std::array<std::uint32_t, 16 * 128> note_engine_{};
  std::uint32_t current_in_block_frame_ = 0;
  NoteObserver note_observer_ = nullptr;
  void* note_observer_ctx_ = nullptr;
};

}  // namespace velora::core
