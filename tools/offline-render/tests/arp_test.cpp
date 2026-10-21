#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "velora/core/arpeggiator.hpp"
#include "velora/core/transport_clock.hpp"

using namespace velora::core;
using namespace velora::offline;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

namespace {

struct ExpandedPitch {
  std::uint8_t note = 0;
  std::uint16_t velocity16 = 0;
};

// Reference pitch list expansion matching the spec.
std::vector<ExpandedPitch> build_pitch_list(const std::vector<ExpandedPitch>& held_in_press_order,
                                            ArpMode mode,
                                            std::uint8_t octaves) {
  std::vector<ExpandedPitch> base = held_in_press_order;
  if (mode != ArpMode::kAsPlayed) {
    std::sort(base.begin(), base.end(), [](const ExpandedPitch& a, const ExpandedPitch& b) {
      return a.note < b.note;
    });
  }
  std::vector<ExpandedPitch> out;
  for (std::uint8_t o = 0; o < octaves; ++o) {
    const std::uint32_t shift = static_cast<std::uint32_t>(o) * 12u;
    for (const auto& p : base) {
      if (static_cast<std::uint32_t>(p.note) + shift <= 127u) {
        out.push_back({static_cast<std::uint8_t>(p.note + shift), p.velocity16});
      }
    }
  }
  return out;
}

ExpandedPitch expected_step_pitch(const std::vector<ExpandedPitch>& list,
                                  ArpMode mode,
                                  std::uint32_t step_idx,
                                  std::uint64_t& rng_state) {
  const std::size_t m = list.size();
  switch (mode) {
    case ArpMode::kUp:
    case ArpMode::kAsPlayed:
      return list[step_idx % m];
    case ArpMode::kDown:
      return list[(m - 1u) - (step_idx % m)];
    case ArpMode::kUpDown: {
      if (m <= 1u) return list[0];
      const std::size_t cycle = 2u * m - 2u;
      const std::size_t pos = static_cast<std::size_t>(step_idx) % cycle;
      const std::size_t idx = (pos < m) ? pos : (cycle - pos);
      return list[idx];
    }
    case ArpMode::kRandom: {
      const std::size_t idx = static_cast<std::size_t>(Arpeggiator::step_rng(rng_state) % m);
      return list[idx];
    }
  }
  return list[0];
}

// Verify that rendering the committed MIDI fixture across a combination of (mode0, rate0, oct0, gate0) on Part 0
// and (mode1, rate1, oct1, gate1) on Part 1 at a given tempo emits the exact Note On and Note Off frames predicted
// by TransportClock::frame_of_tick.
int verify_render_at_tempo(const std::vector<NoteEvent>& base_events,
                           float tempo_bpm,
                           ArpMode mode0, ArpRate rate0, std::uint8_t oct0, float gate0,
                           ArpMode mode1, ArpRate rate1, std::uint8_t oct1, float gate1) {
  // Filter to the first phrase of arp.mid (chord held from frame 0 to frame 48000 = 1.0 s) so the analytical
  // reference for a continuous held chord from frame 0 is exact and unambiguous at any tempo.
  const std::uint64_t release_frame = 48000;
  std::vector<NoteEvent> events;
  std::vector<ExpandedPitch> held[2];
  for (const NoteEvent& e : base_events) {
    if (e.frame == 0 && e.on && (e.channel == 0 || e.channel == 1)) {
      NoteEvent on_ev = e;
      NoteEvent off_ev = e;
      off_ev.frame = release_frame;
      off_ev.on = false;
      off_ev.velocity16 = 0;
      events.push_back(on_ev);
      events.push_back(off_ev);
      held[e.channel].push_back({e.note, e.velocity16});
    }
  }
  std::stable_sort(events.begin(), events.end(), [](const NoteEvent& a, const NoteEvent& b) {
    if (a.frame != b.frame) return a.frame < b.frame;
    return static_cast<int>(a.on) < static_cast<int>(b.on);
  });

  RenderOptions opt;
  opt.tail_seconds = 0.25;
  opt.routing.enabled = true;
  opt.routing.tempo_bpm = tempo_bpm;
  opt.routing.arp[0] = ArpConfig{true, mode0, rate0, oct0, gate0};
  opt.routing.arp[1] = ArpConfig{true, mode1, rate1, oct1, gate1};

  std::vector<EmittedNoteEvent> emitted;
  opt.emitted_notes = &emitted;

  WavData wav;
  std::string err;
  CHECK(render_events(events, opt, wav, err));

  // Build exact analytical expectation per part using TransportClock at `tempo_bpm` (positioned at frame 0).
  TransportClock ref_clock;
  ref_clock.reset();
  ref_clock.set_tempo_tenths(TransportClock::tenths_from_bpm(tempo_bpm));

  for (std::uint32_t part = 0; part < 2; ++part) {
    const ArpConfig& cfg = opt.routing.arp[part];
    const auto list = build_pitch_list(held[part], cfg.mode, cfg.octaves);
    CHECK(!list.empty());

    const std::int64_t step_ticks = arp_rate_ticks(cfg.rate);
    std::uint64_t rng = Arpeggiator::initial_rng_state(part);

    std::vector<EmittedNoteEvent> actual_ons;
    std::vector<EmittedNoteEvent> actual_offs;
    for (const auto& ev : emitted) {
      if (ev.part != part) continue;
      if (ev.on) actual_ons.push_back(ev);
      else actual_offs.push_back(ev);
    }

    // Count expected steps in [0, release_frame).
    std::uint32_t expected_steps = 0;
    for (std::uint32_t s = 0;; ++s) {
      const std::int64_t st_tick = static_cast<std::int64_t>(s) * step_ticks;
      const std::uint64_t on_frame = static_cast<std::uint64_t>(ref_clock.frame_of_tick(st_tick));
      if (on_frame >= release_frame) break;
      ++expected_steps;
    }
    CHECK(actual_ons.size() == expected_steps);
    CHECK(actual_offs.size() == expected_steps);

    for (std::uint32_t s = 0; s < expected_steps; ++s) {
      const std::int64_t st_tick = static_cast<std::int64_t>(s) * step_ticks;
      const std::uint64_t exp_on_frame = static_cast<std::uint64_t>(ref_clock.frame_of_tick(st_tick));
      const ExpandedPitch exp_pitch = expected_step_pitch(list, cfg.mode, s, rng);

      CHECK(actual_ons[s].frame == exp_on_frame);
      CHECK(actual_ons[s].note == exp_pitch.note);
      CHECK(actual_ons[s].velocity16 == exp_pitch.velocity16);

      const std::int64_t next_frame = ref_clock.frame_of_tick(st_tick + step_ticks);
      const std::int64_t step_frames = std::max<std::int64_t>(1, next_frame - static_cast<std::int64_t>(exp_on_frame));
      const double gate_ratio = static_cast<double>(cfg.gate_percent) / 100.0;
      const std::int64_t gate_frames = std::max<std::int64_t>(1, std::llround(static_cast<double>(step_frames) * gate_ratio));
      std::uint64_t exp_off_frame = exp_on_frame + static_cast<std::uint64_t>(gate_frames);
      if (exp_off_frame > release_frame) exp_off_frame = release_frame;

      bool found_off = false;
      for (const auto& off_ev : actual_offs) {
        if (off_ev.note == exp_pitch.note && off_ev.frame == exp_off_frame) {
          found_off = true;
          break;
        }
      }
      CHECK(found_off);
    }
  }
  return 0;
}

}  // namespace

