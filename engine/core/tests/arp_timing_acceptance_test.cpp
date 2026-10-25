// Story 7.9: arpeggiator timing acceptance (offline suite, NFR7 / FR75 / FR76).
//
// Drives the full BlockRenderer path (snapshot -> UMP queue -> per-part event processor -> note observer) with the built-in
// arpeggiator active on two parts at the same time and compares EVERY note-on / note-off frame with a frame computed here in closed
// form, never by calling the arpeggiator's or the transport clock's scheduling code:
//   * the transport grid (transport_clock.hpp): position in 1/30000-tick units, one frame advances `tenths` units, so the frame of
//     tick t is round_half_up(t * 30000 / tenths) at a constant tempo (tenths = bpm x 10);
//   * rate ticks at 960 PPQN (arpeggiator.hpp): 1/4=960 1/8=480 1/16=240 1/32=120 1/4T=640 1/8T=320 1/16T=160 1/32T=80;
//   * swing (arpeggiator.hpp): step k even stays at k*S; step k odd sits at (k-1)*S + llround(2*S*swing/100);
//   * gate (arpeggiator.hpp): note off = note on + max(1, llround(step_frames * gate/100)), step_frames = distance to the next
//     swung step.
// Tolerance is exactly zero frames; a deviation names part, tempo, rate, swing and step index. Deterministic, no wall clock.
//
// Tempo change mid-run: the clock reads the tempo parameter once at the start of each block, and a parameter event applies inside
// its block, so an event at frame 0 of block K changes the tempo from block K+1. Position stays continuous (units keep accumulating
// at the new multiplier). Expectation: a step fires in the first block in which its swung tick lies inside the block at that block's
// tempo (a tick already in the past by the tempo change fires at block frame 0). That piecewise model is evaluated block by block.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "velora/core/arpeggiator.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;

