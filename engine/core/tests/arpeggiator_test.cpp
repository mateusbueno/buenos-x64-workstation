// Story 7.2 / 7.3: Event-processor framework, arpeggiator note generation, swing, latch, sustain pedal (CC 64), and
// seamless scene / Live Set switching while an arpeggiator is running.
// Covers EventBuffer and custom EventProcessor slots, all ArpMode values (Up, Down, UpDown without repeating end notes,
// per-part seeded Random, AsPlayed), Octaves 1-4, all 8 rates (1/4..1/32 and triplets), Gate % (incl. 1-frame floor and >=100%
// re-strike balance), Swing % (50..75%), Latch On/Off and chord replacement, Sustain pedal (CC 64) with Latch Off, exact
// TransportClock grid alignment, Arp Off pass-through, live Arp On<->Off transitions while holding notes, and seamless
// scene / Live Set switching with running arpeggiators.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "velora/core/arpeggiator.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/event_processor.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c)                                                        \
  do {                                                                  \
    if (!(c)) {                                                         \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                         \
    }                                                                   \
  } while (0)

namespace {

struct NoteTrace {
  std::vector<EmittedNoteEvent> events;
  static void callback(void* ctx, const EmittedNoteEvent& ev) noexcept {
    static_cast<NoteTrace*>(ctx)->events.push_back(ev);
  }
};

struct Rig {
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>(64);
  NoteTrace trace;

  Rig() { br->set_note_observer(&NoteTrace::callback, &trace); }

