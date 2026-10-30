#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "smf.hpp"
#include "velora/core/arpeggiator.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/fm_parameters.hpp"
#include "velora/core/ep_parameters.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/ports/sample_source.hpp"
#include "wav.hpp"

namespace velora::offline {

// Routing options (Story 2.7) for the null tests of the fixed topology. Nothing set = no snapshot is published and the render is
// the pre-2.7 one, bit for bit. Parts are the MIDI channels 0..7.
struct RoutingOptions {
  struct Insert {
    std::uint8_t type = 0;  // registry insert_type
    bool bypass = false;
    // The normalised insert parameters (registry insert_param_1..5); unset = the registry defaults. The EQ default is transparent;
    // the compressor default works on the signal (set insert_param_2 = 0 for ratio 1:1).
    bool has_params = false;
    std::array<float, 5> params{0.5f, 0.5f, 0.5f, 0.5f, 0.0f};
  };
  // Master section (Story 2.8), physical units of the master_eq_* / master_comp_* registry parameters. Defaults = registry defaults
  // (EQ flat; compressor ratio 1:1, no makeup: exactly transparent).
  struct MasterEq {
    float low_db = 0.0f, mid_db = 0.0f, mid_hz = 1000.0f, high_db = 0.0f;
    bool bypass = false;
  };
  struct MasterComp {
    float threshold_db = -12.0f, ratio = 1.0f, attack_ms = 10.0f, release_ms = 100.0f, makeup_db = 0.0f;
    bool bypass = false;
  };
  bool enabled = false;                               // any routing option given
  std::array<std::uint8_t, velora::core::params::kPartCount> output{};               // registry part_output per part: 0 main (default), 1 aux
  std::array<float, velora::core::params::kPartCount> send_reverb{}, send_delay{};   // 0..1 per part
  std::array<std::vector<Insert>, velora::core::params::kPartCount> inserts;         // up to 5 per part
  std::array<velora::core::ArpConfig, velora::core::params::kPartCount> arp{};       // per-part arpeggiator config (Story 7.2 / 7.3)
  struct ArpSwitch {
    double seconds = 0.0;
    std::array<velora::core::ArpConfig, velora::core::params::kPartCount> arp{};
  };
  std::vector<ArpSwitch> arp_switches;                // optional mid-render Snapshot switches of per-part ArpConfig (Story 7.3)
  float master_level_db = 0.0f;
  // Reverb (Story 2.10), registry defaults: type 1 (hall), time 2 s, return level 1, REV master send level 1. type: 0 room, 1 hall, 2 plate.
  std::uint8_t reverb_type = 1;
  float reverb_time_s = 2.0f, reverb_level = 1.0f, reverb_master = 1.0f;
  // Delay (Story 2.11), registry defaults: 350 ms, tempo sync off, note 1 (eighth; the registry enum delay_note index), feedback 0.35, return level 1,
  // damping 0.25, ping-pong 0. tempo_bpm is the initial value of the RT plane's tempo (registry default 120); tempo_changes are (seconds, bpm) events
  // sent to the renderer as `tempo_bpm` performance parameter events at that frame (sorted by the renderer).
  float delay_time_ms = 350.0f, delay_feedback = 0.35f, delay_level = 1.0f, delay_damping = 0.25f, delay_pingpong = 0.0f, tempo_bpm = 120.0f;
  bool delay_sync = false;
  std::uint8_t delay_note = 1;
  std::vector<std::pair<double, float>> tempo_changes;
  MasterEq master_eq;
  MasterComp master_comp;
  float line_in_level = 0.0f;                         // 0..1
  const WavData* line_in = nullptr;                   // stereo or mono, 48 kHz (borrowed); read once, silent after its end
};

enum class EngineKind : std::uint8_t { kTestTone, kSample, kOrgan, kFm, kEp };

struct RenderOptions {
  EngineKind engine = EngineKind::kTestTone;
  // EP engine (Story 14.2): voice parameters for modeled electric piano
  velora::core::EpVoicingParameters ep_params{};
  // FM engine only (Story 12.2): voice parameters for 6-operator FM synthesis
  velora::core::FmVoiceParameters fm_params{};
  // Sample engine only: the instrument to play (borrowed, must outlive the call), the RNG seed for
  // lorand/hirand and the interpolation quality. `voices` is the engine's voice capacity.
  const velora::ports::SampleSource* instrument = nullptr;
  std::uint64_t seed = 1;
  std::uint32_t oracle_block_frames = 0;  // test only: emulate sfizz's per-voice block-boundary stall (see SampleEngine::Config)
  // test only (Story 13.10): replay the sfizz oracle's random numbers for the layer choice of lorand/hirand regions (see sfizz_random_replay.hpp). Just before each
  // note event the engine is re-seeded (public SampleEngine::set_seed) so that its first draw is the number sfizz uses for that event; `seed` is ignored. The
  // production generator is unchanged. Not combinable with routing or with `emitted_notes`. `sfizz_replay_skip_draws` discards that many oracle draws first.
  bool sfizz_random_replay = false;
  std::uint32_t sfizz_replay_skip_draws = 0;
  float gain_db = 0.0f;  // sample engine output gain (the sfizz null tests use sfizz's -7.35 dB default master volume)
  velora::core::SampleInterpolation interpolation = velora::core::SampleInterpolation::kHermite3;
  std::uint32_t voices = 32;
  // Sample engine only (Story 1.18): parallel voice render. 0 = off (the serial render, the default), 1 = the three fixed partitions run inline, 3 = three
  // worker threads. 1 and 3 render bit-identically; other values are clamped by the renderer.
  std::uint32_t parallel_voice_threads = 0;
  // Organ engine only: nine drawbar steps 0..8 in drawbar order (16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1'),
  // applied to every organ part (channels 0..7) at frame 0 as performance parameter events.
  std::string drawbars = "008000000";
  // Organ engine only (Story 2.3): percussion and key click applied to every organ part at frame 0 (before any
  // MIDI controller in the file). percussion_on false = percussion off. Values are the registry enum indices
  // (harmonic 0 second / 1 third, decay 0 fast / 1 slow, volume 0 soft / 1 normal); key_click in 0..1.
  bool percussion_on = false;
  int percussion_harmonic = 1, percussion_decay = 0, percussion_volume = 0;
  float key_click = 0.5f;
  // Organ engine only (Story 2.4): vibrato/chorus setting (0 off, 1..6 = V1 V2 V3 C1 C2 C3) and leakage 0..1, applied to
  // every organ part at frame 0 (MIDI CC 81, 83, 87 in the file can change them later). Leakage defaults to the registry value.
  int vibrato = 0;
  float leakage = 0.5f;
  // Organ engine only (Story 2.6): rotary speaker at frame 0 on every part: 0 off (the default), 1 slow, 2 fast, 3 stop. The file's
  // CC 79 (speed: 0-31 stop, 32-95 slow, 96-127 fast) and CC 89 (on/off) change them later; speed changes then ramp.
  int rotary = 0;
  // Organ engine only (Story 2.5): contacts per key of the keybed behind contact_k events (2 or 3).
  unsigned contact_count = 3;
  // Routing (Story 2.7): see RoutingOptions. `aux_out` (optional) receives the Aux bus, same length as the Main output.
  RoutingOptions routing;
  WavData* aux_out = nullptr;
  // Optional trace of every part note event reaching voice allocation (Story 7.2).
  std::vector<velora::core::EmittedNoteEvent>* emitted_notes = nullptr;
  double tail_seconds = 1.0;
  float detune_cents = 0.0f;  // test-only: used to produce a deliberately wrong render
};

// Drives velora::core::BlockRenderer block by block. Each event is submitted in the block that
// contains its frame, as an in-block offset (frame % 64). Output is a pure function of the inputs.
bool render_events(const std::vector<NoteEvent>& events, const RenderOptions& opt, WavData& out,
                   std::string& error);

// Counters of the last sample-engine render, for reports and tests.
struct SampleRenderStats {
  std::uint64_t pool_exhausted = 0;
  std::uint64_t voice_not_found = 0;
  std::uint64_t skipped_unplayable_starts = 0;
  std::uint32_t unplayable_regions = 0;
};
bool render_events(const std::vector<NoteEvent>& events, const RenderOptions& opt, WavData& out,
                   std::string& error, SampleRenderStats* stats);

}  // namespace velora::offline