namespace {

int g_fail = 0;
std::uint64_t g_onsets_checked = 0;
std::uint64_t g_offsets_checked = 0;
std::map<std::int32_t, std::uint64_t> g_ties;  // tempo (tenths) -> note-ons whose exact position is a .5-frame tie

constexpr std::int64_t kUnitsPerTick = 30000;  // 60 s x 48000 Hz x 10 / 960 PPQN (independent restatement of the documented value)

constexpr ArpRate kRates[] = {ArpRate::k1_4, ArpRate::k1_8, ArpRate::k1_16, ArpRate::k1_32,
                              ArpRate::k1_4T, ArpRate::k1_8T, ArpRate::k1_16T, ArpRate::k1_32T};
const char* const kRateNames[] = {"1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T", "1/32T"};
constexpr std::int64_t kRateTicks[] = {960, 480, 240, 120, 640, 320, 160, 80};

constexpr float kSwings[] = {50.0f, 58.3f, 66.7f, 75.0f};

// Restates TransportClock::frame_of_tick (transport_clock.hpp / .cpp): the frame of a tick is round(delta_units / tenths) with ties
// ROUNDING UP (half-up, also for negative values). Half-down or half-even implementations differ on exact .5-frame ties, which occur at
// 80.0 bpm (37.5 frames/tick), 160.0 bpm (18.75, ties at ticks = 2 mod 4) and 240.0 bpm (12.5) for odd tick counts (e.g. swung ticks).
// round_half_up(n / d), d > 0, any sign of n.
std::int64_t round_div(std::int64_t n, std::int64_t d) {
  const std::int64_t num = 2 * n + d, den = 2 * d;
  std::int64_t q = num / den;
  if (num % den != 0 && num < 0) --q;
  return q;
}

std::int64_t swung_tick(std::int64_t k, std::int64_t step_ticks, float swing_pct) {
  if (k % 2 == 0) return k * step_ticks;
  return (k - 1) * step_ticks + std::llround(2.0 * static_cast<double>(step_ticks) * (static_cast<double>(swing_pct) / 100.0));
}

struct PartSpec {
  int rate = 2;  // index into kRates
  float swing = 50.0f;
  std::uint8_t note_a = 60, note_b = 64;
};

struct Expected {
  std::vector<std::uint64_t> on, off;
  std::vector<std::uint8_t> on_note;
};

std::int64_t gate_frames(std::int64_t step_frames) {
  return std::max<std::int64_t>(1, std::llround(static_cast<double>(std::max<std::int64_t>(1, step_frames)) * 0.5));
}

// Constant tempo: pure closed form from absolute tick to absolute frame.
Expected expect_constant(const PartSpec& p, std::int32_t tenths, std::uint64_t total_frames) {
  Expected e;
  const std::int64_t S = kRateTicks[p.rate];
  for (std::int64_t k = 0;; ++k) {
    const std::int64_t on = round_div(swung_tick(k, S, p.swing) * kUnitsPerTick, tenths);
    if (static_cast<std::uint64_t>(on) >= total_frames) break;
    if ((swung_tick(k, S, p.swing) * kUnitsPerTick) % tenths * 2 == tenths) ++g_ties[tenths];
    const std::int64_t next = round_div(swung_tick(k + 1, S, p.swing) * kUnitsPerTick, tenths);
    e.on.push_back(static_cast<std::uint64_t>(on));
    e.on_note.push_back(k % 2 == 0 ? p.note_a : p.note_b);
    e.off.push_back(static_cast<std::uint64_t>(on + gate_frames(next - on)));
  }
  while (!e.off.empty() && e.off.back() >= total_frames) e.off.pop_back();  // off i belongs to on i (gate < step: offs are in order)
  return e;
}

// Tempo schedule: tenths for block b (b <= change_block: t0, later: t1). Piecewise block-by-block model.
Expected expect_piecewise(const PartSpec& p, std::int32_t t0, std::int32_t t1, std::uint32_t change_block, std::uint32_t blocks) {
  Expected e;
  const std::int64_t S = kRateTicks[p.rate];
  std::vector<std::int64_t> units(blocks + 1, 0), tenths(blocks, 0);
  for (std::uint32_t b = 0; b < blocks; ++b) {
    tenths[b] = b <= change_block ? t0 : t1;
    units[b + 1] = units[b] + static_cast<std::int64_t>(kBlockFrames) * tenths[b];
  }
  std::uint32_t b = 0;
  std::size_t triggers = 0;
  for (std::int64_t k = 0;; ++k) {
    const std::int64_t tu = swung_tick(k, S, p.swing) * kUnitsPerTick;
    while (b < blocks && round_div(tu - units[b], tenths[b]) >= static_cast<std::int64_t>(kBlockFrames)) ++b;
    if (b >= blocks) break;
    std::int64_t rel = round_div(tu - units[b], tenths[b]);
    if (rel < 0) {
      // Documented catch-up window (arpeggiator.hpp, "Tempo changes"): a tick that is already past fires at block frame 0 only when it is
      // at most 64 frames (64 x tenths units) before the block start; an older one is dropped (no note, the step is skipped).
      if (tu < units[b] - static_cast<std::int64_t>(kBlockFrames) * tenths[b]) continue;
      rel = 0;
    }
    const std::int64_t next_rel = round_div(swung_tick(k + 1, S, p.swing) * kUnitsPerTick - units[b], tenths[b]);
    const std::int64_t on = static_cast<std::int64_t>(b) * kBlockFrames + rel;
    e.on.push_back(static_cast<std::uint64_t>(on));
    e.on_note.push_back(triggers++ % 2 == 0 ? p.note_a : p.note_b);  // pitch follows the trigger count, parity of k only drives swing
    e.off.push_back(static_cast<std::uint64_t>(on + gate_frames(next_rel - rel)));
  }
  const std::uint64_t total = static_cast<std::uint64_t>(blocks) * kBlockFrames;
  while (!e.off.empty() && e.off.back() >= total) e.off.pop_back();
  return e;
}

struct Trace {
  std::vector<EmittedNoteEvent> ev;
  static void cb(void* ctx, const EmittedNoteEvent& e) noexcept { static_cast<Trace*>(ctx)->ev.push_back(e); }
};

void fail_msg(int part, std::int32_t tenths, const PartSpec& p, const char* what, std::size_t step, std::uint64_t got,
              std::uint64_t want) {
  std::printf("FAIL part=%d tempo=%.1f rate=%s swing=%.1f step=%zu %s: got frame %llu expected %llu (deviation %lld)\n", part,
              tenths / 10.0, kRateNames[p.rate], static_cast<double>(p.swing), step, what,
              static_cast<unsigned long long>(got), static_cast<unsigned long long>(want),
              static_cast<long long>(got) - static_cast<long long>(want));
  ++g_fail;
}

constexpr std::uint16_t kVelocity16 = 40000;  // velocity of every held key; the arpeggiator re-uses it for its note-ons, offs carry 0

void check_parts(std::int32_t tenths, int part_count, const std::vector<EmittedNoteEvent>& all) {
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (all[i].part >= part_count) {
      std::printf("FAIL tempo=%.1f unexpected event from part %d (note %d, %s) at frame %llu; only parts 0..%d are active\n",
                  tenths / 10.0, all[i].part, all[i].note, all[i].on ? "on" : "off", static_cast<unsigned long long>(all[i].frame),
                  part_count - 1);
      ++g_fail;
      return;
    }
  }
}

