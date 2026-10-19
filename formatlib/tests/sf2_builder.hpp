#pragma once
// Test-only SoundFont 2 WRITER (Story 5.3). Produces valid SF2 files in memory from a description
// of samples, instruments and presets; written from the public SoundFont 2.04 specification, never
// shipped, never used by the importer. Negative fixtures are made by building a valid file and
// patching the bytes at the offsets reported in `Layout`.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace sf2b {

// Generator operators used by the tests (section 8.1.2).
enum Op : std::uint16_t {
  startAddrsOffset = 0, endAddrsOffset = 1, startloopAddrsOffset = 2, endloopAddrsOffset = 3,
  startAddrsCoarseOffset = 4, modLfoToPitch = 5, vibLfoToPitch = 6, modEnvToPitch = 7,
  initialFilterFc = 8, initialFilterQ = 9, modLfoToFilterFc = 10, modEnvToFilterFc = 11,
  endAddrsCoarseOffset = 12, modLfoToVolume = 13, chorusEffectsSend = 15, reverbEffectsSend = 16,
  pan = 17, delayModLFO = 21, freqModLFO = 22, delayVibLFO = 23, freqVibLFO = 24, delayModEnv = 25,
  attackModEnv = 26, holdModEnv = 27, decayModEnv = 28, sustainModEnv = 29, releaseModEnv = 30,
  keynumToModEnvHold = 31, keynumToModEnvDecay = 32, keynumToVolEnvHold = 39, keynumToVolEnvDecay = 40, delayVolEnv = 33, attackVolEnv = 34,
  holdVolEnv = 35, decayVolEnv = 36, sustainVolEnv = 37, releaseVolEnv = 38, instrument = 41,
  keyRange = 43, velRange = 44, startloopAddrsCoarseOffset = 45, keynum = 46, velocity = 47,
  initialAttenuation = 48, endloopAddrsCoarseOffset = 50, coarseTune = 51, fineTune = 52,
  sampleID = 53, sampleModes = 54, scaleTuning = 56, exclusiveClass = 57, overridingRootKey = 58,
};

struct Gen {
  std::uint16_t oper = 0, amount = 0;
};
inline Gen g(std::uint16_t op, int v) { return {op, static_cast<std::uint16_t>(static_cast<std::int16_t>(v))}; }
inline Gen range(std::uint16_t op, int lo, int hi) {
  return {op, static_cast<std::uint16_t>((lo & 0xff) | ((hi & 0xff) << 8))};
}

struct Mod {
  std::uint16_t src = 0, dest = 0;
  std::int16_t amount = 0;
  std::uint16_t amt_src = 0, trans = 0;
};

// A zone is a bag: generators (the builder appends the terminal one when `term >= 0`: the
// instrument index in a preset zone, the sample index in an instrument zone) and modulators.
struct Zone {
  std::vector<Gen> gens;
  std::vector<Mod> mods;
  int term = -1;
  std::uint16_t term_oper = 0;  // instrument (41) or sampleID (53), set by pzone()/izone()
  std::vector<Gen> after_term;  // generators written after the terminal one (a reader must ignore them)
};
// A preset zone playing instrument `inst`; an instrument zone playing sample `smp`.
inline Zone pzone(int inst, std::vector<Gen> gens = {}, std::vector<Mod> mods = {}) {
  Zone z; z.gens = std::move(gens); z.mods = std::move(mods); z.term = inst; z.term_oper = 41; return z;
}
inline Zone izone(int smp, std::vector<Gen> gens = {}, std::vector<Mod> mods = {}) {
  Zone z; z.gens = std::move(gens); z.mods = std::move(mods); z.term = smp; z.term_oper = 53; return z;
}
inline Zone gzone(std::vector<Gen> gens = {}, std::vector<Mod> mods = {}) {  // global zone
  Zone z; z.gens = std::move(gens); z.mods = std::move(mods); return z;
}

