// Story 2.7: the fixed routing topology. Bus summation, Main/Aux split, send levels, line-in to the master only, master level,
// insert-chain add/remove/reorder/bypass with a continuous tone (cross-fade continuity), generation lifetime of the effect
// instances (with a held note), the FX budget reservation, and the default path staying bit-identical.
// Analytic references: the signal of every part is rendered alone (trivial routing = the pre-2.7 path) and the expected buses are
// summed from those in the test.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/fx.hpp"
#include "velora/core/mixer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/test_tone_engine.hpp"
#include "velora/core/arena.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/snapshot.hpp"
#include "velora/core/voice_arbiter.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {

struct Cap {
  std::vector<float> l, r, al, ar;
  std::size_t frames() const { return l.size(); }
};

double max_diff(const std::vector<float>& a, const std::vector<float>& b, std::size_t from = 0, std::size_t to = SIZE_MAX) {
  double m = 0;
  to = std::min({to, a.size(), b.size()});
  for (std::size_t i = from; i < to; ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}

double peak(const std::vector<float>& a) {
  double m = 0;
  for (float v : a) m = std::max(m, static_cast<double>(std::fabs(v)));
  return m;
}

// A source with a known, non-trivial stereo signal.
class ToneLineIn final : public velora::ports::LineInSource {
 public:
  static float left(std::uint64_t n) { return 0.30f * std::sin(0.011f * static_cast<float>(n % 100000)); }
  static float right(std::uint64_t n) { return 0.20f * std::cos(0.017f * static_cast<float>(n % 100000)); }
  void read_block(float* l, float* r, std::uint32_t frames) noexcept VELORA_NONBLOCKING override {
    for (std::uint32_t i = 0; i < frames; ++i, ++pos) {
      l[i] = left(pos);
      r[i] = right(pos);
    }
    ++reads;
  }
  std::uint64_t pos = 0, reads = 0;
};

// A test effect with an audible, known effect: a fixed gain.
class GainEffect final : public Effect {
 public:
  explicit GainEffect(float g) : g_(g) {}
  EffectType type() const noexcept override { return EffectType::kEq; }
  std::uint32_t cost_units() const noexcept override { return 1; }
  void process(StereoBlock& b) noexcept override {
    for (std::uint32_t i = 0; i < b.frames; ++i) {
      b.left[i] *= g_;
      b.right[i] *= g_;
    }
  }

 private:
  float g_;
};

std::shared_ptr<FxChain> gain_chain(std::initializer_list<float> gains, bool bypass = false) {
  auto c = std::make_shared<FxChain>();
  std::uint32_t i = 0;
  for (float g : gains) c->set_slot(i++, std::make_unique<GainEffect>(g), bypass);
  return c;
}

struct Rig {
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>(32);
  std::vector<std::unique_ptr<Snapshot>> snaps;
  std::uint64_t rev = 0;
  std::uint32_t ledger = 0;
  std::uint64_t frame_base = 0;  // frames rendered so far

  Snapshot* publish(Snapshot s) {
    s.revision = ++rev;
    s.ledger_index = ledger++ % SnapshotExchange::kLedgerSlots;
    snaps.push_back(std::make_unique<Snapshot>(std::move(s)));
    br->snapshots().publish(snaps.back().get());
    return snaps.back().get();
  }
  void note(bool on, std::uint32_t ch, std::uint32_t n, std::uint32_t frame = 0) {
    br->submit({on ? make_note_on(0, ch, n, 40000) : make_note_off(0, ch, n), frame});
  }
  void send_param(std::uint16_t id, std::uint32_t part, double v, std::uint32_t frame = 0) {
    br->submit({make_param_event(*params::find(id), part, v), frame});
  }
  Cap run(int blocks, bool want_aux = true) {
    Cap c;
    float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
    for (int b = 0; b < blocks; ++b) {
      OutputBuses ob{l, r, want_aux ? al : nullptr, want_aux ? ar : nullptr};
      br->process_block(ob);
      c.l.insert(c.l.end(), l, l + kBlockFrames);
      c.r.insert(c.r.end(), r, r + kBlockFrames);
      c.al.insert(c.al.end(), want_aux ? al : l, (want_aux ? al : l) + kBlockFrames);
      c.ar.insert(c.ar.end(), want_aux ? ar : r, (want_aux ? ar : r) + kBlockFrames);
      if (!want_aux) {
        std::fill(c.al.end() - kBlockFrames, c.al.end(), 0.0f);
        std::fill(c.ar.end() - kBlockFrames, c.ar.end(), 0.0f);
      }
    }
    return c;
  }
};

// The trivial-routing render of one part alone (the reference signal of that part).
Cap part_alone(std::uint32_t ch, std::uint32_t n, int blocks) {
  Rig rig;
  rig.note(true, ch, n);
  return rig.run(blocks);
}

float db_gain(float db) { return db == 0.0f ? 1.0f : std::pow(10.0f, db * 0.05f); }

std::size_t perf(std::uint16_t id, std::uint32_t part) { return params::perf_slot(*params::find(id), part); }

// Story 2.10: the reverb send is real DSP now (its tests are in reverb_test.cpp). These routing tests keep their analytic unity-return
// references by muting the reverb return (reverb_level 0: the reverb adds nothing) wherever a reverb send level is set.
void mute_reverb_return(Snapshot& s) { s.master[static_cast<std::size_t>(params::find(params::kReverbLevel)->master_index)] = 0.0f; }
// Story 2.11: the delay send is real DSP now (its tests are in delay_test.cpp). These routing tests either mute its return (delay_level 0) or use it as a
// pure 1 ms echo -- no feedback, no damping, no ping-pong, return level 1 -- whose wet signal is the send-weighted mono send sum delayed by exactly 48 frames
// (the test-tone engine is mono, so the wet signal equals the dry one shifted).
void mute_delay_return(Snapshot& s) { s.master[static_cast<std::size_t>(params::find(params::kDelayLevel)->master_index)] = 0.0f; }
void short_delay(Snapshot& s) {
  auto set = [&](std::uint16_t id, float v) { s.master[static_cast<std::size_t>(params::find(id)->master_index)] = v; };
  set(params::kDelayTime, 1.0f);
  set(params::kDelayFeedback, 0.0f);
  set(params::kDelayDamping, 0.0f);
  set(params::kDelayPingpong, 0.0f);
  set(params::kDelayLevel, 1.0f);
}
constexpr std::size_t kEchoFrames = 48;
float echo_at(const std::vector<float>& x, std::size_t i) { return i >= kEchoFrames ? x[i - kEchoFrames] : 0.0f; }

// --- bus summation, Main/Aux, sends, master level ---------------------------------------------------------------------------
void test_main_aux_sends_master() {
  constexpr int kBlocks = 40;
  const Cap p0 = part_alone(0, 60, kBlocks), p1 = part_alone(1, 64, kBlocks), p2 = part_alone(2, 67, kBlocks);
  CHECK(peak(p0.l) > 0.05 && peak(p1.l) > 0.05 && peak(p2.l) > 0.05);

  // Main/Aux split only: parts 0, 1 to Main, part 2 to Aux.
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.parts[2].output = params::kPartOutputAux;
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.note(true, 1, 64);
    rig.note(true, 2, 67);
    const Cap c = rig.run(kBlocks);
    std::vector<float> em(c.frames()), ea(c.frames());
    for (std::size_t i = 0; i < c.frames(); ++i) {
      em[i] = p0.l[i] + p1.l[i];
      ea[i] = p2.l[i];
    }
    CHECK(max_diff(c.l, em) < 1e-6);
    CHECK(max_diff(c.r, em) < 1e-6);
    CHECK(max_diff(c.al, ea) < 1e-6 && max_diff(c.ar, ea) < 1e-6);
    CHECK(rig.br->mixer().routed());
  }

  // Sends (the delay is a 1 ms echo returned into the bus the sending part sits on: an Aux part's send leaves Main untouched;
  // the reverb return is muted here, its send levels are set to show that a muted return adds nothing) and the master level trim on both buses.
  {
    constexpr float kMasterDb = -6.0f;
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.parts[2].output = params::kPartOutputAux;
    s.master[static_cast<std::size_t>(params::find(params::kMasterLevel)->master_index)] = kMasterDb;
    mute_reverb_return(s);
    short_delay(s);
    s.perf_init[perf(params::kSendReverbLevel, 0)] = 0.5f;
    s.perf_init[perf(params::kSendDelayLevel, 1)] = 0.25f;
    s.perf_init[perf(params::kSendReverbLevel, 2)] = 1.0f;
    s.perf_init[perf(params::kSendDelayLevel, 2)] = 0.125f;
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.note(true, 1, 64);
    rig.note(true, 2, 67);
    const Cap c = rig.run(kBlocks);
    const float g = db_gain(kMasterDb);
    std::vector<float> em(c.frames()), ea(c.frames());
    for (std::size_t i = 0; i < c.frames(); ++i) {
      em[i] = g * (p0.l[i] + p1.l[i] + 0.25f * echo_at(p1.l, i));
      ea[i] = g * (p2.l[i] + 0.125f * echo_at(p2.l, i));
    }
    CHECK(max_diff(c.l, em) < 2e-6 && max_diff(c.r, em) < 2e-6);
    CHECK(max_diff(c.al, ea) < 2e-6 && max_diff(c.ar, ea) < 2e-6);
  }

  // No Aux buffers given: the Main output is the same, the Aux bus is discarded.
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.parts[2].output = params::kPartOutputAux;
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.note(true, 2, 67);
    const Cap c = rig.run(kBlocks, /*want_aux=*/false);
    CHECK(max_diff(c.l, p0.l) < 1e-6);
  }

  // A send level changed live (a UMP parameter event) reaches the output within a block and ramps.
  {
    Rig rig;
    Snapshot sd = Snapshot::defaults();
    short_delay(sd);
    rig.publish(sd);
    rig.note(true, 0, 60);
    const Cap before = rig.run(10);
    CHECK(max_diff(before.l, p0.l, 0, 10 * kBlockFrames) < 1e-7);  // trivial routing: untouched
    rig.send_param(params::kSendDelayLevel, 0, 1.0, 3);
    const Cap after = rig.run(8);
    // block 0 after the event: the level is taken at the next block (one block of latency), then a one-block ramp
    const std::size_t o = 10 * kBlockFrames;
    CHECK(std::fabs(after.l[5] - p0.l[o + 5]) < 1e-7);  // not yet
    for (std::size_t i = 4 * kBlockFrames; i < 7 * kBlockFrames; ++i)
      if (std::fabs(after.l[i] - (p0.l[o + i] + p0.l[o + i - kEchoFrames])) > 2e-6f) { CHECK(false); break; }
  }
}

