#pragma once
// Fixtures of the SF2 render comparison (Story 5.3), generated in code with the test-only SF2 writer
// (formatlib/tests/sf2_builder.hpp) and a minimal Standard MIDI File writer. Everything is
// deterministic. Nothing is read from a real SoundFont.
//
// Time grid: the SMF has 375 ticks per quarter note at 120 bpm = 750 ticks per second = exactly 64
// frames per tick at 48 kHz. FluidSynth's MIDI player works in whole milliseconds and in 64-frame
// blocks: an event on a tick that is a multiple of 3 (4 ms = 192 frames) is applied exactly one block
// (64 frames) late, any other tick sometimes two blocks late. Every event of every fixture therefore sits
// on a multiple of 3 ticks (checked in Midi::note), and the engine render of a case uses the same events
// shifted by `kOracleLagTicks`, see README.md.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

#include "sf2_builder.hpp"

namespace sf2suite {

inline constexpr unsigned kTicksPerFrameBlock = 1;  // 1 tick = 64 frames
inline constexpr unsigned kOracleLagTicks = 1;      // FluidSynth applies events one block late

struct Ev {
  std::uint32_t tick;
  std::uint8_t a, b, c;
};

class Midi {
 public:
  std::vector<Ev> ev;
  std::uint32_t end_tick = 0;

  void select(int bank, int program) {  // MMA bank select: MSB 128-multiples, LSB; channel 0
    ev.push_back({0, 0xB0, 0, static_cast<std::uint8_t>(bank >> 7)});
    ev.push_back({0, 0xB0, 32, static_cast<std::uint8_t>(bank & 127)});
    ev.push_back({0, 0xC0, static_cast<std::uint8_t>(program), 0});
    ev.push_back({0, 0xB0, 7, 127});  // volume 127: the default modulator then adds no attenuation
  }
  void note(std::uint32_t on, std::uint32_t off, int key, int vel) {
    if (on % 3 != 0 || off % 3 != 0) std::abort();  // see the time grid above
    ev.push_back({on, 0x90, static_cast<std::uint8_t>(key), static_cast<std::uint8_t>(vel)});
    ev.push_back({off, 0x80, static_cast<std::uint8_t>(key), 0});
    end_tick = std::max(end_tick, off);
  }
  // Standard MIDI File, format 0, 375 ppq, tempo 500000 us (120 bpm). `shift_ticks` delays every event
  // after tick 0 (the bank/program setup stays at 0).
  std::vector<std::uint8_t> smf(std::uint32_t shift_ticks, std::uint32_t tail_ticks) const {
    std::vector<Ev> e = ev;
    std::stable_sort(e.begin(), e.end(), [](const Ev& x, const Ev& y) { return x.tick < y.tick; });
    std::vector<std::uint8_t> t;
    vlq(t, 0);
    t.insert(t.end(), {0xFF, 0x51, 3, 0x07, 0xA1, 0x20});
    std::uint32_t last = 0;
    for (const Ev& x : e) {
      const std::uint32_t tk = x.tick + (x.tick > 0 ? shift_ticks : 0);
      vlq(t, tk - last);
      last = tk;
      t.push_back(x.a);
      t.push_back(x.b);
      if ((x.a & 0xF0) != 0xC0) t.push_back(x.c);
    }
    const std::uint32_t end = std::max(last, end_tick + shift_ticks + tail_ticks);
    vlq(t, end - last);
    t.insert(t.end(), {0xFF, 0x2F, 0});
    std::vector<std::uint8_t> f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0x77};
    f.insert(f.end(), {'M', 'T', 'r', 'k'});
    for (int i = 3; i >= 0; --i) f.push_back(static_cast<std::uint8_t>(t.size() >> (8 * i)));
    f.insert(f.end(), t.begin(), t.end());
    return f;
  }

 private:
  static void vlq(std::vector<std::uint8_t>& v, std::uint32_t x) {
    std::uint8_t b[5];
    int n = 0;
    b[n++] = x & 0x7f;
    while ((x >>= 7)) b[n++] = static_cast<std::uint8_t>((x & 0x7f) | 0x80);
    while (n--) v.push_back(b[n]);
  }
};

