#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "smf.hpp"
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
  };
  bool enabled = false;                               // any routing option given
  std::array<std::uint8_t, 8> output{};               // registry part_output per part: 0 main (default), 1 aux
  std::array<float, 8> send_reverb{}, send_delay{};   // 0..1 per part
  std::array<std::vector<Insert>, 8> inserts;         // up to 5 per part
  float master_level_db = 0.0f;
  float line_in_level = 0.0f;                         // 0..1
  const WavData* line_in = nullptr;                   // stereo or mono, 48 kHz (borrowed); read once, silent after its end
};

enum class EngineKind : std::uint8_t { kTestTone, kSample, kOrgan };

struct RenderOptions {
  EngineKind engine = EngineKind::kTestTone;
  // Sample engine only: the instrument to play (borrowed, must outlive the call), the RNG seed for
  // lorand/hirand and the interpolation quality. `voices` is the engine's voice capacity.
  const velora::ports::SampleSource* instrument = nullptr;
  std::uint64_t seed = 1;
  std::uint32_t oracle_block_frames = 0;  // test only: emulate sfizz's per-voice block-boundary stall (see SampleEngine::Config)
  float gain_db = 0.0f;  // sample engine output gain (the sfizz null tests use sfizz's -7.35 dB default master volume)
  velora::core::SampleInterpolation interpolation = velora::core::SampleInterpolation::kHermite3;
  std::uint32_t voices = 32;
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