  void run(std::uint32_t blocks) {
    float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
    for (std::uint32_t b = 0; b < blocks; ++b) {
      OutputBuses ob{l, r, al, ar};
      br->process_block(ob);
    }
  }
};

void test_event_buffer_and_custom_processor() {
  EventBuffer buf;
  CHECK(buf.empty() && buf.size() == 0);
  CHECK(buf.push(make_note_on(0, 0, 64, 30000), 20));
  CHECK(buf.push(make_note_on(0, 0, 60, 30000), 5));
  CHECK(buf.push(make_note_on(0, 0, 67, 30000), 20));  // tie at frame 20 stays after 64
  CHECK(buf.size() == 3);
  CHECK(buf[0].target_frame == 5 && buf[0].ump.note() == 60);
  CHECK(buf[1].target_frame == 20 && buf[1].ump.note() == 64);
  CHECK(buf[2].target_frame == 20 && buf[2].ump.note() == 67);

  // Custom EventProcessor that shifts every note up by 7 semitones and delays it by 3 frames inside the block.
  class FifthDelayProcessor final : public EventProcessor {
   public:
    void reset() noexcept VELORA_NONBLOCKING override { resets++; }
    void process_input(const Ump& ump, std::uint32_t frame, const TransportClock&,
                       EventBuffer& out) noexcept VELORA_NONBLOCKING override {
      const std::uint32_t f = std::min<std::uint32_t>(frame + 3, kBlockFrames - 1);
      if (ump.is_note_on()) {
        out.push(make_note_on(0, ump.channel(), std::min<std::uint32_t>(ump.note() + 7, 127), ump.velocity16()), f);
      } else if (ump.is_note_off()) {
        out.push(make_note_off(0, ump.channel(), std::min<std::uint32_t>(ump.note() + 7, 127), 0), f);
      }
    }
    int resets = 0;
  };

  Rig rig;
  FifthDelayProcessor custom;
  rig.br->set_event_processor(0, &custom);
  CHECK(rig.br->event_processor(0) == &custom);
  CHECK(rig.br->event_processor(1) == &rig.br->arp(1));

  rig.br->submit({make_note_on(0, 0, 60, 40000), 10});
  rig.br->submit({make_note_on(0, 1, 48, 40000), 10});  // part 1 has Arp Off (pass-through)
  rig.br->submit({make_note_off(0, 0, 60, 0), 30});
  rig.br->submit({make_note_off(0, 1, 48, 0), 30});
  rig.run(1);

  CHECK(rig.trace.events.size() == 4);
  if (rig.trace.events.size() == 4) {
    CHECK(rig.trace.events[0].part == 1 && rig.trace.events[0].note == 48 && rig.trace.events[0].on && rig.trace.events[0].frame == 10);
    CHECK(rig.trace.events[1].part == 0 && rig.trace.events[1].note == 67 && rig.trace.events[1].on && rig.trace.events[1].frame == 13);
    CHECK(rig.trace.events[2].part == 1 && rig.trace.events[2].note == 48 && !rig.trace.events[2].on && rig.trace.events[2].frame == 30);
    CHECK(rig.trace.events[3].part == 0 && rig.trace.events[3].note == 67 && !rig.trace.events[3].on && rig.trace.events[3].frame == 33);
  }
}

void test_modes_and_octaves() {
  // 120 BPM: 1/16 = 240 ticks = 6000 frames, 50% gate = 3000 frames.
  auto collect_notes = [](ArpMode mode, std::uint8_t octaves, const std::vector<std::uint8_t>& press_notes,
                          std::uint32_t steps_to_collect, std::uint32_t part = 0) {
    Rig rig;
    ArpConfig cfg;
    cfg.enabled = true;
    cfg.mode = mode;
    cfg.rate = ArpRate::k1_16;
    cfg.octaves = octaves;
    cfg.gate_percent = 50.0f;
    rig.br->set_part_arp(part, cfg);
    for (std::uint8_t n : press_notes) {
      rig.br->submit({make_note_on(0, part, n, 50000), 0});
    }
    const std::uint32_t total_frames = steps_to_collect * 6000;
    const std::uint32_t blocks = (total_frames + kBlockFrames - 1) / kBlockFrames;
    rig.run(blocks);
    std::vector<EmittedNoteEvent> ons;
    std::vector<EmittedNoteEvent> offs;
    for (const auto& ev : rig.trace.events) {
      if (ev.frame < total_frames) {
        if (ev.on) ons.push_back(ev);
        else offs.push_back(ev);
      }
    }
    CHECK(ons.size() == steps_to_collect);
    CHECK(offs.size() == steps_to_collect);
    for (std::uint32_t i = 0; i < steps_to_collect && i < ons.size() && i < offs.size(); ++i) {
      CHECK(ons[i].frame == static_cast<std::uint64_t>(i) * 6000);
      CHECK(offs[i].frame == static_cast<std::uint64_t>(i) * 6000 + 3000);
      CHECK(offs[i].note == ons[i].note);
    }
    std::vector<std::uint8_t> pitches;
    for (const auto& e : ons) pitches.push_back(e.note);
    return pitches;
  };

  // Up mode, 1 octave, pressed out of pitch order (64, 60, 67) -> sorted ascending (60, 64, 67)
  {
    const auto p = collect_notes(ArpMode::kUp, 1, {64, 60, 67}, 6);
    const std::vector<std::uint8_t> exp = {60, 64, 67, 60, 64, 67};
    CHECK(p == exp);
  }
  // Up mode, 4 octaves, clamping > 127 (notes 100, 105 -> oct 0: 100, 105; oct 1: 112, 117; oct 2: 124; oct 3: none)
  {
    const auto p = collect_notes(ArpMode::kUp, 4, {100, 105}, 6);
    const std::vector<std::uint8_t> exp = {100, 105, 112, 117, 124, 100};
    CHECK(p == exp);
  }
  // Down mode, 2 octaves
  {
    const auto p = collect_notes(ArpMode::kDown, 2, {60, 64, 67}, 7);
    const std::vector<std::uint8_t> exp = {79, 76, 72, 67, 64, 60, 79};
    CHECK(p == exp);
  }
  // Up-Down mode: 1 note repeats every step; 2 notes alternate; 3 notes do not repeat end notes
  {
    const auto p1 = collect_notes(ArpMode::kUpDown, 1, {60}, 4);
    const std::vector<std::uint8_t> exp1 = {60, 60, 60, 60};
    CHECK(p1 == exp1);

    const auto p2 = collect_notes(ArpMode::kUpDown, 1, {60, 67}, 5);
    const std::vector<std::uint8_t> exp2 = {60, 67, 60, 67, 60};
    CHECK(p2 == exp2);

    const auto p3 = collect_notes(ArpMode::kUpDown, 1, {60, 64, 67}, 9);
    const std::vector<std::uint8_t> exp3 = {60, 64, 67, 64, 60, 64, 67, 64, 60};
    CHECK(p3 == exp3);

    const auto p3_oct2 = collect_notes(ArpMode::kUpDown, 2, {60, 64, 67}, 11);
    const std::vector<std::uint8_t> exp3_oct2 = {60, 64, 67, 72, 76, 79, 76, 72, 67, 64, 60};
    CHECK(p3_oct2 == exp3_oct2);
  }
  // AsPlayed mode: preserves press order (67, then 60, then 64) across octaves
  {
    const auto p = collect_notes(ArpMode::kAsPlayed, 2, {67, 60, 64}, 7);
    const std::vector<std::uint8_t> exp = {67, 60, 64, 79, 72, 76, 67};
    CHECK(p == exp);
  }
  // Random mode: deterministic per part, reproducible across runs, and part 0 != part 1
  {
    const auto r0_a = collect_notes(ArpMode::kRandom, 2, {60, 64, 67}, 16, 0);
    const auto r0_b = collect_notes(ArpMode::kRandom, 2, {60, 64, 67}, 16, 0);
    const auto r1 = collect_notes(ArpMode::kRandom, 2, {60, 64, 67}, 16, 1);
    CHECK(r0_a == r0_b);
    CHECK(r0_a != r1);

    const std::vector<std::uint8_t> pool = {60, 64, 67, 72, 76, 79};
    std::uint64_t rng = Arpeggiator::initial_rng_state(0);
    for (std::size_t i = 0; i < r0_a.size(); ++i) {
      const std::uint8_t expected = pool[static_cast<std::size_t>(Arpeggiator::step_rng(rng) % pool.size())];
      CHECK(r0_a[i] == expected);
    }
  }
}

void test_rates_tempos_and_grid_alignment() {
  const ArpRate rates[] = {
      ArpRate::k1_4, ArpRate::k1_8, ArpRate::k1_16, ArpRate::k1_32,
      ArpRate::k1_4T, ArpRate::k1_8T, ArpRate::k1_16T, ArpRate::k1_32T,
  };
  const float tempos[] = {120.0f, 97.3f};

  for (float bpm : tempos) {
    for (ArpRate rate : rates) {
      Rig rig;
      Snapshot s = Snapshot::defaults();
      s.revision = 1;
      s.perf_init[params::perf_slot(*params::find(params::kTempoBpm), 0)] = bpm;
      s.parts[0].arp.enabled = true;
      s.parts[0].arp.mode = ArpMode::kUp;
      s.parts[0].arp.rate = rate;
      s.parts[0].arp.octaves = 1;
      s.parts[0].arp.gate_percent = 50.0f;
      rig.br->snapshots().publish(&s);

      rig.br->submit({make_note_on(0, 0, 60, 40000), 0});
      rig.br->submit({make_note_on(0, 0, 64, 40000), 0});

      const std::int64_t step_ticks = arp_rate_ticks(rate);
      TransportClock ref_clock;
      ref_clock.set_tempo_tenths(TransportClock::tenths_from_bpm(bpm));

      rig.run(1500);  // 1500 blocks = 96000 frames = 2.0 s

      std::size_t step = 0;
      for (const auto& ev : rig.trace.events) {
        if (!ev.on) continue;
        const std::int64_t tick = static_cast<std::int64_t>(step) * step_ticks;
        const std::uint64_t expected_frame = static_cast<std::uint64_t>(ref_clock.frame_of_tick(tick));
        CHECK(ev.frame == expected_frame);
        CHECK(ev.note == (step % 2 == 0 ? 60 : 64));
        ++step;
      }
      CHECK(step >= 3);
    }
  }

  // First note pressed mid-block between grid ticks quantizes to the next TransportClock grid tick.
  {
    Rig rig;
    ArpConfig cfg;
    cfg.enabled = true;
    cfg.mode = ArpMode::kUp;
    cfg.rate = ArpRate::k1_16;  // 240 ticks = 6000 frames at 120 BPM
    cfg.octaves = 1;
    cfg.gate_percent = 50.0f;
    rig.br->set_part_arp(0, cfg);

    rig.br->submit({make_note_on(0, 0, 60, 40000), 17});  // after tick 0 (frame 0), so first trigger is tick 240 (frame 6000)
    rig.run(200);  // 12800 frames
    std::vector<std::uint64_t> on_frames;
    for (const auto& ev : rig.trace.events) {
      if (ev.on) on_frames.push_back(ev.frame);
    }
    const std::vector<std::uint64_t> expected = {6000, 12000};
    CHECK(on_frames == expected);
  }
}

void test_gate_floor_and_legato_restrike() {
  // Minimum 1-frame gate: at 300 BPM, 1/32T (80 ticks) is 800 frames; 1% gate = 8 frames; test tiny step / 1% floor.
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.revision = 1;
    s.perf_init[params::perf_slot(*params::find(params::kTempoBpm), 0)] = 300.0f;
    s.parts[0].arp.enabled = true;
    s.parts[0].arp.mode = ArpMode::kUp;
    s.parts[0].arp.rate = ArpRate::k1_32T;
    s.parts[0].arp.octaves = 1;
    s.parts[0].arp.gate_percent = 1.0f;
    rig.br->snapshots().publish(&s);
    rig.br->submit({make_note_on(0, 0, 60, 40000), 0});
    rig.run(30);
    CHECK(rig.trace.events.size() >= 2);
    if (rig.trace.events.size() >= 2) {
      CHECK(rig.trace.events[0].on && rig.trace.events[0].frame == 0);
      CHECK(!rig.trace.events[1].on && rig.trace.events[1].frame >= 1);
    }
  }

