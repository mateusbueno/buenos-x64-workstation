// Bundle loader for the sample engine (Story 1.15): bundle -> float PCM + region table -> SampleEngine.
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

int main() {
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
