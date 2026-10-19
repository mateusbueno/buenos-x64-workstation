// Story 2.7: the routing topology verified with null tests against analytically mixed references, in the offline renderer.
// Each part (a MIDI channel) is rendered alone with the pre-2.7 path; the expected Main / Aux buses are mixed from those in the
// test (Main/Aux assignment, send levels with unity placeholder returns, line-in to the master only, master level), and the render
// with the routing options is compared with them. The default path (no routing option) is already pinned by the existing goldens.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "nulltest.hpp"
#include "velora/core/compressor.hpp"
#include "velora/core/eq.hpp"
#include "velora/core/reverb.hpp"
#include "render.hpp"
#include "wav.hpp"

using namespace velora::offline;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::vector<NoteEvent> notes(std::initializer_list<std::pair<int, int>> chan_note) {
  std::vector<NoteEvent> ev;
  for (const auto& cn : chan_note) {
    NoteEvent e;
    e.on = true;
    e.channel = static_cast<std::uint8_t>(cn.first);
    e.note = static_cast<std::uint8_t>(cn.second);
    e.velocity16 = 40000;
    e.frame = static_cast<std::uint64_t>(cn.first) * 100;  // staggered onsets
    ev.push_back(e);
  }
  for (const auto& cn : chan_note) {
    NoteEvent e;
    e.on = false;
    e.channel = static_cast<std::uint8_t>(cn.first);
    e.note = static_cast<std::uint8_t>(cn.second);
    e.frame = 20000 + static_cast<std::uint64_t>(cn.first) * 50;
    ev.push_back(e);
  }
  return ev;
}

static WavData render(const std::vector<NoteEvent>& ev, const RenderOptions& o, WavData* aux = nullptr) {
  RenderOptions opt = o;
  opt.tail_seconds = 0.3;
  opt.aux_out = aux;
  WavData w;
  std::string err;
  const bool ok = render_events(ev, opt, w, err);
  if (!ok) std::printf("render error: %s\n", err.c_str());
  CHECK(ok);
  return w;
}

// The references are rendered per part alone and end with that part's own notes: the shorter signal is zero beyond its end.
static double max_diff(const WavData& a, const WavData& b) {
  double m = 0;
  const std::size_t n = std::max(a.samples.size(), b.samples.size());
  for (std::size_t i = 0; i < n; ++i) {
    const float x = i < a.samples.size() ? a.samples[i] : 0.0f, y = i < b.samples.size() ? b.samples[i] : 0.0f;
    m = std::max(m, static_cast<double>(std::fabs(x - y)));
  }
  return m;
}

static double peak(const WavData& a) {
  double m = 0;
  for (float v : a.samples) m = std::max(m, static_cast<double>(std::fabs(v)));
  return m;
}

// out = sum of g_i * w_i
static WavData mix(std::initializer_list<std::pair<float, const WavData*>> terms) {
  WavData out;
  std::size_t n = 0;
  for (const auto& t : terms) n = std::max(n, t.second->samples.size());
  out.samples.assign(n, 0.0f);
  for (const auto& t : terms)
    for (std::size_t i = 0; i < t.second->samples.size(); ++i) out.samples[i] += t.first * t.second->samples[i];
  return out;
}

