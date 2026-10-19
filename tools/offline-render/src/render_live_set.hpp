#pragma once
// Live Set rendering (Story 4.3): `velora-offline-render --live-set FILE --library-root DIR` loads a Live Set through the engine host's
// LiveSetLoader (the same code path as engine-host) into a fresh block renderer with the sample engine and the organ bound, and renders a
// MIDI file through it, block by block. The result is a pure function of the Live Set file, the library content and the MIDI file.
//
// Keyboard convention (engine/core/include/velora/core/part_router.hpp): every note on / off in the MIDI file, on any channel, is a keyboard
// event that the Live Set fans out to its parts by zone; sustain (CC 64) goes to every part; the other controllers that the parameter registry
// maps (drawbars, ...) address the part that is their MIDI channel (0..7), as everywhere else.
#include <cstdint>
#include <string>
#include <vector>

#include "smf.hpp"
#include "velora/core/snapshot.hpp"
#include "wav.hpp"

namespace velora::offline {

// A scene recall at a time (Story 4.4, `--scene-at SECONDS:INDEX`): the recall is made between blocks, at the first block boundary at or
// after `frame` (48 kHz), through the SceneController -- the same code path as a ws.v1 RecallScene. Recalls are applied in order of time.
struct SceneRecallAt {
  std::uint64_t frame = 0;
  std::uint32_t index = 0;
};

// A seamless Live Set switch at a time (Story 4.5, `--switch-at SECONDS:LIVESET_FILE`): at the first block boundary at or after `frame` (48 kHz) the
// render asks the LiveSetSwitcher to jump to the Live Set in `live_set_path` -- the same code path as a ws.v1 JumpToLiveSet. The background load is
// simulated deterministically: the switcher runs without a worker thread and the new generation is published `switch_load_blocks` blocks after the
// jump (LiveSetRenderOptions::switch_load_blocks), so a render is a pure function of its inputs. Held, sustained and releasing notes finish on the old
// Live Set, new notes play on the new one.
struct SwitchAt {
  std::uint64_t frame = 0;
  std::string live_set_path;
};

struct LiveSetRenderOptions {
  std::string live_set_path;
  std::string library_root;
  std::uint32_t part_count = 8;  // the tier
  std::uint64_t seed = 1;        // sample engine RNG seed (round robin / random layers)
  std::uint32_t sample_voices = 340;  // sample engine voice capacity (the 256 voice floor plus the 64 voice fade headroom)
  double tail_seconds = 1.0;
  std::vector<SceneRecallAt> scene_recalls;  // sorted by frame (stable) by render_live_set()
  std::vector<SwitchAt> switches;            // sorted by frame (stable) by render_live_set()
  std::uint32_t switch_load_blocks = 0;      // simulated load time of every switch, in 64-frame blocks
};

struct LiveSetRenderReport {
  std::vector<core::PartState> part_states;  // per part of the Live Set
  std::uint64_t pool_exhausted = 0;
  std::uint64_t dropped_out_of_range = 0;
  std::uint64_t unzoned_keys = 0;
  std::uint64_t scenes_recalled = 0;
  std::uint64_t generations_published = 0;  // by the recalls and the switches
  std::uint64_t switches_published = 0;     // Live Set switches that completed
};

// False with `error` set when the Live Set does not load (nothing is rendered) or the input is unusable.
bool render_live_set(const std::vector<NoteEvent>& events, const LiveSetRenderOptions& opt, WavData& out, std::string& error,
                     LiveSetRenderReport* report = nullptr);

}  // namespace velora::offline