  // Gate 150% on a single repeated note: emits NoteOff right before re-striking the same pitch at the next step.
  {
    Rig rig;
    ArpConfig cfg;
    cfg.enabled = true;
    cfg.mode = ArpMode::kUp;
    cfg.rate = ArpRate::k1_16;  // 6000 frames at 120 BPM
    cfg.octaves = 1;
    cfg.gate_percent = 150.0f;  // 9000 frames (> 6000 step)
    rig.br->set_part_arp(0, cfg);
    rig.br->submit({make_note_on(0, 0, 60, 40000), 0});
    rig.run(200);  // 12800 frames

    // At frame 0: NoteOn(60). At frame 6000: NoteOff(60) then NoteOn(60). At frame 12000: NoteOff(60) then NoteOn(60).
    CHECK(rig.trace.events.size() == 5);
    if (rig.trace.events.size() == 5) {
      CHECK(rig.trace.events[0].on && rig.trace.events[0].frame == 0);
      CHECK(!rig.trace.events[1].on && rig.trace.events[1].frame == 6000);
      CHECK(rig.trace.events[2].on && rig.trace.events[2].frame == 6000);
      CHECK(!rig.trace.events[3].on && rig.trace.events[3].frame == 12000);
      CHECK(rig.trace.events[4].on && rig.trace.events[4].frame == 12000);
    }
    CHECK(rig.br->status_count(EngineStatus::kVoiceNotFound) == 0);
  }
}

void test_arp_on_off_transitions_while_holding_notes() {
  Rig rig;
  ArpConfig on_cfg;
  on_cfg.enabled = true;
  on_cfg.mode = ArpMode::kUp;
  on_cfg.rate = ArpRate::k1_16;  // 6000 frames, gate 50% = 3000 frames
  on_cfg.octaves = 1;
  on_cfg.gate_percent = 50.0f;

  ArpConfig off_cfg = on_cfg;
  off_cfg.enabled = false;

  // Start with Arp On and hold chord (60, 64) from frame 0.
  rig.br->set_part_arp(0, on_cfg);
  rig.br->submit({make_note_on(0, 0, 60, 40000), 0});
  rig.br->submit({make_note_on(0, 0, 64, 45000), 0});
  rig.run(20);  // 1280 frames: step 0 NoteOn(60) is currently sounding (gate ends at 3000)
  CHECK(rig.trace.events.size() == 1);
  CHECK(rig.trace.events[0].on && rig.trace.events[0].note == 60 && rig.trace.events[0].frame == 0);

  // Switch Arp Off while (60, 64) are still held: must immediately end arpeggiated note 60 and sound held notes (60, 64).
  rig.br->set_part_arp(0, off_cfg);
  rig.run(1);  // block 20 starts at frame 1280
  CHECK(rig.trace.events.size() == 4);
  if (rig.trace.events.size() == 4) {
    CHECK(!rig.trace.events[1].on && rig.trace.events[1].note == 60 && rig.trace.events[1].frame == 1280);
    CHECK(rig.trace.events[2].on && rig.trace.events[2].note == 60 && rig.trace.events[2].frame == 1280);
    CHECK(rig.trace.events[3].on && rig.trace.events[3].note == 64 && rig.trace.events[3].frame == 1280);
  }
  CHECK(rig.br->test_tone().active_voices() == 2);

  // Switch Arp On again while (60, 64) are still held: ends pass-through notes (60, 64) and resumes arpeggiating on the grid.
  rig.br->set_part_arp(0, on_cfg);
  rig.run(80);  // runs from frame 1344 to 6464 (crosses grid tick 240 at frame 6000)
  CHECK(rig.trace.events.size() == 7);
  if (rig.trace.events.size() == 7) {
    CHECK(!rig.trace.events[4].on && rig.trace.events[4].note == 60 && rig.trace.events[4].frame == 1344);
    CHECK(!rig.trace.events[5].on && rig.trace.events[5].note == 64 && rig.trace.events[5].frame == 1344);
    CHECK(rig.trace.events[6].on && rig.trace.events[6].note == 60 && rig.trace.events[6].frame == 6000);
  }

  // Release both keys: ends the sounding arpeggiated note and leaves 0 active voices.
  rig.br->submit({make_note_off(0, 0, 60, 0), 10});
  rig.br->submit({make_note_off(0, 0, 64, 0), 10});
  rig.run(1);
  CHECK(rig.br->test_tone().active_voices() == 0);
  CHECK(rig.br->status_count(EngineStatus::kVoiceNotFound) == 0);
}