static void test_assignment_sends_master() {
  const std::vector<NoteEvent> all = notes({{0, 60}, {1, 64}, {2, 67}, {3, 72}});
  const RenderOptions plain;
  WavData p[4];
  const std::pair<int, int> cn[4] = {{0, 60}, {1, 64}, {2, 67}, {3, 72}};
  for (int i = 0; i < 4; ++i) p[i] = render(notes({cn[i]}), plain);
  const WavData base = render(all, plain);
  CHECK(peak(p[0]) > 0.05 && peak(base) > 0.1);

  // Routing options present but everything at its default: the render is bit-identical to the default path.
  {
    RenderOptions o;
    o.routing.enabled = true;
    const WavData w = render(all, o);
    CHECK(w.samples.size() == base.samples.size() &&
          std::memcmp(w.samples.data(), base.samples.data(), w.samples.size() * sizeof(float)) == 0);
  }
  // Main/Aux assignment: parts 2 and 3 to Aux. Main = p0 + p1, Aux = p2 + p3 (null tests, -120 dB).
  {
    RenderOptions o;
    o.routing.enabled = true;
    o.routing.output[2] = 1;
    o.routing.output[3] = 1;
    WavData aux;
    const WavData main = render(all, o, &aux);
    const WavData em = mix({{1.0f, &p[0]}, {1.0f, &p[1]}}), ea = mix({{1.0f, &p[2]}, {1.0f, &p[3]}});
    NullParams np;
    np.tolerance_db = -120;
    CHECK(null_test(em, main, np).pass);
    CHECK(null_test(ea, aux, np).pass);
    CHECK(max_diff(em, main) < 1e-6 && max_diff(ea, aux) < 1e-6);
    // Main + Aux = the default mix
    const WavData sum = mix({{1.0f, &main}, {1.0f, &aux}});
    CHECK(max_diff(sum, base) < 2e-6);
  }
  // Send levels (the delay is a unity placeholder return; the reverb return is muted here, the real reverb is tested in reverb_test.cpp)
  // and a master trim of -6 dB; part 3 on Aux with a send too.
  {
    RenderOptions o;
    o.routing.enabled = true;
    o.routing.reverb_level = 0.0f;
    o.routing.output[3] = 1;
    o.routing.send_reverb[0] = 0.5f;
    o.routing.send_delay[1] = 0.25f;
    o.routing.send_reverb[3] = 1.0f;
    o.routing.master_level_db = -6.0f;
    WavData aux;
    const WavData main = render(all, o, &aux);
    const float g = std::pow(10.0f, -6.0f * 0.05f);
    const WavData em = mix({{g, &p[0]}, {g, &p[1]}, {g, &p[2]}, {0.25f * g, &p[1]}});
    const WavData ea = mix({{g, &p[3]}});  // an Aux part's send is returned to Aux: Main is untouched by it
    NullParams np;
    np.tolerance_db = -110;
    CHECK(null_test(em, main, np).pass && null_test(ea, aux, np).pass);
    CHECK(max_diff(em, main) < 3e-6 && max_diff(ea, aux) < 3e-6);
  }
  // Transparent insert chains in every slot type, a bypassed one and an empty slot: audio unchanged. The rotary and the EP amp (Story 2.9) are real
  // DSP, so they are bypassed here; the chorus/phaser at its default (mix 0) is the identity.
  {
    RenderOptions o;
    o.routing.enabled = true;
    for (int part = 0; part < 4; ++part)
      for (int i = 0; i < 5; ++i) {
        RoutingOptions::Insert in;
        in.type = static_cast<std::uint8_t>((part + i) % 6);
        in.bypass = (i == 3) || in.type == 1 || in.type == 3;  // rotary, ep_amp
        if (in.type == 5) {  // compressor: ratio 1:1, no makeup = transparent (Story 2.8)
          in.has_params = true;
          in.params[1] = 0.0f;
        }
        o.routing.inserts[part].push_back(in);
      }
    const WavData w = render(all, o);
    CHECK(max_diff(w, base) < 2e-6);
  }
}

// Story 2.8: master EQ and compressor (in that order, each with its own bypass) and the insert EQ / compressor in the offline renderer.
// Expected signals are the parts rendered alone with the pre-2.7 path, put through the same DSP classes block by block (64 frames) in the test.
template <typename F>
static WavData through(const WavData& in, F&& fn) {
  WavData out = in;
  const std::size_t frames = in.samples.size() / 2;
  for (std::size_t o = 0; o < frames; o += 64) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(64, frames - o));
    float l[64], r[64];
    for (std::uint32_t i = 0; i < n; ++i) {
      l[i] = in.samples[2 * (o + i)];
      r[i] = in.samples[2 * (o + i) + 1];
    }
    velora::core::StereoBlock b{l, r, n};
    fn(b);
    for (std::uint32_t i = 0; i < n; ++i) {
      out.samples[2 * (o + i)] = l[i];
      out.samples[2 * (o + i) + 1] = r[i];
    }
  }
  return out;
}