struct Sample {
  std::string name;
  std::vector<std::int32_t> pcm;  // 16-bit values, or 24-bit values when the builder is in 24-bit mode
  std::uint32_t rate = 48000;
  std::uint8_t pitch = 60;
  std::int8_t correction = 0;
  std::uint16_t type = 1;   // 1 mono, 2 right, 4 left, 8 linked, |0x8000 ROM
  std::uint16_t link = 0;   // index of the paired sample
  // Loop points relative to the sample start; end is the first point after the loop. Both 0 =
  // the default loop (the whole sample).
  std::uint32_t loop_start = 0, loop_end = 0;
};

struct Instrument {
  std::string name;
  std::vector<Zone> zones;  // an optional global zone first (term < 0)
};

struct Preset {
  std::string name;
  std::uint16_t program = 0, bank = 0;
  std::vector<Zone> zones;
};

// Offsets in the built file, for patching.
struct Layout {
  std::size_t riff_size = 4;                      // the RIFF size field
  std::map<std::string, std::size_t> hdr;         // chunk id (or "LIST:INFO") -> header offset
  std::map<std::string, std::size_t> data;        // chunk id -> data offset
  std::size_t smpl_frames = 0;
  std::size_t phdr(std::size_t i) const { return data.at("phdr") + i * 38; }
  std::size_t pbag(std::size_t i) const { return data.at("pbag") + i * 4; }
  std::size_t pmod(std::size_t i) const { return data.at("pmod") + i * 10; }
  std::size_t pgen(std::size_t i) const { return data.at("pgen") + i * 4; }
  std::size_t inst(std::size_t i) const { return data.at("inst") + i * 22; }
  std::size_t ibag(std::size_t i) const { return data.at("ibag") + i * 4; }
  std::size_t imod(std::size_t i) const { return data.at("imod") + i * 10; }
  std::size_t igen(std::size_t i) const { return data.at("igen") + i * 4; }
  std::size_t shdr(std::size_t i) const { return data.at("shdr") + i * 46; }
};

inline void put16(std::vector<std::uint8_t>& v, std::size_t at, unsigned x) {
  v[at] = std::uint8_t(x);
  v[at + 1] = std::uint8_t(x >> 8);
}
inline void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v[at + i] = std::uint8_t(x >> (8 * i));
}

class Builder {
 public:
  bool bits24 = false;
  bool sm24_pad = true;  // an odd number of sample points: pad the sm24 chunk to an even size (the specification) or not
  unsigned ifil_major = 2, ifil_minor = 4;
  std::string bank_name = "Test bank";
  std::vector<Sample> samples;
  std::vector<Instrument> instruments;
  std::vector<Preset> presets;
  Layout layout;