void test_mid_chord_rate_change_and_key_updates() {
  // 1. Changing ArpConfig::rate mid-chord while enabled stays true re-aligns next_grid_tick_ to the new rate's transport grid.
  {
    Rig rig;
    ArpConfig cfg;
    cfg.enabled = true;
    cfg.mode = ArpMode::kUp;
    cfg.rate = ArpRate::k1_4;  // 960 ticks = 24000 frames at 120 BPM
    cfg.octaves = 1;
    cfg.gate_percent = 50.0f;
    rig.br->set_part_arp(0, cfg);
    rig.br->submit({make_note_on(0, 0, 60, 40000), 0});
    rig.br->submit({make_note_on(0, 0, 64, 40000), 0});
    rig.run(20);  // 1280 frames: step 0 triggered at frame 0 (next 1/4 tick would be 960 at frame 24000)

    // Switch rate to 1/16 (240 ticks = 6000 frames): next step must re-align to tick 240 (frame 6000), not wait until 24000.
    cfg.rate = ArpRate::k1_16;
    rig.br->set_part_arp(0, cfg);
    rig.run(180);  // up to frame 12800 (crosses tick 240 = frame 6000 and tick 480 = frame 12000)

    std::vector<std::uint64_t> on_frames;
    for (const auto& ev : rig.trace.events) {
      if (ev.on) on_frames.push_back(ev.frame);
    }
    const std::vector<std::uint64_t> expected = {0, 6000, 12000};
    CHECK(on_frames == expected);
  }

  // 2. Mid-chord note addition, partial key release while other keys remain held, re-striking an already-held key,
  //    and same-frame chord replacement.
  {
    Rig rig;
    ArpConfig cfg;
    cfg.enabled = true;
    cfg.mode = ArpMode::kUp;
    cfg.rate = ArpRate::k1_16;  // 6000 frames
    cfg.octaves = 1;
    cfg.gate_percent = 50.0f;
    rig.br->set_part_arp(0, cfg);

    // Start with (60, 64) at frame 0 -> step 0 at frame 0 plays 60.
    rig.br->submit({make_note_on(0, 0, 60, 40000), 0});
    rig.br->submit({make_note_on(0, 0, 64, 40000), 0});
    rig.run(50);  // 3200 frames

    // Add note 67 mid-chord at frame 3200 -> step 1 at frame 6000 plays 64, step 2 at frame 12000 plays 67.
    rig.br->submit({make_note_on(0, 0, 67, 40000), 0});
    rig.run(150);  // up to frame 12800 (crosses 6000 and 12000)

    // Partially release 60 and 67 at frame 12800 while 64 remains held, and re-strike 64 -> pattern continues without reset.
    rig.br->submit({make_note_off(0, 0, 60, 0), 0});
    rig.br->submit({make_note_off(0, 0, 67, 0), 0});
    rig.br->submit({make_note_on(0, 0, 64, 45000), 0});
    rig.run(100);  // up to frame 19200 (crosses 18000 -> plays 64)

    // Same-frame chord replacement at frame 19200: release 64 and press (72, 76) in the same frame -> restarts at step 0 (72) at frame 24000.
    rig.br->submit({make_note_off(0, 0, 64, 0), 0});
    rig.br->submit({make_note_on(0, 0, 72, 40000), 0});
    rig.br->submit({make_note_on(0, 0, 76, 40000), 0});
    rig.run(100);  // up to frame 25600 (crosses 24000)

    std::vector<std::uint8_t> on_pitches;
    std::vector<std::uint64_t> on_frames;
    for (const auto& ev : rig.trace.events) {
      if (ev.on) {
        on_pitches.push_back(ev.note);
        on_frames.push_back(ev.frame);
      }
    }
    const std::vector<std::uint8_t> exp_pitches = {60, 64, 67, 64, 72};
    const std::vector<std::uint64_t> exp_frames = {0, 6000, 12000, 18000, 24000};
    CHECK(on_pitches == exp_pitches);
    CHECK(on_frames == exp_frames);
  }
}

void test_live_set_router_with_arpeggiator() {
  Rig rig;
  Snapshot s = Snapshot::defaults();
  s.revision = 1;
  s.live_set = true;
  // Part 0: lower split [0..59], +1 octave, Arp Up 1/16
  s.parts[0].low_key = 0;
  s.parts[0].high_key = 59;
  s.parts[0].octave = 1;
  s.parts[0].engine_type = static_cast<std::uint32_t>(EngineTypeId::kTestTone);
  s.parts[0].arp.enabled = true;
  s.parts[0].arp.mode = ArpMode::kUp;
  s.parts[0].arp.rate = ArpRate::k1_16;
  // Part 1: upper split [60..127], Arp Off (pass-through)
  s.parts[1].low_key = 60;
  s.parts[1].high_key = 127;
  s.parts[1].engine_type = static_cast<std::uint32_t>(EngineTypeId::kTestTone);
  s.parts[1].arp.enabled = false;
  for (std::uint32_t p = 2; p < params::kPartCount; ++p) s.parts[p].keyboard_on = 0;
  rig.br->snapshots().publish(&s);

  // Press key 48 (in part 0's zone -> transposed to 60) and key 72 (in part 1's zone -> 72) at frame 0.
  rig.br->submit({make_note_on(0, 0, 48, 40000), 0});
  rig.br->submit({make_note_on(0, 0, 52, 40000), 0});
  rig.br->submit({make_note_on(0, 0, 72, 40000), 0});
  rig.run(100);  // 6400 frames: part 0 plays step 0 (60) at 0, off at 3000, step 1 (64) at 6000; part 1 holds 72 from 0

  std::vector<EmittedNoteEvent> p0, p1;
  for (const auto& ev : rig.trace.events) {
    if (ev.part == 0) p0.push_back(ev);
    if (ev.part == 1) p1.push_back(ev);
  }
  CHECK(p1.size() == 1 && p1[0].on && p1[0].note == 72 && p1[0].frame == 0);
  CHECK(p0.size() == 3);
  if (p0.size() == 3) {
    CHECK(p0[0].on && p0[0].note == 60 && p0[0].frame == 0);
    CHECK(!p0[1].on && p0[1].note == 60 && p0[1].frame == 3000);
    CHECK(p0[2].on && p0[2].note == 64 && p0[2].frame == 6000);
  }

  // Release keys while in the Live Set -> ends sounding notes cleanly with 0 kVoiceNotFound.
  rig.br->submit({make_note_off(0, 0, 48, 0), 10});
  rig.br->submit({make_note_off(0, 0, 52, 0), 10});
  rig.br->submit({make_note_off(0, 0, 72, 0), 10});
  rig.run(1);
  CHECK(rig.br->test_tone().active_voices() == 0);
  CHECK(rig.br->status_count(EngineStatus::kVoiceNotFound) == 0);

  // Press keys again so part 0 is actively sounding an arpeggiated step (at frame 12000) and part 1 is sounding pass-through,
  // then leave the Live Set -> both arp and pass-through voices are released with 0 kVoiceNotFound.
  rig.br->submit({make_note_on(0, 0, 48, 40000), 0});
  rig.br->submit({make_note_on(0, 0, 52, 40000), 0});
  rig.br->submit({make_note_on(0, 0, 72, 40000), 0});
  rig.run(90);
  CHECK(rig.br->test_tone().active_voices() == 2);

  Snapshot plain = Snapshot::defaults();
  plain.revision = 2;
  plain.live_set = false;
  rig.br->snapshots().publish(&plain);
  rig.run(1);
  CHECK(rig.br->test_tone().active_voices() == 0);
  CHECK(rig.br->status_count(EngineStatus::kVoiceNotFound) == 0);
}

