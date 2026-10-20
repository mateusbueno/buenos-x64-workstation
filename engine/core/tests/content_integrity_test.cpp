// Story 6.5: the real-time side of content integrity. A protected sample carries a block-damage map (ports::SamplePcm::damaged_blocks) that non-real-time threads
// set when a block fails its hash check; the sample engine only READS it: a voice fades to silence in kIntegrityFadeFrames before it reaches a damaged block, stays
// silent through it, fades back in after it, and counts the entry (content_integrity_events, published to the control side by the block loop). A sample without a
// map renders exactly as before, bit for bit. No crypto in this test: the damage flags are set by hand, the way the host's I/O edge threads set them.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/ports/sample_source.hpp"

using namespace velora::core;
namespace ports = velora::ports;

static int g_fail = 0;
#define CHECK(c)                                                 \
  do {                                                           \
    if (!(c)) {                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
      ++g_fail;                                                  \
    }                                                            \
  } while (0)

// Allocation counter: while armed, every global operator new is counted. The render path must not allocate (the real-time contract), with a damage map as without.
static std::atomic<long> g_allocs{0};
static std::atomic<bool> g_count_allocs{false};
void* operator new(std::size_t n) {
  if (g_count_allocs.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

constexpr std::uint32_t kBlock = 4096;  // formatlib's frames per block

// One sample (mono or stereo), optionally with a damage map; a looping region over key range lo..hi centred on 60.
struct Instrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<std::vector<float>> data;
  std::vector<std::unique_ptr<std::atomic<std::uint8_t>[]>> maps;
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }

  std::uint32_t add(std::size_t frames, bool stereo, float level, bool with_map, bool varying = false) {
    std::vector<float> buf(frames * (stereo ? 2 : 1));
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = varying ? 0.6f * std::sin(0.05f * static_cast<float>(i)) + 0.1f * std::sin(0.31f * static_cast<float>(i)) : level;
    data.push_back(std::move(buf));
    ports::SamplePcm p;
    p.channels = stereo ? 2 : 1;
    p.frames = frames;
    p.left = data.back().data();
    p.right = stereo ? data.back().data() + frames : nullptr;
    const std::uint32_t blocks = static_cast<std::uint32_t>((frames + kBlock - 1) / kBlock);
    if (with_map) {
      maps.push_back(std::make_unique<std::atomic<std::uint8_t>[]>(blocks));
      p.damaged_blocks = maps.back().get();
      p.damaged_block_frames = kBlock;
      p.damaged_block_count = blocks;
    } else {
      maps.push_back(nullptr);
    }
    pcm.push_back(p);
    return static_cast<std::uint32_t>(pcm.size() - 1);
  }
  ports::SampleRegionDesc& region(std::uint32_t sample, bool loop = true) {
    ports::SampleRegionDesc d;
    d.sample_index = sample;
    d.key_lo = 0;
    d.key_hi = 127;
    d.key_center = 60;
    if (loop) d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    d.ampeg_release = 0.01f;
    regions.push_back(d);
    return regions.back();
  }
  void damage(std::size_t sample, std::size_t block, bool on = true) { maps[sample][block].store(on ? 1 : 0); }
};

std::vector<float> render(SampleEngine& se, std::uint32_t frames, std::vector<float>* right = nullptr) {
  std::vector<float> l(frames, 0.0f), r(frames, 0.0f);
  for (std::uint32_t at = 0; at < frames; at += 64) se.render(l.data() + at, r.data() + at, std::min<std::uint32_t>(64, frames - at));
  if (right) *right = r;
  return l;
}

