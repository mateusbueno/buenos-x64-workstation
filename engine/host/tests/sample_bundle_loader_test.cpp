// Bundle loader for the sample engine (Story 1.15): bundle -> float PCM + region table -> SampleEngine.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "velora/core/sample_engine.hpp"
#include "velora/format/bundle.hpp"
#include "velora/host/sample_bundle_loader.hpp"

using namespace velora;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

static std::vector<std::uint8_t> make_bundle(bool corrupt_second_sample) {
  using namespace velora::format;
  BundleWriter w;
  const auto inst = w.add_instrument("Piano", "Piano.sfz");
  const std::size_t i = inst.value();

  // 0: 16-bit stereo 44.1 kHz, 6000 frames.
  std::vector<std::int32_t> a(6000 * 2);
  for (std::size_t f = 0; f < 6000; ++f) {
    a[f * 2] = static_cast<std::int32_t>(f % 2000) - 1000;
    a[f * 2 + 1] = -static_cast<std::int32_t>(f % 700);
  }
  a[0] = -32768;
  a[1] = 32767;
  (void)w.add_sample(i, {"a", {2, 16, false, 44100}, 6000, a});
  // 1: 24-bit mono 48 kHz, 5000 frames.
  std::vector<std::int32_t> b(5000);
  for (std::size_t f = 0; f < b.size(); ++f) b[f] = (static_cast<std::int32_t>(f) * 1597) % 8000000 - 4000000;
  b[0] = -8388608;
  b[1] = 8388607;
  (void)w.add_sample(i, {"b", {1, 24, false, 48000}, 5000, b});
  // 2: float32 mono, 3000 frames (exchange convention: IEEE bits).
  std::vector<std::int32_t> c(3000);
  for (std::size_t f = 0; f < c.size(); ++f) c[f] = float_to_bits(std::sin(0.01f * static_cast<float>(f)) * 0.5f);
  (void)w.add_sample(i, {"c", {1, 32, true, 48000}, 3000, c});

  Region r0;
  r0.sample = 0; r0.key_lo = 40; r0.key_hi = 60; r0.key_center = 50; r0.vel_lo = 1; r0.vel_hi = 100;
  r0.transpose = -3; r0.tune_cents = 12.5f; r0.volume_db = -4.0f; r0.pan = 30.0f; r0.offset = 20;
  r0.seq_length = 2; r0.seq_position = 2; r0.lorand = 0.25f; r0.hirand = 0.75f;
  r0.loop_mode = LoopMode::kLoopSustain; r0.has_loop_points = true; r0.loop_start = 100; r0.loop_end = 4000;
  r0.ampeg = {0.01f, 5.0f, 0.02f, 0.03f, 0.04f, 60.0f, 0.5f};
  r0.trigger = TriggerMode::kRelease;
  Region r1;
  r1.sample = 1; r1.key_lo = 61; r1.key_hi = 90; r1.key_center = 70; r1.loop_mode = LoopMode::kOneShot;
  Region r2;
  r2.sample = 2; r2.key_lo = 91; r2.key_hi = 100; r2.key_center = 95; r2.loop_mode = LoopMode::kLoopContinuous;
  r2.has_loop_points = true; r2.loop_start = 0; r2.loop_end = 2999;
  (void)w.add_region(i, r0);
  (void)w.add_region(i, r1);
  (void)w.add_region(i, r2);
  auto bytes = w.finalize();
  std::vector<std::uint8_t> out = bytes.take();
  if (corrupt_second_sample) {
    // Flip a bit in the stored data of the last block (the float sample is raw, so its bytes sit at the end).
    out[out.size() - 5] ^= 0x40;
  }
  return out;
}

