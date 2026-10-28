#pragma once
// Story 13.10: TEST-ONLY random-number replay of the sfizz oracle for the offline renderer (`velora-offline-render --sfizz-random-replay`).
//
// sfizz picks the layer of a `lorand`/`hirand` region with one number per note on, note off and controller event, drawn from a fixed-seed
// Numerical Recipes LCG (Synth.cpp's copy of `Random::randomGenerator`, seed 0, no setter). The sample engine draws from its own seeded
// xorshift64*, once per note on / note off. The two can never agree by seeding alone, so a null test of a random-layer instrument compares
// two different layers. This module replays the oracle's sequence: it computes, from the MIDI events, the number sfizz uses for each note
// event, and the renderer re-seeds the engine (the public `SampleEngine::set_seed`) just before each note event so that the engine's first
// draw of that event is that number (to the 2^-24 grid of the engine's float).
//
// The production random generator is not touched: the engine draws exactly as it always does, the harness only chooses the seed. Nothing here
// is linked into a shipped target (velora_offline is a test tool).
#include <cstdint>
#include <vector>

#include "smf.hpp"

namespace velora::offline {

// sfizz `fast_rand` (MathHelpers.h): mem = mem * 1664525 + 1013904223, seed 0, result_type uint32.
struct SfizzLcg {
  std::uint32_t mem = 0;
  std::uint32_t next() noexcept {
    mem = mem * 1664525u + 1013904223u;
    return mem;
  }
};

// `std::uniform_real_distribution<float>(0, 1)` over the 32-bit LCG, as Synth.cpp's `randNoteDistribution_`: float(u32) / 2^32, 1.0 clamped to the
// largest float below 1. (Both libc++ and libstdc++ compute exactly this for a 32-bit generator; a unit test compares with the platform's own.)
float sfizz_uniform_float(std::uint32_t u32) noexcept;

// For every NOTE event (on or off, in the order of `events`) the number sfizz uses for the layer choice of that event. Controller events
// (other than CC 120 / 121 / 123, which return before the draw) advance the generator without producing an entry; pitch bend and contact events do not draw.
// `skip_draws` discards that many draws first (calibration for a patch that draws while it loads; 0 for every patch checked so far).
std::vector<float> sfizz_note_draws(const std::vector<NoteEvent>& events, std::uint32_t skip_draws = 0);

// The engine seed (SampleEngine::set_seed) after which the engine's FIRST next_random() is floor(u * 2^24) / 2^24. Exact inverse of
// set_seed's scramble and of one xorshift64* step; a first draw of 0 (u < 2^-24) is answered with 1/2^24.
std::uint64_t engine_seed_for_first_draw(float u) noexcept;
// The first draw of an engine seeded with `seed` (forward model of set_seed + next_random, for the tests of the inverse).
float engine_first_draw(std::uint64_t seed) noexcept;

}  // namespace velora::offline