// A sum of harmonics of a base period: `cycles` whole periods over `frames` frames (so a loop over the
// sample is seamless). Amplitude 1/h for h in `parts`. 24-bit files get a deterministic low-byte
// dither so the sm24 bytes matter.
inline std::vector<std::int32_t> rich(std::size_t frames, int cycles, bool bits24, std::vector<int> parts = {1, 2, 3, 5},
                                      double level = 0.5) {
  std::vector<std::int32_t> v(frames);
  const double pi2 = 6.283185307179586, peak = (bits24 ? 8388608.0 : 32768.0) * level;
  double norm = 0;
  for (int h : parts) norm += 1.0 / h;
  for (std::size_t i = 0; i < frames; ++i) {
    const double ph = pi2 * cycles * double(i) / double(frames);
    double x = 0;
    for (int h : parts) x += std::sin(ph * h + 0.3 * h) / h;
    // The last 240 frames (5 ms) fade out: a note that reaches the end of a sample that is not looped ends there, and
    // the engine and FluidSynth end a voice differently (the engine starts a 6 ms release, FluidSynth stops dead).
    const std::size_t fade_len = 240;
    const double fade = i + fade_len > frames ? 0.5 + 0.5 * std::cos(3.141592653589793 * double(i + fade_len - frames) / fade_len) : 1.0;
    long long q = std::llround(x / norm * peak * fade);
    if (bits24) q += static_cast<long long>((i * 37u + 11u) & 0xff) - 128;
    v[i] = static_cast<std::int32_t>(q);
  }
  return v;
}

// A decaying percussive hit: two decaying partials (180 Hz and 330 Hz at 48 kHz) over a fast decay.
inline std::vector<std::int32_t> hit(std::size_t frames, bool bits24) {
  std::vector<std::int32_t> v(frames);
  const double pi2 = 6.283185307179586, peak = (bits24 ? 8388608.0 : 32768.0) * 0.6;
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = double(i) / 48000.0;
    // A raised-cosine fade-in over the first 10 ms: a hit that starts at full level would be measured against
    // FluidSynth's 64-frame amplitude ramp at the start of every voice, see README.md.
    const double fade = t < 0.010 ? 0.5 - 0.5 * std::cos(3.141592653589793 * t / 0.010) : 1.0;
    const double tail = i + 480 > frames ? 0.5 + 0.5 * std::cos(3.141592653589793 * double(i + 480 - frames) / 480.0) : 1.0;
    const double x = fade * tail * std::exp(-t * 18.0) * (std::sin(pi2 * 180.0 * t) + 0.5 * std::sin(pi2 * 330.0 * t + 1.0));
    long long q = std::llround(x / 1.5 * peak);
    if (bits24) q += static_cast<long long>((i * 53u + 7u) & 0xff) - 128;
    v[i] = static_cast<std::int32_t>(q);
  }
  return v;
}

// Three places where FluidSynth 2.6.1 departs from the SoundFont specification, which the importer follows to the
// letter. The SF2 file given to the oracle is adjusted so that both render what the specification says (sources are
// FluidSynth's, tools/oracle/.work/fluidsynth/src, see README.md):
//   * initialAttenuation x 0.4 (EMU_ATTENUATION_FACTOR, sfloader/fluid_defsfont.c: "most existing soundfonts expect
//     exactly this (strange, non-standard) behaviour"): the oracle's file carries every attenuation x 2.5;
//   * sustainVolEnv: the envelope value is 1 - cB/1000 over a range of 96 dB (FLUID_PEAK_ATTENUATION 960), i.e. the sustain
//     depth is cB x 0.96: the oracle's file carries every sustain x 25/24;
//   * decay and release take T for 96 dB, not 100: the oracle's file carries those timecents -71 (x 0.9598 in time).
//     Applied ONCE, to the instrument-level value (timecents add between levels, so the shift must not be repeated at the preset
//     level): a fixture that sets decay or release in a preset zone aborts.
// Fixture values are chosen so that the arithmetic is exact (attenuation even, sustain a multiple of 24).
// The master gain (-g 0.8409, tools/oracle/render-fluidsynth.sh) is not part of this rewrite; it is checked on its own by the
// `sf2_calibration` case (a plain looped note: no attenuation, envelope shape, filter or pan).
inline void for_oracle(sf2b::Builder& b) {
  auto scale = [](std::vector<sf2b::Zone>& zones, bool instrument_level) {
    for (auto& z : zones)
      for (auto& g : z.gens) {
        const int v = static_cast<std::int16_t>(g.amount);
        int out = v;
        if (g.oper == sf2b::initialAttenuation) {
          if (v % 2 != 0) std::abort();
          out = v * 5 / 2;
        } else if (g.oper == sf2b::sustainVolEnv) {
          if (v % 24 != 0) std::abort();
          out = v * 25 / 24;
        } else if (g.oper == sf2b::decayVolEnv || g.oper == sf2b::releaseVolEnv) {
          if (!instrument_level) std::abort();  // see above: the shift is applied once
          if (v > -12000) out = v - 71;
        }
        g.amount = static_cast<std::uint16_t>(static_cast<std::int16_t>(out));
      }
  };
  for (auto& i : b.instruments) scale(i.zones, true);
  for (auto& p : b.presets) scale(p.zones, false);
}