// 1. A map that is all zero changes nothing: the output equals that of the same instrument without a map, bit for bit -- for both interpolations, mono and
//    stereo, looped and not, with several notes including pitched ones.
void test_bit_exact_when_undamaged() {
  for (SampleInterpolation q : {SampleInterpolation::kHermite3, SampleInterpolation::kLinear}) {
    Instrument plain, mapped;
    for (Instrument* ins : {&plain, &mapped}) {
      const bool m = ins == &mapped;
      const auto a = ins->add(12000, false, 0.0f, m, true);
      const auto b = ins->add(9000, true, 0.0f, m, true);
      auto& r0 = ins->region(a, true);
      r0.key_hi = 63;
      auto& r1 = ins->region(b, false);
      r1.key_lo = 64;
      r1.pan = 30.0f;
    }
    SampleEngine::Config cfg;
    cfg.interpolation = q;
    SampleEngine e1(cfg), e2(cfg);
    e1.set_instrument(plain);
    e2.set_instrument(mapped);
    for (std::uint32_t note : {48u, 60u, 62u, 72u, 66u, 84u}) {
      CHECK(e1.start_voice(0, note, 40000) == EngineStatus::kOk && e2.start_voice(0, note, 40000) == EngineStatus::kOk);
    }
    std::vector<float> r1, r2;
    const auto o1 = render(e1, 30000, &r1), o2 = render(e2, 30000, &r2);
    CHECK(std::memcmp(o1.data(), o2.data(), o1.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(r1.data(), r2.data(), r1.size() * sizeof(float)) == 0);
    CHECK(e2.content_integrity_events() == 0);
    bool loud = false;
    for (float v : o1) loud = loud || std::fabs(v) > 0.01f;
    CHECK(loud);
  }
}

// The gain a voice of a constant sample has at each output frame, relative to the undamaged level.
struct Gain {
  std::vector<double> g;
  double base = 0;
};
Gain gain_curve(SampleEngine& se, std::uint32_t note, std::uint32_t frames) {
  Gain r;
  CHECK(se.start_voice(0, note, 65535) == EngineStatus::kOk);
  const auto out = render(se, frames);
  r.base = out[300];
  for (float v : out) r.g.push_back(r.base != 0 ? v / r.base : 0.0);
  return r;
}

// 2. The shape of the fade: constant sample, block 1 (frames 4096..8191) damaged, ratio 1. The voice is at full gain until the lookahead reaches the block, falls
//    to 0 by the block's start, is silent through it, and rises again over the next kIntegrityFadeFrames.
void test_fade_shape() {
  for (SampleInterpolation q : {SampleInterpolation::kHermite3, SampleInterpolation::kLinear}) {
    Instrument ins;
    const auto s = ins.add(12000, false, 0.25f, true);
    ins.region(s);
    SampleEngine::Config cfg;
    cfg.interpolation = q;
    SampleEngine se(cfg);
    se.set_instrument(ins);
    ins.damage(0, 1);
    const Gain c = gain_curve(se, 60, 11000);
    CHECK(c.base != 0);
    // Ratio 1, so output frame n plays sample frame n (the envelope's attack aside, which is over by frame 300).
    for (std::size_t n = 300; n < 4000; ++n) CHECK(std::fabs(c.g[n] - 1.0) < 1e-6);                       // before the fade: exactly the undamaged level
    double prev = 1.0;
    std::size_t down = 0;
    for (std::size_t n = 4000; n < 4096; ++n) {
      CHECK(c.g[n] <= prev + 1e-9);
      if (c.g[n] < 1.0 - 1e-9 && c.g[n] > 1e-9) ++down;
      prev = c.g[n];
    }
    CHECK(down >= 60 && down <= 64);                                                                      // a ramp of kIntegrityFadeFrames, not a cut
    CHECK(c.g[4096] == 0.0 || std::fabs(c.g[4096]) < 1e-9);
    for (std::size_t n = 4100; n < 8192; ++n) CHECK(std::fabs(c.g[n]) < 1e-9);                            // the damaged block is silent
    prev = 0.0;
    std::size_t up = 0;
    for (std::size_t n = 8192; n < 8192 + 80; ++n) {
      CHECK(c.g[n] >= prev - 1e-9);
      if (c.g[n] > 1e-9 && c.g[n] < 1.0 - 1e-9) ++up;
      prev = c.g[n];
    }
    CHECK(up >= 60 && up <= 64);
    for (std::size_t n = 8192 + 70; n < 10000; ++n) CHECK(std::fabs(c.g[n] - 1.0) < 1e-6);                // intact audio is untouched again
    CHECK(se.content_integrity_events() == 1);                                                              // one entry, counted once
  }
}

// 3. A pitched voice: the fade is kIntegrityFadeFrames of OUTPUT, so at ratio 2 it starts twice as many sample frames ahead.
void test_fade_pitched() {
  Instrument ins;
  const auto s = ins.add(40000, false, 0.25f, true);
  ins.region(s);
  SampleEngine se{SampleEngine::Config{}};
  se.set_instrument(ins);
  ins.damage(0, 2);  // frames 8192..12287
  const Gain c = gain_curve(se, 72, 9000);  // an octave up: output frame n plays sample frame 2n
  std::size_t first_down = c.g.size(), last_pos = 0;
  for (std::size_t n = 300; n < c.g.size(); ++n) {
    if (c.g[n] < 1.0 - 1e-9 && first_down == c.g.size()) first_down = n;
    if (c.g[n] > 1e-9) last_pos = n;
  }
  CHECK(first_down < c.g.size());
  CHECK(first_down >= 4096 - 70 && first_down <= 4096 - 55);  // 8192 / 2 = 4096, 64 output frames earlier
  CHECK(std::fabs(c.g[4100]) < 1e-9);
  (void)last_pos;
  CHECK(se.content_integrity_events() == 1);
}

// 4. A voice that starts inside a damaged block starts silent (no click, no damaged audio), and still counts once.
void test_start_inside_damage() {
  Instrument ins;
  const auto s = ins.add(12000, false, 0.25f, true);
  auto& r = ins.region(s);
  r.offset = 5000;  // block 1
  SampleEngine se{SampleEngine::Config{}};
  se.set_instrument(ins);
  ins.damage(0, 1);
  CHECK(se.start_voice(0, 60, 65535) == EngineStatus::kOk);
  const auto out = render(se, 3000);
  for (std::size_t n = 0; n < 3000; ++n) CHECK(out[n] == 0.0f);
  CHECK(se.content_integrity_events() == 1);
  // Repaired (the flag cleared): the next voice sounds.
  ins.damage(0, 1, false);
  CHECK(se.start_voice(0, 61, 65535) == EngineStatus::kOk);
  const auto out2 = render(se, 2000);
  float peak = 0;
  for (float v : out2) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.01f);
  CHECK(se.content_integrity_events() == 1);
}