void test_swing_timing_and_gate_scaling() {
  // Sanitize clamps swing_percent to [50.0f, 75.0f].
  {
    ArpConfig c;
    c.swing_percent = 10.0f;
    CHECK(sanitize_arp_config(c).swing_percent == 50.0f);
    c.swing_percent = 95.0f;
    CHECK(sanitize_arp_config(c).swing_percent == 75.0f);
    c.swing_percent = 62.5f;
    CHECK(sanitize_arp_config(c).swing_percent == 62.5f);
  }

  // At 120 BPM, 1/16 = 240 ticks = 6000 frames per straight step, 12000 frames per step pair.
  auto run_swing_case = [](float swing_pct, const std::vector<std::uint64_t>& exp_ons,
                           const std::vector<std::uint64_t>& exp_offs) {
    Rig rig;
    ArpConfig cfg;
    cfg.enabled = true;
    cfg.mode = ArpMode::kUp;
    cfg.rate = ArpRate::k1_16;
    cfg.octaves = 1;
    cfg.gate_percent = 50.0f;
    cfg.swing_percent = swing_pct;
    rig.br->set_part_arp(0, cfg);
    rig.br->submit({make_note_on(0, 0, 60, 45000), 0});
    rig.br->submit({make_note_on(0, 0, 64, 45000), 0});
    rig.run(375);  // 24000 frames (4 steps)

    std::vector<std::uint64_t> ons, offs;
    for (const auto& ev : rig.trace.events) {
      if (ev.frame < 24000) {
        if (ev.on) ons.push_back(ev.frame);
        else offs.push_back(ev.frame);
      }
    }
    CHECK(ons == exp_ons);
    CHECK(offs == exp_offs);
  };

  // 50% swing (straight): steps at 0, 6000, 12000, 18000; 50% gate offs at 3000, 9000, 15000, 21000.
  run_swing_case(50.0f, {0, 6000, 12000, 18000}, {3000, 9000, 15000, 21000});

  // 62.5% swing: odd steps at tick 300 (frame 7500) and tick 780 (frame 19500).
  // Even step duration = 7500 -> 50% gate = 3750 (offs at 3750, 15750).
  // Odd step duration = 4500 -> 50% gate = 2250 (offs at 9750, 21750).
  run_swing_case(62.5f, {0, 7500, 12000, 19500}, {3750, 9750, 15750, 21750});

  // 75% swing (dotted-triplet pair): odd steps at tick 360 (frame 9000) and tick 840 (frame 21000).
  // Even step duration = 9000 -> 50% gate = 4500 (offs at 4500, 16500).
  // Odd step duration = 3000 -> 50% gate = 1500 (offs at 10500, 22500).
  run_swing_case(75.0f, {0, 9000, 12000, 21000}, {4500, 10500, 16500, 22500});

  // Non-integer tempo (97.3 BPM) with 66% swing on 1/8 (480 ticks): verify exact TransportClock::frame_of_tick(swung_step_tick).
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.revision = 1;
    s.perf_init[params::perf_slot(*params::find(params::kTempoBpm), 0)] = 97.3f;
    s.parts[0].arp.enabled = true;
    s.parts[0].arp.mode = ArpMode::kUp;
    s.parts[0].arp.rate = ArpRate::k1_8;
    s.parts[0].arp.octaves = 1;
    s.parts[0].arp.gate_percent = 50.0f;
    s.parts[0].arp.swing_percent = 66.0f;
    rig.br->snapshots().publish(&s);
    rig.br->submit({make_note_on(0, 0, 60, 40000), 0});
    rig.br->submit({make_note_on(0, 0, 67, 40000), 0});

    TransportClock ref_clock;
    ref_clock.set_tempo_tenths(TransportClock::tenths_from_bpm(97.3f));
    const std::int64_t step_ticks = arp_rate_ticks(ArpRate::k1_8);

    rig.run(1500);
    std::size_t step = 0;
    for (const auto& ev : rig.trace.events) {
      if (!ev.on) continue;
      const std::int64_t swung_tick =
          Arpeggiator::swung_step_tick(static_cast<std::int64_t>(step) * step_ticks, step_ticks, 66.0f);
      const std::uint64_t expected_frame = static_cast<std::uint64_t>(ref_clock.frame_of_tick(swung_tick));
      CHECK(ev.frame == expected_frame);
      ++step;
    }
    CHECK(step >= 4);
  }

  // Off-zero entry point falling between an unswung odd tick and its swung position:
  // At 120 BPM with 75% swing on 1/16, unswung odd tick 240 is at frame 6000, swung odd tick 360 is at frame 9000.
  // When a note arrives at frame 7000 (after the unswung tick but before the swung tick),
  // first_grid_tick_from_frame selects tick 240, landing its NoteOn exactly at swung frame 9000.
  {
    Rig rig;
    ArpConfig cfg;
    cfg.enabled = true;
    cfg.mode = ArpMode::kUp;
    cfg.rate = ArpRate::k1_16;
    cfg.octaves = 1;
    cfg.gate_percent = 50.0f;
    cfg.swing_percent = 75.0f;
    rig.br->set_part_arp(0, cfg);
    rig.run(109);  // 109 blocks * 64 = 6976 frames
    // Submit note on at frame 7000 (offset = 7000 - 6976 = 24 frames in block 109)
    rig.br->submit({make_note_on(0, 0, 60, 45000), 24});
    rig.run(50);  // up to 159 blocks * 64 = 10176 frames (crosses swung frame 9000)
    std::vector<std::uint64_t> ons;
    for (const auto& ev : rig.trace.events) {
      if (ev.on) ons.push_back(ev.frame);
    }
    CHECK(ons.size() == 1);
    if (!ons.empty()) {
      CHECK(ons[0] == 9000);
    }
  }
}

