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

struct LiveSetRenderOptions {
  std::string live_set_path;
  std::string library_root;
  std::uint32_t part_count = 8;  // the tier
  std::uint64_t seed = 1;        // sample engine RNG seed (round robin / random layers)
  std::uint32_t sample_voices = 340;  // sample engine voice capacity (the 256 voice floor plus the 64 voice fade headroom)
  double tail_seconds = 1.0;
  std::vector<SceneRecallAt> scene_recalls;  // sorted by frame (stable) by render_live_set()
};

struct LiveSetRenderReport {
  std::vector<core::PartState> part_states;  // per part of the Live Set
  std::uint64_t pool_exhausted = 0;
  std::uint64_t dropped_out_of_range = 0;
  std::uint64_t unzoned_keys = 0;
  std::uint64_t scenes_recalled = 0;
  std::uint64_t generations_published = 0;  // by the recalls
};

// False with `error` set when the Live Set does not load (nothing is rendered) or the input is unusable.
bool render_live_set(const std::vector<NoteEvent>& events, const LiveSetRenderOptions& opt, WavData& out, std::string& error,
                     LiveSetRenderReport* report = nullptr);

}  // namespace velora::offline
