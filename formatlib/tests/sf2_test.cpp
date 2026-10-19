// SoundFont 2 reader and mapping (Story 5.3). Fixtures are generated in code by the test-only
// writer in sf2_builder.hpp; nothing here reads a real SF2 file.
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "sf2_builder.hpp"
#include "test_util.hpp"
#include "velora/format/bundle.hpp"
#include "velora/format/sf2_import.hpp"

using namespace velora::format;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

Sf2ParseResult parse(const Bytes& b) { return parse_sf2_memory(b); }

bool near(double a, double b, double tol = 1e-4) { return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b)); }

// --- the specification's formulas, written independently of the importer ------------------------
double tc2s(int tc) { return tc <= -12000 ? 0.0 : std::min(100.0, std::exp2(tc / 1200.0)); }
int eff(int i, int p, int lo, int hi) { return std::clamp(i + p, lo, hi); }

Region expected_region(const sf2b::Intent& in, const sf2b::Builder& b, std::uint32_t local) {
  const sf2b::Sample& s = b.samples[static_cast<std::size_t>(in.sample)];
  const sf2b::ZS& i = in.inst;
  const sf2b::PS& p = in.pre;
  Region r;
  r.sample = local;
  r.key_lo = static_cast<std::uint8_t>(std::max(i.klo, p.klo));
  r.key_hi = static_cast<std::uint8_t>(std::min(i.khi, p.khi));
  r.vel_lo = static_cast<std::uint8_t>(std::max(i.vlo, p.vlo));
  r.vel_hi = static_cast<std::uint8_t>(std::min(i.vhi, p.vhi));
  r.key_center = static_cast<std::uint8_t>(i.root >= 0 ? i.root : s.pitch);
  r.transpose = eff(i.coarse, p.coarse, -120, 120);
  r.tune_cents = static_cast<float>(eff(i.fine, p.fine, -99, 99) + s.correction);
  r.volume_db = static_cast<float>(-3.0103 - eff(i.atten, p.atten, 0, 1440) / 10.0 - eff(i.q, p.q, 0, 960) / 20.0);
  r.pan = static_cast<float>(eff(i.pan, p.pan, -500, 500) / 5.0);
  if (s.type == 4 && r.pan == 0) r.pan = -100;
  if (s.type == 2 && r.pan == 0) r.pan = 100;
  r.offset = static_cast<std::uint64_t>(i.start + 32768 * i.startc);
  if (i.modes == 1 || i.modes == 3) {
    r.loop_mode = i.modes == 1 ? LoopMode::kLoopContinuous : LoopMode::kLoopSustain;
    r.has_loop_points = true;
    const std::uint32_t ls = s.loop_start + static_cast<std::uint32_t>(i.sloop + 32768 * i.sloopc);
    const std::uint32_t le = (s.loop_end ? s.loop_end : static_cast<std::uint32_t>(s.pcm.size())) +
                             static_cast<std::uint32_t>(i.eloop + 32768 * i.eloopc);
    r.loop_start = ls;
    r.loop_end = le - 1;
  }
  r.ampeg.delay = static_cast<float>(tc2s(eff(i.delay, p.delay, -12000, 5000)));
  r.ampeg.attack = static_cast<float>(tc2s(eff(i.attack, p.attack, -12000, 8000)));
  r.ampeg.hold = static_cast<float>(tc2s(eff(i.hold, p.hold, -12000, 5000)));
  r.ampeg.decay = static_cast<float>(tc2s(eff(i.decay, p.decay, -12000, 8000)) * 0.78173);
  r.ampeg.release = static_cast<float>(tc2s(eff(i.release, p.release, -12000, 8000)) * 0.78173);
  const int sus = eff(i.sustain, p.sustain, 0, 1440);
  r.ampeg.sustain = sus >= 1000 ? 0.f : static_cast<float>(100.0 * std::pow(10.0, -sus / 200.0));
  const int fc = eff(i.fc, p.fc, 1500, 13500);
  if (fc < 13500) {
    r.filter.type = FilterType::kLpf2p;
    r.filter.cutoff_hz = static_cast<float>(8.175798915643707 * std::exp2(fc / 1200.0));
    r.filter.resonance_db = static_cast<float>(std::clamp(eff(i.q, p.q, 0, 960) / 10.0 - 3.01, 0.0, 40.0));
  }
  return r;
}

bool region_close(const Region& a, const Region& b, std::string* why) {
  auto bad = [&](const char* f) { if (why) *why = f; return false; };
  if (a.sample != b.sample) return bad("sample");
  if (a.key_lo != b.key_lo || a.key_hi != b.key_hi || a.key_center != b.key_center) return bad("keys");
  if (a.vel_lo != b.vel_lo || a.vel_hi != b.vel_hi) return bad("velocity");
  if (a.transpose != b.transpose) return bad("transpose");
  if (!near(a.tune_cents, b.tune_cents) || !near(a.volume_db, b.volume_db) || !near(a.pan, b.pan)) return bad("tune/volume/pan");
  if (a.offset != b.offset || a.loop_mode != b.loop_mode || a.has_loop_points != b.has_loop_points ||
      a.loop_start != b.loop_start || a.loop_end != b.loop_end)
    return bad("offset/loop");
  const AmpEnvelope &x = a.ampeg, &y = b.ampeg;
  if (!near(x.delay, y.delay) || !near(x.attack, y.attack) || !near(x.hold, y.hold) || !near(x.decay, y.decay) ||
      !near(x.sustain, y.sustain, 1e-3) || !near(x.release, y.release) || x.start != y.start)
    return bad("envelope");
  if (a.filter.type != b.filter.type || !near(a.filter.cutoff_hz, b.filter.cutoff_hz) ||
      !near(a.filter.resonance_db, b.filter.resonance_db))
    return bad("filter");
  if (a.seq_length != 1 || a.seq_position != 1 || a.lorand != 0 || a.hirand != 1 || a.trigger != TriggerMode::kAttack ||
      a.sw.any() || a.xf != Crossfade{})
    return bad("defaults");
  return true;
}

// The expected regions of one preset key, with the preset's local sample numbering.
std::vector<Region> expected_for(const sf2b::Fixture& f, const std::string& key, std::vector<int>* shdrs) {
  std::vector<Region> out;
  std::vector<int> order;
  for (const auto& in : f.intents) {
    if (in.key != key) continue;
    auto it = std::find(order.begin(), order.end(), in.sample);
    if (it == order.end()) { order.push_back(in.sample); it = order.end() - 1; }
    out.push_back(expected_region(in, f.b, static_cast<std::uint32_t>(it - order.begin())));
  }
  if (shdrs) *shdrs = order;
  return out;
}

const Sf2Preset* find_preset(const Sf2ParseResult& r, const std::string& key) {
  for (const auto& p : r.presets)
    if (p.key == key) return &p;
  return nullptr;
}

// Offset of the first generator record with operator `oper` in the pgen/igen table.
std::size_t find_gen(const Bytes& f, const sf2b::Layout& l, const char* table, unsigned oper) {
  const std::size_t at = l.data.at(table), n = (f[l.hdr.at(table) + 4] | (f[l.hdr.at(table) + 5] << 8)) / 4;
  for (std::size_t i = 0; i < n; ++i)
    if ((f[at + i * 4] | (f[at + i * 4 + 1] << 8)) == int(oper)) return at + i * 4;
  return 0;
}
bool has_warning(const Sf2ParseResult& r, const std::string& preset, const std::string& item) {
  for (const auto& w : r.warnings)
    if (w.preset == preset && w.item == item) return true;
  return false;
}

// One sample, one instrument, one preset 0:0 plus an optional tweak.
template <class Fn>
Sf2ParseResult tiny(Fn&& tweak, bool bits24 = false) {
  sf2b::Builder b;
  b.bits24 = bits24;
  sf2b::add_sample(b, "s", 1000, 20, 48000, 60, 0);
  sf2b::Instrument in;
  in.name = "I";
  in.zones = {sf2b::izone(0)};
  b.instruments = {in};
  sf2b::add_preset(b, "P", 0, 0, {sf2b::pzone(0)});
  tweak(b);
  return parse(b.build());
}