// --- line-in goes to the master (Main) only --------------------------------------------------------------------------------
void test_line_in() {
  constexpr int kBlocks = 20;
  {  // no notes: Main = level x line, Aux silent
    Rig rig;
    ToneLineIn src;
    rig.br->set_line_in(&src);
    Snapshot s = Snapshot::defaults();
    s.perf_init[perf(params::kLineInLevel, 0)] = 0.5f;
    rig.publish(s);
    const Cap c = rig.run(kBlocks);
    std::vector<float> el(c.frames()), er(c.frames());
    for (std::size_t i = 0; i < c.frames(); ++i) {
      el[i] = 0.5f * ToneLineIn::left(i);
      er[i] = 0.5f * ToneLineIn::right(i);
    }
    CHECK(max_diff(c.l, el) < 1e-7 && max_diff(c.r, er) < 1e-7);
    CHECK(peak(c.al) == 0.0 && peak(c.ar) == 0.0);
    CHECK(src.reads == static_cast<std::uint64_t>(kBlocks));
  }
  {  // level 0 (the default): nothing is heard, the source is still read in step with the block clock
    Rig rig;
    ToneLineIn src;
    rig.br->set_line_in(&src);
    rig.publish(Snapshot::defaults());
    const Cap c = rig.run(kBlocks);
    CHECK(peak(c.l) == 0.0 && peak(c.r) == 0.0 && peak(c.al) == 0.0);
    CHECK(src.reads == static_cast<std::uint64_t>(kBlocks) && !rig.br->mixer().routed());
  }
  {  // with a part on Aux and one on Main: line-in is only in Main; master level applies to it too
    const Cap p0 = part_alone(0, 60, kBlocks), p2 = part_alone(2, 67, kBlocks);
    Rig rig;
    ToneLineIn src;
    rig.br->set_line_in(&src);
    Snapshot s = Snapshot::defaults();
    s.parts[2].output = params::kPartOutputAux;
    s.perf_init[perf(params::kLineInLevel, 0)] = 1.0f;
    s.master[static_cast<std::size_t>(params::find(params::kMasterLevel)->master_index)] = -6.0f;
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.note(true, 2, 67);
    const Cap c = rig.run(kBlocks);
    const float g = db_gain(-6.0f);
    std::vector<float> em(c.frames()), ea(c.frames());
    for (std::size_t i = 0; i < c.frames(); ++i) {
      em[i] = g * (p0.l[i] + ToneLineIn::left(i));
      ea[i] = g * p2.l[i];
    }
    CHECK(max_diff(c.l, em) < 2e-6);
    CHECK(max_diff(c.al, ea) < 2e-6);
  }
  {  // line_in_level is a live performance parameter: 0 -> 1 ramps over one block
    Rig rig;
    ToneLineIn src;
    rig.br->set_line_in(&src);
    rig.publish(Snapshot::defaults());
    rig.run(4);
    rig.send_param(params::kLineInLevel, 0, 1.0, 0);
    const Cap c = rig.run(6);
    const std::size_t o = 4 * kBlockFrames;
    bool ok = true;
    for (std::size_t i = 2 * kBlockFrames; i < 6 * kBlockFrames; ++i)
      ok = ok && std::fabs(c.l[i] - ToneLineIn::left(o + i)) < 1e-6f;
    CHECK(ok);
    CHECK(peak(c.al) == 0.0);
  }
}