  std::vector<std::uint8_t> build() {
    layout = Layout{};
    std::vector<std::uint8_t> f;
    f.insert(f.end(), {'R', 'I', 'F', 'F', 0, 0, 0, 0, 's', 'f', 'b', 'k'});
    // INFO
    std::size_t info = begin_list(f, "INFO");
    layout.hdr["LIST:INFO"] = info;
    chunk(f, "ifil", [&](auto& d) { le16(d, ifil_major); le16(d, ifil_minor); });
    chunk(f, "isng", [&](auto& d) { str(d, "EMU8000"); });
    chunk(f, "INAM", [&](auto& d) { str(d, bank_name); });
    end_chunk(f, info);
    // sdta
    std::size_t sdta = begin_list(f, "sdta");
    layout.hdr["LIST:sdta"] = sdta;
    std::vector<std::uint32_t> start(samples.size()), end(samples.size());
    std::vector<std::int32_t> all;  // 24- or 16-bit values of every sample plus 46 zero points
    for (std::size_t i = 0; i < samples.size(); ++i) {
      start[i] = static_cast<std::uint32_t>(all.size());
      all.insert(all.end(), samples[i].pcm.begin(), samples[i].pcm.end());
      end[i] = static_cast<std::uint32_t>(all.size());
      all.insert(all.end(), 46, 0);
    }
    layout.smpl_frames = all.size();
    chunk(f, "smpl", [&](auto& d) {
      for (std::int32_t v : all) le16(d, static_cast<std::uint16_t>(static_cast<std::int16_t>(bits24 ? (v >> 8) : v)));
    });
    if (bits24)
      chunk(f, "sm24", [&](auto& d) {
        for (std::int32_t v : all) d.push_back(std::uint8_t(v & 0xff));
        if ((all.size() & 1) && sm24_pad) d.push_back(0);
      });
    end_chunk(f, sdta);
    // pdta
    std::size_t pdta = begin_list(f, "pdta");
    layout.hdr["LIST:pdta"] = pdta;
    std::vector<std::uint8_t> phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
    std::size_t nbag = 0, ngen = 0, nmod = 0;
    for (const Preset& p : presets) {
      name20(phdr, p.name);
      le16(phdr, p.program);
      le16(phdr, p.bank);
      le16(phdr, static_cast<unsigned>(nbag));
      le32(phdr, 0); le32(phdr, 0); le32(phdr, 0);
      zones(p.zones, pbag, pgen, pmod, nbag, ngen, nmod);
    }
    name20(phdr, "EOP"); le16(phdr, 0); le16(phdr, 0); le16(phdr, static_cast<unsigned>(nbag)); le32(phdr, 0); le32(phdr, 0); le32(phdr, 0);
    le16(pbag, static_cast<unsigned>(ngen)); le16(pbag, static_cast<unsigned>(nmod));
    le16(pgen, 0); le16(pgen, 0);
    pmod.insert(pmod.end(), 10, 0);
    nbag = ngen = nmod = 0;
    for (const Instrument& in : instruments) {
      name20(inst, in.name);
      le16(inst, static_cast<unsigned>(nbag));
      zones(in.zones, ibag, igen, imod, nbag, ngen, nmod);
    }
    name20(inst, "EOI"); le16(inst, static_cast<unsigned>(nbag));
    le16(ibag, static_cast<unsigned>(ngen)); le16(ibag, static_cast<unsigned>(nmod));
    le16(igen, 0); le16(igen, 0);
    imod.insert(imod.end(), 10, 0);
    for (std::size_t i = 0; i < samples.size(); ++i) {
      const Sample& s = samples[i];
      name20(shdr, s.name);
      le32(shdr, start[i]);
      le32(shdr, end[i]);
      const std::uint32_t ls = s.loop_start, le_ = s.loop_end ? s.loop_end : static_cast<std::uint32_t>(s.pcm.size());
      le32(shdr, start[i] + ls);
      le32(shdr, start[i] + le_);
      le32(shdr, s.rate);
      shdr.push_back(s.pitch);
      shdr.push_back(std::uint8_t(s.correction));
      le16(shdr, s.link);
      le16(shdr, s.type);
    }
    name20(shdr, "EOS");
    shdr.insert(shdr.end(), 46 - 20, 0);
    const std::pair<const char*, std::vector<std::uint8_t>*> tabs[] = {
        {"phdr", &phdr}, {"pbag", &pbag}, {"pmod", &pmod}, {"pgen", &pgen}, {"inst", &inst},
        {"ibag", &ibag}, {"imod", &imod}, {"igen", &igen}, {"shdr", &shdr}};
    for (const auto& t : tabs) chunk(f, t.first, [&](auto& d) { d = *t.second; });
    end_chunk(f, pdta);
    put32(f, layout.riff_size, static_cast<std::uint32_t>(f.size() - 8));
    return f;
  }