static void test_load_and_convert() {
  const auto bytes = make_bundle(false);
  auto rd = format::BundleReader::open_memory(bytes);
  CHECK(rd.ok());
  const auto loaded = host::load_instrument(rd.value(), std::string_view("Piano.sfz"));
  CHECK(loaded.ok() && loaded.instrument);
  const host::BundleInstrument& ins = *loaded.instrument;
  CHECK(ins.sample_count() == 3 && ins.region_count() == 3 && ins.failed_samples() == 0);
  const auto view = ins.instrument();

  // Samples: lossless integer -> float (value / 2^(bits-1)), planar, rate and shape preserved.
  const ports::SamplePcm& a = view.samples[0];
  CHECK(a.channels == 2 && a.frames == 6000 && a.sample_rate == 44100 && a.left && a.right);
  CHECK(a.left[0] == -1.0f && a.right[0] == 32767.0f / 32768.0f);
  CHECK(a.left[5] == static_cast<float>(5 - 1000) / 32768.0f && a.right[5] == -5.0f / 32768.0f);
  const ports::SamplePcm& b = view.samples[1];
  CHECK(b.channels == 1 && b.frames == 5000 && b.right == nullptr);
  CHECK(b.left[0] == -1.0f && b.left[1] == 8388607.0f / 8388608.0f);
  CHECK(b.left[2] == static_cast<float>((2 * 1597) % 8000000 - 4000000) / 8388608.0f);
  const ports::SamplePcm& c = view.samples[2];
  CHECK(c.channels == 1 && c.frames == 3000);
  for (std::size_t f = 0; f < 3000; f += 97) CHECK(c.left[f] == std::sin(0.01f * static_cast<float>(f)) * 0.5f);  // bit exact
  CHECK(ins.resident_bytes() == (6000 * 2 + 5000 + 3000) * sizeof(float));

  // Region table: every field carried over.
  const ports::SampleRegionDesc& r = view.regions[0];
  CHECK(r.sample_index == 0 && r.key_lo == 40 && r.key_hi == 60 && r.key_center == 50 && r.vel_lo == 1 && r.vel_hi == 100);
  CHECK(r.transpose == -3 && r.tune_cents == 12.5f && r.volume_db == -4.0f && r.pan == 30.0f && r.offset == 20);
  CHECK(r.seq_length == 2 && r.seq_position == 2 && r.lorand == 0.25f && r.hirand == 0.75f);
  CHECK(r.loop_mode == ports::SampleLoopMode::kLoopSustain && r.has_loop_points && r.loop_start == 100 && r.loop_end == 4000);
  CHECK(r.ampeg_delay == 0.01f && r.ampeg_start == 5.0f && r.ampeg_attack == 0.02f && r.ampeg_hold == 0.03f &&
        r.ampeg_decay == 0.04f && r.ampeg_sustain == 60.0f && r.ampeg_release == 0.5f);
  CHECK(r.trigger == ports::SampleTrigger::kRelease);
  CHECK(view.regions[1].loop_mode == ports::SampleLoopMode::kOneShot && view.regions[1].trigger == ports::SampleTrigger::kAttack);
  CHECK(view.regions[2].loop_mode == ports::SampleLoopMode::kLoopContinuous);

  // Index and key lookups: exact match only, nothing substituted.
  CHECK(host::load_instrument(rd.value(), std::string_view("piano.sfz")).status == host::LoadStatus::kInstrumentNotFound);
  CHECK(host::load_instrument(rd.value(), std::string_view("")).status == host::LoadStatus::kInstrumentNotFound);
  CHECK(host::load_instrument_by_index(rd.value(), 1).status == host::LoadStatus::kInstrumentIndex);
  CHECK(!host::load_instrument_by_index(rd.value(), 1).instrument);
  CHECK(host::load_instrument_by_index(rd.value(), 0).ok());

  // Plays: the loaded float data goes through the engine untouched (mono 24-bit sample, region 1).
  core::SampleEngine::Config cfg;
  cfg.max_voices = 4;
  core::SampleEngine eng(cfg);
  CHECK(eng.set_instrument(ins) && eng.unplayable_regions() == 0);
  eng.start_voice(0, 70, (127u * 65535u + 63u) / 127u);
  float l[4] = {}, rr[4] = {};
  eng.render(l, rr, 4);
  const double g = std::cos(2047.0 * 1.57079632679489661923 / 4094.0) * 1.4125375446227544;
  CHECK(std::fabs(l[0] - b.left[0] * g) < 1e-6 && std::fabs(l[1] - b.left[1] * g) < 1e-6);
}

