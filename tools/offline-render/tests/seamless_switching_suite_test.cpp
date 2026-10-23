// Story 4.8: Seamless switching render suite (NFR14, FR79, FR80, FR81, FR82, AR2).
// Automated renders that switch scenes, Live Sets (preloaded in-setlist step and non-preloaded jump),
// and load setlists during held, sustained, and releasing notes, verifying with the gap/truncation detector
// that no drop in envelope or early cutoff occurs for notes started before the switch.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "gap_detector.hpp"
#include "render_live_set.hpp"
#include "smf.hpp"
#include "wav.hpp"

#ifndef SEAMLESS_FIXTURES_DIR
#define SEAMLESS_FIXTURES_DIR "tools/offline-render/tests/live_set"
#endif

using namespace velora::offline;

static int g_fail = 0;
#define CHECK(c)                                                        \
  do {                                                                  \
    if (!(c)) {                                                         \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
      ++g_fail;                                                         \
    }                                                                   \
  } while (0)

static std::string fixture_path(const std::string& rel) {
  return std::string(SEAMLESS_FIXTURES_DIR) + "/" + rel;
}

// Unit test the gap/truncation detector on synthetic signals.
static void test_detector_synthetic() {
  const std::size_t kSr = 48000;
  const std::size_t kLen = kSr * 4;  // 4 seconds
  WavData base;
  base.sample_rate = kSr;
  base.channels = 2;
  base.samples.resize(kLen * 2, 0.0f);

  // Generate a note decaying from 0.5s to 3.5s
  for (std::size_t f = kSr / 2; f < kSr * 7 / 2; ++f) {
    const double t = static_cast<double>(f - kSr / 2) / kSr;
    const double env = std::exp(-t * 0.8);  // natural decay
    const float s = static_cast<float>(0.5 * env * std::sin(2.0 * M_PI * 440.0 * f / kSr));
    base.samples[f * 2] = s;
    base.samples[f * 2 + 1] = s;
  }

  // 1. Bit-identical candidate must PASS
  WavData cand_good = base;
  GapDetectorOptions opt;
  opt.switch_frame = kSr * 2;  // switch at 2.0s
  GapReport rep = detect_gaps_and_truncation(base, cand_good, opt);
  CHECK(rep.pass);

  // 2. Candidate with early truncation at 2.5s must FAIL
  WavData cand_trunc = base;
  for (std::size_t f = kSr * 5 / 2; f < kLen; ++f) {
    cand_trunc.samples[f * 2] = 0.0f;
    cand_trunc.samples[f * 2 + 1] = 0.0f;
  }
  rep = detect_gaps_and_truncation(base, cand_trunc, opt);
  CHECK(!rep.pass);
  CHECK(rep.gap_time_sec >= 2.45 && rep.gap_time_sec <= 2.55);

  // 3. Candidate with a 50 ms gap / dropout at 2.2s must FAIL
  WavData cand_gap = base;
  const std::size_t gap_start = static_cast<std::size_t>(2.2 * kSr);
  const std::size_t gap_end = gap_start + (kSr * 50 / 1000);
  for (std::size_t f = gap_start; f < gap_end; ++f) {
    cand_gap.samples[f * 2] = 0.0f;
    cand_gap.samples[f * 2 + 1] = 0.0f;
  }
  rep = detect_gaps_and_truncation(base, cand_gap, opt);
  CHECK(!rep.pass);
  CHECK(rep.gap_time_sec >= 2.15 && rep.gap_time_sec <= 2.25);
}

