#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "smf.hpp"
#include "wav.hpp"

namespace velora::offline {

struct RenderOptions {
  std::uint32_t voices = 32;
  double tail_seconds = 1.0;
  float detune_cents = 0.0f;  // test-only: used to produce a deliberately wrong render
};

// Drives velora::core::BlockRenderer block by block. Each event is submitted in the block that
// contains its frame, as an in-block offset (frame % 64). Output is a pure function of the inputs.
bool render_events(const std::vector<NoteEvent>& events, const RenderOptions& opt, WavData& out,
                   std::string& error);

}  // namespace velora::offline