 private:
  static void le16(std::vector<std::uint8_t>& d, unsigned x) { d.push_back(std::uint8_t(x)); d.push_back(std::uint8_t(x >> 8)); }
  static void le32(std::vector<std::uint8_t>& d, std::uint32_t x) { for (int i = 0; i < 4; ++i) d.push_back(std::uint8_t(x >> (8 * i))); }
  static void name20(std::vector<std::uint8_t>& d, const std::string& s) {
    for (std::size_t i = 0; i < 20; ++i) d.push_back(i < s.size() && i < 19 ? std::uint8_t(s[i]) : 0);
  }
  static void str(std::vector<std::uint8_t>& d, const std::string& s) {
    for (char c : s) d.push_back(std::uint8_t(c));
    d.push_back(0);
    if (d.size() & 1) d.push_back(0);
  }
  std::size_t begin_list(std::vector<std::uint8_t>& f, const char* type) {
    const std::size_t at = f.size();
    f.insert(f.end(), {'L', 'I', 'S', 'T', 0, 0, 0, 0});
    f.insert(f.end(), type, type + 4);
    return at;
  }
  void end_chunk(std::vector<std::uint8_t>& f, std::size_t hdr) {
    put32(f, hdr + 4, static_cast<std::uint32_t>(f.size() - hdr - 8));
    if (f.size() & 1) f.push_back(0);
  }
  template <class Fn>
  void chunk(std::vector<std::uint8_t>& f, const char* id, Fn&& fill) {
    const std::size_t hdr = f.size();
    f.insert(f.end(), id, id + 4);
    le32(f, 0);
    std::vector<std::uint8_t> d;
    fill(d);
    f.insert(f.end(), d.begin(), d.end());
    layout.hdr[id] = hdr;
    layout.data[id] = hdr + 8;
    put32(f, hdr + 4, static_cast<std::uint32_t>(d.size()));
    if (d.size() & 1) f.push_back(0);
  }
  static void zones(const std::vector<Zone>& zs, std::vector<std::uint8_t>& bag, std::vector<std::uint8_t>& gen,
                    std::vector<std::uint8_t>& mod, std::size_t& nbag, std::size_t& ngen, std::size_t& nmod) {
    for (const Zone& z : zs) {
      le16(bag, static_cast<unsigned>(ngen));
      le16(bag, static_cast<unsigned>(nmod));
      ++nbag;
      for (const Gen& x : z.gens) { le16(gen, x.oper); le16(gen, x.amount); ++ngen; }
      if (z.term >= 0) {
        le16(gen, z.term_oper); le16(gen, static_cast<unsigned>(z.term)); ++ngen;
        for (const Gen& x : z.after_term) { le16(gen, x.oper); le16(gen, x.amount); ++ngen; }
      }
      for (const Mod& m : z.mods) {
        le16(mod, m.src); le16(mod, m.dest); le16(mod, static_cast<std::uint16_t>(m.amount)); le16(mod, m.amt_src); le16(mod, m.trans);
        ++nmod;
      }
    }
  }
};


// ---------------------------------------------------------------------------------------------
// High-level zone descriptions (SF2 units) and the General-MIDI-style fixture.

// Instrument-level values (absolute; the fields hold the specification defaults).
struct ZS {
  int klo = 0, khi = 127, vlo = 0, vhi = 127, root = -1, coarse = 0, fine = 0, atten = 0, pan = 0, modes = 0;
  int delay = -12000, attack = -12000, hold = -12000, decay = -12000, sustain = 0, release = -12000;
  int fc = 13500, q = 0, start = 0, startc = 0, sloop = 0, sloopc = 0, eloop = 0, eloopc = 0;
  std::vector<Gen> extra;
  std::vector<Mod> mods;