static void test_failed_sample_is_skipped() {
  const auto bytes = make_bundle(true);
  format::ReaderOptions opt;
  auto rd = format::BundleReader::open_memory(bytes, opt);
  CHECK(rd.ok());
  const auto loaded = host::load_instrument_by_index(rd.value(), 0);
  CHECK(loaded.ok());  // the instrument still loads: the bad sample is left empty
  CHECK(loaded.instrument->failed_samples() == 1);
  const auto view = loaded.instrument->instrument();
  CHECK(view.samples[2].frames == 0 && view.samples[2].left == nullptr);
  CHECK(view.samples[0].frames == 6000 && view.samples[1].frames == 5000);
  core::SampleEngine::Config cfg;
  core::SampleEngine eng(cfg);
  eng.set_instrument(*loaded.instrument);
  CHECK(eng.unplayable_regions() == 1);  // the region on the failed sample is skipped, never garbage
  CHECK(eng.start_voice(0, 95, 65535) == core::EngineStatus::kOk && eng.active_voices() == 0);
  CHECK(eng.skipped_unplayable_starts() == 1);
}


// Samples the engine must never see: zero frames and non-finite float data, crafted with the writer.
static void test_unusable_samples() {
  using namespace velora::format;
  BundleWriter w;
  const std::size_t i = w.add_instrument("X", "X.sfz").value();
  std::vector<std::int32_t> good(100, 1000), nanv(100), infv(100), empty;
  for (auto& b : nanv) b = float_to_bits(0.25f);
  nanv[40] = 0x7FC00000;                       // quiet NaN
  for (auto& b : infv) b = float_to_bits(0.25f);
  infv[7] = 0x7F800000;                        // +infinity
  CHECK(w.add_sample(i, {"good", {1, 16, false, 48000}, 100, good}) == Error::kNone);   // 0
  CHECK(w.add_sample(i, {"empty", {1, 16, false, 48000}, 0, empty}) == Error::kNone);   // 1
  CHECK(w.add_sample(i, {"nan", {1, 32, true, 48000}, 100, nanv}) == Error::kNone);     // 2
  CHECK(w.add_sample(i, {"inf", {1, 32, true, 48000}, 100, infv}) == Error::kNone);     // 3
  for (std::uint32_t s = 0; s < 4; ++s) {
    if (s == 1) continue;  // the writer refuses a region on a zero-frame sample
    Region r;
    r.sample = s; r.key_lo = r.key_hi = static_cast<std::uint8_t>(60 + s); r.key_center = 60;
    CHECK(w.add_region(i, r) == Error::kNone);
  }
  auto bytes = w.finalize();
  auto rd = BundleReader::open_memory(bytes.value());
  CHECK(rd.ok());
  const auto loaded = host::load_instrument(rd.value(), std::string_view("X.sfz"));
  CHECK(loaded.ok() && loaded.instrument->failed_samples() == 3);
  const auto v = loaded.instrument->instrument();
  CHECK(v.samples[0].frames == 100 && v.samples[0].left);
  for (int s = 1; s < 4; ++s) CHECK(v.samples[s].frames == 0 && v.samples[s].left == nullptr);
  core::SampleEngine::Config cfg;
  core::SampleEngine eng(cfg);
  eng.set_instrument(*loaded.instrument);
  CHECK(eng.unplayable_regions() == 2);
  for (std::uint32_t n = 62; n < 64; ++n) CHECK(eng.start_voice(0, n, 65535) == core::EngineStatus::kOk && eng.active_voices() == 0);
  CHECK(eng.start_voice(0, 60, 65535) == core::EngineStatus::kOk && eng.active_voices() == 1);
}

// The PCM converter's rejection branches (formats the writer itself refuses to produce).
static void test_pcm_to_float() {
  using format::PcmFormat;
  std::vector<float> out;
  const std::vector<std::int32_t> two = {1, 2}, four = {1, 2, 3, 4};
  CHECK(host::pcm_to_float({1, 16, false, 48000}, two, 2, out) && out[1] == 2.0f / 32768.0f);
  CHECK(host::pcm_to_float({2, 24, false, 48000}, four, 2, out) && out.size() == 4 && out[1] == 3.0f / 8388608.0f);  // planar
  CHECK(!host::pcm_to_float({1, 0, false, 48000}, two, 2, out));      // bits below 1
  CHECK(!host::pcm_to_float({1, 33, false, 48000}, two, 2, out));     // bits above 32
  CHECK(!host::pcm_to_float({1, 16, true, 48000}, two, 2, out));      // float flag with 16 bits
  CHECK(!host::pcm_to_float({3, 16, false, 48000}, two, 2, out));     // channel count
  CHECK(!host::pcm_to_float({1, 16, false, 48000}, two, 3, out));     // size mismatch
  CHECK(host::pcm_to_float({1, 32, false, 48000}, two, 2, out));      // 32-bit integers convert (inexactly, documented)
  const std::vector<std::int32_t> nanbits = {format::float_to_bits(1.0f), 0x7FC00000};
  CHECK(!host::pcm_to_float({1, 32, true, 48000}, nanbits, 2, out));
}