void test_latch_on_off_and_chord_replacement() {
  Rig rig;
  ArpConfig cfg;
  cfg.enabled = true;
  cfg.mode = ArpMode::kUp;
  cfg.rate = ArpRate::k1_16;  // 6000 frames at 120 BPM
  cfg.octaves = 1;
  cfg.gate_percent = 50.0f;   // 3000 frames
  cfg.latch = true;
  rig.br->set_part_arp(0, cfg);

  // Press chord (60, 64, 67) at frame 0, then release all keys at frame 512.
  rig.br->submit({make_note_on(0, 0, 60, 45000), 0});
  rig.br->submit({make_note_on(0, 0, 64, 45000), 0});
  rig.br->submit({make_note_on(0, 0, 67, 45000), 0});
  rig.run(8);  // 512 frames
  CHECK(rig.br->arp(0).held_note_count() == 3);
  CHECK(rig.br->arp(0).physically_held_note_count() == 3);

  rig.br->submit({make_note_off(0, 0, 60, 0), 0});
  rig.br->submit({make_note_off(0, 0, 64, 0), 0});
  rig.br->submit({make_note_off(0, 0, 67, 0), 0});
  rig.run(304);  // up to frame 19968 (crosses step 0 off at 3000, step 1 at 6000, step 2 at 12000, step 3 at 18000)
  CHECK(rig.br->arp(0).held_note_count() == 3);
  CHECK(rig.br->arp(0).physically_held_note_count() == 0);

  // At frame 19968 (while step 3's note 60 is sounding 18000..21000), press a new chord (72, 76) and release it at frame 20608.
  // Because all physical keys were released, (72, 76) replaces (60, 64, 67) without cutting step 3's gate or double-triggering,
  // and starts the new latched chord at the next grid step (frame 24000).
  rig.br->submit({make_note_on(0, 0, 72, 45000), 0});
  rig.br->submit({make_note_on(0, 0, 76, 45000), 0});
  rig.run(10);  // up to frame 20608
  CHECK(rig.br->arp(0).held_note_count() == 2);
  CHECK(rig.br->arp(0).physically_held_note_count() == 2);

  rig.br->submit({make_note_off(0, 0, 72, 0), 0});
  rig.br->submit({make_note_off(0, 0, 76, 0), 0});
  rig.run(167);  // up to frame 31296 (crosses step 3 off at 21000, step 4 at 24000 -> 72, step 5 at 30000 -> 76)
  CHECK(rig.br->arp(0).held_note_count() == 2);
  CHECK(rig.br->arp(0).physically_held_note_count() == 0);

  // Switch Latch Off at frame 31296 while latched note 76 (started at 30000, gate off at 33000) is playing.
  // Note 76 finishes its gate at 33000, and at the next step boundary (36000) the latched notes are cleared and the pattern stops.
  cfg.latch = false;
  rig.br->set_part_arp(0, cfg);
  rig.run(150);  // up to frame 40896 (well past 36000)
  CHECK(rig.br->arp(0).held_note_count() == 0);
  CHECK(rig.br->test_tone().active_voices() == 0);

  std::vector<std::uint8_t> on_pitches;
  std::vector<std::uint64_t> on_frames;
  std::vector<std::uint64_t> off_frames;
  for (const auto& ev : rig.trace.events) {
    if (ev.on) {
      on_pitches.push_back(ev.note);
      on_frames.push_back(ev.frame);
    } else {
      off_frames.push_back(ev.frame);
    }
  }
  const std::vector<std::uint8_t> exp_pitches = {60, 64, 67, 60, 72, 76};
  const std::vector<std::uint64_t> exp_ons = {0, 6000, 12000, 18000, 24000, 30000};
  const std::vector<std::uint64_t> exp_offs = {3000, 9000, 15000, 21000, 27000, 33000};
  CHECK(on_pitches == exp_pitches);
  CHECK(on_frames == exp_ons);
  CHECK(off_frames == exp_offs);

  // Also test switching Latch Off when gate_percent = 150% (gate would extend to 9000, past the next step at 6000):
  // switching Latch Off at frame 1280 ends the latched note at the next step boundary (frame 6000) and stops the pattern.
  {
    Rig rig2;
    ArpConfig cfg2 = cfg;
    cfg2.latch = true;
    cfg2.gate_percent = 150.0f;  // 9000-frame gate on a 6000-frame step
    rig2.br->set_part_arp(0, cfg2);
    rig2.br->submit({make_note_on(0, 0, 60, 45000), 0});
    rig2.br->submit({make_note_off(0, 0, 60, 0), 10});
    rig2.run(20);  // 1280 frames
    CHECK(rig2.br->test_tone().active_voices() == 1);

    cfg2.latch = false;
    rig2.br->set_part_arp(0, cfg2);
    rig2.run(70);  // up to frame 5760 (still before the next step at 6000): voice is still sounding!
    CHECK(rig2.br->test_tone().active_voices() == 1);

    rig2.run(10);  // crosses frame 6000 (the next step): latched note ends at 6000 and no new step fires!
    CHECK(rig2.br->test_tone().active_voices() == 0);
    CHECK(rig2.br->arp(0).held_note_count() == 0);
    CHECK(rig2.trace.events.size() == 2);
    if (rig2.trace.events.size() == 2) {
      CHECK(rig2.trace.events[0].on && rig2.trace.events[0].frame == 0);
      CHECK(!rig2.trace.events[1].on && rig2.trace.events[1].frame == 6000);
    }
  }
}