// Bundle round trip of a parsed file: every preset one instrument, regions and decoded PCM compared.
void check_bundle_round_trip(const sf2b::Fixture& f, const Sf2ParseResult& r) {
  BundleWriter w;
  CHECK_ERR(w.set_source_id("sf2-test"), Error::kNone);
  std::vector<std::vector<int>> shdr_of;
  for (const auto& p : r.presets) {
    auto idx = w.add_instrument(p.name, p.key);
    CHECK(idx.ok());
    if (!idx) return;
    std::vector<std::int32_t> pcm;
    for (const auto& s : p.samples) {
      CHECK_ERR(r.samples->read(s, pcm), Error::kNone);
      SampleInput in;
      in.name = s.name;
      in.format = s.format;
      in.frame_count = s.frames;
      in.data = pcm;
      CHECK_ERR(w.add_sample(idx.value(), in), Error::kNone);
    }
    for (const auto& reg : p.regions) CHECK_ERR(w.add_region(idx.value(), reg), Error::kNone);
  }
  auto bytes = w.finalize();
  CHECK(bytes.ok());
  if (!bytes) return;
  auto rd = BundleReader::open_memory(bytes.value());
  CHECK(rd.ok());
  if (!rd) return;
  CHECK_ERR(rd.value().verify_all(), Error::kNone);
  CHECK(rd.value().instruments().size() == r.presets.size());
  for (std::size_t i = 0; i < r.presets.size() && i < rd.value().instruments().size(); ++i) {
    const auto& ins = rd.value().instruments()[i];
    CHECK(ins.key == r.presets[i].key && ins.name == r.presets[i].name);
    CHECK(ins.regions == r.presets[i].regions);
    CHECK(ins.sample_count == r.presets[i].samples.size());
    for (std::size_t k = 0; k < ins.sample_count; ++k) {
      const auto& si = rd.value().samples()[ins.first_sample + k];
      const auto& src = f.b.samples[r.presets[i].samples[k].shdr];
      CHECK(si.frame_count == src.pcm.size());
      CHECK(si.format.bits_per_sample == (f.b.bits24 ? 24 : 16) && si.format.channels == 1);
      CHECK(si.format.sample_rate == src.rate);
      std::vector<std::int32_t> got(si.frame_count);
      CHECK_ERR(rd.value().read_frames(ins.first_sample + k, 0, si.frame_count, got), Error::kNone);
      CHECK(got == src.pcm);  // bit-identical to the SF2 words (16-bit, or smpl<<8 | sm24 in 24-bit)
    }
  }
}

void gm_tests(bool bits24) {
  const sf2b::Fixture f = sf2b::make_gm(bits24);
  sf2b::Builder bb = f.b;
  const Bytes file = bb.build();
  const Sf2ParseResult r = parse(file);
  CHECK(r.ok());
  for (const auto& i : r.issues) std::printf("  issue: %s\n", i.message.c_str());
  if (!r.ok()) return;
  CHECK(r.stats.bits24 == bits24);
  CHECK(r.presets.size() == 6 && r.stats.presets == 6 && r.stats.instruments == 4 && r.stats.shdr_samples == 7);
  const char* keys[] = {"0:0", "0:1", "0:48", "8:0", "128:0", "1:5"};
  for (std::size_t i = 0; i < 6 && i < r.presets.size(); ++i) CHECK(r.presets[i].key == keys[i]);
  CHECK(r.presets[0].name == "Grand" && r.presets[4].name == "Standard kit");
  // The only ignored item: the default velocity -> filter cutoff modulator, once for each preset that has a filter.
  CHECK(r.warnings.size() == 2 && r.stats.ignored_modulators == 2 && r.stats.ignored_generators == 0);
  for (const auto& w : r.warnings)
    CHECK(w.item == "default modulator velocity -> initialFilterFc (-2400 cents)" && (w.preset == "0:0" || w.preset == "0:1"));

  // Structural match: every intended zone against the imported region, field by field.
  std::size_t total = 0;
  for (const char* k : keys) {
    const Sf2Preset* p = find_preset(r, k);
    CHECK(p != nullptr);
    if (!p) continue;
    std::vector<int> order;
    const auto want = expected_for(f, k, &order);
    CHECK(p->regions.size() == want.size());
    for (std::size_t i = 0; i < want.size() && i < p->regions.size(); ++i) {
      std::string why;
      const bool ok = region_close(p->regions[i], want[i], &why);
      if (!ok) std::printf("  region %s #%zu differs in %s\n", k, i, why.c_str());
      CHECK(ok);
    }
    CHECK(p->samples.size() == order.size());
    for (std::size_t i = 0; i < order.size() && i < p->samples.size(); ++i) CHECK(int(p->samples[i].shdr) == order[i]);
    total += want.size();
  }
  CHECK(r.stats.zones == total);
  // "never_used" (shdr 5) is in no preset's sample list.
  for (const auto& p : r.presets)
    for (const auto& s : p.samples) CHECK(s.shdr != 5);
  // Shared samples are stored once per preset: the Piano's sine_a serves two zones with one entry.
  CHECK(r.presets[0].samples.size() == 2 && r.presets[0].regions.size() == 3);
  CHECK(r.presets[0].regions[0].sample == r.presets[0].regions[2].sample);
  // Spot values the formulas above could mirror wrongly: 2400 timecents = 4 s, 1200 = 2 s.
  const Region& pad = r.presets[2].regions[0];
  CHECK(pad.transpose == -12 && pad.key_center == 66 && pad.pan == 100.f && pad.loop_mode == LoopMode::kLoopSustain);
  CHECK(pad.offset == 100 && pad.loop_start == 350 && pad.loop_end == 2499);  // 300+50, 2700-200 exclusive end - 1
  CHECK(near(pad.ampeg.release, std::exp2(1800 / 1200.0) * 0.78173));  // 1200 + 600 timecents
  const Region& st = r.presets[3].regions[0];
  CHECK(st.pan == -80.f && r.presets[3].regions[1].pan == 80.f);  // the zones' own pans (-400 / 400)
  CHECK(r.presets[0].regions[1].loop_mode == LoopMode::kLoopContinuous && r.presets[0].regions[1].tune_cents == 12.f);
  CHECK(r.presets[0].regions[2].filter.type == FilterType::kLpf2p);
  CHECK(near(r.presets[0].regions[2].filter.cutoff_hz, 8.175798915643707 * std::exp2(9000 / 1200.0)));
  CHECK(near(r.presets[0].regions[2].filter.resonance_db, 12.0 - 3.01));  // q/10 - 3.01 dB
  CHECK(r.presets[0].regions[0].filter.type == FilterType::kNone);

  // Sample data decodes bit-identical to the file's words; 24-bit assembles smpl<<8 | sm24.
  std::vector<std::int32_t> pcm;
  for (const auto& p : r.presets)
    for (const auto& s : p.samples) {
      CHECK(s.format.bits_per_sample == (bits24 ? 24 : 16) && s.format.channels == 1 && !s.format.is_float);
      CHECK_ERR(r.samples->read(s, pcm), Error::kNone);
      CHECK(pcm == f.b.samples[s.shdr].pcm);
    }
  if (bits24) {  // the low bytes really matter: the fixture has values that are not multiples of 256
    bool odd = false;
    for (std::int32_t v : f.b.samples[0].pcm) odd = odd || (v & 0xff) != 0;
    CHECK(odd);
  }
  check_bundle_round_trip(f, r);
  // An out-of-range reference is refused, not read.
  Sf2SampleRef bad = r.presets[0].samples[0];
  bad.first_frame = 1u << 30;
  CHECK_ERR(r.samples->read(bad, pcm), Error::kOutOfRange);
}

void single_instrument_file() {
  sf2b::Builder b;
  sf2b::add_sample(b, "solo", 4800, 100, 48000, 69, 0);
  sf2b::ZS z;
  z.modes = 1; z.attack = -1200; z.release = -600;
  sf2b::Instrument in;
  in.name = "Solo";
  in.zones = {sf2b::izone(0, z.gens())};
  b.instruments = {in};
  sf2b::add_preset(b, "Solo", 0, 0, {sf2b::pzone(0)});
  const Sf2ParseResult r = parse(b.build());
  CHECK(r.ok() && r.presets.size() == 1 && r.presets[0].key == "0:0" && r.presets[0].regions.size() == 1);
  if (!r.ok()) return;
  CHECK(r.presets[0].regions[0].key_center == 69 && r.presets[0].regions[0].has_loop_points &&
        r.presets[0].regions[0].loop_start == 0 && r.presets[0].regions[0].loop_end == 4799);
}