// Story 5.2: the Story 5.1 extension (keyswitches, CC crossfades, filter) reaches the sample-source port unchanged, and a region without
// it converts to the port's "not used" defaults.
static void test_extension_reaches_the_port() {
  using namespace velora::format;
  Region r;
  r.sw = {36, 39, 37, 40, 41, 60, 36};
  r.xf.in_count = 2;
  r.xf.out_count = 1;
  r.xf.curve = XfCurve::kGain;
  r.xf.in[0] = {1, 10, 60};
  r.xf.in[1] = {74, 0, 127};
  r.xf.out[0] = {11, 70, 120};
  r.filter = {FilterType::kBpf2p, 1234.5f, 7.5f};
  const ports::SampleRegionDesc d = host::to_port_region(r);
  CHECK(d.sw.lokey == 36 && d.sw.hikey == 39 && d.sw.last == 37 && d.sw.down == 40 && d.sw.up == 41 && d.sw.previous == 60 && d.sw.default_key == 36);
  CHECK(d.xfin_count == 2 && d.xfout_count == 1 && d.xf_curve == ports::SampleXfCurve::kGain);
  CHECK(d.xfin[0].cc == 1 && d.xfin[0].lo == 10 && d.xfin[0].hi == 60 && d.xfin[1].cc == 74 && d.xfin[1].hi == 127);
  CHECK(d.xfout[0].cc == 11 && d.xfout[0].lo == 70 && d.xfout[0].hi == 120);
  CHECK(d.filter_type == ports::SampleFilterType::kBpf2p && d.filter_cutoff_hz == 1234.5f && d.filter_resonance_db == 7.5f);
  for (auto t : {FilterType::kLpf1p, FilterType::kLpf2p, FilterType::kHpf1p, FilterType::kHpf2p}) {
    Region f;
    f.filter = {t, 500.0f, 0.0f};
    CHECK(static_cast<int>(host::to_port_region(f).filter_type) == static_cast<int>(t));  // the two enums agree value for value
  }
  Region many;
  many.xf.in_count = 5;  // more than the port holds: not truncated, so the engine can refuse the region
  CHECK(host::to_port_region(many).xfin_count > ports::kSampleMaxCcCrossfades);
  const ports::SampleRegionDesc plain = host::to_port_region(Region{});
  CHECK(plain.sw.last == -1 && plain.sw.lokey == -1 && plain.sw.default_key == -1 && plain.xfin_count == 0 && plain.xfout_count == 0 &&
        plain.filter_type == ports::SampleFilterType::kNone && plain.xf_curve == ports::SampleXfCurve::kPower);
  // Through a real bundle: write, read back, load.
  BundleWriter w;
  const auto inst = w.add_instrument("Ext", "Ext.sfz");
  std::vector<std::int32_t> pcm(2000, 1000);
  (void)w.add_sample(inst.value(), {"a", {1, 16, false, 48000}, 2000, pcm});
  Region e = r;
  e.sample = 0;
  e.key_lo = 0;
  e.key_hi = 127;
  e.key_center = 60;
  CHECK(w.add_region(inst.value(), e) == Error::kNone);
  auto fin = w.finalize();
  const std::vector<std::uint8_t> bytes = fin.take();
  auto rd = BundleReader::open_memory(bytes);
  CHECK(rd.ok());
  if (!rd.ok()) return;
  const auto loaded = host::load_instrument_by_index(rd.value(), 0);
  CHECK(loaded.ok());
  if (!loaded.ok()) return;
  const ports::SampleRegionDesc& g = loaded.instrument->instrument().regions[0];
  CHECK(g.sw.last == 37 && g.xfin_count == 2 && g.xfout[0].cc == 11 && g.filter_type == ports::SampleFilterType::kBpf2p && g.filter_cutoff_hz == 1234.5f);
}