static void test_master_and_insert_effects() {
  const auto ev = notes({{0, 60}, {1, 64}, {2, 67}});
  const WavData base = render(ev, RenderOptions{});
  CHECK(peak(base) > 0.01);

  // Defaults (EQ flat, compressor ratio 1:1 without makeup) and any bypassed settings: the signal of the plain render
  {
    RenderOptions o;
    o.routing.enabled = true;
    CHECK(max_diff(render(ev, o), base) < 2e-6);
    o.routing.master_eq = {9, -9, 500, 6, true};
    o.routing.master_comp = {-50, 20, 1, 50, 12, true};
    CHECK(max_diff(render(ev, o), base) < 2e-6);
  }
  // Master EQ alone, master compressor alone, both (EQ first)
  {
    RenderOptions o;
    o.routing.enabled = true;
    o.routing.master_eq = {-4, 5, 2500, 3, false};
    const WavData eq = render(ev, o);
    velora::core::EqEffect ref_eq;
    ref_eq.set_physical(-4, 5, 2500, 3, true);
    const WavData want_eq = through(base, [&](velora::core::StereoBlock& b) { ref_eq.process(b); });
    CHECK(max_diff(eq, want_eq) < 2e-6 && max_diff(eq, base) > 1e-3);

    RenderOptions c;
    c.routing.enabled = true;
    c.routing.master_comp = {-30, 3, 5, 150, 2, false};
    const WavData comp = render(ev, c);
    velora::core::CompressorEffect ref_comp;
    ref_comp.set_physical(-30, 3, 5, 150, 2, true);
    const WavData want_comp = through(base, [&](velora::core::StereoBlock& b) { ref_comp.process(b); });
    CHECK(max_diff(comp, want_comp) < 2e-6 && max_diff(comp, base) > 1e-3);

    o.routing.master_comp = c.routing.master_comp;
    const WavData both = render(ev, o);
    velora::core::EqEffect e2;
    velora::core::CompressorEffect c2;
    e2.set_physical(-4, 5, 2500, 3, true);
    c2.set_physical(-30, 3, 5, 150, 2, true);
    const WavData want_both = through(base, [&](velora::core::StereoBlock& b) { e2.process(b); c2.process(b); });
    CHECK(max_diff(both, want_both) < 2e-6);
    // each stage has its own bypass: bypassing the EQ leaves the compressor alone and vice versa
    RenderOptions only_comp = o;
    only_comp.routing.master_eq.bypass = true;
    CHECK(max_diff(render(ev, only_comp), want_comp) < 2e-6);
    RenderOptions only_eq = o;
    only_eq.routing.master_comp.bypass = true;
    CHECK(max_diff(render(ev, only_eq), want_eq) < 2e-6);
  }
  // Insert EQ / compressor on part 0: the other parts are untouched (Main = fx(part 0) + parts 1, 2)
  {
    const WavData p0 = render(notes({{0, 60}}), RenderOptions{}), p12 = render(notes({{1, 64}, {2, 67}}), RenderOptions{});
    RenderOptions o;
    o.routing.enabled = true;
    RoutingOptions::Insert e, c;
    e.type = 4;
    e.has_params = true;
    e.params = {0.9f, 0.2f, 0.6f, 0.8f, 0.0f};
    c.type = 5;
    c.has_params = true;
    c.params = {0.2f, 0.6f, 0.3f, 0.4f, 0.15f};
    o.routing.inserts[0] = {e, c};
    const WavData got = render(ev, o);
    velora::core::EqEffect ie;
    velora::core::CompressorEffect ic;
    ie.set_params(e.params);
    ic.set_params(c.params);
    WavData fx0 = through(p0, [&](velora::core::StereoBlock& b) { ie.process(b); ic.process(b); });
    const WavData want = mix({{1.0f, &fx0}, {1.0f, &p12}});
    CHECK(max_diff(got, want) < 2e-6 && max_diff(got, base) > 1e-3);
    // a bypassed slot does nothing
    o.routing.inserts[0][0].bypass = true;
    o.routing.inserts[0][1].bypass = true;
    CHECK(max_diff(render(ev, o), base) < 2e-6);
  }
}