struct Case {
  std::string name;
  std::string file;      // the SF2 file it lives in (without extension)
  int bank = 0, program = 0;
  Midi midi;
  std::uint32_t tail_ticks = 100;  // quiet ticks after the last note off in the MIDI
};

struct Files {
  std::vector<std::pair<std::string, sf2b::Builder>> sf2;  // name -> builder
  std::vector<Case> cases;
};

inline int add(sf2b::Builder& b, const char* name, std::vector<std::int32_t> pcm, std::uint32_t rate, int pitch, int corr = 0) {
  sf2b::Sample s;
  s.name = name;
  s.pcm = std::move(pcm);
  s.rate = rate;
  s.pitch = static_cast<std::uint8_t>(pitch);
  s.correction = static_cast<std::int8_t>(corr);
  b.samples.push_back(std::move(s));
  return static_cast<int>(b.samples.size()) - 1;
}

inline Files make_files() {
  using namespace sf2b;
  Files out;

  // ---- the General-MIDI-style bank ------------------------------------------------------------
  {
    Builder b;
    b.bank_name = "Velora SF2 suite bank";
    const bool k = false;
    const int s0 = add(b, "rich_a", rich(9600, 20, k), 48000, 60);                       // 100 Hz fundamental at the root
    const int s1 = add(b, "rich_44k", rich(8820, 20, k), 44100, 48, -10);                // 44.1 kHz, root 48, -10 cents
    const int s2 = add(b, "rich_96k", rich(19200, 20, k), 96000, 72);                    // 96 kHz
    const int sl = add(b, "stereo_L", rich(9600, 20, k, {1, 2, 4}), 48000, 60);
    const int sr = add(b, "stereo_R", rich(9600, 20, k, {1, 3, 5}), 48000, 60);
    const int sh = add(b, "hit", hit(7200, k), 48000, 36);
    const int sb = add(b, "bright", rich(9600, 100, k, {1, 2, 3, 5, 8}), 48000, 60);      // 500 Hz fundamental
    b.samples[s0].loop_start = 960;
    b.samples[s0].loop_end = 8640;
    std::vector<Instrument> ins;
    // 0 "Keys": three zones, two with a velocity split on the same keys
    {
      ZS a; a.klo = 36; a.khi = 59; a.root = 57; a.fine = 30; a.atten = 60;
      ZS bz; bz.klo = 60; bz.khi = 84; bz.vhi = 80;
      ZS c; c.klo = 60; c.khi = 84; c.vlo = 81; c.fine = -25; c.atten = 30;
      ZS g; g.attack = -7200; g.release = -4800;  // a global zone: 15 ms attack and 62 ms release for every zone
      Instrument i; i.name = "Keys";
      i.zones = {gzone(g.gens()), izone(s0, a.gens()), izone(s1, bz.gens()), izone(s2, c.gens())};
      ins.push_back(i);
    }
    // 1 "Envelope": a continuous loop under a full volume envelope, two zones
    {
      ZS e1; e1.klo = 48; e1.khi = 62; e1.modes = 1; e1.delay = -6000; e1.attack = -4800; e1.hold = -4800; e1.decay = -2400;
      e1.sustain = 144; e1.release = -2400;
      ZS e2; e2.klo = 63; e2.khi = 84; e2.modes = 1; e2.attack = -4800; e2.release = -3600; e2.atten = 40;
      Instrument i; i.name = "Envelope";
      i.zones = {izone(s0, e1.gens()), izone(s0, e2.gens())};
      ins.push_back(i);
    }
    // 2 "Loops": the loop in the sample header, moved by the zone's loop offsets; loop until release; no loop with a start offset
    {
      ZS l1; l1.klo = 48; l1.khi = 55; l1.modes = 1; l1.sloop = 480; l1.eloop = -480;
      ZS l2; l2.klo = 56; l2.khi = 63; l2.modes = 3;
      ZS l3; l3.klo = 64; l3.khi = 71; l3.modes = 0; l3.start = 480;
      ZS g; g.attack = -7200; g.release = -4800;
      Instrument i; i.name = "Loops";
      i.zones = {gzone(g.gens()), izone(s0, l1.gens()), izone(s0, l2.gens()), izone(s0, l3.gens())};
      ins.push_back(i);
    }
    // 3 "Stereo": a linked left/right pair with hard pans in its zones and a panned mono zone; preset-level offsets
    {
      ZS st1; st1.klo = 48; st1.khi = 72; st1.pan = -500; st1.atten = 20;
      ZS st2; st2.klo = 48; st2.khi = 72; st2.pan = 500; st2.atten = 20;
      ZS m; m.klo = 73; m.khi = 84; m.pan = -300; m.atten = 60;
      ZS g; g.attack = -7200; g.release = -4800;
      Instrument i; i.name = "Stereo";
      i.zones = {gzone(g.gens()), izone(sl, st1.gens()), izone(sr, st2.gens()), izone(s0, m.gens())};
      ins.push_back(i);
      b.samples[sl].type = 4; b.samples[sl].link = static_cast<std::uint16_t>(sr);
      b.samples[sr].type = 2; b.samples[sr].link = static_cast<std::uint16_t>(sl);
    }
    // 4 "Filter": low-pass zones (the resonant one at a different Q), played at full velocity only
    {
      ZS f1; f1.klo = 48; f1.khi = 59; f1.fc = 11700;           // 7.0 kHz
      ZS f2; f2.klo = 60; f2.khi = 71; f2.fc = 10200; f2.q = 60;  // 2.96 kHz, 6 dB peak
      ZS f3; f3.klo = 72; f3.khi = 84; f3.fc = 9000; f3.q = 100;  // 1.48 kHz, 10 dB peak
      ZS g; g.attack = -7200; g.release = -4800;
      Instrument i; i.name = "Filter";
      i.zones = {gzone(g.gens()), izone(sb, f1.gens()), izone(sb, f2.gens()), izone(sb, f3.gens())};
      ins.push_back(i);
    }
    // 5 "Kit": one-shot drums on three keys
    {
      ZS g; g.release = -4800;
      ZS d1; d1.klo = 36; d1.khi = 36;
      ZS d2; d2.klo = 38; d2.khi = 38; d2.coarse = 5; d2.release = -3600;
      ZS d3; d3.klo = 42; d3.khi = 42; d3.fine = 33; d3.atten = 100; d3.root = 36;
      Instrument i; i.name = "Kit";
      i.zones = {gzone(g.gens()), izone(sh, d1.gens()), izone(sh, d2.gens()), izone(sh, d3.gens())};
      ins.push_back(i);
    }
    // 6 "Filter Q0": a filter without resonance (Butterworth in the SF2 definition) with content at its cutoff, see README.md
    {
      ZS g; g.attack = -7200; g.release = -4800;
      ZS q; q.klo = 72; q.khi = 84; q.fc = 9000;  // 1.48 kHz, Q 0
      Instrument i; i.name = "Filter Q0";
      i.zones = {gzone(g.gens()), izone(sb, q.gens())};
      ins.push_back(i);
    }
    // 7 "Calibration": a plain looped note, nothing but the sample: the check of the oracle's master gain
    {
      ZS g; g.attack = -7200; g.release = -4800;
      ZS c; c.modes = 1;
      Instrument i; i.name = "Calibration";
      i.zones = {gzone(g.gens()), izone(s0, c.gens())};
      ins.push_back(i);
    }
    b.instruments = ins;
    // The stereo preset adds offsets at preset level (cents and centibels).
    PS po; po.fine = 15; po.atten = 40;
    add_preset(b, "Keys", 0, 0, {pzone(0)});
    add_preset(b, "Envelope", 0, 1, {pzone(1)});
    add_preset(b, "Loops", 0, 2, {pzone(2)});
    add_preset(b, "Stereo", 0, 3, {pzone(3, po.gens())});
    add_preset(b, "Filter", 0, 4, {pzone(4)});
    add_preset(b, "Standard kit", 128, 0, {pzone(5)});
    add_preset(b, "Filter Q0", 0, 5, {pzone(6)});
    add_preset(b, "Calibration", 0, 6, {pzone(7)});
    out.sf2.emplace_back("sf2_bank", std::move(b));

    auto mk = [&](const char* name, int bank, int prog) {
      Case c;
      c.name = name; c.file = "sf2_bank"; c.bank = bank; c.program = prog;
      c.midi.select(bank, prog);
      return c;
    };
    // sf2_keys: key splits, tuning, velocity layers and the velocity boundary 80/81
    {
      Case c = mk("sf2_keys", 0, 0);
      const int keys[] = {40, 52, 59, 60, 60, 66, 72, 84};
      const int vels[] = {127, 100, 64, 80, 81, 127, 100, 110};
      std::uint32_t t = 75;
      for (int i = 0; i < 8; ++i) { c.midi.note(t, t + 60, keys[i], vels[i]); t += 81; }
      out.cases.push_back(std::move(c));
    }
    // sf2_envelope
    {
      Case c = mk("sf2_envelope", 0, 1);
      c.midi.note(75, 375, 60, 127);    // delay, attack, hold, decay to sustain, then released during sustain
      c.midi.note(414, 534, 72, 110);
      c.midi.note(576, 615, 64, 127);   // released during its decay
      c.tail_ticks = 150;
      out.cases.push_back(std::move(c));
    }
    // sf2_loops
    {
      Case c = mk("sf2_loops", 0, 2);
      c.midi.note(75, 315, 50, 127);    // continuous loop with offsets, held past the sample end
      c.midi.note(345, 486, 58, 127);   // loop until release: released inside the loop
      c.midi.note(516, 717, 66, 127);   // no loop, start offset: ends by itself
      c.tail_ticks = 150;
      out.cases.push_back(std::move(c));
    }
    // sf2_stereo
    {
      Case c = mk("sf2_stereo", 0, 3);
      const int keys[] = {60, 52, 69, 76, 82};
      std::uint32_t t = 75;
      for (int k : keys) { c.midi.note(t, t + 81, k, 127); t += 102; }
      out.cases.push_back(std::move(c));
    }
    // sf2_filter
    {
      Case c = mk("sf2_filter", 0, 4);
      const int keys[] = {48, 55, 60, 66, 72, 80};
      std::uint32_t t = 75;
      for (int k : keys) { c.midi.note(t, t + 60, k, 127); t += 81; }
      out.cases.push_back(std::move(c));
    }
    // sf2_filter_q0
    {
      Case c = mk("sf2_filter_q0", 0, 5);
      c.midi.note(75, 135, 72, 127);
      c.midi.note(156, 216, 76, 127);
      out.cases.push_back(std::move(c));
    }
    // sf2_calibration: unity gain of the oracle (no attenuation, no pan, no filter, no envelope shape beyond a 15 ms attack)
    {
      Case c = mk("sf2_calibration", 0, 6);
      c.midi.note(75, 255, 60, 127);
      c.midi.note(300, 480, 60, 127);
      out.cases.push_back(std::move(c));
    }
    // sf2_kit: percussion bank 128
    {
      Case c = mk("sf2_kit", 128, 0);
      c.midi.note(75, 105, 36, 127);
      c.midi.note(165, 195, 38, 100);
      c.midi.note(255, 285, 42, 127);
      c.midi.note(345, 375, 36, 60);
      out.cases.push_back(std::move(c));
    }
  }

  // ---- a single-instrument file, 16-bit and 24-bit variants ------------------------------------
  for (bool k : {false, true}) {
    Builder b;
    b.bits24 = k;
    b.bank_name = k ? "Velora single instrument 24-bit" : "Velora single instrument";
    const int s = add(b, "solo", rich(9600, 40, k, {1, 2, 3}, 0.6), 48000, 69);
    b.samples[s].loop_start = 480;
    b.samples[s].loop_end = 9120;
    ZS z; z.modes = 1; z.attack = -3600; z.hold = -6000; z.decay = -1200; z.sustain = 96; z.release = -2400; z.atten = 20;
    Instrument i; i.name = "Solo";
    i.zones = {izone(s, z.gens())};
    b.instruments = {i};
    add_preset(b, "Solo", 0, 0, {pzone(0)});
    const std::string file = k ? "sf2_single24" : "sf2_single";
    out.sf2.emplace_back(file, std::move(b));
    Case c;
    c.name = file; c.file = file; c.bank = 0; c.program = 0;
    c.midi.select(0, 0);
    c.midi.note(75, 276, 69, 127);
    c.midi.note(306, 426, 74, 90);
    c.midi.note(456, 516, 62, 127);
    c.tail_ticks = 100;
    out.cases.push_back(std::move(c));
  }
  return out;
}

}  // namespace sf2suite