static void test_trigger_and_end_reach_the_port() {
  using namespace velora::format;
  const std::pair<TriggerMode, ports::SampleTrigger> modes[] = {
      {TriggerMode::kAttack, ports::SampleTrigger::kAttack}, {TriggerMode::kRelease, ports::SampleTrigger::kRelease},
      {TriggerMode::kFirst, ports::SampleTrigger::kFirst}, {TriggerMode::kLegato, ports::SampleTrigger::kLegato},
      {TriggerMode::kReleaseKey, ports::SampleTrigger::kReleaseKey}};
  for (const auto& m : modes) {
    Region r;
    r.trigger = m.first;
    CHECK(host::to_port_region(r).trigger == m.second);
  }
  Region plain;
  CHECK(!host::to_port_region(plain).has_end && host::to_port_region(plain).end == 0);
  // Through a real bundle (format 2.2): write, read back, load.
  BundleWriter w;
  const auto inst = w.add_instrument("Trig", "Trig.sfz");
  std::vector<std::int32_t> pcm(2000, 1000);
  (void)w.add_sample(inst.value(), {"a", {1, 16, false, 48000}, 2000, pcm});
  Region a;
  a.trigger = TriggerMode::kReleaseKey;
  a.has_end = true;
  a.end = 1500;
  Region b;
  b.has_end = true;
  b.end = 0;
  CHECK(w.add_region(inst.value(), a) == Error::kNone && w.add_region(inst.value(), b) == Error::kNone);
  const std::vector<std::uint8_t> bytes = w.finalize().take();
  auto rd = BundleReader::open_memory(bytes);
  CHECK(rd.ok() && rd.value().format_minor() == 2);
  if (!rd.ok()) return;
  const auto loaded = host::load_instrument_by_index(rd.value(), 0);
  CHECK(loaded.ok());
  if (!loaded.ok()) return;
  const auto regs = loaded.instrument->instrument().regions;
  CHECK(regs.size() == 2 && regs[0].trigger == ports::SampleTrigger::kReleaseKey && regs[0].has_end && regs[0].end == 1500 && regs[1].has_end && regs[1].end == 0);
}

static void test_amp_map_reaches_the_port() {
  using namespace velora::format;
  Region r;
  r.amp.veltrack = -40.f; r.amp.amplitude = 85.f; r.amp.keytrack = 0.75f; r.amp.keycenter = 49;
  r.amp.vel2attack = 0.11f; r.amp.vel2decay = 0.22f; r.amp.vel2sustain = -33.f; r.amp.vel2release = 0.44f;
  r.amp.curve_count = 3;
  r.amp.curve[0] = {2, 0.1f}; r.amp.curve[1] = {70, 0.6f}; r.amp.curve[2] = {126, 0.9f};
  const ports::SampleRegionDesc d = host::to_port_region(r);
  CHECK(d.amp_veltrack == -40.f && d.amplitude == 85.f && d.amp_keytrack == 0.75f && d.amp_keycenter == 49);
  CHECK(d.vel2attack == 0.11f && d.vel2decay == 0.22f && d.vel2sustain == -33.f && d.vel2release == 0.44f);
  CHECK(d.vel_point_count == 3 && d.vel_points[0].vel == 2 && d.vel_points[0].gain == 0.1f && d.vel_points[1].vel == 70 &&
        d.vel_points[1].gain == 0.6f && d.vel_points[2].vel == 126 && d.vel_points[2].gain == 0.9f && d.vel_points[3].gain == 0.0f);
  const ports::SampleRegionDesc plain = host::to_port_region(Region{});
  CHECK(plain.amp_veltrack == 100.f && plain.amplitude == 100.f && plain.amp_keytrack == 0.f && plain.amp_keycenter == 60 && plain.vel_point_count == 0 &&
        plain.vel2attack == 0.f && plain.vel2decay == 0.f && plain.vel2sustain == 0.f && plain.vel2release == 0.f);
  // Through a real bundle (format 2.3).
  BundleWriter w;
  const auto inst = w.add_instrument("Amp", "Amp.sfz");
  std::vector<std::int32_t> pcm(2000, 1000);
  (void)w.add_sample(inst.value(), {"a", {1, 16, false, 48000}, 2000, pcm});
  CHECK(w.add_region(inst.value(), r) == Error::kNone);
  auto rd = BundleReader::open_memory(w.finalize().take());
  CHECK(rd.ok() && rd.value().format_minor() == 3);
  if (!rd.ok()) return;
  const auto loaded = host::load_instrument_by_index(rd.value(), 0);
  CHECK(loaded.ok());
  if (!loaded.ok()) return;
  const auto& g = loaded.instrument->instrument().regions[0];
  CHECK(g.vel2decay == 0.22f && g.vel2release == 0.44f && g.vel_point_count == 3 && g.vel_points[1].vel == 70 && g.amp_keycenter == 49);
}