static void test_channel_limit() {
  RenderOptions o;
  o.routing.enabled = true;
  WavData w;
  std::string err;
  CHECK(!render_events(notes({{9, 60}}), o, w, err) && err.find("channels 0..7") != std::string::npos);
  CHECK(render_events(notes({{9, 60}}), RenderOptions{}, w, err));  // without routing options the pre-2.7 behaviour stays
}

static void test_line_in() {
  WavData line;
  line.channels = 2;
  line.sample_rate = 48000;
  const std::size_t frames = 48000;
  line.samples.resize(frames * 2);
  for (std::size_t i = 0; i < frames; ++i) {
    line.samples[2 * i] = 0.3f * std::sin(0.011f * static_cast<float>(i));
    line.samples[2 * i + 1] = 0.2f * std::cos(0.017f * static_cast<float>(i));
  }
  const std::vector<NoteEvent> ev = notes({{0, 60}, {2, 67}});
  const WavData p0 = render(notes({{0, 60}}), RenderOptions{}), p2 = render(notes({{2, 67}}), RenderOptions{});
  RenderOptions o;
  o.routing.enabled = true;
  o.routing.output[2] = 1;
  o.routing.line_in = &line;
  o.routing.line_in_level = 0.5f;
  WavData aux;
  const WavData main = render(ev, o, &aux);
  // expected: Main = p0 + 0.5 * line (zero after the file's end), Aux = p2 only: the line-in never reaches Aux
  WavData em = p0, ea = p2;
  em.samples.resize(main.samples.size(), 0.0f);
  ea.samples.resize(main.samples.size(), 0.0f);
  for (std::size_t i = 0; i < em.frames(); ++i)
    if (i < frames) {
      em.samples[2 * i] += 0.5f * line.samples[2 * i];
      em.samples[2 * i + 1] += 0.5f * line.samples[2 * i + 1];
    }
  CHECK(max_diff(em, main) < 2e-6 && max_diff(ea, aux) < 2e-6);
  NullParams np;
  np.tolerance_db = -110;
  CHECK(null_test(em, main, np).pass && null_test(ea, aux, np).pass);
  // level 0: the line-in is not heard anywhere
  o.routing.line_in_level = 0.0f;
  const WavData silent = render(ev, o, &aux);
  CHECK(max_diff(p0, silent) < 1e-6 && max_diff(p2, aux) < 1e-6);
  // mono line-in feeds both channels
  WavData mono;
  mono.channels = 1;
  mono.samples.assign(frames, 0.25f);
  RenderOptions om;
  om.routing.enabled = true;
  om.routing.line_in = &mono;
  om.routing.line_in_level = 1.0f;
  const WavData mm = render(notes({}), om);
  CHECK(std::fabs(mm.samples[2 * 1000] - 0.25f) < 1e-6f && std::fabs(mm.samples[2 * 1000 + 1] - 0.25f) < 1e-6f);
  // a wrong-rate line-in is refused
  WavData bad = mono;
  bad.sample_rate = 44100;
  RenderOptions ob;
  ob.routing.enabled = true;
  ob.routing.line_in = &bad;
  WavData w;
  std::string err;
  CHECK(!render_events(notes({{0, 60}}), ob, w, err));
}

// The reverb through the offline renderer (Story 2.10): the wet signal equals the stand-alone ReverbSend fed with the part's dry signal scaled
// by the send level and the REV master level, per bus; with every send at 0 (or the REV master at 0, or the return level at 0) the render is
// bit-identical to the default path.
static WavData reverb_of(const WavData& dry, float gain, velora::core::ReverbType type, float time_s, float level, std::size_t frames) {
  velora::core::ReverbSend rv;
  rv.configure(type, time_s, level, /*instant=*/true);
  WavData out;
  out.samples.assign(frames * 2, 0.0f);
  float l[64], r[64];
  for (std::size_t pos = 0; pos < frames; pos += 64) {
    for (std::size_t i = 0; i < 64; ++i) {
      const std::size_t k = (pos + i) * 2;
      l[i] = k < dry.samples.size() ? gain * dry.samples[k] : 0.0f;
      r[i] = k + 1 < dry.samples.size() ? gain * dry.samples[k + 1] : 0.0f;
    }
    velora::core::StereoBlock b{l, r, 64};
    rv.process(b);
    for (std::size_t i = 0; i < 64 && pos + i < frames; ++i) {
      out.samples[(pos + i) * 2] = l[i];
      out.samples[(pos + i) * 2 + 1] = r[i];
    }
  }
  return out;
}

