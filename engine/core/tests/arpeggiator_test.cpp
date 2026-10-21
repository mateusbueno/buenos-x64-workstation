// Story 7.2: Event-processor framework and arpeggiator note generation.
// Covers EventBuffer and custom EventProcessor slots, all ArpMode values (Up, Down, UpDown without repeating end notes,
// per-part seeded Random, AsPlayed), Octaves 1-4, all 8 rates (1/4..1/32 and triplets), Gate % (incl. 1-frame floor and >=100%
// re-strike balance), exact TransportClock grid alignment, Arp Off pass-through, live Arp On<->Off transitions while holding
// notes, and Live Set PartRouter + Arpeggiator integration.
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

}  // namespace

int main() {
  test_event_buffer_and_custom_processor();
  test_modes_and_octaves();
  test_rates_tempos_and_grid_alignment();
  test_gate_floor_and_legato_restrike();
  test_arp_on_off_transitions_while_holding_notes();
  test_mid_chord_rate_change_and_key_updates();
  test_live_set_router_with_arpeggiator();
  if (g_fail) {
    std::printf("%d failures\n", g_fail);
    return 1;
  }
  std::printf("arpeggiator_tests: ok\n");
  return 0;
}