void test_sustain_pedal_with_latch_off() {
  Rig rig;
  ArpConfig cfg;
  cfg.enabled = true;
  cfg.mode = ArpMode::kUp;
  cfg.rate = ArpRate::k1_16;  // 6000 frames
  cfg.octaves = 1;
  cfg.gate_percent = 50.0f;   // 3000 frames
  cfg.latch = false;
  rig.br->set_part_arp(0, cfg);

  // Sustain pedal down (CC 64 = 127 -> 0xFFFFFFFFu), press (60, 64) at frame 0, release keys at frame 512.
  rig.br->submit({make_control_change(0, 0, Ump::kControllerSustain, 0xFFFFFFFFu), 0});
  rig.br->submit({make_note_on(0, 0, 60, 45000), 0});
  rig.br->submit({make_note_on(0, 0, 64, 45000), 0});
  rig.run(8);  // 512 frames
  rig.br->submit({make_note_off(0, 0, 60, 0), 0});
  rig.br->submit({make_note_off(0, 0, 64, 0), 0});

  // Run past step 0's gate off (frame 3000) to frame 3200: VoiceArbiter must NOT defer the arpeggiator's step NoteOff!
  rig.run(42);  // total 50 blocks = 3200 frames
  CHECK(rig.br->test_tone().active_voices() == 0);
  CHECK(rig.br->arp(0).held_note_count() == 2);
  CHECK(rig.br->arp(0).physically_held_note_count() == 0);

  // Run to frame 12800 (crosses step 1 at 6000, off at 9000, step 2 at 12000).
  rig.run(150);  // total 200 blocks = 12800 frames (step 2 note 60 is currently sounding 12000..15000)
  CHECK(rig.br->test_tone().active_voices() == 1);

  // Release sustain pedal (CC 64 = 0) at frame 12810: removes unheld notes, ends sounding step note at 12810, stops pattern.
  rig.br->submit({make_control_change(0, 0, Ump::kControllerSustain, 0u), 10});
  rig.run(100);
  CHECK(rig.br->test_tone().active_voices() == 0);
  CHECK(rig.br->arp(0).held_note_count() == 0);

  std::vector<std::uint64_t> ons, offs;
  for (const auto& ev : rig.trace.events) {
    if (ev.on) ons.push_back(ev.frame);
    else offs.push_back(ev.frame);
  }
  const std::vector<std::uint64_t> exp_ons = {0, 6000, 12000};
  const std::vector<std::uint64_t> exp_offs = {3000, 9000, 12810};
  CHECK(ons == exp_ons);
  CHECK(offs == exp_offs);
  CHECK(rig.br->status_count(EngineStatus::kVoiceNotFound) == 0);

  // Live Set sustain pedal test: verify that arps_[p] holds notes on listening parts
  // (router_.pedal_down_target(p)) when live_set = true without VoiceArbiter deferring arp step note-offs.
  {
    Rig rig2;
    Snapshot s = Snapshot::defaults();
    s.revision = 1;
    s.live_set = true;
    s.parts[0].engine_type = static_cast<std::uint32_t>(EngineTypeId::kTestTone);
    s.parts[0].keyboard_on = 1;
    s.parts[0].arp.enabled = true;
    s.parts[0].arp.mode = ArpMode::kUp;
    s.parts[0].arp.rate = ArpRate::k1_16;
    s.parts[0].arp.gate_percent = 50.0f;
    s.parts[0].arp.latch = false;
    for (std::uint32_t p = 1; p < params::kPartCount; ++p) s.parts[p].keyboard_on = 0;
    rig2.br->snapshots().publish(&s);

    // Sustain pedal down on channel 0, press (60, 64) at frame 0, release keys at frame 512.
    rig2.br->submit({make_control_change(0, 0, Ump::kControllerSustain, 0xFFFFFFFFu), 0});
    rig2.br->submit({make_note_on(0, 0, 60, 45000), 0});
    rig2.br->submit({make_note_on(0, 0, 64, 45000), 0});
    rig2.run(8);  // 512 frames
    rig2.br->submit({make_note_off(0, 0, 60, 0), 0});
    rig2.br->submit({make_note_off(0, 0, 64, 0), 0});

    // Run to frame 3200 (past step 0 gate off at 3000): voice is stopped (not deferred by VoiceArbiter).
    rig2.run(42);
    CHECK(rig2.br->test_tone().active_voices() == 0);
    CHECK(rig2.br->arp(0).held_note_count() == 2);
    CHECK(rig2.br->arp(0).physically_held_note_count() == 0);

    // Run past step 1 (6000..9000) into step 2 at 12000.
    rig2.run(150);
    CHECK(rig2.br->test_tone().active_voices() == 1);

    // Release pedal at frame 12810.
    rig2.br->submit({make_control_change(0, 0, Ump::kControllerSustain, 0u), 10});
    rig2.run(100);
    CHECK(rig2.br->test_tone().active_voices() == 0);
    CHECK(rig2.br->arp(0).held_note_count() == 0);
    CHECK(rig2.br->status_count(EngineStatus::kVoiceNotFound) == 0);
  }
}

struct ConstantSampleSource final : velora::ports::SampleSource {
  std::vector<velora::ports::SampleRegionDesc> regions;
  std::vector<velora::ports::SamplePcm> pcm;
  std::vector<float> samples;

  explicit ConstantSampleSource(float level) : samples(4800, level) {
    velora::ports::SamplePcm p;
    p.sample_rate = 48000;
    p.channels = 1;
    p.frames = samples.size();
    p.left = samples.data();
    pcm.push_back(p);

    velora::ports::SampleRegionDesc d;
    d.sample_index = 0;
    d.key_lo = 0;
    d.key_hi = 127;
    d.key_center = 60;
    d.loop_mode = velora::ports::SampleLoopMode::kLoopContinuous;
    regions.push_back(d);
  }

  velora::ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }
};

