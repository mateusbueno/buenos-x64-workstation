// 10,000 blocks of organ note traffic, drawbar changes (UMP param events and CC) and pedal through the block
// loop with the arbiter reserved, inside a nonblocking function. RTSan aborts on any malloc/lock/blocking call
// in the generator hook, voices, drawbar ramps, param routing and arbiter.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "velora/core/block_renderer.hpp"
#include "velora/core/contact_events.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"

using namespace velora::core;

namespace {

void run_block(BlockRenderer& br, float* l, float* r, std::uint32_t i) noexcept [[clang::nonblocking]] {
  const std::uint32_t part = i % 3;
  const std::uint32_t note = 28 + (i * 7) % 73;
  br.submit({make_note_on(0, part, note, 1000 + (i * 977) % 64000), (i * 5) % 64});
  br.submit({make_note_off(0, part, 28 + ((i + 31) * 7) % 73), (i * 11) % 64});
  if (i % 4 == 0) {  // a drawbar change at a varying in-block offset
    const params::ParamDef* d = params::find(params::kDrawbar16 + i % 9);
    br.submit({make_param_event(*d, part, static_cast<double>((i / 4) % 9)), (i * 13) % 64});
  }
  if (i % 9 == 0) {
    Ump ev;
    if (cc_to_param_event(make_control_change(0, part, 70 + i % 9, 0x10000000u * (i % 16)), ev)) br.submit({ev, 7});
  }
  if (i % 6 == 0) {  // percussion on/off, harmonic, decay, volume and key click (Story 2.3): CC 80, 84, 85, 86, 82
    static constexpr std::uint32_t kPercCc[5] = {80, 84, 85, 86, 82};
    Ump ev;
    const std::uint32_t k = (i / 6) % 5;
    if (cc_to_param_event(make_control_change(0, part, kPercCc[k], ((i / 30) % 3) * 0x7FFFFFFFu), ev)) br.submit({ev, 21});
  }
  if (i % 5 == 0) {  // vibrato/chorus on/off, type and leakage (Story 2.4): CC 81, 87, 83
    static constexpr std::uint32_t kScanCc[3] = {81, 87, 83};
    Ump ev;
    const std::uint32_t k = (i / 5) % 3;
    if (cc_to_param_event(make_control_change(0, part, kScanCc[k], ((i / 15) % 7) * 0x2492492Fu), ev)) br.submit({ev, 33});
  }
  if (i % 3 == 1) {  // contact_k traffic (Story 2.5), valid and malformed (index 0..4, open without close, duplicates)
    const std::uint32_t note = 28 + (i * 5) % 73;
    br.submit({make_contact_event(0, part, note, (i / 3) % 5, ((i / 15) % 2) == 0), (i * 3) % 64});
  }
  if (i % 700 == 0) br.submit({make_control_change(0, 0, 64, 0xFFFFFFFFu), 2});  // pedal down
  if (i % 700 == 350) br.submit({make_control_change(0, 0, 64, 0u), 2});         // pedal up
  if (i % 500 == 0)
    for (std::uint32_t n = 28; n <= 100; ++n) br.submit({make_note_on(0, 3, n, 30000), n % 64});  // full chord burst
  if (i % 500 == 250)
    for (std::uint32_t n = 28; n <= 100; ++n) br.submit({make_note_off(0, 3, n), n % 64});
  br.process_block(l, r);
}

// All scanner settings cycled during a held 73-key chord (Story 2.4): on/off and type through UMP parameter events at
// varying in-block offsets, leakage moving too, many blocks, on a part that has the full chord down.
void run_scanner_block(BlockRenderer& br, float* l, float* r, std::uint32_t i) noexcept [[clang::nonblocking]] {
  const std::uint32_t part = 4;
  if (i % 3 == 0) {  // a new setting every 3 blocks; every 7th one switches the scanner off
    const std::uint32_t k = i / 3;
    br.submit({make_param_event(*params::find(params::kVibratoChorusType), part, static_cast<double>(k % 6)), (i * 5) % 64});
    br.submit({make_param_event(*params::find(params::kVibratoChorusOn), part, k % 7 == 6 ? 0.0 : 1.0), (i * 5 + 17) % 64});
  }
  if (i % 10 == 0) br.submit({make_param_event(*params::find(params::kLeakage), part, static_cast<double>((i / 10) % 5) * 0.25), 9});
  br.process_block(l, r);
}

// Contact traffic (Story 2.5) on part 5: a 73-key sweep where every key's contacts close in order 1, 2, 3 and open 3, 2, 1 across
// 438-block cycles, with Note On/Off following and a few out-of-order events, at varying in-block offsets.
void run_contact_block(BlockRenderer& br, float* l, float* r, std::uint32_t i) noexcept [[clang::nonblocking]] {
  const std::uint32_t part = 5;
  const std::uint32_t note = 28 + i % 73;
  const std::uint32_t phase = (i / 73) % 6;           // 0..2 close 1..3, 3..5 open 3..1
  const std::uint32_t k = phase < 3 ? phase + 1 : 6 - phase;
  br.submit({make_contact_event(0, part, note, k, phase < 3), (i * 7) % 64});
  if (phase == 2) br.submit({make_note_on(0, part, note, 30000), (i * 7) % 64});
  if (phase == 5) br.submit({make_note_off(0, part, note), (i * 7 + 9) % 64});
  if (i % 11 == 0) br.submit({make_contact_event(0, part, note, 1 + i % 3, i % 2 == 0), (i * 5) % 64});  // out of order
  br.process_block(l, r);
}

// Rotary speaker (Story 2.6) on part 6 under a held 73-key chord: the speed cycles stop/slow/fast through UMP parameter events at
// varying in-block offsets (and through CC 79), and the stage is switched on and off now and then, for many blocks.
void run_rotary_block(BlockRenderer& br, float* l, float* r, std::uint32_t i) noexcept [[clang::nonblocking]] {
  const std::uint32_t part = 6;
  if (i % 17 == 0) {
    const std::uint32_t k = i / 17;
    br.submit({make_param_event(*params::find(params::kRotarySpeed), part, static_cast<double>(k % 3)), (i * 7) % 64});
  }
  if (i % 41 == 5) {
    Ump ev;
    if (cc_to_param_event(make_control_change(0, part, 79, ((i / 41) % 3) * 0x7FFFFFFFu), ev)) br.submit({ev, 11});
  }
  if (i % 400 == 399) br.submit({make_param_event(*params::find(params::kRotaryOn), part, (i / 400) % 4 == 3 ? 0.0 : 1.0), (i * 3) % 64});
  br.process_block(l, r);
}

}  // namespace