// Builds a MIDI stream exercising:
// 1. Held notes crossing the switch
// 2. Releasing notes whose release tails cross the switch
// 3. Pedal-sustained notes (key released under pedal, sustained across switch, pedal released later)
// All notes start BEFORE the switch so the test purely verifies notes started before the switch.
static std::vector<NoteEvent> make_seamless_test_events(double switch_sec) {
  std::vector<NoteEvent> ev;
  auto add_note = [&](double t_on, double t_off, std::uint8_t key, std::uint8_t vel) {
    NoteEvent on, off;
    on.frame = static_cast<std::uint64_t>(std::round(t_on * 48000.0));
    on.tick = static_cast<std::uint64_t>(std::round(t_on * 960.0));
    on.on = true;
    on.channel = 0;
    on.note = key;
    on.velocity16 = static_cast<std::uint16_t>(vel) << 8;

    off.frame = static_cast<std::uint64_t>(std::round(t_off * 48000.0));
    off.tick = static_cast<std::uint64_t>(std::round(t_off * 960.0));
    off.on = false;
    off.channel = 0;
    off.note = key;
    off.velocity16 = 0;

    ev.push_back(on);
    ev.push_back(off);
  };
  auto add_pedal = [&](double t, bool down) {
    NoteEvent p;
    p.frame = static_cast<std::uint64_t>(std::round(t * 48000.0));
    p.tick = static_cast<std::uint64_t>(std::round(t * 960.0));
    p.cc = true;
    p.channel = 0;
    p.note = 64;
    p.value = down ? 127 : 0;
    ev.push_back(p);
  };

  // 1. Held notes across switch: chord 52 + 55 held from switch - 1.5s to switch + 1.5s
  add_note(switch_sec - 1.5, switch_sec + 1.5, 52, 90);
  add_note(switch_sec - 1.5, switch_sec + 1.5, 55, 85);

  // 2. Releasing note: key 50 down at switch - 1.0s, released at switch - 0.2s
  // (release phase sounds across the switch)
  add_note(switch_sec - 1.0, switch_sec - 0.2, 50, 95);

  // 3. Pedal-sustained notes: pedal down at switch - 0.6s, key 40 down at switch - 0.5s,
  // key released at switch - 0.1s (sustained by pedal across switch until pedal up at switch + 1.2s)
  add_pedal(switch_sec - 0.6, true);
  add_note(switch_sec - 0.5, switch_sec - 0.1, 40, 90);
  add_pedal(switch_sec + 1.2, false);

  std::stable_sort(ev.begin(), ev.end(), [](const NoteEvent& a, const NoteEvent& b) {
    if (a.frame != b.frame) return a.frame < b.frame;
    return a.on < b.on;
  });
  return ev;
}

// 1. Scene change during held, pedal-sustained, and releasing notes.
static void test_seamless_scene_change() {
  const std::string ls_path = fixture_path("scenes.liveset");
  const std::string root_path = fixture_path("root");
  const double switch_sec = 2.0;

  const std::vector<NoteEvent> events = make_seamless_test_events(switch_sec);

  LiveSetRenderOptions base_opt;
  base_opt.live_set_path = ls_path;
  base_opt.library_root = root_path;
  base_opt.tail_seconds = 1.0;

  WavData base_wav;
  std::string err;
  CHECK(render_live_set(events, base_opt, base_wav, err));

  // Switched render: scene recall at 2.0s
  LiveSetRenderOptions sw_opt = base_opt;
  SceneRecallAt r1;
  r1.frame = static_cast<std::uint64_t>(switch_sec * 48000);
  r1.index = 1;  // chorus
  sw_opt.scene_recalls = {r1};

  WavData sw_wav;
  CHECK(render_live_set(events, sw_opt, sw_wav, err));

  // Verify gap/truncation detector across the scene change
  GapDetectorOptions gopt;
  gopt.switch_frame = r1.frame;
  GapReport rep = detect_gaps_and_truncation(base_wav, sw_wav, gopt);
  if (!rep.pass) {
    std::printf("Scene change failed gap detection: %s\n", rep.message.c_str());
  }
  CHECK(rep.pass);
}

