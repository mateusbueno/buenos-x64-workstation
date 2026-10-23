#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "velora/format/sysex_6op.hpp"

namespace fmsuite {

struct Ev {
  std::uint32_t tick;
  std::uint8_t a, b, c;
};

// 1 tick = 64 frames (at 48 kHz, 1 tick = 1.333 ms; 750 ticks = 1 sec)
// Standard format 0 SMF with 375 ppq at 120 BPM (500,000 us per beat -> 1 beat = 0.5s = 375 ticks)
inline void vlq(std::vector<std::uint8_t>& out, std::uint32_t v) {
  std::uint8_t buf[5];
  int i = 0;
  buf[i++] = v & 0x7F;
  while ((v >>= 7) > 0) buf[i++] = 0x80 | (v & 0x7F);
  while (i > 0) out.push_back(buf[--i]);
}

class Midi {
 public:
  std::vector<Ev> ev;
  std::uint32_t end_tick = 0;

  void note(std::uint32_t on_tick, std::uint32_t off_tick, int key, int vel) {
    ev.push_back({on_tick, 0x90, static_cast<std::uint8_t>(key), static_cast<std::uint8_t>(vel)});
    ev.push_back({off_tick, 0x80, static_cast<std::uint8_t>(key), 0});
    end_tick = std::max(end_tick, off_tick);
  }

  std::vector<std::uint8_t> smf(std::uint32_t tail_ticks = 375) const {
    std::vector<Ev> e = ev;
    std::stable_sort(e.begin(), e.end(), [](const Ev& x, const Ev& y) { return x.tick < y.tick; });
    std::vector<std::uint8_t> t;
    vlq(t, 0);
    // Tempo 120 BPM = 500,000 us
    t.insert(t.end(), {0xFF, 0x51, 3, 0x07, 0xA1, 0x20});
    std::uint32_t last = 0;
    for (const Ev& x : e) {
      vlq(t, x.tick - last);
      last = x.tick;
      t.push_back(x.a);
      t.push_back(x.b);
      t.push_back(x.c);
    }
    const std::uint32_t end = std::max(last, end_tick + tail_ticks);
    vlq(t, end - last);
    t.insert(t.end(), {0xFF, 0x2F, 0}); // End of track
    // Header chunk: Format 0, 1 track, 375 ticks per quarter note
    std::vector<std::uint8_t> f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0x77};
    f.insert(f.end(), {'M', 'T', 'r', 'k'});
    for (int i = 3; i >= 0; --i) f.push_back(static_cast<std::uint8_t>(t.size() >> (8 * i)));
    f.insert(f.end(), t.begin(), t.end());
    return f;
  }
};

struct CaseDef {
  std::string name;
  velora::format::FmVoiceParameters voice;
  Midi midi;
};

