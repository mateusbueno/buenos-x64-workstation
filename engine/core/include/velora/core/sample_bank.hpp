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
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "velora/core/sample_filter.hpp"
#include "velora/ports/sample_source.hpp"

namespace velora::core {

struct SampleBankRegion {
  velora::ports::SampleRegionDesc d;
  const float* left = nullptr;
  const float* right = nullptr;
  std::uint64_t frames = 0;
  bool stereo = false;
  bool playable = false;
  // Story 13.3. disabled: `end=0` (the region never plays and is not in the per-note lookup). end_cut: min(end, sample length) in frames when the region has an
  // `end`, else -1; the last played frame is end_cut - 1.
  bool disabled = false;
  std::int64_t end_cut = -1;
  // Story 13.4: the note-on amplitude map. vel_shaped: amp_veltrack other than 100 or an amp_velcurve_N table (else the old velocity-squared expression is
  // used untouched); vel_table indexes SampleBank::vel_tables (-1 = none); amp_veltrack is the normalized (percent / 100) value; key_gain_db / key_center are
  // amp_keytrack / amp_keycenter; vel2: any ampeg_vel2* is non-zero.
  bool vel_shaped = false, vel2 = false;
  std::int32_t vel_table = -1;
  float amp_veltrack = 1.0f, key_gain_db = 0.0f;
  std::uint8_t key_center = 60;
  // Story 13.5: the region has CC gates (cc_gate), CC modulation entries (cc_mod) and, among them, entries on the amplitude envelope (eg_cc) or the pseudo-CCs
  // 135 / 136 (a bank with one draws two extra random numbers per note on, see SampleBank::uses_cc_random).
  bool cc_gate = false, cc_mod = false, eg_cc = false;
  // Story 13.6: the exponential release rate of an off_mode=time cut (off_time seconds, at least sfizz's 6 ms); unused for the other modes.
  float off_rate = 0.0f;
  // Story 13.7. bends: the region follows the pitch wheel (SampleRegionDesc::bend_enabled and sane). bend_smooth_frames: the ramp length in engine frames (sfizz
  // LinearSmoother: below 2 = at once). bend_fac_max: the largest pitch factor the wheel can produce (bounds the stream window). cc_trigger / no_note: the region
  // starts voices from its controller triggers / never from a note.
  bool bends = false, cc_trigger = false, no_note = false;
  std::int32_t bend_smooth_frames = 0;
  float bend_fac_max = 1.0f;
  // Story 13.9. modded: the region has an amplitude / pitch / filter LFO or a pitch / filter EG (the voice then runs the modulation path per frame); lfo_a, lfo_p, lfo_f,
  // eg_p, eg_f say which; cutmod: the first filter's cutoff follows lfo_f / eg_f (coefficients recomputed every 16 frames, smoothed per frame); eq_on: at least one EQ band
  // can change the sound (a band at gain 0 with no velocity or controller gain is transparent and skipped); mod_fac_max: the largest pitch factor the pitch LFO and EG can
  // produce (bounds the stream window).
  bool modded = false, lfo_a = false, lfo_p = false, lfo_f = false, eg_p = false, eg_f = false, cutmod = false, eq_on = false;
  float mod_fac_max = 1.0f;
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
  // Story 5.2. Keyswitch gating (any sw_last/sw_down/sw_up/sw_previous), CC crossfades, filter (coefficients fixed at prepare time:
  // the cutoff has no modulation in the subset, so every voice of the region starts with these).
  bool has_keyswitch = false;
  bool has_xfade = false;
  bool filtered = false;
  SampleFilterCoeffs filter;
  // Story 13.8: a tuned first filter (key track, velocity track, CC entries) or a second filter. A voice of the region copies the coefficients above (and `filter2`)
  // when no tuning exists (the Story 5.2 numbers, bit for bit) and computes its own at start otherwise. filter2_on: the second filter exists.
  bool filter_dyn = false, filter2_on = false, filter2_dyn = false;
  SampleFilterCoeffs filter2;
  // Story 6.5: the damage map of the region's sample (SamplePcm::damaged_blocks), nullptr for every unprotected sample. Read only, with relaxed loads;
  // set by non-real-time threads that found a block of a protected library damaged.
  const std::atomic<std::uint8_t>* damage = nullptr;
  std::uint32_t damage_block_frames = 0, damage_block_count = 0;
  // Story 1.16: the sample has a tail that is not resident (only a head is). Reaching the head end is a stream underrun (see SampleEngine). total_frames is the stored length.
  bool has_tail = false;
  std::uint64_t total_frames = 0;
  // Story 1.17 (streaming): the reader of the tail and the resident loop segments of the sample (borrowed from the source the bank's `owner` keeps alive). A voice of a
  // region with `tail_source` may stream the tail when the engine has a StreamService. loop_seek: for a loop on a sample with a tail, the first frame after the
  // resident run (head, then segments) that starts at the loop start: where the stream re-seeks when the voice wraps (-1 = none / not needed).
  const velora::ports::BlockSource* tail_source = nullptr;
  std::uint32_t tail_sample = 0;
  const velora::ports::SamplePcmSegment* segs = nullptr;
  std::uint32_t seg_count = 0;
  std::int64_t loop_seek = -1;
  std::int32_t damage_shift = -1;  // log2(damage_block_frames) when it is a power of two (the block index is then a shift), else -1 (a division)
};

struct SampleBank {
  // Identity token, unique per prepare() for the life of the process (Story 5.2): pointer equality can alias a freed and re-allocated
  // bank, this cannot. Parts compare it to tell "the same instrument again" from "another instrument".
  std::uint64_t id = 0;
  std::vector<SampleBankRegion> regions;
  // Story 13.5: the curve tables of the CC modulation entries (128 entries, sfizz Curve), built at prepare; cc_curve_slot[c] indexes it for SFZ curve index c
  // (-1 = not used by any region). cc_defaults: the controller values a part starts with when it binds this bank (set_cc / set_hdcc).
  std::vector<std::array<float, 128>> cc_curves;
  std::array<std::int16_t, 256> cc_curve_slot = [] { std::array<std::int16_t, 256> a{}; a.fill(-1); return a; }();
  std::vector<velora::ports::SampleCcDefault> cc_defaults;
  bool uses_cc_random = false;
  // Story 13.6: some region has an off_by or a polyphony limit, so every voice start of this bank runs the choke / limit checks (a bank without any skips them
  // entirely and renders exactly as before).
  bool uses_voice_groups = false;
  // Story 13.7: some region follows the pitch wheel / is started by a controller. A bank with neither ignores the wheel and runs no controller trigger code.
  // cct_list: region indices grouped by trigger controller (a region with several triggers appears once per controller): controller c uses cct_list[cct_begin[c],
  // cct_begin[c+1]).
  bool uses_bend = false, uses_cc_triggers = false;
  std::array<std::uint32_t, 129> cct_begin{};
  std::vector<std::uint32_t> cct_list;
  std::vector<std::array<float, 128>> vel_tables;  // Story 13.4: the amp_velcurve_N curves, one table of 128 entries per region that has points
  std::vector<std::uint32_t> note_list;         // region indices, grouped by note
  std::array<std::uint32_t, 129> note_begin{};  // note n uses note_list[note_begin[n], note_begin[n+1])
  std::vector<std::uint32_t> seq_counter;       // audio thread only
  std::vector<std::uint8_t> seq_switched;       // audio thread only
  std::uint32_t unplayable_regions = 0;
  // Story 5.2. switch_key[k] = 1: pressing key k sets the part's "last keyswitch" (k is some region's sw_last or lies inside a
  // sw_lokey..sw_hikey range). default_switch: sw_default of the last region (table order) that has one, -1 = none; a part bound
  // to the bank starts with it as its last keyswitch.
  std::array<std::uint8_t, 128> switch_key{};
  std::int16_t default_switch = -1;
  bool uses_keyswitches = false, uses_filters = false, uses_crossfades = false;
  // Story 13.12: some playable region has a second filter. The Live Set loader raises kCostFeatureSecondFilter in the reservation when a loaded bank has it, so the sample
  // engine declares (and the arbiter budgets) the second filter's cost only for Live Sets that can run one.
  bool uses_second_filter = false;
  std::shared_ptr<const void> owner;            // keeps the PCM source alive
};

}  // namespace velora::core