// 2. In-setlist Live Set step (preloaded, 0 simulated load blocks).
static void test_seamless_in_setlist_step_preloaded() {
  const std::string ls_a = fixture_path("switch/switch_a.liveset");
  const std::string ls_b = fixture_path("switch/switch_b.liveset");
  const std::string root_path = fixture_path("root");
  const double switch_sec = 3.0;

  const std::vector<NoteEvent> events = make_seamless_test_events(switch_sec);

  LiveSetRenderOptions base_opt;
  base_opt.live_set_path = ls_a;
  base_opt.library_root = root_path;
  base_opt.tail_seconds = 1.5;

  WavData base_wav;
  std::string err;
  CHECK(render_live_set(events, base_opt, base_wav, err));

  // Preloaded step: switch_load_blocks = 0
  LiveSetRenderOptions sw_opt = base_opt;
  SwitchAt sw;
  sw.frame = static_cast<std::uint64_t>(switch_sec * 48000);
  sw.live_set_path = ls_b;
  sw_opt.switches = {sw};
  sw_opt.switch_load_blocks = 0;  // instantaneous preloaded switch

  WavData sw_wav;
  CHECK(render_live_set(events, sw_opt, sw_wav, err));

  // Run gap detector across the preloaded switch
  GapDetectorOptions gopt;
  gopt.switch_frame = sw.frame;
  GapReport rep = detect_gaps_and_truncation(base_wav, sw_wav, gopt);
  if (!rep.pass) {
    std::printf("Preloaded step failed gap detection: %s\n", rep.message.c_str());
  }
  CHECK(rep.pass);
}

// 3. Jump to a non-preloaded Live Set (simulated background load time).
static void test_seamless_non_preloaded_jump() {
  const std::string ls_a = fixture_path("switch/switch_a.liveset");
  const std::string ls_b = fixture_path("switch/switch_b.liveset");
  const std::string root_path = fixture_path("root");
  const double switch_sec = 3.0;

  const std::vector<NoteEvent> events = make_seamless_test_events(switch_sec);

  LiveSetRenderOptions base_opt;
  base_opt.live_set_path = ls_a;
  base_opt.library_root = root_path;
  base_opt.tail_seconds = 1.5;

  WavData base_wav;
  std::string err;
  CHECK(render_live_set(events, base_opt, base_wav, err));

  // Non-preloaded jump: switch_load_blocks = 100 (~133 ms background load)
  LiveSetRenderOptions sw_opt = base_opt;
  SwitchAt sw;
  sw.frame = static_cast<std::uint64_t>(switch_sec * 48000);
  sw.live_set_path = ls_b;
  sw_opt.switches = {sw};
  sw_opt.switch_load_blocks = 100;

  WavData sw_wav;
  CHECK(render_live_set(events, sw_opt, sw_wav, err));

  // Run gap detector across the non-preloaded jump
  GapDetectorOptions gopt;
  gopt.switch_frame = sw.frame;
  GapReport rep = detect_gaps_and_truncation(base_wav, sw_wav, gopt);
  if (!rep.pass) {
    std::printf("Non-preloaded jump failed gap detection: %s\n", rep.message.c_str());
  }
  CHECK(rep.pass);
}

// 4. Setlist load during held, pedal-sustained, and releasing notes.
static void test_seamless_setlist_load() {
  const std::string ls_a = fixture_path("switch/switch_a.liveset");
  const std::string root_path = fixture_path("root");
  const double switch_sec = 2.0;

  const std::vector<NoteEvent> events = make_seamless_test_events(switch_sec);

  LiveSetRenderOptions base_opt;
  base_opt.live_set_path = ls_a;
  base_opt.library_root = root_path;
  base_opt.tail_seconds = 1.5;

  WavData base_wav;
  std::string err;
  CHECK(render_live_set(events, base_opt, base_wav, err));

  // Setlist load at switch_sec
  LiveSetRenderOptions sw_opt = base_opt;
  SetlistLoadAt sl;
  sl.frame = static_cast<std::uint64_t>(switch_sec * 48000);
  sl.setlist_id = "test_setlist_id";
  sw_opt.setlist_loads = {sl};

  WavData sw_wav;
  LiveSetRenderReport rep_report;
  CHECK(render_live_set(events, sw_opt, sw_wav, err, &rep_report));
  CHECK(rep_report.setlists_loaded == 1);

  // Loading a setlist does not affect sounding audio: candidate and baseline are bit-identical
  GapDetectorOptions gopt;
  gopt.switch_frame = sl.frame;
  GapReport rep = detect_gaps_and_truncation(base_wav, sw_wav, gopt);
  if (!rep.pass) {
    std::printf("Setlist load failed gap detection: %s\n", rep.message.c_str());
  }
  CHECK(rep.pass);
}