inline std::vector<CaseDef> make_cases() {
  std::vector<CaseDef> cases;

  // Case 1: fm_algo01_feedback (Alg 1, Feedback 3, serial chain with feedback)
  {
    CaseDef c;
    c.name = "fm_algo01_feedback";
    c.voice.algorithm = 1;
    c.voice.feedback = 3;
    c.voice.lfo_pitch_mod_depth = 0;
    c.voice.lfo_amp_mod_depth = 0;
    c.voice.lfo_pitch_mod_sens = 0;

    // OP1 (Carrier)
    c.voice.operators[0].total_level = 95;
    c.voice.operators[0].freq_coarse = 1;
    c.voice.operators[0].rates = {99, 99, 99, 99};
    c.voice.operators[0].levels = {99, 99, 99, 0};

    // OP2 (Modulator for OP1)
    c.voice.operators[1].total_level = 72;
    c.voice.operators[1].freq_coarse = 1;
    c.voice.operators[1].rates = {99, 99, 99, 99};
    c.voice.operators[1].levels = {99, 99, 99, 0};

    // OP3 (Carrier 2)
    c.voice.operators[2].total_level = 85;
    c.voice.operators[2].freq_coarse = 1;
    c.voice.operators[2].rates = {99, 99, 99, 99};
    c.voice.operators[2].levels = {99, 99, 99, 0};

    // OP4, OP5, OP6 (Modulator stack with feedback on OP6)
    for (int i = 3; i < 6; ++i) {
      c.voice.operators[i].total_level = static_cast<uint8_t>(68 + (i - 3) * 3);
      c.voice.operators[i].freq_coarse = static_cast<uint8_t>(1 + (i % 2));
      c.voice.operators[i].rates = {99, 99, 99, 99};
      c.voice.operators[i].levels = {99, 99, 99, 0};
    }

    c.midi.note(0, 750, 60, 100);    // C4 for 1.0s
    cases.push_back(std::move(c));
  }

  // Case 2: fm_algo05_epiano (Alg 5, classic 2-carrier EP with dynamic touch)
  {
    CaseDef c;
    c.name = "fm_algo05_epiano";
    c.voice.algorithm = 5;
    c.voice.feedback = 0;
    c.voice.lfo_pitch_mod_depth = 0;
    c.voice.lfo_amp_mod_depth = 0;
    c.voice.lfo_pitch_mod_sens = 0;

    // OP1 (Carrier 1 - body)
    c.voice.operators[0].total_level = 99;
    c.voice.operators[0].freq_coarse = 1;
    c.voice.operators[0].rates = {99, 75, 40, 50};
    c.voice.operators[0].levels = {99, 85, 0, 0};
    c.voice.operators[0].velocity_sens = 0;

    // OP2 (Modulator for OP1)
    c.voice.operators[1].total_level = 88;
    c.voice.operators[1].freq_coarse = 1;
    c.voice.operators[1].rates = {99, 60, 30, 45};
    c.voice.operators[1].levels = {99, 70, 0, 0};
    c.voice.operators[1].velocity_sens = 0;

    // OP3 (Carrier 2 - chime)
    c.voice.operators[2].total_level = 85;
    c.voice.operators[2].freq_coarse = 1;
    c.voice.operators[2].rates = {99, 70, 40, 50};
    c.voice.operators[2].levels = {99, 80, 0, 0};
    c.voice.operators[2].velocity_sens = 0;

    // OP4 (Modulator for OP3)
    c.voice.operators[3].total_level = 82;
    c.voice.operators[3].freq_coarse = 2;
    c.voice.operators[3].rates = {99, 55, 30, 45};
    c.voice.operators[3].levels = {99, 60, 0, 0};
    c.voice.operators[3].velocity_sens = 0;

    // OP5 & OP6 (Upper modulators)
    c.voice.operators[4].total_level = 78;
    c.voice.operators[4].freq_coarse = 1;
    c.voice.operators[4].rates = {99, 50, 25, 40};
    c.voice.operators[4].levels = {99, 50, 0, 0};

    c.voice.operators[5].total_level = 80;
    c.voice.operators[5].freq_coarse = 1;
    c.voice.operators[5].rates = {99, 50, 25, 40};
    c.voice.operators[5].levels = {99, 50, 0, 0};

    // Arpeggiated chord with velocity dynamics (longer gates allowing natural FM EP decay)
    c.midi.note(0, 900, 60, 64);
    c.midi.note(300, 1200, 63, 96);
    c.midi.note(600, 1500, 67, 112);
    c.midi.note(900, 1800, 70, 127);
    cases.push_back(std::move(c));
  }

  // Case 3: fm_algo16_parallel (Alg 16, Feedback 0, 3 parallel pairs)
  {
    CaseDef c;
    c.name = "fm_algo16_parallel";
    c.voice.algorithm = 16;
    c.voice.feedback = 0;
    c.voice.lfo_pitch_mod_depth = 0;
    c.voice.lfo_amp_mod_depth = 0;
    c.voice.lfo_pitch_mod_sens = 0;

    // OP1 (Carrier 1, ratio 1)
    c.voice.operators[0].total_level = 95;
    c.voice.operators[0].freq_coarse = 1;
    c.voice.operators[0].rates = {99, 99, 99, 99};
    c.voice.operators[0].levels = {99, 99, 99, 0};

    // OP2 (Modulator 1, ratio 2)
    c.voice.operators[1].total_level = 80;
    c.voice.operators[1].freq_coarse = 2;
    c.voice.operators[1].rates = {99, 99, 99, 99};
    c.voice.operators[1].levels = {99, 99, 99, 0};

    // OP3 (Carrier 2, ratio 1)
    c.voice.operators[2].total_level = 90;
    c.voice.operators[2].freq_coarse = 1;
    c.voice.operators[2].rates = {99, 99, 99, 99};
    c.voice.operators[2].levels = {99, 99, 99, 0};

    // OP4 (Modulator 2, ratio 3)
    c.voice.operators[3].total_level = 75;
    c.voice.operators[3].freq_coarse = 3;
    c.voice.operators[3].rates = {99, 99, 99, 99};
    c.voice.operators[3].levels = {99, 99, 99, 0};

    // OP5 (Modulator 3, ratio 4)
    c.voice.operators[4].total_level = 70;
    c.voice.operators[4].freq_coarse = 4;
    c.voice.operators[4].rates = {99, 99, 99, 99};
    c.voice.operators[4].levels = {99, 99, 99, 0};

    // OP6 (Modulator 4)
    c.voice.operators[5].total_level = 65;
    c.voice.operators[5].freq_coarse = 1;
    c.voice.operators[5].rates = {99, 99, 99, 99};
    c.voice.operators[5].levels = {99, 99, 99, 0};

    c.midi.note(0, 1125, 48, 100);
    c.midi.note(375, 1125, 60, 100);
    c.midi.note(750, 1125, 72, 100);
    cases.push_back(std::move(c));
  }

  // Case 4: fm_algo32_all_carriers (Alg 32, Feedback 0, harmonic additive series with key scaling)
  {
    CaseDef c;
    c.name = "fm_algo32_all_carriers";
    c.voice.algorithm = 32;
    c.voice.feedback = 0;
    c.voice.lfo_pitch_mod_depth = 0;
    c.voice.lfo_amp_mod_depth = 0;
    c.voice.lfo_pitch_mod_sens = 0;

    // OP1..OP6: 6 harmonics (1x, 2x, 3x, 4x, 5x, 6x) with declining levels
    for (int i = 0; i < 6; ++i) {
      c.voice.operators[i].total_level = static_cast<uint8_t>(99 - i * 6);
      c.voice.operators[i].freq_coarse = static_cast<uint8_t>(i + 1);
      c.voice.operators[i].rates = {99, 99, 99, 99};
      c.voice.operators[i].levels = {99, 99, 99, 0};
      c.voice.operators[i].kbd_breakpoint = 60;
      c.voice.operators[i].left_depth = 0;
      c.voice.operators[i].right_depth = 0;
      c.voice.operators[i].kbd_rate_scaling = 0;
    }

    c.midi.note(0, 750, 60, 100);
    cases.push_back(std::move(c));
  }

  // Case 5: fm_envelope_decay_release (Alg 1, multi-stage envelope decay & release)
  {
    CaseDef c;
    c.name = "fm_envelope_decay_release";
    c.voice.algorithm = 1;
    c.voice.feedback = 2;
    c.voice.lfo_pitch_mod_depth = 0;
    c.voice.lfo_amp_mod_depth = 0;
    c.voice.lfo_pitch_mod_sens = 0;

    // Carrier OP1
    c.voice.operators[0].total_level = 99;
    c.voice.operators[0].freq_coarse = 1;
    c.voice.operators[0].rates = {99, 50, 40, 45};
    c.voice.operators[0].levels = {99, 75, 50, 0};

    // Modulators with varying rates
    for (int i = 1; i < 6; ++i) {
      c.voice.operators[i].total_level = static_cast<uint8_t>(85 - i * 4);
      c.voice.operators[i].freq_coarse = static_cast<uint8_t>(1 + (i % 2));
      c.voice.operators[i].rates = {
          static_cast<uint8_t>(80 - i * 5),
          static_cast<uint8_t>(45 - i * 3),
          static_cast<uint8_t>(30 + i * 2),
          static_cast<uint8_t>(40 - i * 2)};
      c.voice.operators[i].levels = {99, 70, 40, 0};
    }

    // Short note (staccato) followed by long held note
    c.midi.note(0, 150, 60, 100);    // 0.2s gate (releases early during decay)
    c.midi.note(300, 1125, 65, 110); // 1.1s gate (reaches sustain, then releases)
    cases.push_back(std::move(c));
  }

  return cases;
}

} // namespace fmsuite