// --- the default path is unchanged ----------------------------------------------------------------------------------------
void test_default_path_bit_exact() {
  auto play = [](bool snapshot) {
    Rig rig;
    if (snapshot) rig.publish(Snapshot::defaults());
    for (std::uint32_t i = 0; i < 6; ++i) rig.note(true, i % 4, 48 + 5 * i, i * 9);
    Cap c = rig.run(30);
    rig.note(false, 1, 53);
    Cap d = rig.run(10);
    c.l.insert(c.l.end(), d.l.begin(), d.l.end());
    c.r.insert(c.r.end(), d.r.begin(), d.r.end());
    CHECK(!rig.br->mixer().routed());
    return c;
  };
  const Cap a = play(false), b = play(true);
  CHECK(a.l.size() == b.l.size() && std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0);
  CHECK(std::memcmp(a.r.data(), b.r.data(), a.r.size() * sizeof(float)) == 0);

  // All parts to Main at unity but with transparent effects in the chains (the routed path): the same audio within float rounding.
  // Story 2.9: the rotary and the EP amp are real DSP and never transparent, so those slots are bypassed (a bypassed slot is skipped bit for
  // bit); the chorus/phaser at its registry default (mix 0) is the identity.
  Rig rig;
  Snapshot s = Snapshot::defaults();
  for (std::uint32_t p = 0; p < 4; ++p) {
    s.parts[p].insert_count = 5;
    for (std::uint32_t i = 0; i < 5; ++i) {
      s.parts[p].inserts[i].type = static_cast<std::uint8_t>(1 + (p + i) % 5);
      // EQ at the default (0 dB) is exactly transparent; the compressor needs ratio 1 and no makeup to be (Story 2.8).
      s.parts[p].inserts[i].params[1] = s.parts[p].inserts[i].type == params::kInsertTypeCompressor ? 0.0f : 0.5f;
      s.parts[p].inserts[i].bypass = s.parts[p].inserts[i].type == params::kInsertTypeRotary || s.parts[p].inserts[i].type == params::kInsertTypeEpAmp;
    }
  }
  build_chains(s);
  rig.publish(s);
  for (std::uint32_t i = 0; i < 6; ++i) rig.note(true, i % 4, 48 + 5 * i, i * 9);
  const Cap c = rig.run(30);
  CHECK(rig.br->mixer().routed());
  CHECK(max_diff(c.l, a.l, 0, 30 * kBlockFrames) < 1e-6);
}