void warnings_tests() {
  // A preset with an LFO, a modulation envelope and a custom modulator imports, and the report names
  // exactly those, per preset. A second preset without them has no warnings.
  sf2b::Builder b;
  sf2b::add_sample(b, "s", 2000, 40, 48000, 60, 0);
  sf2b::ZS z;
  z.extra = {sf2b::g(sf2b::vibLfoToPitch, 50), sf2b::g(sf2b::delayModLFO, -1200), sf2b::g(sf2b::freqModLFO, 100),
             sf2b::g(sf2b::modEnvToFilterFc, 500), sf2b::g(sf2b::attackModEnv, -1200), sf2b::g(sf2b::sustainModEnv, 300),
             sf2b::g(sf2b::reverbEffectsSend, 400)};
  z.mods = {sf2b::Mod{0x0093, 5, 100, 0, 0},    // CC19 -> modLfoToPitch: custom
            sf2b::Mod{0x0502, 48, 960, 0, 0},   // the default velocity -> attenuation: not reported
            sf2b::Mod{0x0587, 48, 960, 0, 0}};  // the default CC7 -> attenuation
  sf2b::Instrument weird;
  weird.name = "Weird";
  weird.zones = {sf2b::izone(0, z.gens(), z.mods)};
  sf2b::Instrument plain;
  plain.name = "Plain";
  plain.zones = {sf2b::izone(0)};
  b.instruments = {weird, plain};
  sf2b::add_preset(b, "Weird", 0, 0, {sf2b::pzone(0)});
  sf2b::add_preset(b, "Plain", 0, 1, {sf2b::pzone(1)});
  const Sf2ParseResult r = parse(b.build());
  CHECK(r.ok() && r.presets.size() == 2);
  if (!r.ok()) return;
  CHECK(r.presets[0].regions.size() == 1 && r.presets[1].regions.size() == 1);
  std::set<std::string> want = {"vibLfoToPitch", "delayModLFO", "freqModLFO", "modEnvToFilterFc", "attackModEnv",
                                "sustainModEnv", "reverbEffectsSend",
                                "modulator src=0x0093 dest=modLfoToPitch amount=100 amtsrc=0x0000 trans=0"};
  std::set<std::string> got;
  for (const auto& w : r.warnings) {
    CHECK(w.preset == "0:0");
    CHECK(!w.message.empty());
    got.insert(w.item);
  }
  CHECK(got == want);
  CHECK(r.warnings.size() == want.size());
  CHECK(r.stats.ignored_generators == 7 && r.stats.ignored_modulators == 1 && r.stats.ignored_total() == 8);
  // The sounding values are unaffected: it is exactly the plain region.
  CHECK(r.presets[0].regions[0] == r.presets[1].regions[0]);

  // Other ignored features.
  auto t = tiny([](sf2b::Builder& bb) {
    bb.instruments[0].zones = {sf2b::izone(0, {sf2b::g(sf2b::scaleTuning, 50), sf2b::g(sf2b::exclusiveClass, 3),
                                              sf2b::g(sf2b::endAddrsOffset, -10), sf2b::Gen{61, 5},
                                              sf2b::g(sf2b::keynum, 60), sf2b::g(sf2b::velocity, 90),
                                              sf2b::g(sf2b::keynumToVolEnvHold, 100), sf2b::g(sf2b::chorusEffectsSend, 100)})};
  });
  CHECK(t.ok());
  for (const char* item : {"scaleTuning", "exclusiveClass", "endAddrsOffset", "generator #61", "keynum", "velocity",
                           "keynumToVolEnvHold", "chorusEffectsSend"})
    CHECK(has_warning(t, "0:0", item));
  CHECK(t.warnings.size() == 8);
  // Defaults are not warnings: scaleTuning 100, exclusiveClass 0, a zero offset.
  auto d = tiny([](sf2b::Builder& bb) {
    bb.instruments[0].zones = {sf2b::izone(0, {sf2b::g(sf2b::scaleTuning, 100), sf2b::g(sf2b::exclusiveClass, 0),
                                              sf2b::g(sf2b::endAddrsOffset, 0), sf2b::g(sf2b::modLfoToPitch, 0)})};
  });
  CHECK(d.ok() && d.warnings.empty());
  // An instrument-only generator at preset level is ignored with a warning (the loop is not applied).
  auto pl = tiny([](sf2b::Builder& bb) {
    bb.presets[0].zones = {sf2b::pzone(0, {sf2b::g(sf2b::sampleModes, 1), sf2b::g(sf2b::overridingRootKey, 30)})};
  });
  CHECK(pl.ok() && pl.presets[0].regions[0].loop_mode == LoopMode::kNoLoop && pl.presets[0].regions[0].key_center == 60);
  CHECK(has_warning(pl, "0:0", "sampleModes (preset level)") && has_warning(pl, "0:0", "overridingRootKey (preset level)"));
  // A default modulator with another amount is a custom one; a global-zone modulator counts for each zone.
  auto m = tiny([](sf2b::Builder& bb) {
    bb.instruments[0].zones = {sf2b::gzone({}, {sf2b::Mod{0x0502, 48, 0, 0, 0}}), sf2b::izone(0), sf2b::izone(0)};
  });
  CHECK(m.ok() && m.presets[0].regions.size() == 2 && m.warnings.size() == 1 && m.stats.ignored_modulators == 1);
  CHECK(m.warnings.size() == 1 && m.warnings[0].message.find("2 zones") != std::string::npos);
  // Preset-level modulators too, and linking modulators.
  auto pm = tiny([](sf2b::Builder& bb) {
    bb.presets[0].zones = {sf2b::pzone(0, {}, {sf2b::Mod{0x0013, static_cast<std::uint16_t>(0x8000 | 2), 5, 0, 0}})};
  });
  CHECK(pm.ok() && pm.warnings.size() == 1 && pm.warnings[0].item.find("dest=link 2") != std::string::npos);
  // A stereo sample plays with the zone's own pan; without one it is played hard panned. No warning either way.
  auto st = tiny([](sf2b::Builder& bb) {
    bb.samples[0].type = 4;
    bb.samples[0].link = 0;
    bb.instruments[0].zones = {sf2b::izone(0, {sf2b::g(sf2b::pan, -300)})};
  });
  CHECK(st.ok() && st.presets[0].regions[0].pan == -60.f && st.warnings.empty());
  auto st0 = tiny([](sf2b::Builder& bb) { bb.samples[0].type = 2; bb.samples[0].link = 0; });
  CHECK(st0.ok() && st0.presets[0].regions[0].pan == 100.f && st0.warnings.empty());
  auto st1 = tiny([](sf2b::Builder& bb) { bb.samples[0].type = 4; bb.samples[0].link = 0; });
  CHECK(st1.ok() && st1.presets[0].regions[0].pan == -100.f && st1.warnings.empty());
  auto st2 = tiny([](sf2b::Builder& bb) { bb.samples[0].type = 8; bb.samples[0].link = 0; });  // linked: mono
  CHECK(st2.ok() && st2.presets[0].regions[0].pan == 0.f);
}