// 5. Negative test cases: verify that actual audio gaps or truncations on rendered cases trigger failure.
static void test_seamless_negative_cases() {
  const std::string ls_a = fixture_path("switch/switch_a.liveset");
  const std::string ls_b = fixture_path("switch/switch_b.liveset");
  const std::string root_path = fixture_path("root");
  const double switch_sec = 3.0;

  const std::vector<NoteEvent> events = make_seamless_test_events(switch_sec);

  LiveSetRenderOptions base_opt;
  base_opt.live_set_path = ls_a;
  base_opt.library_root = root_path;
  base_opt.tail_seconds = 1.5;

  WavData base_wav;
  std::string err;
  CHECK(render_live_set(events, base_opt, base_wav, err));

  LiveSetRenderOptions sw_opt = base_opt;
  SwitchAt sw;
  sw.frame = static_cast<std::uint64_t>(switch_sec * 48000);
  sw.live_set_path = ls_b;
  sw_opt.switches = {sw};
  sw_opt.switch_load_blocks = 0;

  WavData sw_wav;
  CHECK(render_live_set(events, sw_opt, sw_wav, err));

  GapDetectorOptions gopt;
  gopt.switch_frame = sw.frame;

  // Negative 1: 20 ms gap / dropout inserted right at switch
  WavData gap_wav = sw_wav;
  const std::size_t gap_start = sw.frame + 480;  // 10 ms after switch
  const std::size_t gap_end = gap_start + (48000 * 20 / 1000);  // 20 ms long
  for (std::size_t f = gap_start; f < gap_end && f * 2 + 1 < gap_wav.samples.size(); ++f) {
    gap_wav.samples[f * 2] = 0.0f;
    gap_wav.samples[f * 2 + 1] = 0.0f;
  }
  GapReport rep_gap = detect_gaps_and_truncation(base_wav, gap_wav, gopt);
  CHECK(!rep_gap.pass);

  // Negative 2: Truncation: voices abruptly killed 100 ms after switch
  WavData trunc_wav = sw_wav;
  const std::size_t trunc_start = sw.frame + 4800;  // 100 ms after switch
  for (std::size_t f = trunc_start; f * 2 + 1 < trunc_wav.samples.size(); ++f) {
    trunc_wav.samples[f * 2] = 0.0f;
    trunc_wav.samples[f * 2 + 1] = 0.0f;
  }
  GapReport rep_trunc = detect_gaps_and_truncation(base_wav, trunc_wav, gopt);
  CHECK(!rep_trunc.pass);

  // Negative 3: Audio mismatch before switch
  WavData pre_mismatch_wav = sw_wav;
  pre_mismatch_wav.samples[48000] += 0.2f;
  GapReport rep_pre = detect_gaps_and_truncation(base_wav, pre_mismatch_wav, gopt);
  CHECK(!rep_pre.pass);
}

int main() {
  std::printf("Running seamless switching render suite (Story 4.8)...\n");
  test_detector_synthetic();
  test_seamless_scene_change();
  test_seamless_in_setlist_step_preloaded();
  test_seamless_non_preloaded_jump();
  test_seamless_setlist_load();
  test_seamless_negative_cases();

  if (g_fail) {
    std::printf("seamless_switching_suite: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("seamless_switching_suite: all cases passed (zero gaps, zero truncations)\n");
  return 0;
}