// 5. A loop whose lookahead wraps: the sample (two blocks) loops over its whole length and block 0 -- the first block of the loop -- is damaged. A voice that plays
//    block 1 fades out BEFORE the loop point, is silent through block 0, fades in when it leaves it, and the entry is counted once, not once per loop frame.
void test_loop_wrap_lookahead() {
  Instrument ins;
  const auto s = ins.add(8192, false, 0.25f, true);
  auto& r = ins.region(s);
  r.has_loop_points = true;
  r.loop_start = 0;
  r.loop_end = 8191;
  r.offset = 4096;  // the voice starts in block 1
  SampleEngine se{SampleEngine::Config{}};
  se.set_instrument(ins);
  ins.damage(0, 0);
  const Gain c = gain_curve(se, 60, 11000);
  CHECK(c.base != 0);
  for (std::size_t n = 300; n < 4000; ++n) CHECK(std::fabs(c.g[n] - 1.0) < 1e-6);   // block 1 plays at full level ...
  CHECK(c.g[4050] < 1.0 && c.g[4050] > 0.0);                                         // ... fades before the loop point (output frame 4096) ...
  for (std::size_t n = 4100; n < 8190; ++n) CHECK(std::fabs(c.g[n]) < 1e-9);         // ... is silent through block 0 ...
  for (std::size_t n = 8192 + 70; n < 8192 + 100; ++n) CHECK(std::fabs(c.g[n] - 1.0) < 1e-6);  // ... and plays block 1 again
  CHECK(se.content_integrity_events() == 1);
}