static void test_reverb() {
  const std::vector<NoteEvent> all = notes({{0, 60}, {1, 64}, {2, 67}, {3, 72}});
  const RenderOptions plain;
  const WavData base = render(all, plain);
  WavData p0 = render(notes({{0, 60}}), plain), p3 = render(notes({{3, 72}}), plain);
  // idle settings: bit-identical to the default path whatever the reverb type, time and (unused) level
  for (int ty = 0; ty < 3; ++ty) {
    RenderOptions o;
    o.routing.enabled = true;
    o.routing.reverb_type = static_cast<std::uint8_t>(ty);
    o.routing.reverb_time_s = 10.0f;
    const WavData w = render(all, o);
    CHECK(w.samples.size() == base.samples.size() && std::memcmp(w.samples.data(), base.samples.data(), w.samples.size() * sizeof(float)) == 0);
  }
  {  // sends up but the REV master at 0: bit-identical as well; return level 0: the wet is silent, the dry untouched
    RenderOptions o;
    o.routing.enabled = true;
    o.routing.send_reverb[0] = 1.0f;
    o.routing.send_reverb[3] = 0.5f;
    o.routing.reverb_master = 0.0f;
    const WavData w = render(all, o);
    CHECK(w.samples.size() == base.samples.size() && std::memcmp(w.samples.data(), base.samples.data(), w.samples.size() * sizeof(float)) == 0);
    o.routing.reverb_master = 1.0f;
    o.routing.reverb_level = 0.0f;
    const WavData w2 = render(all, o);
    CHECK(w2.samples.size() == base.samples.size() && std::memcmp(w2.samples.data(), base.samples.data(), w2.samples.size() * sizeof(float)) == 0);
  }
  // Main and Aux each have their own reverb; send x REV master scales the input
  for (int ty = 0; ty < 3; ++ty) {
    const velora::core::ReverbType type = static_cast<velora::core::ReverbType>(ty);
    RenderOptions o;
    o.routing.enabled = true;
    o.routing.output[3] = 1;
    o.routing.send_reverb[0] = 0.5f;
    o.routing.send_reverb[3] = 1.0f;
    o.routing.reverb_type = static_cast<std::uint8_t>(ty);
    o.routing.reverb_time_s = 1.5f;
    o.routing.reverb_level = 0.7f;
    o.routing.reverb_master = 0.8f;
    WavData aux;
    const WavData main = render(all, o, &aux);
    const std::size_t frames = main.samples.size() / 2;
    // Main: the dry mix of parts 0..2 plus the wet of part 0; Aux: part 3 plus its own wet
    const WavData wet_main = reverb_of(p0, 0.5f * 0.8f, type, 1.5f, 0.7f, frames), wet_aux = reverb_of(p3, 1.0f * 0.8f, type, 1.5f, 0.7f, frames);
    const WavData dry_main = render(notes({{0, 60}, {1, 64}, {2, 67}}), plain);
    const WavData em = mix({{1.0f, &dry_main}, {1.0f, &wet_main}}), ea = mix({{1.0f, &p3}, {1.0f, &wet_aux}});
    NullParams np;
    np.tolerance_db = -110;
    CHECK(null_test(em, main, np).pass && null_test(ea, aux, np).pass);
    CHECK(max_diff(em, main) < 3e-6 && max_diff(ea, aux) < 3e-6);
    CHECK(peak(wet_main) > 0.005 && peak(wet_aux) > 0.005);
  }
  // wrong settings produce a different render: type, time, level and REV master each move the output
  RenderOptions o;
  o.routing.enabled = true;
  o.routing.send_reverb[0] = 0.5f;
  o.routing.reverb_time_s = 1.5f;
  o.routing.reverb_level = 0.7f;
  const WavData ref = render(all, o);
  RenderOptions w1 = o, w2 = o, w3 = o, w4 = o;
  w1.routing.reverb_type = 2;
  w2.routing.reverb_time_s = 1.7f;
  w3.routing.reverb_level = 0.6f;
  w4.routing.reverb_master = 0.9f;
  for (const RenderOptions* w : {&w1, &w2, &w3, &w4}) {
    NullParams np;
    np.tolerance_db = -100;
    CHECK(!null_test(ref, render(all, *w), np).pass);
  }
}