static void test_cc_map_reaches_the_port() {
  using namespace velora::format;
  Region r;
  r.cc.gate_count = 2;
  r.cc.gates[0] = {64, 64.f, 100.f};
  r.cc.gates[1] = {70, 0.f, std::nextafter(11.f, 0.f)};
  r.cc.mod_count = 3;
  r.cc.mods[0] = {CcTarget::kAmplitude, 93, 7, 100.f};
  r.cc.mods[1] = {CcTarget::kTune, 131, 0, -50.f};
  r.cc.mods[2] = {CcTarget::kEgRelease, 141, 9, 1.5f};
  const ports::SampleRegionDesc d = host::to_port_region(r);
  CHECK(d.cc_gate_count == 2 && d.cc_gates[0].cc == 64 && d.cc_gates[0].lo == 64.f && d.cc_gates[0].hi == 100.f && d.cc_gates[1].cc == 70);
  CHECK(d.cc_mod_count == 3 && d.cc_mods[0].target == ports::SampleCcTarget::kAmplitude && d.cc_mods[0].cc == 93 && d.cc_mods[0].curve == 7 && d.cc_mods[0].depth == 100.f);
  CHECK(d.cc_mods[1].target == ports::SampleCcTarget::kTune && d.cc_mods[1].cc == 131 && d.cc_mods[2].target == ports::SampleCcTarget::kEgRelease && d.cc_mods[2].curve == 9);
  const ports::SampleRegionDesc plain = host::to_port_region(Region{});
  CHECK(plain.cc_gate_count == 0 && plain.cc_mod_count == 0);
  // Through a real bundle (format 2.4), with the instrument's CC defaults and curves.
  BundleWriter w;
  const auto inst = w.add_instrument("Cc", "Cc.sfz");
  std::vector<std::int32_t> pcm(2000, 1000);
  (void)w.add_sample(inst.value(), {"a", {1, 16, false, 48000}, 2000, pcm});
  CHECK(w.add_region(inst.value(), r) == Error::kNone);
  InstrumentCcSetup setup;
  setup.defaults = {{64, 90.f}};
  setup.curves = {{7, {{0, 0.f}, {64, 0.9f}, {127, 1.f}}}, {9, {{0, 1.f}, {127, 0.f}}}};
  CHECK(w.set_instrument_cc_setup(inst.value(), setup) == Error::kNone);
  const auto bundle_bytes = w.finalize().take();  // the reader borrows it
  auto rd = BundleReader::open_memory(bundle_bytes);
  CHECK(rd.ok() && rd.value().format_minor() == 4);
  if (!rd.ok()) return;
  const auto loaded = host::load_instrument_by_index(rd.value(), 0);
  CHECK(loaded.ok());
  if (!loaded.ok()) return;
  const ports::InstrumentView v = loaded.instrument->instrument();
  CHECK(v.regions[0].cc_gate_count == 2 && v.regions[0].cc_mod_count == 3 && v.regions[0].cc_mods[2].curve == 9);
  CHECK(v.cc_defaults.size() == 1 && v.cc_defaults[0].cc == 64 && v.cc_defaults[0].value == 90.f);
  CHECK(v.curves.size() == 2 && v.curves[0].index == 7 && v.curves[0].points.size() == 3 && v.curves[0].points[1].x == 64 && v.curves[0].points[1].y == 0.9f &&
        v.curves[1].index == 9 && v.curves[1].points.size() == 2);
  // The engine plays it: CC64 is seeded to 90, inside the 64..100 gate, so a note sounds; with the controller at 0 the same note is silent.
  auto note_peak = [&](double cc64) {
    velora::core::SampleEngine::Config cfg;
    cfg.max_voices = 4;
    velora::core::SampleEngine eng(cfg);
    eng.set_instrument(*loaded.instrument);
    if (cc64 >= 0) eng.set_part_cc(0, 64, static_cast<std::uint32_t>(cc64 / 127.0 * 4294967295.0));
    eng.set_part_cc(0, 93, 0xFFFFFFFFu);  // amplitude entry at full
    eng.start_voice(0, 50, 0xFFFF);
    std::vector<float> l(256), r(256);
    eng.render(l.data(), r.data(), 256);
    float pk = 0.0f;
    for (float x : l) pk = std::max(pk, std::fabs(x));
    return pk;
  };
  CHECK(note_peak(-1) > 0.0f);   // no controller message: the seed (90) is inside the gate
  CHECK(note_peak(0) == 0.0f);
}