// --- organ: the per-part split equals the direct render ----------------------------------------------------------------------
void test_organ_split() {
  auto play = [](bool routed, Cap* aux_probe) {
    Rig rig;
    OrganEngine organ;
    CHECK(rig.br->bind_engine(organ));
    rig.br->set_channel_engine(0, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
    rig.br->set_channel_engine(1, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
    if (routed) {
      Snapshot s = Snapshot::defaults();
      s.parts[0].insert_count = 1;
      s.parts[0].inserts[0].type = params::kInsertTypeEq;
      s.parts[1].output = params::kPartOutputAux;
      build_chains(s);
      rig.publish(s);
    }
    rig.note(true, 0, 60);
    rig.note(true, 0, 64);
    rig.note(true, 1, 48);
    Cap c = rig.run(60);
    if (aux_probe) *aux_probe = c;
    return c;
  };
  Cap routed_cap;
  const Cap direct = play(false, nullptr);
  const Cap routed = play(true, &routed_cap);
  // direct = part0 + part1 in Main; routed = part0 in Main, part1 in Aux: Main + Aux equals the direct sum
  std::vector<float> sum(direct.frames());
  for (std::size_t i = 0; i < sum.size(); ++i) sum[i] = routed.l[i] + routed.al[i];
  CHECK(peak(direct.l) > 0.01 && peak(routed.al) > 0.005 && peak(routed.l) > 0.005);
  CHECK(max_diff(sum, direct.l) < 2e-6);
}


// --- the sample engine through the routed path; channel mapping of render_split -----------------------------------------------
struct SineInstrument final : velora::ports::SampleSource {
  std::vector<float> data;
  std::vector<velora::ports::SamplePcm> pcm;
  std::vector<velora::ports::SampleRegionDesc> regions;
  SineInstrument() {
    data.resize(30000);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = 0.4f * std::sin(0.05f * static_cast<float>(i));
    velora::ports::SamplePcm p;
    p.sample_rate = 48000;
    p.channels = 1;
    p.frames = data.size();
    p.left = data.data();
    pcm.push_back(p);
    velora::ports::SampleRegionDesc d;
    d.sample_index = 0;
    d.key_lo = 0;
    d.key_hi = 127;
    d.key_center = 60;
    regions.push_back(d);
  }
  velora::ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }
};

void test_sample_engine_routed() {
  constexpr int kBlocks = 40;
  SineInstrument ins;
  auto run_case = [&](bool routed, std::initializer_list<std::pair<int, int>> chan_notes, Cap* out) {
    SampleEngine::Config c;
    c.max_voices = 16;
    SampleEngine eng(c);
    Rig rig;
    CHECK(eng.set_instrument(ins) && rig.br->bind_engine(eng));
    for (std::uint32_t ch = 0; ch < 16; ++ch) rig.br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kSample));
    if (routed) {
      Snapshot s = Snapshot::defaults();
      s.parts[2].output = params::kPartOutputAux;
      mute_reverb_return(s);
      mute_delay_return(s);  // the sample engine is stereo, the delay send is a mono sum: delay_test.cpp covers the delay, here the muted return adds nothing
      s.perf_init[perf(params::kSendReverbLevel, 2)] = 0.5f;
      s.perf_init[perf(params::kSendDelayLevel, 0)] = 0.25f;
      rig.publish(s);
    }
    for (const auto& cn : chan_notes) rig.note(true, cn.first, cn.second);
    *out = rig.run(kBlocks);
  };
  Cap a0, a1, a2, all;
  run_case(false, {{0, 60}}, &a0);
  run_case(false, {{1, 64}}, &a1);
  run_case(false, {{2, 67}}, &a2);
  run_case(true, {{0, 60}, {1, 64}, {2, 67}}, &all);
  CHECK(peak(a0.l) > 0.05 && peak(a2.r) > 0.05);
  std::vector<float> em(all.frames()), er(all.frames()), ea(all.frames()), ear(all.frames());
  for (std::size_t i = 0; i < all.frames(); ++i) {
    em[i] = a0.l[i] + a1.l[i];
    er[i] = a0.r[i] + a1.r[i];
    ea[i] = a2.l[i];
    ear[i] = a2.r[i];
  }
  CHECK(max_diff(all.l, em) < 2e-6 && max_diff(all.r, er) < 2e-6);
  CHECK(max_diff(all.al, ea) < 2e-6 && max_diff(all.ar, ear) < 2e-6);
  // the null test against the same notes with trivial routing: Main + Aux (no sends) equals the plain mix
  Cap all_plain, routed_nosend;
  run_case(false, {{0, 60}, {1, 64}, {2, 67}}, &all_plain);
  {
    SampleEngine::Config c;
    c.max_voices = 16;
    SampleEngine eng(c);
    Rig rig;
    CHECK(eng.set_instrument(ins) && rig.br->bind_engine(eng));
    for (std::uint32_t ch = 0; ch < 16; ++ch) rig.br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kSample));
    Snapshot s = Snapshot::defaults();
    s.parts[2].output = params::kPartOutputAux;
    rig.publish(s);
    for (const auto& cn : {std::pair<int, int>{0, 60}, {1, 64}, {2, 67}}) rig.note(true, cn.first, cn.second);
    routed_nosend = rig.run(kBlocks);
  }
  std::vector<float> sum(all_plain.frames());
  for (std::size_t i = 0; i < sum.size(); ++i) sum[i] = routed_nosend.l[i] + routed_nosend.al[i];
  CHECK(max_diff(sum, all_plain.l) < 2e-6);
}