void semantics_tests() {
  // Zones after the first without a terminal generator are ignored; generators after the terminal one too;
  // ranges are honoured wherever they stand; an empty intersection drops the pair without a warning.
  auto r = tiny([](sf2b::Builder& bb) {
    sf2b::Zone after = sf2b::izone(0);
    after.after_term = {sf2b::g(sf2b::pan, 500)};
    sf2b::Zone late_range = sf2b::izone(0, {sf2b::g(sf2b::initialAttenuation, 40), sf2b::range(sf2b::keyRange, 10, 20)});
    bb.instruments[0].zones = {sf2b::gzone({sf2b::g(sf2b::initialAttenuation, 100)}), after,
                               sf2b::gzone({sf2b::g(sf2b::pan, -500)}),  // a global-looking zone in the middle: ignored
                               late_range, sf2b::izone(0, {sf2b::range(sf2b::keyRange, 90, 100)})};
    bb.presets[0].zones = {sf2b::pzone(0, {sf2b::range(sf2b::keyRange, 0, 50)})};
  });
  CHECK(r.ok() && r.presets[0].regions.size() == 2);  // the 90..100 zone has an empty intersection with 0..50
  if (r.presets[0].regions.size() == 2) {
    const Region &a = r.presets[0].regions[0], &b = r.presets[0].regions[1];
    CHECK(a.pan == 0.f && a.key_lo == 0 && a.key_hi == 50 && near(a.volume_db, -3.0103 - 10.0));   // global atten 100 cB, ignored pan after terminal
    CHECK(b.key_lo == 10 && b.key_hi == 20 && near(b.volume_db, -3.0103 - 4.0));                    // zone value beats the global one
  }
  CHECK(r.warnings.empty());
  // Preset values add; ranges (velocity) intersect; values clamp to the generator range.
  auto a = tiny([](sf2b::Builder& bb) {
    bb.instruments[0].zones = {sf2b::izone(0, {sf2b::g(sf2b::coarseTune, 100), sf2b::g(sf2b::fineTune, 90), sf2b::range(sf2b::velRange, 20, 100),
                                              sf2b::g(sf2b::initialAttenuation, 1400)})};
    bb.presets[0].zones = {sf2b::pzone(0, {sf2b::g(sf2b::coarseTune, 50), sf2b::g(sf2b::fineTune, 50), sf2b::range(sf2b::velRange, 50, 127),
                                          sf2b::g(sf2b::initialAttenuation, 500)})};
  });
  CHECK(a.ok() && a.presets[0].regions.size() == 1);
  if (a.ok() && !a.presets[0].regions.empty()) {
    const Region& x = a.presets[0].regions[0];
    CHECK(x.transpose == 120 && x.tune_cents == 99.f && x.vel_lo == 50 && x.vel_hi == 100);
    CHECK(near(x.volume_db, -144.0));  // 1440 cB clamped, then the volume floor of the model
  }
  // Root key: overridingRootKey, else the sample's pitch, 255 = 60; pitch correction in cents adds to fineTune.
  auto rk = tiny([](sf2b::Builder& bb) { bb.samples[0].pitch = 255; bb.samples[0].correction = -30; });
  CHECK(rk.ok() && rk.presets[0].regions[0].key_center == 60 && rk.presets[0].regions[0].tune_cents == -30.f);
  // sampleModes 2 is no loop; 0 and 1/3 as documented; coarse offsets are 32768 frames.
  auto sm = tiny([](sf2b::Builder& bb) {
    bb.samples[0].pcm.resize(70000, 5);
    bb.instruments[0].zones = {sf2b::izone(0, {sf2b::g(sf2b::sampleModes, 2)}),
                               sf2b::izone(0, {sf2b::g(sf2b::startAddrsCoarseOffset, 1), sf2b::g(sf2b::startAddrsOffset, 7),
                                              sf2b::g(sf2b::sampleModes, 1), sf2b::g(sf2b::endloopAddrsCoarseOffset, -1)})};
    bb.samples[0].loop_start = 1000;
    bb.samples[0].loop_end = 69000;
  });
  CHECK(sm.ok() && sm.presets[0].regions.size() == 2);
  if (sm.ok() && sm.presets[0].regions.size() == 2) {
    CHECK(sm.presets[0].regions[0].loop_mode == LoopMode::kNoLoop && !sm.presets[0].regions[0].has_loop_points);
    const Region& x = sm.presets[0].regions[1];
    CHECK(x.offset == 32768 + 7 && x.loop_start == 1000 && x.loop_end == 69000 - 32768 - 1);
  }
  // Envelope anchors: timecents 0 = 1 s, -12000 and below = 0 (instant), 8000 = about 100 s (capped), sustain 1000 cB = 0.
  auto ev = tiny([](sf2b::Builder& bb) {
    bb.instruments[0].zones = {sf2b::izone(0, {sf2b::g(sf2b::attackVolEnv, 0), sf2b::g(sf2b::holdVolEnv, -12000), sf2b::g(sf2b::delayVolEnv, 1200),
                                              sf2b::g(sf2b::decayVolEnv, 8000), sf2b::g(sf2b::sustainVolEnv, 1000), sf2b::g(sf2b::releaseVolEnv, 8000)})};
  });
  CHECK(ev.ok());
  if (ev.ok()) {
    const AmpEnvelope& e = ev.presets[0].regions[0].ampeg;
    CHECK(e.attack == 1.f && e.hold == 0.f && e.delay == 2.f && near(e.decay, 78.173, 1e-3) && near(e.release, 78.173, 1e-3) && e.sustain == 0.f);
  }
  // Filter anchors: 13500 = none (even with a Q, whose gain reduction of q/20 dB still applies), 1500 cents = 19.4 Hz,
  // the model's Q is q/10 - 3.01 dB clamped to 0..40.
  auto fl = tiny([](sf2b::Builder& bb) {
    bb.instruments[0].zones = {sf2b::izone(0, {sf2b::g(sf2b::initialFilterQ, 300)}),
                               sf2b::izone(0, {sf2b::g(sf2b::initialFilterFc, 1500), sf2b::g(sf2b::initialFilterQ, 960)})};
  });
  CHECK(fl.ok() && fl.presets[0].regions.size() == 2);
  if (fl.ok() && fl.presets[0].regions.size() == 2) {
    CHECK(fl.presets[0].regions[0].filter.type == FilterType::kNone && fl.presets[0].regions[0].filter.resonance_db == 0.f);
    CHECK(near(fl.presets[0].regions[0].volume_db, -3.0103 - 15.0));  // 300 cB: 30 dB peak, half of it taken off the level
    CHECK(near(fl.presets[0].regions[1].filter.cutoff_hz, 19.4, 5e-3) && fl.presets[0].regions[1].filter.resonance_db == 40.f);
    CHECK(near(fl.presets[0].regions[1].volume_db, -3.0103 - 48.0));
  }
  auto q0 = tiny([](sf2b::Builder& bb) { bb.instruments[0].zones = {sf2b::izone(0, {sf2b::g(sf2b::initialFilterFc, 4000), sf2b::g(sf2b::initialFilterQ, 20), sf2b::g(sf2b::initialAttenuation, 100)})}; });
  CHECK(q0.ok());
  if (q0.ok()) {
    const Region& x = q0.presets[0].regions[0];
    CHECK(x.filter.type == FilterType::kLpf2p && x.filter.resonance_db == 0.f);  // 2 dB peak: below Butterworth + 3.01, clamped to Q = 1
    CHECK(near(x.volume_db, -3.0103 - 10.0 - 1.0));
  }
  // The pan law compensation: a centred mono zone has volume kSf2PanLawDb.
  auto pc = tiny([](sf2b::Builder&) {});
  CHECK(pc.ok() && pc.presets[0].regions[0].volume_db == kSf2PanLawDb && pc.presets[0].regions[0].pan == 0.f);
  // A preset without regions is still an (empty) instrument; two presets may share an instrument.
  auto ep = tiny([](sf2b::Builder& bb) {
    sf2b::add_preset(bb, "Empty", 0, 1, {});
    sf2b::add_preset(bb, "Again", 3, 7, {sf2b::pzone(0), sf2b::pzone(0)});
  });
  // An empty preset is not published as an instrument: it is listed with the code ignored_empty_preset.
  CHECK(ep.ok() && ep.presets.size() == 2 && ep.stats.ignored_empty_presets == 1 && ep.warnings.size() == 1);
  CHECK(ep.warnings[0].code == "ignored_empty_preset" && ep.warnings[0].preset == "0:1");
  CHECK(ep.presets[1].key == "3:7" && ep.presets[1].regions.size() == 2 && ep.presets[1].samples.size() == 1);
  // A preset zone playing an instrument without sample zones yields nothing, an empty preset zone list too.
  auto ns = tiny([](sf2b::Builder& bb) { bb.instruments[0].zones = {sf2b::gzone({sf2b::g(sf2b::pan, 10)})}; });
  CHECK(ns.ok() && ns.presets.empty() && ns.stats.ignored_empty_presets == 1);
}