  std::vector<Gen> gens() const {
    std::vector<Gen> o;
    if (klo != 0 || khi != 127) o.push_back(range(Op::keyRange, klo, khi));
    if (vlo != 0 || vhi != 127) o.push_back(range(Op::velRange, vlo, vhi));
    auto add = [&](std::uint16_t op, int v, int def) { if (v != def) o.push_back(g(op, v)); };
    add(Op::startAddrsOffset, start, 0); add(Op::startAddrsCoarseOffset, startc, 0);
    add(Op::startloopAddrsOffset, sloop, 0); add(Op::startloopAddrsCoarseOffset, sloopc, 0);
    add(Op::endloopAddrsOffset, eloop, 0); add(Op::endloopAddrsCoarseOffset, eloopc, 0);
    add(Op::initialFilterFc, fc, 13500); add(Op::initialFilterQ, q, 0); add(Op::pan, pan, 0);
    add(Op::delayVolEnv, delay, -12000); add(Op::attackVolEnv, attack, -12000); add(Op::holdVolEnv, hold, -12000);
    add(Op::decayVolEnv, decay, -12000); add(Op::sustainVolEnv, sustain, 0); add(Op::releaseVolEnv, release, -12000);
    add(Op::initialAttenuation, atten, 0); add(Op::coarseTune, coarse, 0); add(Op::fineTune, fine, 0);
    add(Op::sampleModes, modes, 0); add(Op::overridingRootKey, root, -1);
    for (const Gen& x : extra) o.push_back(x);
    return o;
  }
};

// Preset-level values: offsets added to the instrument's (all default to 0), plus the two ranges.
struct PS {
  int klo = 0, khi = 127, vlo = 0, vhi = 127, coarse = 0, fine = 0, atten = 0, pan = 0;
  int delay = 0, attack = 0, hold = 0, decay = 0, sustain = 0, release = 0, fc = 0, q = 0;
  std::vector<Gen> extra;
  std::vector<Mod> mods;

  std::vector<Gen> gens() const {
    std::vector<Gen> o;
    if (klo != 0 || khi != 127) o.push_back(range(Op::keyRange, klo, khi));
    if (vlo != 0 || vhi != 127) o.push_back(range(Op::velRange, vlo, vhi));
    auto add = [&](std::uint16_t op, int v) { if (v != 0) o.push_back(g(op, v)); };
    add(Op::initialFilterFc, fc); add(Op::initialFilterQ, q); add(Op::pan, pan);
    add(Op::delayVolEnv, delay); add(Op::attackVolEnv, attack); add(Op::holdVolEnv, hold);
    add(Op::decayVolEnv, decay); add(Op::sustainVolEnv, sustain); add(Op::releaseVolEnv, release);
    add(Op::initialAttenuation, atten); add(Op::coarseTune, coarse); add(Op::fineTune, fine);
    for (const Gen& x : extra) o.push_back(x);
    return o;
  }
};

// The field-wise merge of a global zone and a zone (the zone wins where it differs from the default).
template <class T>
inline T merged(const T& gl, const T& z) {
  const T d;
  T m = gl;
  auto pick = [&](auto T::*f) { if (z.*f != d.*f) m.*f = z.*f; };
  pick(&T::klo); pick(&T::khi); pick(&T::vlo); pick(&T::vhi); pick(&T::coarse); pick(&T::fine);
  pick(&T::atten); pick(&T::pan); pick(&T::delay); pick(&T::attack); pick(&T::hold); pick(&T::decay);
  pick(&T::sustain); pick(&T::release); pick(&T::fc); pick(&T::q);
  if constexpr (std::is_same_v<T, ZS>) {
    pick(&T::root); pick(&T::modes); pick(&T::start); pick(&T::startc); pick(&T::sloop); pick(&T::sloopc);
    pick(&T::eloop); pick(&T::eloopc);
  }
  return m;
}

// One intended region: where it comes from and the merged instrument-level / preset-level values.
struct Intent {
  std::string key;  // "bank:program"
  int sample = 0;   // shdr index
  ZS inst;
  PS pre;
};