void test_render_split_channel_mapping() {
  // Direct: each voice lands in the buffers of its own UMP channel (and only there).
  auto check = [](Engine& e, const char* what) {
    constexpr std::uint32_t kN = 256;
    std::vector<std::vector<float>> l(Engine::kSplitChannels, std::vector<float>(kN, 0.0f)), r = l;
    float* lp[Engine::kSplitChannels];
    float* rp[Engine::kSplitChannels];
    for (std::uint32_t c = 0; c < Engine::kSplitChannels; ++c) {
      lp[c] = l[c].data();
      rp[c] = r[c].data();
    }
    CHECK(e.start_voice(3, 60, 40000) == EngineStatus::kOk);
    CHECK(e.start_voice(11, 64, 40000) == EngineStatus::kOk);
    e.render_split(lp, rp, kN);
    for (std::uint32_t c = 0; c < Engine::kSplitChannels; ++c) {
      const double pk = peak(l[c]);
      const bool want = c == 3 || c == 11;
      if (want ? !(pk > 0.001) : pk != 0.0) {
        std::printf("%s: channel %u %s\n", what, c, want ? "silent" : "not silent");
        CHECK(false);
      }
    }
  };
  {
    Arena arena(64 * 1024);
    TestToneEngine tone(arena, 8);
    check(tone, "test tone");
  }
  {
    SineInstrument ins;
    SampleEngine::Config c;
    c.max_voices = 8;
    SampleEngine eng(c);
    CHECK(eng.set_instrument(ins));
    check(eng, "sample");
  }
  // In the Mixer, channel c is part c (16 parts, Story 4.1): a note on channel 11 sounds on part 11 (here assigned to Aux).
  Rig rig;
  Snapshot s = Snapshot::defaults();
  s.parts[11].output = params::kPartOutputAux;
  rig.publish(s);
  rig.note(true, 11, 60);
  const Cap c = rig.run(10);
  CHECK(peak(c.al) > 0.05 && peak(c.l) == 0.0);
}

// The single-output overload folds Aux into Main; half an Aux bus is refused.
void test_aux_fold_and_bad_buses() {
  Cap folded, split;
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.parts[2].output = params::kPartOutputAux;
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.note(true, 2, 67);
    split = rig.run(20);
  }
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.parts[2].output = params::kPartOutputAux;
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.note(true, 2, 67);
    float l[kBlockFrames], r[kBlockFrames];
    for (int b = 0; b < 20; ++b) {
      rig.br->process_block(l, r);
      folded.l.insert(folded.l.end(), l, l + kBlockFrames);
    }
    CHECK(rig.br->aux_folded_blocks() > 0);
    std::vector<float> sum(split.frames());
    for (std::size_t i = 0; i < sum.size(); ++i) sum[i] = split.l[i] + split.al[i];
    CHECK(max_diff(folded.l, sum) < 1e-7);
    CHECK(peak(folded.l) > peak(split.l) + 0.01);
  }
  {  // only one Aux channel given: nothing rendered, Main silenced, counted
    Rig rig;
    rig.note(true, 0, 60);
    float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames];
    for (float& v : l) v = 1.0f;
    rig.br->process_block(OutputBuses{l, r, al, nullptr});
    CHECK(rig.br->rejected_output_blocks() == 1 && peak(Cap{std::vector<float>(l, l + kBlockFrames), {}, {}, {}}.l) == 0.0);
    rig.br->process_block(OutputBuses{l, r, nullptr, nullptr});  // no Aux at all is fine
    CHECK(rig.br->rejected_output_blocks() == 1);
  }
}