void compare(int part, std::int32_t tenths, const PartSpec& p, const Expected& ex, const std::vector<EmittedNoteEvent>& all) {
  std::vector<EmittedNoteEvent> on, off;
  for (const auto& e : all) {
    if (e.part != part) continue;
    (e.on ? on : off).push_back(e);
  }
  if (on.size() != ex.on.size()) {
    std::printf("FAIL part=%d tempo=%.1f rate=%s swing=%.1f note-on count %zu expected %zu\n", part, tenths / 10.0,
                kRateNames[p.rate], static_cast<double>(p.swing), on.size(), ex.on.size());
    ++g_fail;
  }
  if (off.size() != ex.off.size()) {
    std::printf("FAIL part=%d tempo=%.1f rate=%s swing=%.1f note-off count %zu expected %zu\n", part, tenths / 10.0,
                kRateNames[p.rate], static_cast<double>(p.swing), off.size(), ex.off.size());
    ++g_fail;
  }
  for (std::size_t i = 0; i < std::min(on.size(), ex.on.size()); ++i) {
    ++g_onsets_checked;
    if (on[i].frame != ex.on[i]) fail_msg(part, tenths, p, "note-on", i, on[i].frame, ex.on[i]);
    if (on[i].velocity16 != kVelocity16) {
      std::printf("FAIL part=%d tempo=%.1f rate=%s swing=%.1f step=%zu note-on velocity %d expected %d\n", part, tenths / 10.0,
                  kRateNames[p.rate], static_cast<double>(p.swing), i, on[i].velocity16, kVelocity16);
      ++g_fail;
    }
    if (on[i].note != ex.on_note[i]) {
      std::printf("FAIL part=%d tempo=%.1f rate=%s swing=%.1f step=%zu pitch %d expected %d\n", part, tenths / 10.0,
                  kRateNames[p.rate], static_cast<double>(p.swing), i, on[i].note, ex.on_note[i]);
      ++g_fail;
    }
  }
  for (std::size_t i = 0; i < std::min(off.size(), ex.off.size()); ++i) {
    ++g_offsets_checked;
    if (off[i].frame != ex.off[i]) fail_msg(part, tenths, p, "note-off", i, off[i].frame, ex.off[i]);
    if (off[i].note != ex.on_note[i] || off[i].velocity16 != 0) {
      std::printf("FAIL part=%d tempo=%.1f rate=%s swing=%.1f step=%zu note-off pitch %d velocity %d expected pitch %d velocity 0\n",
                  part, tenths / 10.0, kRateNames[p.rate], static_cast<double>(p.swing), i, off[i].note, off[i].velocity16,
                  ex.on_note[i]);
      ++g_fail;
    }
  }
}

struct Run {
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>(64);
  Trace trace;
  Snapshot snap = Snapshot::defaults();

  Run(std::int32_t tenths, const PartSpec* parts, int part_count) {
    br->set_note_observer(&Trace::cb, &trace);
    snap.revision = 1;
    snap.perf_init[params::perf_slot(*params::find(params::kTempoBpm), 0)] = static_cast<float>(tenths) / 10.0f;
    for (int i = 0; i < part_count; ++i) {
      auto& a = snap.parts[static_cast<std::size_t>(i)].arp;
      a.enabled = true;
      a.mode = ArpMode::kUp;
      a.rate = kRates[parts[i].rate];
      a.octaves = 1;
      a.gate_percent = 50.0f;
      a.swing_percent = parts[i].swing;
    }
    br->snapshots().publish(&snap);
    for (int i = 0; i < part_count; ++i) {
      br->submit({make_note_on(0, static_cast<std::uint32_t>(i), parts[i].note_a, 40000), 0});
      br->submit({make_note_on(0, static_cast<std::uint32_t>(i), parts[i].note_b, 40000), 0});
    }
  }

  void block() {
    float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
    OutputBuses ob{l, r, al, ar};
    br->process_block(ob);
  }
};

PartSpec part_a(int rate, float swing) { return PartSpec{rate, swing, 60, 64}; }
PartSpec part_b(int rate, float swing) { return PartSpec{rate, swing, 48, 55}; }