void names_tests() {
  auto n = tiny([](sf2b::Builder& bb) {
    bb.presets[0].name = std::string("Caf\xE9 \x01x  ");  // Latin-1 e-acute, a control character, trailing blanks
    bb.samples[0].name = "";
    bb.presets.push_back({});
    bb.presets.back().name = "";
    bb.presets.back().program = 5;
    bb.presets.back().zones = {sf2b::pzone(0)};
    sf2b::Preset u;
    u.name = "\xC3\xA9t\xC3\xA9";  // valid UTF-8 stays
    u.program = 6;
    u.zones = {sf2b::pzone(0)};
    bb.presets.push_back(u);
    sf2b::Preset bad;
    bad.name = "\xC3(\xFF\xFE";  // invalid sequences become Latin-1 characters
    bad.program = 7;
    bad.zones = {sf2b::pzone(0)};
    bb.presets.push_back(bad);
  });
  CHECK(n.ok() && n.presets.size() == 4);
  if (!n.ok()) return;
  CHECK(n.presets[0].name == "Caf\xC3\xA9  x");  // the control character became a blank, trailing blanks are gone
  CHECK(n.presets[1].name == "0:5");                 // empty name -> the key
  CHECK(n.presets[2].name == "\xC3\xA9t\xC3\xA9");
  CHECK(n.presets[0].samples[0].name == "sample 0");
  for (const auto& p : n.presets) {  // every name is valid UTF-8
    for (std::size_t i = 0; i < p.name.size();) {
      const unsigned char c = static_cast<unsigned char>(p.name[i]);
      std::size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
      CHECK(k != 0 && i + k <= p.name.size());
      if (k == 0) break;
      for (std::size_t j = 1; j < k; ++j) CHECK((static_cast<unsigned char>(p.name[i + j]) & 0xC0) == 0x80);
      i += k;
    }
  }
}

void unsupported_tests() {
  auto kinds = [](const Sf2ParseResult& r) {
    std::set<std::string> s;
    for (const auto& i : r.issues) s.insert(i.kind == Sf2IssueKind::kUnsupported ? "unsupported:" + i.item : "damaged");
    return s;
  };
  auto rom = tiny([](sf2b::Builder& bb) { bb.samples[0].type = 0x8001; });
  CHECK(!rom.ok() && !rom.damaged() && kinds(rom) == std::set<std::string>{"unsupported:sample"});
  auto rate = tiny([](sf2b::Builder& bb) { bb.samples[0].rate = 22050; });
  CHECK(!rate.ok() && !rate.damaged() && kinds(rate) == std::set<std::string>{"unsupported:sample"});
  CHECK(!rate.issues.empty() && rate.issues[0].message.find("22050") != std::string::npos);
  auto ver = tiny([](sf2b::Builder& bb) { bb.ifil_major = 3; });
  CHECK(!ver.ok() && !ver.damaged() && kinds(ver) == std::set<std::string>{"unsupported:ifil"});
  // An unreferenced bad sample is harmless.
  auto unref = tiny([](sf2b::Builder& bb) {
    sf2b::add_sample(bb, "rom", 100, 5, 48000, 60, 0);
    bb.samples.back().type = 0x8001;
    sf2b::add_sample(bb, "slow", 100, 5, 8000, 60, 0);
  });
  CHECK(unref.ok() && unref.presets[0].regions.size() == 1);
  // Several unsupported samples are all listed; damage wins over unsupported.
  auto two = tiny([](sf2b::Builder& bb) {
    sf2b::add_sample(bb, "slow", 100, 5, 8000, 60, 0);
    bb.instruments[0].zones = {sf2b::izone(0), sf2b::izone(1)};
    bb.samples[0].rate = 11025;
  });
  CHECK(!two.ok() && two.issues.size() == 2);
  auto both = tiny([](sf2b::Builder& bb) {
    sf2b::add_sample(bb, "slow", 100, 5, 8000, 60, 0);
    bb.samples[0].type = 3;  // invalid: structural damage
    bb.instruments[0].zones = {sf2b::izone(0), sf2b::izone(1)};
  });
  CHECK(!both.ok() && both.damaged() && both.issues.size() == 1);
  // 44.1, 88.2 and 96 kHz are accepted.
  for (std::uint32_t hz : {44100u, 88200u, 96000u}) {
    auto ok = tiny([hz](sf2b::Builder& bb) { bb.samples[0].rate = hz; });
    CHECK(ok.ok() && ok.presets[0].samples[0].format.sample_rate == hz);
  }
}

// --- damage ----------------------------------------------------------------------------------------

struct Damage {
  const char* name;
  void (*mutate)(Bytes&, const sf2b::Layout&);
};

void rename4(Bytes& f, std::size_t at, const char* id) { std::memcpy(&f[at], id, 4); }