// Mixer robustness: a misbehaving line-in, and the first generation applying instantly after silent blocks.
void test_mixer_robustness() {
  class BadLineIn final : public velora::ports::LineInSource {
   public:
    void read_block(float* l, float* r, std::uint32_t frames) noexcept VELORA_NONBLOCKING override {
      for (std::uint32_t i = 0; i < frames; ++i) {
        l[i] = (i % 2) ? std::nanf("") : 0.25f;
        r[i] = (i % 3) ? INFINITY : -0.25f;
      }
    }
  };
  {
    Rig rig;
    BadLineIn bad;
    rig.br->set_line_in(&bad);
    Snapshot s = Snapshot::defaults();
    s.perf_init[perf(params::kLineInLevel, 0)] = 1.0f;
    rig.publish(s);
    const Cap c = rig.run(5);
    bool finite = true;
    for (float v : c.l) finite = finite && std::isfinite(v);
    for (float v : c.r) finite = finite && std::isfinite(v);
    CHECK(finite && peak(c.l) > 0.2);
  }
  {  // silent blocks first, then the first generation: send level and master apply at once (no ramp from the defaults)
    const Cap p0 = part_alone(0, 60, 10);
    Rig rig;
    rig.run(5);
    Snapshot s = Snapshot::defaults();
    short_delay(s);
    s.perf_init[perf(params::kSendDelayLevel, 0)] = 0.5f;
    rig.publish(s);
    rig.note(true, 0, 60);
    const Cap c = rig.run(10);
    bool ok = true;
    for (std::size_t i = 0; i < c.frames(); ++i) ok = ok && std::fabs(c.l[i] - (p0.l[i] + 0.5f * echo_at(p0.l, i))) < 2e-6f;
    CHECK(ok);
  }
  {  // two generations published before the first block with chains: no generation reference is left behind once they settle
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.parts[0].insert_count = 1;
    s.parts[0].chain = gain_chain({1.0f});
    rig.publish(s);
    rig.run(3);
    Snapshot s2 = s;
    s2.parts[0].chain = gain_chain({1.0f});
    rig.publish(s2);
    rig.run(1);
    Snapshot s3 = s2;
    s3.parts[0].chain = gain_chain({1.0f});
    rig.publish(s3);  // arrives mid-fade: waits
    rig.run(1);
    Snapshot s4 = s3;
    s4.parts[0].chain.reset();
    s4.parts[0].insert_count = 0;
    rig.publish(s4);  // replaces the waiting chain
    rig.run(20);
    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < SnapshotExchange::kLedgerSlots; ++i) total += rig.br->snapshots().refs(i);
    CHECK(total == 0 && rig.br->mixer().chains_settled());
  }
}

// --- insert chains: add, remove, reorder, bypass while a tone sounds ---------------------------------------------------------
void test_chain_crossfade() {
  constexpr int kBlocks = 60;
  const Cap x = part_alone(0, 57, kBlocks);  // the continuous tone, no routing
  const double peak_x = peak(x.l);
  double step_x = 0;
  for (std::size_t i = 1; i < x.l.size(); ++i) step_x = std::max(step_x, static_cast<double>(std::fabs(x.l[i] - x.l[i - 1])));

  Rig rig;
  Snapshot s0 = Snapshot::defaults();
  rig.publish(s0);
  rig.note(true, 0, 57);
  Cap y = rig.run(10);
  CHECK(!rig.br->mixer().routed());

  // add a chain [gain 0.5] at block 10: linear 5 ms cross-fade from 1 to 0.5, then 0.5
  Snapshot s1 = s0;
  s1.parts[0].insert_count = 1;
  s1.parts[0].inserts[0].type = params::kInsertTypeEq;
  s1.parts[0].chain = gain_chain({0.5f});
  rig.publish(s1);
  Cap d = rig.run(10);
  y.l.insert(y.l.end(), d.l.begin(), d.l.end());
  const std::size_t o = 10 * kBlockFrames;
  CHECK(rig.br->mixer().chains_settled() && rig.br->mixer().chain_entries(0) == 1);
  double worst = 0;
  for (std::uint32_t f = 0; f < 240; ++f) {
    const float w = static_cast<float>(f + 1) / 240.0f;
    worst = std::max(worst, static_cast<double>(std::fabs(y.l[o + f] - x.l[o + f] * (1.0f - 0.5f * w))));
  }
  CHECK(worst < 3e-5);
  CHECK(max_diff(std::vector<float>(y.l.begin() + o + 260, y.l.end()), [&] {
          std::vector<float> h(x.l.begin() + o + 260, x.l.begin() + y.l.size());
          for (float& v : h) v *= 0.5f;
          return h;
        }()) < 1e-6);

  // remove it (a generation without the chain): back to 1 over 5 ms
  Snapshot s2 = s1;
  s2.parts[0].insert_count = 0;
  s2.parts[0].chain.reset();
  rig.publish(s2);
  d = rig.run(10);
  y.l.insert(y.l.end(), d.l.begin(), d.l.end());
  CHECK(max_diff(std::vector<float>(y.l.begin() + 20 * kBlockFrames + 260, y.l.end()),
                 std::vector<float>(x.l.begin() + 20 * kBlockFrames + 260, x.l.begin() + y.l.size())) < 1e-6);

  // the largest sample-to-sample step over the whole run: the tone's own step plus the ramp's (0.5/240 of the peak)
  double step_y = 0;
  for (std::size_t i = 1; i < y.l.size(); ++i) step_y = std::max(step_y, static_cast<double>(std::fabs(y.l[i] - y.l[i - 1])));
  CHECK(step_y <= step_x + 0.5 / 240.0 * peak_x + 1e-6);

  rig.run(4);
  CHECK(rig.br->mixer().chains_settled());
}