#ifdef OFFLINE_RENDER_EXE
static bool run(const std::string& args) {
  const std::string cmd = std::string("\"") + OFFLINE_RENDER_EXE + "\" " + args + " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

static void test_cli() {
  const std::string d = OFFLINE_TEST_DIR, mid = std::string("\"") + ROUTING_TEST_MIDI + "\"";
  const std::string plain = d + "/routing_plain.wav", main = d + "/routing_main.wav", aux = d + "/routing_aux.wav";
  CHECK(run("--midi " + mid + " --tail-seconds 0.25 --out \"" + plain + "\""));
  CHECK(run("--midi " + mid + " --tail-seconds 0.25 --part-output 0:aux --inserts 0:eq,compressor! --inserts 1:rotary,chorus_phaser,ep_amp "
            "--aux-out \"" + aux + "\" --out \"" + main + "\""));
  WavData wp, wm, wa;
  std::string err;
  CHECK(read_wav(plain, wp, err) && read_wav(main, wm, err) && read_wav(aux, wa, err));
  CHECK(wp.samples.size() == wm.samples.size() && wm.samples.size() == wa.samples.size());
  CHECK(peak(wa) > 0.001);  // part 0 (the golden MIDI's channel) is on Aux
  const WavData sum = mix({{1.0f, &wm}, {1.0f, &wa}});
  CHECK(max_diff(sum, wp) < 2e-6);
  // sends reach Main through the unity placeholder return (delay; the reverb return is muted)
  const std::string sent = d + "/routing_sent.wav";
  CHECK(run("--midi " + mid + " --tail-seconds 0.25 --send 0:0.5:0.25 --reverb-level 0 --out \"" + sent + "\""));
  WavData ws;
  CHECK(read_wav(sent, ws, err));
  const WavData es = mix({{1.25f, &wp}});
  CHECK(max_diff(es, ws) < 3e-6);
  // bad arguments are refused with exit status 2 and no output
  const std::string none = d + "/routing_none.wav";
  std::remove(none.c_str());
  const char* const bad[] = {"--part-output 8:aux", "--part-output 0:side", "--send 0:1.5", "--send 9:0.1", "--inserts 0:reverb",
                             "--inserts 0:eq,eq,eq,eq,eq,eq", "--master-level 20", "--line-in-level 2", "--inserts x:eq",
                             "--reverb cave", "--reverb-time 11", "--reverb-time 0.1", "--reverb-level 2", "--rev-master -1", "--rev-master nan"};
  for (const char* b : bad) {
    const std::string cmd = std::string("\"") + OFFLINE_RENDER_EXE + "\" --midi " + mid + " --out \"" + none + "\" " + b + " >/dev/null 2>&1";
    const int rc = std::system(cmd.c_str());
    CHECK(rc != 0 && !std::ifstream(none).good());
  }
}
#endif

int main() {
  test_assignment_sends_master();
  test_line_in();
  test_master_and_insert_effects();
  test_channel_limit();
  test_reverb();
#ifdef OFFLINE_RENDER_EXE
  test_cli();
#endif
  if (g_fail) std::printf("offline_routing_tests: %d failure(s)\n", g_fail);
  else std::printf("offline_routing_tests: ok\n");
  return g_fail ? 1 : 0;
}
