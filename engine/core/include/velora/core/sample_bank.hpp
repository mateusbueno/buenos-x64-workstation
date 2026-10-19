#pragma once
// SampleBank (Story 4.3): the prepared, immutable-in-structure form of ONE instrument for the SampleEngine -- the per-region
// constants (pitch ratio, gains, envelope rates, loops) and the per-note region lookup. It is built on the control thread
// (SampleEngine::prepare(), which allocates) and then only READ by the audio thread, so a Live Set can carry one bank per part
// inside its snapshot generation (PartCfg::sample_bank) and the sample engine can switch a part to another instrument at a
// generation change without allocating. The only state that changes at run time is the round-robin counters (seq_counter /
// seq_switched), which belong to the audio thread alone.
//
// Lifetime: the bank keeps the PCM source alive through `owner` (an opaque shared pointer, e.g. host::BundleInstrument), so the
// engine's borrowed spans stay valid for as long as any generation, or any sounding voice's generation, references the bank.
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "velora/ports/sample_source.hpp"

namespace velora::core {

struct SampleBankRegion {
  velora::ports::SampleRegionDesc d;
  const float* left = nullptr;
  const float* right = nullptr;
  std::uint64_t frames = 0;
  bool stereo = false;
  bool playable = false;
  bool uses_sequence = false;
  bool loops = false;  // loop_continuous / loop_sustain with a valid loop range
  std::int64_t loop_start = 0, loop_end = 0, loop_size = 0;
  float speed = 1.0f;       // sample rate / engine rate
  float base_cents = 0.0f;  // key-independent part of the pitch (tune + transpose)
  float vol_gain = 1.0f;
  float chan_l = 1.0f, chan_r = 1.0f;  // pan law and compensation, per output channel
  // Amplitude envelope, converted once (engine rate known at prepare time).
  std::int32_t env_delay = 0, env_hold = 0;
  float env_start = 0.0f, env_attack_step = 1.0f, env_decay_rate = 0.0f, env_sustain = 1.0f, env_release_rate = 0.0f;
  bool env_free_running = false;
};

struct SampleBank {
  std::vector<SampleBankRegion> regions;
  std::vector<std::uint32_t> note_list;         // region indices, grouped by note
  std::array<std::uint32_t, 129> note_begin{};  // note n uses note_list[note_begin[n], note_begin[n+1])
  std::vector<std::uint32_t> seq_counter;       // audio thread only
  std::vector<std::uint8_t> seq_switched;       // audio thread only
  std::uint32_t unplayable_regions = 0;
  std::shared_ptr<const void> owner;            // keeps the PCM source alive
};

}  // namespace velora::core