// A waveform of `frames` frames with `cycles` whole periods (so a loop over it is seamless), in
// 16-bit (peak ~ 0.45 of full scale) or 24-bit values with low-byte noise (to exercise sm24).
inline std::vector<std::int32_t> wave(std::size_t frames, int cycles, bool bits24, int harmonic = 3) {
  std::vector<std::int32_t> v(frames);
  const double pi2 = 6.283185307179586, peak = bits24 ? 8388608.0 * 0.6 : 32768.0 * 0.6;
  for (std::size_t i = 0; i < frames; ++i) {
    const double ph = pi2 * cycles * double(i) / double(frames);
    const double x = std::sin(ph) + std::sin(ph * harmonic + 0.5) / 3.0;
    long long q = std::llround(x * peak * 0.75);
    if (bits24) q += (long long)((i * 37u + 11u) & 0xff) - 128;
    v[i] = static_cast<std::int32_t>(q);
  }
  return v;
}

struct Fixture {
  Builder b;
  std::vector<Intent> intents;  // in the order the parser must produce regions (per preset, file order)
};

inline int add_sample(Builder& b, const char* name, std::size_t frames, int cycles, std::uint32_t rate, int pitch,
                      int corr) {
  Sample s;
  s.name = name;
  s.pcm = wave(frames, cycles, b.bits24);
  s.rate = rate;
  s.pitch = static_cast<std::uint8_t>(pitch);
  s.correction = static_cast<std::int8_t>(corr);
  b.samples.push_back(std::move(s));
  return static_cast<int>(b.samples.size()) - 1;
}
inline void add_preset(Builder& b, const char* name, int bank, int prog, std::vector<Zone> zones) {
  Preset p;
  p.name = name; p.bank = static_cast<std::uint16_t>(bank); p.program = static_cast<std::uint16_t>(prog);
  p.zones = std::move(zones);
  b.presets.push_back(std::move(p));
}