// 6. The block loop publishes the engines' count to the control side (LastValueTable), once per block.
void test_published_counter() {
  Instrument ins;
  const auto s = ins.add(12000, false, 0.25f, true);
  ins.region(s);
  SampleEngine::Config cfg;
  auto se = std::make_unique<SampleEngine>(cfg);
  se->set_instrument(ins);
  auto br = std::make_unique<BlockRenderer>();
  CHECK(br->bind_engine(*se));
  for (std::uint32_t ch = 0; ch < 16; ++ch) br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kSample));
  float l[kBlockFrames], r[kBlockFrames];
  CHECK(br->submit({make_note_on(0, 0, 60, 65535), 0}));
  br->process_block(l, r);
  CHECK(br->last_values().load_content_integrity() == 0);
  ins.damage(0, 1);
  for (int i = 0; i < 80; ++i) br->process_block(l, r);
  CHECK(br->last_values().load_content_integrity() == 1);
  CHECK(se->content_integrity_events() == 1);
}

// 7. A map that cannot be indexed is no map (a zero block size or count), and a frame beyond the map counts as undamaged.
void test_degenerate_maps() {
  Instrument ins;
  const auto s = ins.add(12000, false, 0.25f, true);
  ins.region(s);
  ins.pcm[0].damaged_block_count = 1;  // the map only covers block 0
  SampleEngine se{SampleEngine::Config{}};
  se.set_instrument(ins);
  ins.damage(0, 1);  // outside the declared map: ignored
  const Gain c = gain_curve(se, 60, 6000);
  for (std::size_t n = 300; n < 6000; ++n) CHECK(std::fabs(c.g[n] - 1.0) < 1e-6);
  CHECK(se.content_integrity_events() == 0);
  Instrument bad;
  const auto s2 = bad.add(12000, false, 0.25f, true);
  bad.region(s2);
  bad.pcm[0].damaged_block_frames = 0;  // cannot be indexed: treated as none
  SampleEngine se2{SampleEngine::Config{}};
  se2.set_instrument(bad);
  bad.damage(0, 0);
  CHECK(se2.start_voice(0, 60, 65535) == EngineStatus::kOk);
  const auto out = render(se2, 2000);
  float peak = 0;
  for (float v : out) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.01f && se2.content_integrity_events() == 0);
}


// 8. The flag goes up while the playhead is already INSIDE the look-ahead window: the gain is capped by the distance to the block (never full gain close to it), is
//    non-increasing, and is 0 when the block is reached. The slope is bounded by the fade step except for the single step the cap forces.
void test_flag_inside_window() {
  for (int mode = 0; mode < 3; ++mode) {  // 0: mono ratio 1, 1: stereo ratio 1, 2: mono ratio 0.5
    Instrument ins;
    const auto s = ins.add(12000, mode == 1, 0.25f, true);
    ins.region(s);
    SampleEngine se{SampleEngine::Config{}};
    se.set_instrument(ins);
    const std::uint32_t note = mode == 2 ? 48 : 60;  // 48: an octave down, ratio 0.5
    const double ratio = mode == 2 ? 0.5 : 1.0;
    CHECK(se.start_voice(0, note, 65535) == EngineStatus::kOk);
    const std::uint32_t pre = static_cast<std::uint32_t>(std::floor((4096 - 30) / ratio));  // output frames until 30 sample frames before block 1
    std::vector<float> out = render(se, pre);
    const float base = out[300];
    ins.damage(0, 1);  // flagged with 30 sample frames to go: less than the fade window
    const std::vector<float> rest = render(se, static_cast<std::uint32_t>(std::ceil(4200 / ratio)));
    out.insert(out.end(), rest.begin(), rest.end());
    double prev = 1.0;
    std::size_t zero_at = 0;
    for (std::size_t n = pre; n < out.size(); ++n) {
      const double sample_pos = static_cast<double>(n) * ratio;  // sample frame this output frame plays
      if (sample_pos > 8100.0) break;                            // the fade back in after the block is checked by test_fade_shape
      const double g = out[n] / base;
      CHECK(g <= prev + 1e-6);  // never rises while it is going down
      prev = g;
      if (zero_at == 0 && std::fabs(g) < 1e-7) zero_at = n;
      if (sample_pos >= 4096.0 + 1.0) CHECK(std::fabs(g) < 1e-7);
      // never more than dist / look of full gain in the window (with a little slack for the position rounding)
      const double dist = 4096.0 - sample_pos, look = std::ceil(ratio * 64.0) + 1.0;
      if (dist >= 0 && dist < look) CHECK(g <= dist / look + 0.03);
    }
    CHECK(zero_at != 0 && static_cast<double>(zero_at) * ratio <= 4096.0 + 2.0);
    CHECK(se.content_integrity_events() == 1);
  }
}