void test_seamless_scene_and_live_set_switching_with_running_arp() {
  Rig rig;
  SampleEngine::Config scfg;
  scfg.max_voices = 32;
  scfg.interpolation = SampleInterpolation::kLinear;
  SampleEngine sample_engine(scfg);
  CHECK(rig.br->bind_engine(sample_engine));

  auto src_a = std::make_shared<ConstantSampleSource>(0.25f);
  auto src_b = std::make_shared<ConstantSampleSource>(0.75f);
  auto bank_a = sample_engine.prepare(*src_a, src_a);
  auto bank_b = sample_engine.prepare(*src_b, src_b);
  CHECK(bank_a != nullptr && bank_b != nullptr);

  // Generation 1 (Scene / Live Set 1): Part 0 on SampleEngine with bank_a (level 0.25), Arp On, Up, 1/16 (6000 frames), 50% gate (3000 frames).
  Snapshot s1 = Snapshot::defaults();
  s1.revision = 1;
  s1.program_generation = 1;
  s1.ledger_index = 0;
  s1.live_set = true;
  s1.parts[0].engine_type = static_cast<std::uint32_t>(EngineTypeId::kSample);
  s1.parts[0].sample_bank = bank_a;
  s1.parts[0].arp.enabled = true;
  s1.parts[0].arp.mode = ArpMode::kUp;
  s1.parts[0].arp.rate = ArpRate::k1_16;
  s1.parts[0].arp.octaves = 1;
  s1.parts[0].arp.gate_percent = 50.0f;
  for (std::uint32_t p = 1; p < params::kPartCount; ++p) s1.parts[p].keyboard_on = 0;
  rig.br->snapshots().publish(&s1);

  // Press chord (60, 64) at frame 0 -> Step 0 starts note 60 on bank_a at frame 0 with gate ending at frame 3000.
  rig.br->submit({make_note_on(0, 0, 60, 65535), 0});
  rig.br->submit({make_note_on(0, 0, 64, 65535), 0});
  rig.run(20);  // 1280 frames (mid-gate of Step 0)
  CHECK(sample_engine.active_voices() == 1);

  // Generation 2 (Scene / Live Set switch while Arp is running and Step 0 is mid-gate!):
  // Part 0 switches to bank_b (level 0.75), Arp remains On.
  Snapshot s2 = s1;
  s2.revision = 2;
  s2.program_generation = 2;
  s2.ledger_index = 1;
  s2.parts[0].sample_bank = bank_b;
  rig.br->snapshots().publish(&s2);

  // Render from frame 1280 to frame 2944 (26 blocks): Step 0 must keep sounding on bank_a (level ~0.25) and keep s1 referenced!
  float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
  for (std::uint32_t b = 0; b < 26; ++b) {
    OutputBuses ob{l, r, al, ar};
    rig.br->process_block(ob);
    CHECK(std::fabs(l[0] - 0.25f) < 0.02f);
  }
  CHECK(!rig.br->snapshots().safe_to_free(&s1));
  CHECK(sample_engine.active_voices() == 1);

  // Render past frame 3000 (Step 0 gate off) up to frame 5952: Step 0 ends at frame 3000, freeing s1, and Step 1 fires at frame 6000 on bank_b!
  rig.run(47);  // 20 + 26 + 47 = 93 blocks = 5952 frames
  CHECK(sample_engine.active_voices() == 0);
  CHECK(rig.br->snapshots().safe_to_free(&s1));

  // Block 93 (frames 5952..6016) triggers Step 1 (note 64) at exact frame 6000 on bank_b (level ~0.75), with no gap or double trigger!
  rig.run(2);  // up to frame 6080
  {
    OutputBuses ob{l, r, al, ar};
    rig.br->process_block(ob);  // frames 6080..6144
    CHECK(std::fabs(l[0] - 0.75f) < 0.02f);
  }
  CHECK(sample_engine.active_voices() == 1);

  // Generation 3 (Scene / Live Set switch at frame 6144 that turns Arp Off while Step 1's note 64 is mid-gate 6000..9000!):
  // Already-sounding step note 64 must NOT be cut at frame 6144; it must finish its full gate until frame 9000 on bank_b!
  Snapshot s3 = s2;
  s3.revision = 3;
  s3.program_generation = 3;
  s3.ledger_index = 2;
  s3.parts[0].arp.enabled = false;
  rig.br->snapshots().publish(&s3);

  for (std::uint32_t b = 0; b < 40; ++b) {  // frames 6144..8704 (still inside Step 1's 6000..9000 gate)
    OutputBuses ob{l, r, al, ar};
    rig.br->process_block(ob);
    CHECK(std::fabs(l[0] - 0.75f) < 0.02f);
  }
  CHECK(sample_engine.active_voices() == 1);

  // Advance past frame 9000 and the sample voice's release tail (50 more blocks -> frame 11904): Step 1 finishes its gate at frame 9000!
  rig.run(50);
  CHECK(sample_engine.active_voices() == 0);
  CHECK(rig.br->snapshots().safe_to_free(&s2));

  // Release keys 60 and 64 while Arp is Off: no unmatched NoteOff or kVoiceNotFound!
  rig.br->submit({make_note_off(0, 0, 60, 0), 0});
  rig.br->submit({make_note_off(0, 0, 64, 0), 0});
  rig.run(2);
  CHECK(sample_engine.active_voices() == 0);
  CHECK(rig.br->status_count(EngineStatus::kVoiceNotFound) == 0);

  // Regression test (deferred-work Arpeggiator stuck voice):
  // 1) Seamless scene switch disables Arp while a step note with a long gate (longer than 64 blocks) is sounding,
  //    followed by an explicit set_part_arp(off) before the gate expires -> must cut the sounding step note immediately.
  // 2) Note pressed while Arp is Off (passthrough_sounding = true), then Arp is enabled with Latch On and the key is
  //    released BEFORE the next block runs apply_mode_transition(), followed by disabling Arp -> must NOT orphan the
  //    passthrough voice in Held state!
  {
    Rig rig_reg;
    ArpConfig c_on;
    c_on.enabled = true;
    c_on.mode = ArpMode::kUp;
    c_on.rate = ArpRate::k1_4;  // 24000 frames at 120 BPM
    c_on.octaves = 1;
    c_on.gate_percent = 100.0f;  // 24000 frames gate (> 64 blocks = 4096 frames)
    c_on.latch = true;

    Snapshot sa = Snapshot::defaults();
    sa.revision = 1;
    sa.parts[0].arp = c_on;
    rig_reg.br->snapshots().publish(&sa);
    rig_reg.br->submit({make_note_on(0, 0, 60, 50000), 0});
    rig_reg.run(2);  // frame 0 triggers note 60 with off_frame = 24000
    CHECK(rig_reg.br->test_tone().active_voices() == 1);

    // Seamless generation switch turns Arp Off at block 2 (frame 128): step note 60 keeps sounding toward frame 24000.
    Snapshot sb = sa;
    sb.revision = 2;
    sb.parts[0].arp.enabled = false;
    rig_reg.br->snapshots().publish(&sb);
    rig_reg.run(2);
    CHECK(rig_reg.br->test_tone().active_voices() == 1);

    // Explicit non-seamless set_part_arp(off) + key release must immediately stop the still-sounding step note!
    ArpConfig c_off = c_on;
    c_off.enabled = false;
    c_off.latch = false;
    rig_reg.br->set_part_arp(0, c_off);
    rig_reg.br->submit({make_note_off(0, 0, 60, 0), 0});
    rig_reg.run(2);
    CHECK(rig_reg.br->test_tone().active_voices() == 0);

    // Sub-case 2: Passthrough note pressed while Arp is Off, then Arp enabled with Latch On and key released
    // in the same block before apply_mode_transition runs, then Arp disabled.
    rig_reg.br->submit({make_note_on(0, 0, 64, 50000), 0});
    rig_reg.run(1);  // note 64 is passthrough_sounding = true, physically_held = true
    CHECK(rig_reg.br->test_tone().active_voices() == 1);

    rig_reg.br->arp(0).set_config(c_on);
    // Simulate key release while held_[i].passthrough_sounding is still true and Latch is On, then disable Arp:
    rig_reg.br->submit({make_note_off(0, 0, 64, 0), 0});
    rig_reg.run(1);
    rig_reg.br->set_part_arp(0, c_off);
    rig_reg.run(2);
    CHECK(rig_reg.br->test_tone().active_voices() == 0);
  }

  std::vector<EmittedNoteEvent> expected_trace = {
      {0, 0, 60, 65535, true},
      {3000, 0, 60, 0, false},
      {6000, 0, 64, 65535, true},
      {9000, 0, 64, 0, false},
  };
  CHECK(rig.trace.events.size() == expected_trace.size());
  for (std::size_t i = 0; i < std::min(rig.trace.events.size(), expected_trace.size()); ++i) {
    CHECK(rig.trace.events[i].frame == expected_trace[i].frame);
    CHECK(rig.trace.events[i].part == expected_trace[i].part);
    CHECK(rig.trace.events[i].note == expected_trace[i].note);
    CHECK(rig.trace.events[i].on == expected_trace[i].on);
  }
}

}  // namespace

int main() {
  test_event_buffer_and_custom_processor();
  test_modes_and_octaves();
  test_rates_tempos_and_grid_alignment();
  test_gate_floor_and_legato_restrike();
  test_arp_on_off_transitions_while_holding_notes();
  test_mid_chord_rate_change_and_key_updates();
  test_live_set_router_with_arpeggiator();
  test_swing_timing_and_gate_scaling();
  test_latch_on_off_and_chord_replacement();
  test_sustain_pedal_with_latch_off();
  test_seamless_scene_and_live_set_switching_with_running_arp();
  if (g_fail) {
    std::printf("%d failures\n", g_fail);
    return 1;
  }
  std::printf("arpeggiator_tests: ok\n");
  return 0;
}