// A General-MIDI-style bank: several presets across banks, a percussion bank 128, shared samples,
// a stereo pair, looped and one-shot samples, envelopes, filters, key and velocity splits, preset
// level offsets.
inline Fixture make_gm(bool bits24) {
  Fixture f;
  Builder& b = f.b;
  b.bits24 = bits24;
  const int sine_a = add_sample(b, "sine_a", 2400, 50, 48000, 60, 0);   // 0
  const int saw_b = add_sample(b, "saw_b", 2000, 40, 44100, 48, 12);     // 1, pitch correction +12 cents
  b.samples[saw_b].loop_start = 200;
  b.samples[saw_b].loop_end = 1800;
  const int st_l = add_sample(b, "stereo_L", 1800, 36, 48000, 60, 0);    // 2
  const int st_r = add_sample(b, "stereo_R", 1800, 36, 48000, 60, 0);    // 3
  b.samples[st_l].type = 4; b.samples[st_l].link = static_cast<std::uint16_t>(st_r);
  b.samples[st_r].type = 2; b.samples[st_r].link = static_cast<std::uint16_t>(st_l);
  const int hit = add_sample(b, "drum_hit", 1500, 10, 48000, 36, 0);     // 4
  add_sample(b, "never_used", 900, 9, 48000, 72, 0);                     // 5: no zone references it
  const int pad = add_sample(b, "pad_loop", 3000, 60, 96000, 64, -7);    // 6
  b.samples[pad].loop_start = 300;
  b.samples[pad].loop_end = 2700;

  // Instrument 0 "Piano": a global zone with attenuation and release, three zones.
  ZS pg;
  pg.atten = 60; pg.release = -1200;
  ZS pz1; pz1.khi = 59; pz1.root = 57;                                   // bass: root override
  ZS pz2; pz2.klo = 60; pz2.vhi = 90;                                    // mid: looped saw, soft layer
  pz2.modes = 1; pz2.attack = -2400; pz2.decay = -1200; pz2.sustain = 200; pz2.hold = -3600; pz2.delay = -4800;
  ZS pz3; pz3.klo = 60; pz3.vlo = 91; pz3.coarse = 12; pz3.fine = -20; pz3.fc = 9000; pz3.q = 120; pz3.pan = -250;
  Instrument i0;
  i0.name = "Piano";
  i0.zones = {gzone(pg.gens()), izone(sine_a, pz1.gens()), izone(saw_b, pz2.gens()), izone(sine_a, pz3.gens())};
  // Instrument 1 "Pad": no global zone, a loop-until-release zone with every offset form.
  ZS pad1; pad1.modes = 3; pad1.start = 100; pad1.sloop = 50; pad1.eloop = -200; pad1.pan = 500;
  pad1.delay = -3600; pad1.attack = -1200; pad1.hold = 0; pad1.decay = 0; pad1.sustain = 400; pad1.release = 1200;
  pad1.atten = 30; pad1.root = 66;
  Instrument i1;
  i1.name = "Pad";
  i1.zones = {izone(pad, pad1.gens())};
  // Instrument 2 "Stereo": a left and a right sample on the same keys, both with a pan.
  ZS st1; st1.pan = -400; st1.atten = 10;
  ZS st2; st2.pan = 400; st2.atten = 10;
  Instrument i2;
  i2.name = "Stereo";
  i2.zones = {izone(st_l, st1.gens()), izone(st_r, st2.gens())};
  // Instrument 3 "Kit": one zone per drum key sharing one sample.
  ZS k1; k1.klo = 36; k1.khi = 36;
  ZS k2; k2.klo = 38; k2.khi = 38; k2.coarse = 5; k2.release = -2400; k2.root = 36;
  ZS k3; k3.klo = 42; k3.khi = 42; k3.fine = 33; k3.atten = 100;
  Instrument i3;
  i3.name = "Kit";
  i3.zones = {izone(hit, k1.gens()), izone(hit, k2.gens()), izone(hit, k3.gens())};
  b.instruments = {i0, i1, i2, i3};

  const PS none;
  auto push = [&](const char* key, int sample, const ZS& inst, const PS& pre) { f.intents.push_back({key, sample, inst, pre}); };
  // 0:0 "Grand": the three zones of the Piano.
  add_preset(b, "Grand", 0, 0, {pzone(0)});
  push("0:0", sine_a, merged(pg, pz1), none);
  push("0:0", saw_b, merged(pg, pz2), none);
  push("0:0", sine_a, merged(pg, pz3), none);
  // 0:1 "Bright": a preset global zone (fine tune, filter offset, attenuation) and a key limited zone.
  PS bg; bg.fine = 10; bg.fc = -1200; bg.atten = 20;
  PS bz; bz.khi = 100; bz.coarse = -2;
  add_preset(b, "Bright", 0, 1, {gzone(bg.gens()), pzone(0, bz.gens())});
  push("0:1", sine_a, merged(pg, pz1), merged(bg, bz));
  push("0:1", saw_b, merged(pg, pz2), merged(bg, bz));
  push("0:1", sine_a, merged(pg, pz3), merged(bg, bz));
  // 0:48 "Pad": preset level coarse -12 and a longer release.
  PS pp; pp.coarse = -12; pp.release = 600;
  add_preset(b, "Pad", 0, 48, {pzone(1, pp.gens())});
  push("0:48", pad, pad1, pp);
  // 8:0 "Wide stereo": both stereo samples.
  add_preset(b, "Wide stereo", 8, 0, {pzone(2)});
  push("8:0", st_l, st1, none);
  push("8:0", st_r, st2, none);
  // 128:0 "Standard kit": the percussion bank.
  add_preset(b, "Standard kit", 128, 0, {pzone(3)});
  push("128:0", hit, k1, none);
  push("128:0", hit, k2, none);
  push("128:0", hit, k3, none);
  // 1:5 "Split layers": preset zones with key ranges. The Piano limited to keys 0..47 keeps only its
  // bass zone; the Kit limited to 48..127 keeps nothing (its keys are 36, 38 and 42).
  PS l1; l1.khi = 47; PS l2; l2.klo = 48;
  add_preset(b, "Split layers", 1, 5, {pzone(0, l1.gens()), pzone(3, l2.gens())});
  push("1:5", sine_a, merged(pg, pz1), l1);
  return f;
}

}  // namespace sf2b