void damage_tests() {
  const sf2b::Fixture fx = sf2b::make_gm(false);
  sf2b::Builder b = fx.b;
  const Bytes good = b.build();
  const sf2b::Layout L = b.layout;
  CHECK(parse(good).ok());

  const Damage cases[] = {
      {"riff size beyond the file", [](Bytes& f, const sf2b::Layout& l) { sf2b::put32(f, l.riff_size, 0x7fffff00u); }},
      {"riff size one past the file", [](Bytes& f, const sf2b::Layout& l) { sf2b::put32(f, l.riff_size, static_cast<std::uint32_t>(f.size() - 7)); }},
      {"riff size tiny", [](Bytes& f, const sf2b::Layout& l) { sf2b::put32(f, l.riff_size, 2); }},
      {"not RIFF", [](Bytes& f, const sf2b::Layout&) { f[0] = 'X'; }},
      {"not sfbk", [](Bytes& f, const sf2b::Layout&) { f[11] = 'x'; }},
      {"shorter than a header", [](Bytes& f, const sf2b::Layout&) { f.resize(7); }},
      {"truncated in pdta", [](Bytes& f, const sf2b::Layout& l) { f.resize(l.data.at("pgen") + 6); }},
      {"truncated in smpl", [](Bytes& f, const sf2b::Layout& l) { f.resize(l.data.at("smpl") + 100); }},
      {"sdta size beyond the file", [](Bytes& f, const sf2b::Layout& l) { sf2b::put32(f, l.hdr.at("LIST:sdta") + 4, 0x40000000u); }},
      {"smpl size beyond its list", [](Bytes& f, const sf2b::Layout& l) { sf2b::put32(f, l.hdr.at("smpl") + 4, 0x7ffffff0u); }},
            {"LIST shorter than its type", [](Bytes& f, const sf2b::Layout& l) { sf2b::put32(f, l.hdr.at("LIST:INFO") + 4, 2); }},
      {"missing ifil", [](Bytes& f, const sf2b::Layout& l) { rename4(f, l.hdr.at("ifil"), "ifiX"); }},
      {"ifil wrong size", [](Bytes& f, const sf2b::Layout& l) { sf2b::put32(f, l.hdr.at("ifil") + 4, 2); }},
      {"missing shdr", [](Bytes& f, const sf2b::Layout& l) { rename4(f, l.hdr.at("shdr"), "shdX"); }},
      {"missing igen", [](Bytes& f, const sf2b::Layout& l) { rename4(f, l.hdr.at("igen"), "igeX"); }},
      {"repeated phdr", [](Bytes& f, const sf2b::Layout& l) { rename4(f, l.hdr.at("pbag"), "phdr"); }},
      {"missing sdta", [](Bytes& f, const sf2b::Layout& l) { rename4(f, l.hdr.at("LIST:sdta") + 8, "sdtX"); }},
      {"phdr size not a multiple", [](Bytes& f, const sf2b::Layout& l) { sf2b::put32(f, l.hdr.at("phdr") + 4, 38 * 7 + 1); }},
      {"pbag size mismatch", [](Bytes& f, const sf2b::Layout& l) { sf2b::put32(f, l.hdr.at("pbag") + 4, 5); }},
      {"inst size zero (no terminal)", [](Bytes& f, const sf2b::Layout& l) { sf2b::put32(f, l.hdr.at("inst") + 4, 0); }},
      {"shdr size not a multiple", [](Bytes& f, const sf2b::Layout& l) { sf2b::put32(f, l.hdr.at("shdr") + 4, 46 * 3 + 2); }},
      {"phdr terminal index wrong", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.phdr(6) + 24, 1); }},
      {"phdr index beyond pbag", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.phdr(2) + 24, 900); }},
      {"phdr indexes go backwards", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.phdr(1) + 24, 9); }},
      {"pbag generator index beyond pgen", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.pbag(1), 0xFFF); }},
      {"pbag modulator index beyond pmod", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.pbag(1) + 2, 5); }},
      {"pbag terminal generator index", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.pbag(8), 0); }},
      {"inst bag index beyond ibag", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.inst(1) + 20, 800); }},
      {"inst terminal index", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.inst(4) + 20, 2); }},
      {"ibag generator index beyond igen", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.ibag(2), 0x7000); }},
      {"ibag modulator index beyond imod", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.ibag(2) + 2, 3); }},
      {"ibag terminal modulator index", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.ibag(10) + 2, 1); }},
      {"preset zone instrument out of range", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, find_gen(f, l, "pgen", 41) + 2, 99); }},
      {"sampleID out of range", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, find_gen(f, l, "igen", 53) + 2, 99); }},
      {"duplicate bank:preset", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.phdr(1) + 20, 0); sf2b::put16(f, l.phdr(1) + 22, 0); }},
      {"sample start after end", [](Bytes& f, const sf2b::Layout& l) { sf2b::put32(f, l.shdr(0) + 20, 5000); }},
      {"sample end beyond sdta", [](Bytes& f, const sf2b::Layout& l) { sf2b::put32(f, l.shdr(1) + 24, 0x7fffffffu); }},
      {"invalid sampleType", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.shdr(0) + 44, 3); }},
      {"sampleLink out of range", [](Bytes& f, const sf2b::Layout& l) { sf2b::put16(f, l.shdr(2) + 42, 400); }},
  };
  for (const Damage& d : cases) {
    Bytes f = good;
    d.mutate(f, L);
    const Sf2ParseResult r = parse(f);
    const bool ok = !r.ok() && r.damaged() && r.issues.size() == 1 && !r.issues[0].message.empty() &&
                    r.presets.empty() && r.warnings.empty() && !r.samples;
    if (!ok) std::printf("  damage case '%s': ok=%d damaged=%d issues=%zu %s\n", d.name, r.ok(), r.damaged(), r.issues.size(),
                         r.issues.empty() ? "" : r.issues[0].message.c_str());
    CHECK(ok);
  }

  // Damage that needs a different base file.
  {  // sm24 of the wrong size, and sm24 without smpl data on a 16-bit file
    sf2b::Fixture f24 = sf2b::make_gm(true);
    Bytes f = f24.b.build();
    CHECK(parse(f).ok());
    sf2b::put32(f, f24.b.layout.hdr.at("sm24") + 4, 40);
    const auto r = parse(f);
    CHECK(!r.ok() && r.damaged());
  }
  {  // zero presets
    sf2b::Builder e;
    sf2b::add_sample(e, "s", 100, 5, 48000, 60, 0);
    const auto r = parse(e.build());
    CHECK(!r.ok() && r.damaged());
  }
  {  // model limits are not damage: such a zone is dropped with a structured warning, the file goes on
    auto drops = [](const Sf2ParseResult& r, const char* needle) {
      for (const auto& w : r.warnings)
        if (w.code == "ignored_zone" && w.preset == "0:0" && w.item.find(needle) != std::string::npos) return true;
      return false;
    };
    auto lp = tiny([](sf2b::Builder& bb) {
      bb.instruments[0].zones = {sf2b::izone(0, {sf2b::g(sf2b::sampleModes, 1), sf2b::g(sf2b::endloopAddrsCoarseOffset, 1)}), sf2b::izone(0)};
    });
    CHECK(lp.ok() && lp.presets.size() == 1 && lp.presets[0].regions.size() == 1 && lp.stats.ignored_zones == 1 && drops(lp, "loop points"));
    auto lp2 = tiny([](sf2b::Builder& bb) {
      bb.instruments[0].zones = {sf2b::izone(0, {sf2b::g(sf2b::sampleModes, 3), sf2b::g(sf2b::startloopAddrsOffset, -1)}), sf2b::izone(0)};
    });
    CHECK(lp2.ok() && lp2.presets[0].regions.size() == 1 && drops(lp2, "loop points"));
    auto so = tiny([](sf2b::Builder& bb) { bb.instruments[0].zones = {sf2b::izone(0, {sf2b::g(sf2b::startAddrsOffset, 1000)}), sf2b::izone(0)}; });
    CHECK(so.ok() && so.presets[0].regions.size() == 1 && drops(so, "start offset"));
    auto sn = tiny([](sf2b::Builder& bb) { bb.instruments[0].zones = {sf2b::izone(0, {sf2b::g(sf2b::startAddrsOffset, -1)})}; });
    CHECK(sn.ok() && sn.presets.empty() && sn.stats.ignored_empty_presets == 1 && drops(sn, "start offset"));
    // validate_region failure (a loop inside a sample whose shdr loop is inverted is a zone problem too)
    auto bad_loop = tiny([](sf2b::Builder& bb) {
      bb.samples[0].loop_start = 900;
      bb.samples[0].loop_end = 100;
      bb.instruments[0].zones = {sf2b::izone(0, {sf2b::g(sf2b::sampleModes, 1)}), sf2b::izone(0)};
    });
    CHECK(bad_loop.ok() && bad_loop.presets[0].regions.size() == 1 && bad_loop.stats.ignored_zones == 1);
    // A loop that is damaged on an unlooped zone is irrelevant.
    auto ok = tiny([](sf2b::Builder& bb) { bb.samples[0].loop_start = 900; bb.samples[0].loop_end = 10; });
    CHECK(ok.ok() && ok.warnings.empty());
  }
  {  // an empty referenced sample
    auto es = tiny([](sf2b::Builder& bb) { bb.samples[0].pcm.clear(); });
    CHECK(!es.ok() && es.damaged());
  }
  {  // a preset zone x instrument zone explosion is refused before it is expanded
    sf2b::Builder e;
    sf2b::add_sample(e, "s", 100, 5, 48000, 60, 0);
    sf2b::Instrument in;
    in.name = "I";
    for (int i = 0; i < 2100; ++i) in.zones.push_back(sf2b::izone(0));
    e.instruments = {in};
    sf2b::Preset p;
    p.name = "P";
    for (int i = 0; i < 2100; ++i) p.zones.push_back(sf2b::pzone(0));
    e.presets = {p};
    const auto r = parse(e.build());
    CHECK(!r.ok() && r.damaged() && r.issues[0].message.find("combinations") != std::string::npos);
  }
  {  // more regions than the documented bound
    sf2b::Builder e;
    sf2b::add_sample(e, "s", 100, 5, 48000, 60, 0);
    sf2b::Instrument in;
    in.name = "I";
    for (int i = 0; i < 700; ++i) in.zones.push_back(sf2b::izone(0));
    e.instruments = {in};
    sf2b::Preset p;
    p.name = "P";
    for (int i = 0; i < 800; ++i) p.zones.push_back(sf2b::pzone(0));
    e.presets = {p};
    const auto r = parse(e.build());
    CHECK(!r.ok() && r.damaged() && r.issues[0].message.find("regions") != std::string::npos);
  }
  {  // a zone count that stays inside the bounds still imports
    sf2b::Builder e;
    sf2b::add_sample(e, "s", 100, 5, 48000, 60, 0);
    sf2b::Instrument in;
    in.name = "I";
    for (int i = 0; i < 50; ++i) in.zones.push_back(sf2b::izone(0));
    e.instruments = {in};
    sf2b::add_preset(e, "P", 0, 0, {sf2b::pzone(0), sf2b::pzone(0)});
    const auto r = parse(e.build());
    CHECK(r.ok() && r.presets[0].regions.size() == 100);
  }
  {  // unreadable files
    const auto r = parse_sf2_file("/nonexistent/dir/x.sf2");
    CHECK(!r.ok() && r.damaged() && !r.issues[0].message.empty());
    const auto d = parse_sf2_file("/tmp");
    CHECK(!d.ok() && d.damaged());
  }
  {  // trailing bytes after the RIFF chunk are ignored; unknown chunks and lists are skipped
    Bytes f = good;
    f.insert(f.end(), {'j', 'u', 'n', 'k', 1, 2, 3});
    CHECK(parse(f).ok());
  }
}

// --- fuzz ------------------------------------------------------------------------------------------

struct Xs {
  std::uint64_t s;
  std::uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
};