// Two simultaneous parts (A at rate r / swing s; B at a different rate and swing), constant tempo.
void run_constant_case(std::int32_t tenths, const PartSpec& a, const PartSpec& b, std::uint32_t blocks) {
  PartSpec specs[2] = {a, b};
  Run run(tenths, specs, 2);
  for (std::uint32_t i = 0; i < blocks; ++i) {
    run.block();
    if (i == 0 && run.br->clock().tempo_tenths() != tenths) {
      std::printf("FAIL tempo=%d clock tempo_tenths %d\n", tenths, run.br->clock().tempo_tenths());
      ++g_fail;
    }
  }
  const std::uint64_t total = static_cast<std::uint64_t>(blocks) * kBlockFrames;
  check_parts(tenths, 2, run.trace.ev);
  compare(0, tenths, a, expect_constant(a, tenths, total), run.trace.ev);
  compare(1, tenths, b, expect_constant(b, tenths, total), run.trace.ev);
}

void test_matrix() {
  // 72, 120, 174, 97.3, 133.7 bpm (the last three: fractional frames/tick) and 80, 160, 240 bpm (exact .5-frame ties, see round_div)
  const std::int32_t tempos[] = {720, 1200, 1740, 973, 1337, 800, 1600, 2400};
  for (std::int32_t t : tempos) {
    for (int r = 0; r < 8; ++r) {
      for (int s = 0; s < 4; ++s) {
        // Part B always runs another rate and another swing than part A; both must be unaffected by the other.
        run_constant_case(t, part_a(r, kSwings[s]), part_b((r + 3) % 8, kSwings[(s + 1) % 4]), 2500);
      }
    }
  }
}

// Part alone vs part next to a busy neighbour: the onsets of A are identical (independence), checked through the same closed form.
void test_independence() {
  const std::int32_t t = 1337;
  const PartSpec a = part_a(6, 66.7f);
  run_constant_case(t, a, part_b(0, 50.0f), 4000);
  run_constant_case(t, a, part_b(7, 75.0f), 4000);
  PartSpec solo[1] = {a};
  Run run(t, solo, 1);
  for (int i = 0; i < 4000; ++i) run.block();
  check_parts(t, 1, run.trace.ev);
  compare(0, t, a, expect_constant(a, t, 4000ull * kBlockFrames), run.trace.ev);
}

void test_tempo_change() {
  const std::int32_t t0s[] = {1200, 973, 1740};
  const std::int32_t t1s[] = {1500, 720, 1337};
  for (int c = 0; c < 3; ++c) {
    for (int r = 0; r < 8; r += 3) {
      const PartSpec a = part_a(r, kSwings[c + 1]);
      const PartSpec b = part_b((r + 5) % 8, kSwings[(c + 2) % 4]);
      PartSpec specs[2] = {a, b};
      constexpr std::uint32_t blocks = 3000, change_block = 1000;
      Run run(t0s[c], specs, 2);
      const params::ParamDef& tempo = *params::find(params::kTempoBpm);
      for (std::uint32_t i = 0; i < blocks; ++i) {
        if (i == change_block) {
          run.br->submit({make_param_event(tempo, 0, static_cast<double>(t1s[c]) / 10.0), 0});
        }
        run.block();
        // The clock reports the tempo of the block just rendered: old up to and including the event's block, new after it.
        if (run.br->clock().tempo_tenths() != (i <= change_block ? t0s[c] : t1s[c])) {
          std::printf("FAIL tempo change: block %u rendered at tempo_tenths %d\n", i, run.br->clock().tempo_tenths());
          ++g_fail;
        }
      }
      check_parts(t0s[c], 2, run.trace.ev);
      compare(0, t0s[c], a, expect_piecewise(a, t0s[c], t1s[c], change_block, blocks), run.trace.ev);
      compare(1, t0s[c], b, expect_piecewise(b, t0s[c], t1s[c], change_block, blocks), run.trace.ev);
    }
  }
}