// 9. A note that starts shortly before a damaged block starts at the gain the distance allows, not at full gain.
void test_note_on_near_damage() {
  Instrument ins;
  const auto s = ins.add(12000, false, 0.25f, true);
  auto& r = ins.region(s);
  r.offset = 4096 - 20;  // 20 frames before block 1
  SampleEngine se{SampleEngine::Config{}};
  se.set_instrument(ins);
  ins.damage(0, 1);
  CHECK(se.start_voice(0, 60, 65535) == EngineStatus::kOk);
  const auto out = render(se, 400);
  float peak = 0;
  for (std::size_t n = 0; n < 40; ++n) peak = std::max(peak, std::fabs(out[n]));
  // The undamaged level of this sample is 0.25 * the engine's unity; compare with a voice far from damage.
  Instrument far;
  const auto s2 = far.add(12000, false, 0.25f, true);
  far.region(s2);
  SampleEngine se2{SampleEngine::Config{}};
  se2.set_instrument(far);
  CHECK(se2.start_voice(0, 60, 65535) == EngineStatus::kOk);
  const auto out2 = render(se2, 400);
  const float full = out2[300];
  CHECK(peak < full * (20.0f / 65.0f) + 0.01f * full);  // at most dist / look of the full level at the first frame
  for (std::size_t n = 21; n < 400; ++n) CHECK(std::fabs(out[n]) < 1e-9f);  // silent from the block on
  CHECK(se.content_integrity_events() == 1);
}

// 10. The render path allocates nothing, with a damage map or without, with the full 340 voices playing and damaged blocks in play (counter based: no timing).
void test_no_allocation_340_voices() {
  for (int with_map = 0; with_map < 2; ++with_map) {
    Instrument ins;
    const auto s = ins.add(60000, true, 0.2f, with_map != 0, true);
    ins.region(s);
    SampleEngine::Config cfg;
    cfg.max_voices = 340;
    SampleEngine se(cfg);
    se.set_instrument(ins);
    std::uint32_t started = 0;
    for (std::uint32_t ch = 0; ch < 3 && started < 340; ++ch)
      for (std::uint32_t note = 0; note < 128 && started < 340; ++note)
        if (se.start_voice(ch, note, 40000) == EngineStatus::kOk) ++started;
    CHECK(started == 340);
    std::vector<float> l(64), r(64);
    g_allocs.store(0);
    g_count_allocs.store(true);
    for (int b = 0; b < 600; ++b) {
      if (with_map && b == 200) ins.damage(0, 3);
      if (with_map && b == 300) ins.damage(0, 5);
      std::fill(l.begin(), l.end(), 0.0f);
      std::fill(r.begin(), r.end(), 0.0f);
      se.render(l.data(), r.data(), 64);
    }
    g_count_allocs.store(false);
    CHECK(g_allocs.load() == 0);
    CHECK(se.active_voices() > 300);
    if (with_map) CHECK(se.content_integrity_events() > 0);
  }
}

}  // namespace

int main() {
  test_bit_exact_when_undamaged();
  test_fade_shape();
  test_fade_pitched();
  test_start_inside_damage();
  test_loop_wrap_lookahead();
  test_published_counter();
  test_degenerate_maps();
  test_flag_inside_window();
  test_note_on_near_damage();
  test_no_allocation_340_voices();
  if (g_fail == 0) std::printf("content_integrity_test: ok\n");
  return g_fail == 0 ? 0 : 1;
}