void fuzz_tests() {
  sf2b::Fixture fx = sf2b::make_gm(true);
  const Bytes good = fx.b.build();
  const sf2b::Layout L = fx.b.layout;
  const std::size_t pdta = L.hdr.at("LIST:pdta");
  Xs rng{0x9e3779b97f4a7c15ull};
  std::size_t ok_count = 0, damaged_count = 0;
  for (int iter = 0; iter < 6000; ++iter) {
    Bytes f = good;
    const unsigned muts = 1 + static_cast<unsigned>(rng.next() % 4);
    for (unsigned m = 0; m < muts; ++m) {
      const std::uint64_t kind = rng.next() % 10;
      // Most mutations land in the chunk headers and the tables, where the structure lives.
      std::size_t pos = (rng.next() % 5 < 4) ? pdta + rng.next() % (f.size() - pdta) : rng.next() % 300;
      if (rng.next() % 11 == 0) pos = rng.next() % f.size();
      if (pos >= f.size()) pos = f.size() - 1;
      switch (kind) {
        case 0: case 1: case 2: f[pos] ^= static_cast<std::uint8_t>(1u << (rng.next() % 8)); break;
        case 3: case 4: f[pos] = static_cast<std::uint8_t>(rng.next()); break;
        case 5: if (pos + 1 < f.size()) sf2b::put16(f, pos, static_cast<unsigned>(rng.next())); break;
        case 6: if (pos + 3 < f.size()) sf2b::put32(f, pos, static_cast<std::uint32_t>(rng.next())); break;
        case 7: if (pos + 3 < f.size()) sf2b::put32(f, pos, 0xFFFFFFFFu); break;
        case 8: f.resize(pos + 1); break;
        default: if (pos + 1 < f.size()) sf2b::put16(f, pos, 0); break;
      }
    }
    const Sf2ParseResult r = parse(f);
    if (r.ok()) {
      ++ok_count;
      CHECK(r.samples != nullptr);
      std::vector<std::int32_t> pcm;
      for (const auto& p : r.presets) {
        for (const auto& reg : p.regions) {
          CHECK(reg.sample < p.samples.size());
          if (reg.sample < p.samples.size()) CHECK(validate_region(reg, p.samples[reg.sample].frames) == Error::kNone);
        }
        for (const auto& s : p.samples) CHECK_ERR(r.samples->read(s, pcm), Error::kNone);
      }
    } else {
      ++damaged_count;
      CHECK(!r.issues.empty() && r.presets.empty() && r.warnings.empty());
    }
  }
  CHECK(damaged_count > 1000 && ok_count > 0);  // the mutations really exercise both outcomes
  // Every truncation point of a small file.
  sf2b::Builder small;
  sf2b::add_sample(small, "s", 300, 6, 48000, 60, 0);
  sf2b::Instrument in;
  in.name = "I";
  in.zones = {sf2b::izone(0)};
  small.instruments = {in};
  sf2b::add_preset(small, "P", 0, 0, {sf2b::pzone(0)});
  const Bytes sb = small.build();
  for (std::size_t n = 0; n < sb.size(); ++n) {
    Bytes cut(sb.begin(), sb.begin() + static_cast<std::ptrdiff_t>(n));
    CHECK(!parse(cut).ok());
  }
  CHECK(parse(sb).ok());
  // Random garbage never crashes.
  for (int i = 0; i < 200; ++i) {
    Bytes junk(static_cast<std::size_t>(rng.next() % 600));
    for (auto& c : junk) c = static_cast<std::uint8_t>(rng.next());
    if (i % 2 && junk.size() > 12) std::memcpy(junk.data(), "RIFF\0\0\0\0sfbk", 12);
    CHECK(!parse(junk).ok());
  }
}


// --- robustness of the container and of lazily examined samples ---------------------------------------

void tolerance_tests() {
  sf2b::Builder b;
  sf2b::add_sample(b, "s", 1000, 20, 48000, 60, 0);
  sf2b::add_sample(b, "unused", 500, 10, 48000, 60, 0);
  sf2b::Instrument in;
  in.name = "I";
  in.zones = {sf2b::izone(0)};
  b.instruments = {in};
  sf2b::add_preset(b, "P", 0, 0, {sf2b::pzone(0)});
  Bytes good = b.build();
  const sf2b::Layout L = b.layout;
  CHECK(parse(good).ok());
  {  // an UNREFERENCED sample with broken geometry does not damage the file
    Bytes f = good;
    sf2b::put32(f, L.shdr(1) + 20, 0x7ffffff0u);   // start beyond everything
    sf2b::put32(f, L.shdr(1) + 24, 3);             // and after its end
    sf2b::put16(f, L.shdr(1) + 44, 3);             // and an invalid type
    const auto r = parse(f);
    CHECK(r.ok() && r.presets.size() == 1);
    sf2b::put32(f, L.shdr(0) + 20, 0x7ffffff0u);   // the referenced one is damaged
    CHECK(parse(f).damaged());
  }
  {  // fewer than 8 bytes of slack at the end of a container (here: pdta, the last list) are tolerated
    const std::size_t pd = L.hdr.at("LIST:pdta") + 4;
    for (unsigned extra : {1u, 3u, 7u}) {
      Bytes g = good;
      g.insert(g.end(), extra, 0);
      auto sz = [&](std::size_t at, unsigned d) { std::uint32_t v = std::uint32_t(g[at]) | (std::uint32_t(g[at + 1]) << 8) | (std::uint32_t(g[at + 2]) << 16) | (std::uint32_t(g[at + 3]) << 24); sf2b::put32(g, at, v + d); };
      sz(pd, extra);
      sz(L.riff_size, extra);
      CHECK(parse(g).ok());
    }
  }
  {  // an odd byte size of smpl loses its last byte (the 46 padding points are the last)
    Bytes f = good;
    sf2b::put32(f, L.hdr.at("smpl") + 4, static_cast<std::uint32_t>(L.smpl_frames * 2 - 1));
    CHECK(parse(f).ok());
  }
  {  // a file whose presets are all empty is usable but has no preset
    auto e = tiny([](sf2b::Builder& bb) { bb.instruments[0].zones = {sf2b::gzone({})}; });
    CHECK(e.ok() && e.presets.empty() && e.stats.ignored_empty_presets == 1);
  }
  // An odd number of sample points with 24-bit data: sm24 padded to an even size (specification) or exactly n bytes.
  for (bool pad : {true, false}) {
    sf2b::Builder bb;
    bb.bits24 = true;
    bb.sm24_pad = pad;
    sf2b::add_sample(bb, "s", 1001, 20, 48000, 60, 0);  // 1001 + 46 points: odd
    sf2b::Instrument oi;
    oi.name = "I";
    oi.zones = {sf2b::izone(0)};
    bb.instruments = {oi};
    sf2b::add_preset(bb, "P", 0, 0, {sf2b::pzone(0)});
    const Bytes bytes = bb.build();  // must outlive the result: the sample handle reads from it
    const auto o = parse(bytes);
    CHECK(o.ok() && o.presets.size() == 1 && o.stats.bits24);
    if (!o.ok() || o.presets.empty()) continue;
    std::vector<std::int32_t> pcm;
    CHECK_ERR(o.samples->read(o.presets[0].samples[0], pcm), Error::kNone);
    const auto want = bb.samples[0].pcm;
    CHECK(pcm.size() == 1001);
    CHECK(pcm == want);
    CHECK(!pcm.empty() && pcm.back() == want.back());  // the last frame is bit exact
    if (pcm != want)
      for (std::size_t k = 0; k < pcm.size() && k < want.size(); ++k)
        if (pcm[k] != want[k]) { std::printf("  pad=%d: first difference at %zu: %d vs %d\n", pad, k, pcm[k], want[k]); break; }
  }
}

// --- global-zone ranges ------------------------------------------------------------------------------