static void test_voice_groups_reach_the_port() {
  using namespace velora::format;
  Region r;
  r.vg.group = 3; r.vg.has_off_by = true; r.vg.off_by = -2; r.vg.off_mode = OffMode::kTime; r.vg.off_time = 0.125f;
  r.vg.polyphony = 4; r.vg.group_polyphony = 9; r.vg.note_polyphony = 2; r.vg.note_selfmask = false;
  const ports::SampleRegionDesc d = host::to_port_region(r);
  CHECK(d.group == 3 && d.has_off_by && d.off_by == -2 && d.off_mode == ports::SampleOffMode::kTime && d.off_time == 0.125f);
  CHECK(d.polyphony == 4 && d.group_polyphony == 9 && d.note_polyphony == 2 && !d.note_selfmask);
  const ports::SampleRegionDesc plain = host::to_port_region(Region{});
  CHECK(plain.group == 0 && !plain.has_off_by && plain.off_mode == ports::SampleOffMode::kFast && plain.polyphony == 0 && plain.note_selfmask);
  // Through a real bundle (format 2.5): two regions of one group, the second cuts the first, played by the engine.
  BundleWriter w;
  const auto inst = w.add_instrument("Vg", "Vg.sfz");
  std::vector<std::int32_t> pcm(60000, 1000);
  (void)w.add_sample(inst.value(), {"a", {1, 16, false, 48000}, 60000, pcm});
  Region a;
  a.key_lo = a.key_hi = a.key_center = 60;
  a.vg.group = 1; a.vg.has_off_by = true; a.vg.off_by = 2;
  Region b = a;
  b.key_lo = b.key_hi = b.key_center = 62;
  b.vg = VoiceGroup{};
  b.vg.group = 2;
  CHECK(w.add_region(inst.value(), a) == Error::kNone);
  CHECK(w.add_region(inst.value(), b) == Error::kNone);
  const auto bundle_bytes = w.finalize().take();
  auto rd = BundleReader::open_memory(bundle_bytes);
  CHECK(rd.ok() && rd.value().format_minor() == 5);
  if (!rd.ok()) return;
  const auto loaded = host::load_instrument_by_index(rd.value(), 0);
  CHECK(loaded.ok());
  if (!loaded.ok()) return;
  const ports::InstrumentView v = loaded.instrument->instrument();
  CHECK(v.regions[0].group == 1 && v.regions[0].has_off_by && v.regions[0].off_by == 2 && v.regions[1].group == 2 && !v.regions[1].has_off_by);
  velora::core::SampleEngine::Config cfg;
  cfg.max_voices = 4;
  velora::core::SampleEngine eng(cfg);
  eng.set_instrument(*loaded.instrument);
  eng.start_voice(0, 60, 0xFFFF);
  std::vector<float> l(256), rr(256);
  eng.render(l.data(), rr.data(), 256);
  eng.start_voice(0, 62, 0xFFFF);
  velora::core::VoiceInfo vi;
  unsigned releasing = 0, held = 0;
  for (std::uint32_t slot = 0; slot < eng.voice_capacity(); ++slot)
    if (eng.voice_info(slot, vi)) (vi.state == velora::core::VoiceState::kHeld ? held : releasing)++;
  CHECK(held == 1 && releasing == 1);  // the note-60 voice was cut by the note-62 voice
}

int main() {
  test_voice_groups_reach_the_port();
  test_cc_map_reaches_the_port();
  test_amp_map_reaches_the_port();
  test_trigger_and_end_reach_the_port();
  test_extension_reaches_the_port();
  test_unusable_samples();
  test_pcm_to_float();
  test_load_and_convert();
  test_failed_sample_is_skipped();
  if (g_fail) {
    std::printf("sample_bundle_loader_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("sample_bundle_loader_test: ok\n");
  return 0;
}