// Add / reorder / bypass / remove at a pace faster than the fade (a change per block, up to 5 chains in flight): the net gain is 1
// everywhere, so the output must equal the tone (the cross-fade weights always sum to 1: no dip, no bump).
void test_chain_changes_keep_the_tone() {
  constexpr int kBlocks = 120;
  const Cap x = part_alone(0, 57, kBlocks);
  Rig rig;
  Snapshot cur = Snapshot::defaults();
  rig.publish(cur);
  rig.note(true, 0, 57);
  std::vector<float> y;
  std::uint32_t max_entries = 0;
  for (int b = 0; b < kBlocks; ++b) {
    if (b >= 10 && b < 100) {  // a new chain every block from block 10 on, cycling gains with net 1.0 / bypass / none
      const int k = b - 10;
      auto chain = std::make_shared<FxChain>();
      switch (k % 5) {
        case 0: chain->set_slot(0, std::make_unique<GainEffect>(0.5f), false); chain->set_slot(1, std::make_unique<GainEffect>(2.0f), false); break;
        case 1: chain->set_slot(0, std::make_unique<GainEffect>(2.0f), false); chain->set_slot(1, std::make_unique<GainEffect>(0.5f), false); break;
        case 2: chain->set_slot(0, std::make_unique<GainEffect>(0.5f), /*bypass=*/true); break;
        case 3:
          chain->set_slot(0, std::make_unique<GainEffect>(4.0f), false);
          chain->set_slot(1, std::make_unique<GainEffect>(0.25f), false);
          chain->set_slot(2, std::make_unique<GainEffect>(1.0f), false);
          break;
        default: chain = nullptr; break;
      }
      cur.parts[0].chain = chain;
      cur.parts[0].insert_count = chain ? static_cast<std::uint8_t>(chain->slot_count()) : 0;
      rig.publish(cur);
    }
    const Cap blk = rig.run(1);
    y.insert(y.end(), blk.l.begin(), blk.l.end());
    max_entries = std::max(max_entries, rig.br->mixer().chain_entries(0));
  }
  CHECK(max_diff(y, x.l) < 5e-5);
  // no generation reference leaked by the skipped / faded chains: the held note and (if any) the current chain are all that is left
  std::uint32_t total_refs = 0;
  for (std::uint32_t i = 0; i < SnapshotExchange::kLedgerSlots; ++i) total_refs += rig.br->snapshots().refs(i);
  CHECK(total_refs == 1 + (rig.br->mixer().current_chain(0) ? 1u : 0u));
  CHECK(max_entries == 2 && max_entries <= Mixer::kMaxChains);  // never more than two chains in flight
  CHECK(rig.br->mixer().chains_settled());
}

// --- generation lifetime of the effect instances ---------------------------------------------------------------------------
void test_effect_lifetime() {
  const std::int64_t base = Effect::live_instances();
  Rig rig;
  auto& ex = rig.br->snapshots();
  Snapshot a = Snapshot::defaults();
  a.parts[0].insert_count = 1;
  a.parts[0].chain = gain_chain({1.0f});
  Snapshot* sa = rig.publish(a);
  a.parts[0].chain.reset();
  rig.note(true, 0, 60);  // held across every swap below
  rig.run(4);
  CHECK(ex.refs(sa->ledger_index) == 2);  // the held note and the chain entry

  Snapshot b = a;
  b.parts[0].chain = gain_chain({1.0f});
  Snapshot* sb = rig.publish(b);
  b.parts[0].chain.reset();  // (the local copy must not keep the instance alive)
  rig.run(1);  // fade in progress: the old chain is still processing
  CHECK(!ex.safe_to_free(sa) && ex.refs(sa->ledger_index) == 2);
  CHECK(Effect::live_instances() >= base + kOutputBusCount * 4 + 2);  // per bus: reverb, delay, master EQ, compressor
  rig.run(6);  // the fade (240 frames) is over
  CHECK(rig.br->mixer().chains_settled());
  CHECK(ex.refs(sa->ledger_index) == 1);  // only the held note keeps generation a (and its instance)
  CHECK(!ex.safe_to_free(sa));
  CHECK(ex.refs(sb->ledger_index) == 1);  // the current chain entry

  // generations that nobody uses are free to go: publish c; b has no note and no chain entry after the fade
  Snapshot c = b;
  c.parts[0].chain = gain_chain({1.0f});
  Snapshot* sc = rig.publish(c);
  c.parts[0].chain.reset();
  rig.run(8);
  CHECK(ex.safe_to_free(sb) && !ex.safe_to_free(sc) && !ex.safe_to_free(sa));
  const std::int64_t before = Effect::live_instances();
  for (auto it = rig.snaps.begin(); it != rig.snaps.end(); ++it)
    if (it->get() == sb) {
      it->reset();  // the control thread frees generation b; its chain instance dies with it
      break;
    }
  CHECK(Effect::live_instances() == before - 1);

  // release the note: a is no longer referenced
  rig.note(false, 0, 60);
  rig.run(12);
  CHECK(ex.refs(sa->ledger_index) == 0 && ex.safe_to_free(sa));
}