void global_range_tests() {
  using namespace sf2b;
  auto run = [](std::vector<Zone> inst_zones, std::vector<Zone> preset_zones) {
    return tiny([&](Builder& bb) {
      bb.instruments[0].zones = inst_zones;
      bb.presets[0].zones = preset_zones;
    });
  };
  // Instrument global range 40..80 / velocity 10..100: a zone without its own range inherits both, a zone with its own key range overrides the
  // key range only.
  auto r = run({gzone({range(keyRange, 40, 80), range(velRange, 10, 100)}), izone(0), izone(0, {range(keyRange, 50, 60)})}, {pzone(0)});
  CHECK(r.ok() && r.presets[0].regions.size() == 2);
  if (r.ok() && r.presets[0].regions.size() == 2) {
    const auto& a = r.presets[0].regions[0];
    const auto& b = r.presets[0].regions[1];
    CHECK(a.key_lo == 40 && a.key_hi == 80 && a.vel_lo == 10 && a.vel_hi == 100);
    CHECK(b.key_lo == 50 && b.key_hi == 60 && b.vel_lo == 10 && b.vel_hi == 100);
  }
  // Preset global range 45..70 / velocity 20..90 intersects with the instrument's; the preset zone inherits, another overrides.
  auto p = run({gzone({range(keyRange, 40, 80), range(velRange, 10, 100)}), izone(0)},
               {gzone({range(keyRange, 45, 70), range(velRange, 20, 90)}), pzone(0), pzone(0, {range(keyRange, 0, 48)}), pzone(0, {range(velRange, 95, 127)})});
  CHECK(p.ok() && p.presets[0].regions.size() == 3);
  if (p.ok() && p.presets[0].regions.size() == 3) {
    const auto& c = p.presets[0].regions[2];
    CHECK(c.key_lo == 45 && c.key_hi == 70 && c.vel_lo == 95 && c.vel_hi == 100);  // own velocity range replaces the preset global one
    const auto& a = p.presets[0].regions[0];
    const auto& b = p.presets[0].regions[1];
    CHECK(a.key_lo == 45 && a.key_hi == 70 && a.vel_lo == 20 && a.vel_hi == 90);   // inherits the preset global
    CHECK(b.key_lo == 40 && b.key_hi == 48 && b.vel_lo == 20 && b.vel_hi == 90);   // own key range overrides the global one, velocity inherits
  }
}

// --- the specification's units, from the specification text (no constant of the importer is used) ----------------

void spec_table_tests() {
  using namespace sf2b;
  // Section 8.1.2/8.1.3: initialAttenuation is in centibels (0.1 dB per unit); the engine's mono pan law is +3.01 dB above the
  // specification's (centre: sin(pi/4) per channel), which the region volume undoes; sustainVolEnv is centibels of attenuation below full
  // scale ("1000 indicates full attenuation": 100 dB); decayVolEnv and releaseVolEnv are timecents (2^(tc/1200) s) for a 100% change
  // of that 100 dB scale, and the engine's decay/release time is "nine time constants" (9 x 8.6859 dB = 78.17 dB per t); pan is in 0.1 %
  // units with -500 and 500 fully left and right.
  const double pi = 3.14159265358979323846;
  const double centre = 20.0 * std::log10(std::sin(pi / 4.0));   // dB the specification's pan law gives a centred mono sample, re 1.0
  const double nine_tau_db = 9.0 * 20.0 * std::log10(std::exp(1.0));
  auto t = tiny([](Builder& bb) {
    std::vector<Zone> z;
    for (int a : {0, 10, 100, 600, 1440}) z.push_back(izone(0, {g(initialAttenuation, a)}));
    for (int su : {0, 50, 144, 500, 999, 1000, 1440}) z.push_back(izone(0, {g(sustainVolEnv, su)}));
    for (int tc : {-12000, -2400, 0, 1200, 4000}) z.push_back(izone(0, {g(decayVolEnv, tc), g(releaseVolEnv, tc)}));
    for (int pn : {-500, -250, 0, 100, 500, 700}) z.push_back(izone(0, {g(pan, pn)}));
    bb.instruments[0].zones = z;
  });
  CHECK(t.ok() && t.presets[0].regions.size() == 5 + 7 + 5 + 6);
  if (!t.ok() || t.presets[0].regions.size() != 23) return;
  const auto& R = t.presets[0].regions;
  std::size_t i = 0;
  for (int a : {0, 10, 100, 600, 1440}) {
    CHECK(near(R[i].volume_db, std::max(-144.0, centre - a * 0.1)));  // 0.1 dB per centibel
    ++i;
  }
  i = 5;
  for (int su : {0, 50, 144, 500, 999, 1000, 1440}) {
    const double want = su >= 1000 ? 0.0 : 100.0 * std::pow(10.0, -su / 200.0);
    CHECK(near(R[i].ampeg.sustain, want, 1e-3));
    ++i;
  }
  i = 12;
  for (int tc : {-12000, -2400, 0, 1200, 4000}) {
    const double secs = tc <= -12000 ? 0.0 : std::pow(2.0, tc / 1200.0);
    const double want = secs * nine_tau_db / 100.0;
    CHECK(near(R[i].ampeg.decay, want) && near(R[i].ampeg.release, want));
    ++i;
  }
  i = 17;
  for (int pn : {-500, -250, 0, 100, 500, 700}) {
    const double want = 100.0 * std::clamp(pn / 500.0, -1.0, 1.0);
    CHECK(near(R[i].pan, want));
    ++i;
  }
  // The compensation is exactly the specification's centre gain: a zone with no attenuation sits at -3.0103 dB.
  CHECK(near(R[0].volume_db, centre));
}

// --- generator scanning is linear -----------------------------------------------------------------------

void complexity_test() {
  using namespace sf2b;
  Builder b;
  add_sample(b, "s", 1000, 20, 48000, 60, 0);
  Instrument in;
  in.name = "I";
  Preset p;
  p.name = "P";
  const int kZones = 300, kGens = 20;
  std::uint64_t total_gens = 0;
  auto filler = [&]() {
    std::vector<Gen> v;
    for (int k = 0; k < kGens; ++k) v.push_back(g(fineTune, 0));  // harmless: the default value
    total_gens += v.size() + 1;
    return v;
  };
  for (int i = 0; i < kZones; ++i) in.zones.push_back(izone(0, filler()));
  for (int i = 0; i < kZones; ++i) p.zones.push_back(pzone(0, filler()));
  b.instruments = {in};
  b.presets = {p};
  const auto r = parse(b.build());
  CHECK(r.ok() && r.presets.size() == 1 && r.presets[0].regions.size() == std::size_t(kZones) * kZones);
  // Every generator record is examined once for the instrument and twice for the preset (collection and expansion),
  // never once per zone pair (that would be 90000 x 21): a counted bound, no clock.
  CHECK(r.stats.generator_scans <= total_gens + total_gens / 2 + 2 * kZones);
  CHECK(r.stats.generator_scans > 0);
}

void file_test() {
  const fs::path dir = fs::temp_directory_path() / ("velora_sf2_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  sf2b::Fixture f = sf2b::make_gm(false);
  const Bytes bytes = f.b.build();
  {
    std::ofstream o(dir / "gm.sf2", std::ios::binary);
    o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  const auto r = parse_sf2_file((dir / "gm.sf2").string());
  CHECK(r.ok() && r.presets.size() == 6);
  const auto m = parse(bytes);
  CHECK(m.ok() && m.presets.size() == r.presets.size());
  for (std::size_t i = 0; i < r.presets.size() && i < m.presets.size(); ++i) {
    CHECK(r.presets[i].regions == m.presets[i].regions && r.presets[i].key == m.presets[i].key);
    std::vector<std::int32_t> a, b;
    for (std::size_t k = 0; k < r.presets[i].samples.size(); ++k) {
      CHECK_ERR(r.samples->read(r.presets[i].samples[k], a), Error::kNone);
      CHECK_ERR(m.samples->read(m.presets[i].samples[k], b), Error::kNone);
      CHECK(a == b);
    }
  }
  fs::remove_all(dir);
}

void generator_names() {
  CHECK(std::string(sf2_generator_name(0)) == "startAddrsOffset" && std::string(sf2_generator_name(8)) == "initialFilterFc");
  CHECK(std::string(sf2_generator_name(58)) == "overridingRootKey" && std::string(sf2_generator_name(43)) == "keyRange");
  CHECK(std::string(sf2_generator_name(53)) == "sampleID" && std::string(sf2_generator_name(41)) == "instrument");
  CHECK(sf2_generator_name(61) == nullptr);
}

}  // namespace

int main() {
  gm_tests(false);
  gm_tests(true);
  single_instrument_file();
  warnings_tests();
  semantics_tests();
  names_tests();
  unsupported_tests();
  damage_tests();
  fuzz_tests();
  tolerance_tests();
  global_range_tests();
  spec_table_tests();
  complexity_test();
  file_test();
  generator_names();
  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("format_sf2_test: all checks passed\n");
  return 0;
}