// 10 simulated minutes at one tempo, two parts, fractional frames per tick: no drift, the final onset deviates by 0 frames.
void test_long_run() {
  const std::int32_t t = 1740;  // 174 bpm
  constexpr std::uint32_t blocks = 10u * 60u * 48000u / kBlockFrames;  // 450000 blocks = 28.8 M frames
  const PartSpec a = part_a(7, 66.7f);   // 1/32T
  const PartSpec b = part_b(2, 58.3f);   // 1/16
  PartSpec specs[2] = {a, b};
  Run run(t, specs, 2);
  for (std::uint32_t i = 0; i < blocks; ++i) run.block();
  const std::uint64_t total = static_cast<std::uint64_t>(blocks) * kBlockFrames;
  check_parts(t, 2, run.trace.ev);
  compare(0, t, a, expect_constant(a, t, total), run.trace.ev);
  compare(1, t, b, expect_constant(b, t, total), run.trace.ev);
  // Explicit end-of-run deviation per part (last onset).
  for (int part = 0; part < 2; ++part) {
    const PartSpec& p = part == 0 ? a : b;
    const Expected ex = expect_constant(p, t, total);
    std::uint64_t last = 0;
    for (const auto& e : run.trace.ev)
      if (e.part == part && e.on) last = e.frame;
    if (ex.on.empty() || last != ex.on.back()) {
      std::printf("FAIL long run part=%d last onset %llu expected %llu\n", part, static_cast<unsigned long long>(last),
                  ex.on.empty() ? 0ull : static_cast<unsigned long long>(ex.on.back()));
      ++g_fail;
    }
  }
}

// Onsets exactly on block edges, asserted per case with literal expected values (frame = block * 64 + offset), part A next to a busy part B.
struct BoundaryCase {
  std::int32_t tenths;
  int rate;
  float swing;
  std::size_t step;
  std::uint64_t frame;
  std::uint32_t block, offset;
};

void test_block_boundaries() {
  const BoundaryCase cases[] = {
      {1200, 0, 50.0f, 1, 24000, 375, 0},   // 1/4 at 120 bpm: second onset exactly at the start of block 375
      {973, 5, 58.3f, 9, 90432, 1413, 0},   // 1/8T swung, fractional tempo: first frame of block 1413
      {800, 7, 58.3f, 5, 15488, 242, 0},    // 80 bpm (tie tempo): first frame of block 242
      {1740, 3, 50.0f, 3, 6207, 96, 63},    // 1/32 at 174 bpm: last frame of block 96
      {1337, 7, 58.3f, 9, 16447, 256, 63},  // 1/32T swung at 133.7 bpm: last frame of block 256
  };
  for (const BoundaryCase& c : cases) {
    const PartSpec specs[2] = {part_a(c.rate, c.swing), part_b((c.rate + 3) % 8, 50.0f)};
    Run run(c.tenths, specs, 2);
    for (std::uint32_t i = 0; i < c.block + 2; ++i) run.block();
    check_parts(c.tenths, 2, run.trace.ev);
    std::size_t idx = 0;
    bool found = false;
    for (const auto& e : run.trace.ev) {
      if (e.part != 0 || !e.on) continue;
      if (idx++ == c.step) {
        found = true;
        if (e.frame != c.frame || e.frame / kBlockFrames != c.block || e.frame % kBlockFrames != c.offset) {
          std::printf("FAIL boundary part=0 tempo=%.1f rate=%s swing=%.1f step=%zu: frame %llu (block %llu offset %llu) expected %llu (block %u offset %u)\n",
                      c.tenths / 10.0, kRateNames[c.rate], static_cast<double>(c.swing), c.step,
                      static_cast<unsigned long long>(e.frame), static_cast<unsigned long long>(e.frame / kBlockFrames),
                      static_cast<unsigned long long>(e.frame % kBlockFrames), static_cast<unsigned long long>(c.frame), c.block, c.offset);
          ++g_fail;
        }
        break;
      }
    }
    if (!found) {
      std::printf("FAIL boundary tempo=%.1f rate=%s step=%zu: onset missing\n", c.tenths / 10.0, kRateNames[c.rate], c.step);
      ++g_fail;
    }
  }
}

void test_tie_coverage() {
  for (std::int32_t t : {800, 1600, 2400}) {
    if (g_ties[t] == 0) {
      std::printf("FAIL tempo=%.1f: no .5-frame tie onset was exercised\n", t / 10.0);
      ++g_fail;
    }
  }
}

}  // namespace

int main() {
  test_matrix();
  test_independence();
  test_tempo_change();
  test_long_run();
  test_block_boundaries();
  test_tie_coverage();
  std::printf("arp_timing_acceptance: %llu onsets and %llu offsets compared, %d failure(s)\n",
              static_cast<unsigned long long>(g_onsets_checked), static_cast<unsigned long long>(g_offsets_checked), g_fail);
  return g_fail == 0 ? 0 : 1;
}