// --- FX budget reservation ---------------------------------------------------------------------------------------------------
void test_fx_budget() {
  Snapshot s = Snapshot::defaults();
  s.parts[0].insert_count = 2;
  s.parts[0].inserts[0].type = params::kInsertTypeRotary;
  s.parts[0].inserts[1].type = params::kInsertTypeCompressor;
  s.parts[3].insert_count = 3;
  s.parts[3].inserts[0].type = params::kInsertTypeEq;
  s.parts[3].inserts[1].type = params::kInsertTypeEpAmp;         // Story 2.9: the oversampled EP amp declares its larger cost
  s.parts[3].inserts[2].type = params::kInsertTypeChorusPhaser;
  const std::uint64_t config_only = fx_cost_units(s);  // chains not built: counted from the configured types
  build_chains(s);
  const std::uint64_t built = fx_cost_units(s);
  CHECK(config_only == built);
  CHECK(built == kFixedFxCost + effect_cost_units(EffectType::kRotary) + effect_cost_units(EffectType::kCompressor) +
                     effect_cost_units(EffectType::kEq) + effect_cost_units(EffectType::kEpAmp) + effect_cost_units(EffectType::kChorusPhaser));
  CHECK(effect_cost_units(EffectType::kEpAmp) > effect_cost_units(EffectType::kEq));
  CHECK(fx_cost_units(Snapshot::defaults()) == kFixedFxCost);

  VoiceArbiter::LiveSetConfig cfg;
  CHECK(fill_fx_costs(s, cfg));
  CHECK(cfg.fx_instance_count == 8 + 5);  // per output bus: reverb, delay, master EQ, compressor; plus 5 inserts
  std::uint64_t sum = 0;
  for (std::uint32_t i = 0; i < cfg.fx_instance_count; ++i) sum += cfg.fx_instance_costs[i];
  CHECK(sum == built);

  // reservation order: FX first, then the organ reservation, then the floors
  BlockRenderer br(8);
  OrganEngine organ;
  CHECK(br.bind_engine(organ));
  cfg.organ_rotary_cost = OrganEngine::reservation_cost_units(1);
  cfg.budget = built + cfg.organ_rotary_cost + 100;
  CHECK(br.arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kOk);
  CHECK(br.arbiter().reservation().fx == built);
  CHECK(br.arbiter().reservation().organ_rotary == cfg.organ_rotary_cost);
  CHECK(br.arbiter().reservation().voice_budget == 100);
  cfg.budget = built + cfg.organ_rotary_cost - 1;  // the FX fits, the organ behind it does not
  CHECK(br.arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kOrganExceedsBudget);
  cfg.budget = built - 1;  // the FX alone does not fit: reported before the organ is even looked at
  CHECK(br.arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kFxExceedsBudget);
  // the flat fx_cost and the computed instance costs add up
  cfg.budget = built + 50 + cfg.organ_rotary_cost;
  cfg.fx_cost = 50;
  CHECK(br.arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kOk && br.arbiter().reservation().fx == built + 50);
  cfg.fx_instance_count = static_cast<std::uint32_t>(cfg.fx_instance_costs.size() + 1);
  CHECK(br.arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kInvalidConfig);

  // the budget at which nothing is ever refused covers every slot of every part
  Snapshot full = Snapshot::defaults();
  for (PartCfg& p : full.parts) {
    p.insert_count = 5;
    for (auto& in : p.inserts) in.type = params::kInsertTypeRotary;
  }
  CHECK(fx_cost_units(full) <= kMaxFxCost && fx_cost_units(full) == kMaxFxCost);
  VoiceArbiter::LiveSetConfig big;
  build_chains(full);
  CHECK(fill_fx_costs(full, big) && big.fx_instance_count <= VoiceArbiter::LiveSetConfig::kMaxFxInstances);
}

void test_effect_factory() {
  CHECK(make_effect(EffectType::kNone) == nullptr);
  CHECK(make_effect(static_cast<EffectType>(params::kInsertTypeCount)) == nullptr);
  for (std::uint8_t t = 1; t < params::kInsertTypeCount; ++t) {
    auto fx = make_effect(static_cast<EffectType>(t));
    CHECK(fx && static_cast<std::uint8_t>(fx->type()) == t && fx->cost_units() == effect_cost_units(static_cast<EffectType>(t)));
    float l[64], r[64];
    for (int i = 0; i < 64; ++i) l[i] = r[i] = 0.01f * static_cast<float>(i);
    StereoBlock b{l, r, 64};
    fx->process(b);
    // The EQ at its default (0 dB on every band) and the chorus/phaser at its default (mix 0) pass the signal through bit for bit; the compressor at
    // its default setting (threshold -30 dB, ratio 4.5:1), the EP amp and the rotary work on it (the real DSP is tested in eq_compressor_tests,
    // chorus_phaser_tests, ep_amp_tests and rotary_insert_tests).
    const bool transparent = t == params::kInsertTypeEq || t == params::kInsertTypeChorusPhaser;
    if (transparent) CHECK(l[63] == 0.01f * 63.0f && r[10] == 0.01f * 10.0f);
    else CHECK(l[63] != 0.01f * 63.0f || r[10] != 0.01f * 10.0f);
  }
  const std::int64_t n0 = Effect::live_instances();
  { auto fx = make_effect(EffectType::kEq); CHECK(Effect::live_instances() == n0 + 1); }
  CHECK(Effect::live_instances() == n0);
}

}  // namespace

int main() {
  test_effect_factory();
  test_main_aux_sends_master();
  test_line_in();
  test_default_path_bit_exact();
  test_organ_split();
  test_sample_engine_routed();
  test_render_split_channel_mapping();
  test_aux_fold_and_bad_buses();
  test_mixer_robustness();
  test_chain_crossfade();
  test_chain_changes_keep_the_tone();
  test_effect_lifetime();
  test_fx_budget();
  if (g_fail) std::printf("routing_tests: %d failure(s)\n", g_fail);
  else std::printf("routing_tests: ok\n");
  return g_fail ? 1 : 0;
}