int main() {
  const SmfResult smf = load_smf_file(ARP_TEST_MIDI);
  CHECK(smf.ok);
  CHECK(!smf.events.empty());

  const float kTempos[2] = {120.0f, 97.3f};
  const ArpMode kModes[5] = {ArpMode::kUp, ArpMode::kDown, ArpMode::kUpDown, ArpMode::kRandom, ArpMode::kAsPlayed};
  const ArpRate kRates[8] = {ArpRate::k1_4,   ArpRate::k1_8,   ArpRate::k1_16,  ArpRate::k1_32,
                             ArpRate::k1_4T,  ArpRate::k1_8T,  ArpRate::k1_16T, ArpRate::k1_32T};

  // 1. Sweep all 5 modes x 2 tempos with 2 active parts simultaneously.
  for (float bpm : kTempos) {
    for (int m = 0; m < 5; ++m) {
      const ArpMode mode0 = kModes[m];
      const ArpMode mode1 = kModes[(m + 2) % 5];
      if (verify_render_at_tempo(smf.events, bpm,
                                 mode0, ArpRate::k1_16, 2, 75.0f,
                                 mode1, ArpRate::k1_8T, 2, 80.0f) != 0) {
        return 1;
      }
    }
  }

  // 2. Sweep all 8 rates x 2 tempos across the 2 active parts.
  for (float bpm : kTempos) {
    for (int r = 0; r < 8; ++r) {
      const ArpRate rate0 = kRates[r];
      const ArpRate rate1 = kRates[(r + 4) % 8];
      if (verify_render_at_tempo(smf.events, bpm,
                                 ArpMode::kUp, rate0, 1, 50.0f,
                                 ArpMode::kDown, rate1, 2, 60.0f) != 0) {
        return 1;
      }
    }
  }

  // 3. Sweep all octaves 1..4 x 2 tempos across the 2 active parts.
  for (float bpm : kTempos) {
    for (std::uint8_t oct = 1; oct <= 4; ++oct) {
      const std::uint8_t oct1 = static_cast<std::uint8_t>((oct % 4) + 1);
      if (verify_render_at_tempo(smf.events, bpm,
                                 ArpMode::kUpDown, ArpRate::k1_16, oct, 80.0f,
                                 ArpMode::kAsPlayed, ArpRate::k1_16T, oct1, 80.0f) != 0) {
        return 1;
      }
    }
  }

  // 4. Verify gate = 100% and gate > 100% (legato overlap) on 2 active parts at both tempos.
  for (float bpm : kTempos) {
    if (verify_render_at_tempo(smf.events, bpm,
                               ArpMode::kUp, ArpRate::k1_8, 1, 100.0f,
                               ArpMode::kDown, ArpRate::k1_8, 1, 150.0f) != 0) {
      return 1;
    }
  }

  // 5. Story 7.3: Verify Swing % (50%, 62.5%, 75%) timing and swung step gate scaling across both tempos.
  auto make_note_ev = [](std::uint64_t frame, std::uint8_t ch, std::uint8_t note, std::uint16_t vel16, bool on) {
    NoteEvent e{};
    e.frame = frame;
    e.on = on;
    e.channel = ch;
    e.note = note;
    e.velocity16 = vel16;
    return e;
  };
  auto make_cc_ev = [](std::uint64_t frame, std::uint8_t ch, std::uint8_t cc_num, std::uint8_t val7) {
    NoteEvent e{};
    e.frame = frame;
    e.channel = ch;
    e.note = cc_num;
    e.cc = true;
    e.value = val7;
    return e;
  };

  for (float bpm : kTempos) {
    for (float swing : {50.0f, 62.5f, 75.0f}) {
      const std::uint64_t release_frame = 48000;
      std::vector<NoteEvent> events = {
          make_note_ev(0, 0, 60, 50000, true),
          make_note_ev(0, 0, 64, 50000, true),
          make_note_ev(release_frame, 0, 60, 0, false),
          make_note_ev(release_frame, 0, 64, 0, false),
      };
      RenderOptions opt;
      opt.tail_seconds = 0.25;
      opt.routing.enabled = true;
      opt.routing.tempo_bpm = bpm;
      opt.routing.arp[0] = ArpConfig{true, ArpMode::kUp, ArpRate::k1_16, 1, 50.0f, swing, false};
      std::vector<EmittedNoteEvent> emitted;
      opt.emitted_notes = &emitted;
      WavData wav;
      std::string err;
      CHECK(render_events(events, opt, wav, err));

      TransportClock ref_clock;
      ref_clock.reset();
      ref_clock.set_tempo_tenths(TransportClock::tenths_from_bpm(bpm));
      const std::int64_t step_ticks = arp_rate_ticks(ArpRate::k1_16);

      std::vector<EmittedNoteEvent> ons, offs;
      for (const auto& ev : emitted) {
        if (ev.on) ons.push_back(ev);
        else offs.push_back(ev);
      }
      CHECK(ons.size() >= 6);
      CHECK(ons.size() == offs.size());
      for (std::size_t s = 0; s < ons.size(); ++s) {
        const std::int64_t grid_tick = static_cast<std::int64_t>(s) * step_ticks;
        const std::int64_t swung_tick = Arpeggiator::swung_step_tick(grid_tick, step_ticks, swing);
        const std::int64_t next_swung_tick = Arpeggiator::swung_step_tick(grid_tick + step_ticks, step_ticks, swing);
        const std::uint64_t exp_on = static_cast<std::uint64_t>(ref_clock.frame_of_tick(swung_tick));
        const std::int64_t f_next = ref_clock.frame_of_tick(next_swung_tick);
        const std::int64_t step_frames = std::max<std::int64_t>(1, f_next - static_cast<std::int64_t>(exp_on));
        const std::int64_t gate_frames = std::max<std::int64_t>(1, std::llround(static_cast<double>(step_frames) * 0.5));
        const std::uint64_t exp_off = std::min<std::uint64_t>(release_frame, exp_on + static_cast<std::uint64_t>(gate_frames));
        CHECK(ons[s].frame == exp_on);
        CHECK(offs[s].frame == exp_off);
      }
    }
  }

  // 6. Story 7.3: Verify Latch On (release all keys, replace latched chord on next NoteOn, switch Latch Off ending at next step).
  {
    // 120 BPM, 1/16 = 6000 frames, 50% gate = 3000 frames.
    // Press (60, 64) at frame 0, release at frame 500; press new chord (67, 72) at frame 13000 (after step 2 at 12000), release at 13500;
    // switch Latch Off at 25280 frames (0.526667 s, during step 4 at 24000..27000) -> ends at next step (30000), no step at 30000.
    std::vector<NoteEvent> events = {
        make_note_ev(0, 0, 60, 50000, true),
        make_note_ev(0, 0, 64, 50000, true),
        make_note_ev(500, 0, 60, 0, false),
        make_note_ev(500, 0, 64, 0, false),
        make_note_ev(13000, 0, 67, 50000, true),
        make_note_ev(13000, 0, 72, 50000, true),
        make_note_ev(13500, 0, 67, 0, false),
        make_note_ev(13500, 0, 72, 0, false),
    };
    RenderOptions opt;
    opt.tail_seconds = 0.35;
    opt.routing.enabled = true;
    opt.routing.tempo_bpm = 120.0f;
    opt.routing.arp[0] = ArpConfig{true, ArpMode::kUp, ArpRate::k1_16, 1, 50.0f, 50.0f, true};
    RoutingOptions::ArpSwitch sw;
    sw.seconds = 25280.0 / 48000.0;
    sw.arp = opt.routing.arp;
    sw.arp[0].latch = false;
    opt.routing.arp_switches.push_back(sw);

    std::vector<EmittedNoteEvent> emitted;
    opt.emitted_notes = &emitted;
    WavData wav;
    std::string err;
    CHECK(render_events(events, opt, wav, err));

    std::vector<std::uint8_t> on_notes;
    std::vector<std::uint64_t> on_frames, off_frames;
    for (const auto& ev : emitted) {
      if (ev.on) {
        on_notes.push_back(ev.note);
        on_frames.push_back(ev.frame);
      } else {
        off_frames.push_back(ev.frame);
      }
    }
    const std::vector<std::uint8_t> exp_notes = {60, 64, 60, 67, 72};
    const std::vector<std::uint64_t> exp_ons = {0, 6000, 12000, 18000, 24000};
    const std::vector<std::uint64_t> exp_offs = {3000, 9000, 15000, 21000, 27000};
    CHECK(on_notes == exp_notes);
    CHECK(on_frames == exp_ons);
    CHECK(off_frames == exp_offs);
  }

  // 7. Story 7.3: Verify Sustain pedal (CC 64) with Latch Off holds notes in the pattern without holding step NoteOffs in VoiceArbiter.
  {
    std::vector<NoteEvent> events = {
        make_cc_ev(0, 0, 64, 127),
        make_note_ev(0, 0, 60, 50000, true),
        make_note_ev(0, 0, 64, 50000, true),
        make_note_ev(500, 0, 60, 0, false),
        make_note_ev(500, 0, 64, 0, false),
        make_cc_ev(13000, 0, 64, 0),
    };
    RenderOptions opt;
    opt.tail_seconds = 0.25;
    opt.routing.enabled = true;
    opt.routing.tempo_bpm = 120.0f;
    opt.routing.arp[0] = ArpConfig{true, ArpMode::kUp, ArpRate::k1_16, 1, 50.0f, 50.0f, false};

    std::vector<EmittedNoteEvent> emitted;
    opt.emitted_notes = &emitted;
    WavData wav;
    std::string err;
    CHECK(render_events(events, opt, wav, err));

    std::vector<std::uint64_t> on_frames, off_frames;
    for (const auto& ev : emitted) {
      if (ev.on) on_frames.push_back(ev.frame);
      else off_frames.push_back(ev.frame);
    }
    const std::vector<std::uint64_t> exp_ons = {0, 6000, 12000};
    const std::vector<std::uint64_t> exp_offs = {3000, 9000, 13000};
    CHECK(on_frames == exp_ons);
    CHECK(off_frames == exp_offs);

    // Between step 0's gate off (frame 3000) and step 1's NoteOn (frame 6000), the audio must be silent (proving VoiceArbiter did not hold step 0).
    for (std::uint64_t f = 3100; f < 5900; ++f) {
      CHECK(wav.samples[f * 2] == 0.0f && wav.samples[f * 2 + 1] == 0.0f);
    }
  }

  // 8. Story 7.3: Verify scene / Live Set Snapshot switch mid-gate does not truncate already-sounding arpeggiated step notes.
  {
    std::vector<NoteEvent> events = {
        make_note_ev(0, 0, 60, 50000, true),
        make_note_ev(0, 0, 64, 50000, true),
        make_note_ev(15000, 0, 60, 0, false),
        make_note_ev(15000, 0, 64, 0, false),
    };
    RenderOptions opt;
    opt.tail_seconds = 0.25;
    opt.routing.enabled = true;
    opt.routing.tempo_bpm = 120.0f;
    opt.routing.arp[0] = ArpConfig{true, ArpMode::kUp, ArpRate::k1_16, 1, 50.0f, 50.0f, false};

    // Switch 1 at frame 1280 (mid-gate of step 0 [0..3000]): Arp stays On.
    RoutingOptions::ArpSwitch sw1;
    sw1.seconds = 1280.0 / 48000.0;
    sw1.arp = opt.routing.arp;
    opt.routing.arp_switches.push_back(sw1);

    // Switch 2 at frame 7280 (mid-gate of step 1 [6000..9000]): Arp turns Off in the new scene.
    RoutingOptions::ArpSwitch sw2;
    sw2.seconds = 7280.0 / 48000.0;
    sw2.arp = opt.routing.arp;
    sw2.arp[0].enabled = false;
    opt.routing.arp_switches.push_back(sw2);

    std::vector<EmittedNoteEvent> emitted;
    opt.emitted_notes = &emitted;
    WavData wav;
    std::string err;
    CHECK(render_events(events, opt, wav, err));

    // Step 0 plays 0..3000 and Step 1 plays 6000..9000 without truncation at frame 7280!
    std::vector<std::uint64_t> on_frames, off_frames;
    for (const auto& ev : emitted) {
      if (ev.on) on_frames.push_back(ev.frame);
      else off_frames.push_back(ev.frame);
    }
    const std::vector<std::uint64_t> exp_ons = {0, 6000};
    const std::vector<std::uint64_t> exp_offs = {3000, 9000};
    CHECK(on_frames == exp_ons);
    CHECK(off_frames == exp_offs);

    // Audio remains non-silent throughout [7300, 8900] after the scene switch at frame 7280!
    float peak_after_switch = 0.0f;
    for (std::uint64_t f = 7300; f < 8900; ++f) {
      peak_after_switch = std::max(peak_after_switch, std::fabs(wav.samples[f * 2]));
    }
    CHECK(peak_after_switch > 0.01f);
  }

  std::printf("arp_tests: all mode/rate/octave/gate/swing/latch/sustain/switch checks passed\n");
  return 0;
}