constexpr float kPartPeak = 1.0f;  // see the bound derivation near the end of main()
constexpr float kPartsRotaryPhase = 7.0f * kPartPeak;

int main() {
  auto organ = std::make_unique<OrganEngine>();
  auto br = std::make_unique<BlockRenderer>(256);
  br->bind_engine(*organ);
  for (std::uint32_t ch = 0; ch < 7; ++ch) br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
  VoiceArbiter::LiveSetConfig cfg;
  cfg.organ_rotary_cost = OrganEngine::reservation_cost_units(7);
  cfg.budget = cfg.organ_rotary_cost + 256;
  int rc = 0;
  if (br->arbiter().reserve(cfg) != VoiceArbiter::ReserveStatus::kOk) {
    std::printf("rtsan_organ_test: reserve failed\n");
    return 1;
  }
  float l[kBlockFrames], r[kBlockFrames];
  double energy = 0.0;
  float peak = 0.0f;
  std::uint32_t peak_voices = 0;
  for (std::uint32_t i = 0; i < 10000; ++i) {
    run_block(*br, l, r, i);
    for (std::uint32_t k = 0; k < kBlockFrames; ++k) {
      energy += static_cast<double>(l[k]) * l[k];
      peak = std::max(peak, std::fabs(l[k]));
      if (!std::isfinite(l[k])) rc = 1;
    }
    peak_voices = std::max(peak_voices, organ->active_voices());
  }
  // Scanner phase: a held 73-key chord on part 4, every setting cycled for 6,000 blocks.
  for (std::uint32_t n = 28; n <= 100; ++n) br->submit({make_note_on(0, 4, n, 30000), n % 64});
  double scan_energy = 0.0;
  for (std::uint32_t i = 0; i < 6000; ++i) {
    run_scanner_block(*br, l, r, i);
    for (std::uint32_t k = 0; k < kBlockFrames; ++k) {
      scan_energy += static_cast<double>(l[k]) * l[k];
      if (!std::isfinite(l[k])) rc = 1;
    }
  }
  if (!(scan_energy > 0.0)) rc = 1;
  // Contact phase: part 5 keys follow multi-contact sequences (valid and malformed) for 6,000 blocks.
  double contact_energy = 0.0;
  for (std::uint32_t i = 0; i < 6000; ++i) {
    run_contact_block(*br, l, r, i);
    for (std::uint32_t k = 0; k < kBlockFrames; ++k) {
      contact_energy += static_cast<double>(l[k]) * l[k];
      if (!std::isfinite(l[k])) rc = 1;
    }
  }
  if (!(contact_energy > 0.0) || organ->contact_events_applied() == 0 || organ->contact_events_ignored() == 0 ||
      br->contact_events_routed() == 0)
    rc = 1;
  // Rotary phase: a held 73-key chord on part 6 with the rotary stage on, speeds cycled for 8,000 blocks.
  br->submit({make_param_event(*params::find(params::kRotaryOn), 6, 1.0), 0});
  for (std::uint32_t n = 28; n <= 100; ++n) br->submit({make_note_on(0, 6, n, 30000), n % 64});
  double rotary_energy = 0.0, rotary_diff = 0.0;
  float rotary_peak = 0.0f;
  for (std::uint32_t i = 0; i < 8000; ++i) {
    run_rotary_block(*br, l, r, i);
    for (std::uint32_t k = 0; k < kBlockFrames; ++k) {
      rotary_energy += static_cast<double>(l[k]) * l[k];
      rotary_diff += std::fabs(static_cast<double>(l[k]) - r[k]);
      rotary_peak = std::max({rotary_peak, std::fabs(l[k]), std::fabs(r[k])});
      if (!std::isfinite(l[k]) || !std::isfinite(r[k])) rc = 1;
    }
  }
  const auto rs = organ->rotary_status(6);
  if (!(rotary_energy > 0.0) || !(rotary_diff > 0.0) || !(rs.horn_hz >= 0.0f) || rotary_peak > kPartsRotaryPhase) rc = 1;
  std::printf("rtsan_organ_test: rotary energy=%.3g stereo diff=%.3g target=%d horn=%.2f Hz\n", rotary_energy, rotary_diff,
              static_cast<int>(rs.target), static_cast<double>(rs.horn_hz));
  std::printf("rtsan_organ_test: contact energy=%.3g applied=%llu ignored=%llu\n", contact_energy,
              (unsigned long long)organ->contact_events_applied(), (unsigned long long)organ->contact_events_ignored());
  std::printf("rtsan_organ_test: energy=%.3g peak=%.3f peak_voices=%u organ_steals=%llu rejected=%llu\n", energy, peak,
              peak_voices, (unsigned long long)br->arbiter().steals(static_cast<std::uint32_t>(EngineTypeId::kOrgan)),
              (unsigned long long)br->param_events_rejected());
  // Bound: every part's output, with or without the rotary stage, ends in the organ's soft limit, so each channel of each part stays
  // at or below 1.0 (rotary_tests measures 1.000 on the loudest 73-key chord through the stage, which is the reason for that limit).
  // The first loop plays parts 0..3 only, so the sum of the parts is at most 4.0 per channel; in the rotary phase all 7 parts may sound.
  constexpr float kParts = 4.0f * kPartPeak;
  if (!(energy > 0.0) || peak > kParts || peak_voices < 73 ||
      br->arbiter().steals(static_cast<std::uint32_t>(EngineTypeId::kOrgan)) != 0)
    rc = 1;
  return rc;
}
