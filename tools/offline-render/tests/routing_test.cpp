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
  // Send levels (unity placeholder returns into Main) and a master trim of -6 dB; part 3 on Aux with a send too.
  {
    RenderOptions o;
    o.routing.enabled = true;
    o.routing.output[3] = 1;
    o.routing.send_reverb[0] = 0.5f;
    o.routing.send_delay[1] = 0.25f;
    o.routing.send_reverb[3] = 1.0f;
    o.routing.master_level_db = -6.0f;
    WavData aux;
    const WavData main = render(all, o, &aux);
    const float g = std::pow(10.0f, -6.0f * 0.05f);
    const WavData em = mix({{g, &p[0]}, {g, &p[1]}, {g, &p[2]}, {0.5f * g, &p[0]}, {0.25f * g, &p[1]}});
    const WavData ea = mix({{g, &p[3]}, {1.0f * g, &p[3]}});  // an Aux part's send is returned to Aux: Main is untouched by it
    NullParams np;
    np.tolerance_db = -110;
    CHECK(null_test(em, main, np).pass && null_test(ea, aux, np).pass);
    CHECK(max_diff(em, main) < 3e-6 && max_diff(ea, aux) < 3e-6);
  }
  // Pass-through insert chains in every slot type, a bypassed one and an empty slot: audio unchanged.
  {
    RenderOptions o;
    o.routing.enabled = true;
    for (int part = 0; part < 4; ++part)
      for (int i = 0; i < 5; ++i) {
        RoutingOptions::Insert in;
        in.type = static_cast<std::uint8_t>((part + i) % 6);
        in.bypass = (i == 3);
        o.routing.inserts[part].push_back(in);
      }
    const WavData w = render(all, o);
    CHECK(max_diff(w, base) < 2e-6);
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
  // sends reach Main through the unity placeholder return
  const std::string sent = d + "/routing_sent.wav";
  CHECK(run("--midi " + mid + " --tail-seconds 0.25 --send 0:0.5:0.25 --out \"" + sent + "\""));
  WavData ws;
  CHECK(read_wav(sent, ws, err));
  const WavData es = mix({{1.75f, &wp}});
  CHECK(max_diff(es, ws) < 3e-6);
  // bad arguments are refused with exit status 2 and no output
  const std::string none = d + "/routing_none.wav";
  std::remove(none.c_str());
  const char* const bad[] = {"--part-output 8:aux", "--part-output 0:side", "--send 0:1.5", "--send 9:0.1", "--inserts 0:reverb",
                             "--inserts 0:eq,eq,eq,eq,eq,eq", "--master-level 20", "--line-in-level 2", "--inserts x:eq"};
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
  test_channel_limit();
#ifdef OFFLINE_RENDER_EXE
  test_cli();
#endif
  if (g_fail) std::printf("offline_routing_tests: %d failure(s)\n", g_fail);
  else std::printf("offline_routing_tests: ok\n");
  return g_fail ? 1 : 0;
}
