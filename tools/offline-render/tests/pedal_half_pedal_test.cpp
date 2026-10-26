// Story 3.11: half-pedal damping of a sample instrument, rendered through render_live_set() (the same path as `velora-offline-render --live-set`). One
// sample part (a looped constant, ampeg_release 0.4 s) is played with a note off at 0.5 s under three sustain pedal positions: up (CC 64 = 0), half (CC 64 = 40
// of 127, below the hold threshold) and down (CC 64 = 127). The release after the note off must be slowest for the held pedal (the note does not release at
// all), fastest with the pedal up and in between with the half pedal: its time constant is the up one divided by the documented law
// (SampleEngine::half_pedal_scale). A binary pedal (0 / 127) is unchanged. Library and Live Set are built here at run time (no golden).
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "render_live_set.hpp"
#include "velora/format/bundle.hpp"
#include "velora/host/live_set_store.hpp"

namespace fmt = velora::format;
namespace ls = velora::host::liveset;
namespace off = velora::offline;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

static bool mkdir_p(const std::string& p) {
  for (std::size_t i = 1; i <= p.size(); ++i) {
    if (i != p.size() && p[i] != '/') continue;
    if (::mkdir(p.substr(0, i).c_str(), 0755) != 0 && errno != EEXIST) return false;
  }
  return true;
}

static off::NoteEvent note(std::uint64_t frame, bool on, std::uint8_t key) {
  off::NoteEvent e;
  e.frame = frame;
  e.on = on;
  e.channel = 5;  // any keyboard source: in a Live Set the channel is not a part address
  e.note = key;
  e.velocity16 = 65535;
  return e;
}
static off::NoteEvent cc(std::uint64_t frame, std::uint8_t number, std::uint8_t value) {
  off::NoteEvent e;
  e.frame = frame;
  e.cc = true;
  e.channel = 5;
  e.note = number;
  e.value = value;
  return e;
}



// Mean absolute value of the left channel over frames [from, to).
static double level(const off::WavData& w, std::size_t from, std::size_t to) {
  double s = 0;
  for (std::size_t i = from; i < to; ++i) s += std::fabs(w.samples[i * w.channels]);
  return s / static_cast<double>(to - from);
}

int main() {
  const std::string work = std::string(OFFLINE_TEST_DIR) + "/pedal_half_pedal";
  std::system(("rm -rf '" + work + "'").c_str());
  const std::string root = work + "/root";
  const fmt::Uuid lib = fmt::library_id_from_fingerprint({reinterpret_cast<const std::uint8_t*>("half-pedal-test-1"), 16});
  fmt::BundleWriter w;
  w.set_library_id(lib);
  auto ins = w.add_instrument("dc/hold.sfz", "dc/hold.sfz");
  CHECK(ins.ok());
  std::vector<std::int32_t> pcm(4800, 10000);
  CHECK(w.add_sample(ins.value(), {"s", {1, 16, false, 48000}, 4800, pcm}) == fmt::Error::kNone);
  fmt::Region r;
  r.key_lo = 0;
  r.key_hi = 127;
  r.key_center = 60;
  r.loop_mode = fmt::LoopMode::kLoopContinuous;
  r.ampeg.release = 0.4f;
  CHECK(w.add_region(ins.value(), r) == fmt::Error::kNone);
  auto bytes = w.finalize();
  CHECK(bytes.ok());
  auto probe = fmt::BundleReader::open_memory(bytes.value());
  CHECK(probe.ok());
  if (g_fail) return 1;
  const std::string lib_id = fmt::uuid_to_string(lib), bundle_id = fmt::to_hex(probe.value().bundle_id());
  CHECK(mkdir_p(root + "/lib/" + lib_id + "/" + bundle_id));
  {
    std::ofstream f(root + "/lib/" + lib_id + "/" + bundle_id + "/bundle.vlb", std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
  }
  CHECK(::symlink(bundle_id.c_str(), (root + "/lib/" + lib_id + "/current").c_str()) == 0);

  velora::host::liveset::lsp::LiveSet s = ls::make_default_live_set("5d0a7c31-9b2e-4f68-a417-3c8e1b6f0d25", "Half pedal", 8);
  s.mutable_parts(0)->mutable_timbre()->set_library_id(lib_id);
  s.mutable_parts(0)->mutable_timbre()->set_instrument_key("dc/hold.sfz");
  CHECK(ls::validate(s, 8).ok());
  std::string ls_bytes;
  CHECK(ls::serialize(s, ls_bytes) == ls::Status::kOk);
  const std::string ls_path = work + "/half.liveset";
  { std::ofstream f(ls_path, std::ios::binary | std::ios::trunc); f.write(ls_bytes.data(), static_cast<std::streamsize>(ls_bytes.size())); }

  off::LiveSetRenderOptions opt;
  opt.live_set_path = ls_path;
  opt.library_root = root;
  opt.tail_seconds = 1.0;

  constexpr std::uint64_t kOff = 24000;  // the note off, 0.5 s
  auto render = [&](int pedal_cc) {
    std::vector<off::NoteEvent> ev = {cc(0, 64, static_cast<std::uint8_t>(pedal_cc)), note(0, true, 60), note(kOff, false, 60)};
    off::WavData out;
    std::string err;
    CHECK(off::render_live_set(ev, opt, out, err, nullptr));
    return out;
  };
  const off::WavData up = render(0), half = render(40), down = render(127), pre = render(63), c32 = render(32), c56 = render(56);
  // The note itself sounds the same in all three (the pedal does not change the attack).
  const double on_up = level(up, 10000, 20000);
  CHECK(on_up > 0.05);
  CHECK(std::fabs(level(half, 10000, 20000) - on_up) < 1e-6 && std::fabs(level(down, 10000, 20000) - on_up) < 1e-6);
  // After the note off: 0.05 s .. 0.07 s, then 0.20 s .. 0.22 s.
  const std::size_t a0 = kOff + 2400, a1 = kOff + 3360, b0 = kOff + 9600, b1 = kOff + 10560;
  const double up_a = level(up, a0, a1), half_a = level(half, a0, a1), down_a = level(down, a0, a1);
  const double up_b = level(up, b0, b1), half_b = level(half, b0, b1), down_b = level(down, b0, b1);
  std::printf("levels after note off (0.05 s / 0.20 s): up %.5f %.5f, half %.5f %.5f, down %.5f %.5f (note %.5f)\n", up_a, up_b, half_a, half_b, down_a, down_b, on_up);
  CHECK(std::fabs(down_a - on_up) < 1e-6 && std::fabs(down_b - on_up) < 1e-6);  // the held note does not release at all
  CHECK(up_a < on_up * 0.95 && up_b < up_a);                                    // the up pedal releases
  CHECK(half_a > up_a && half_b > up_b);                                         // half pedal: slower than up ...
  CHECK(half_a < down_a && half_b < down_b);                                     // ... faster than down
  // The law: release time x 1 / scale, so ln(level) falls at `scale` times the up rate (levels still well above the release threshold).
  const double scale = 1.0 - 0.9 * 40.0 / 63.5;
  const double r_up = std::log(up_b / up_a), r_half = std::log(half_b / half_a);
  CHECK(r_up < 0 && std::fabs(r_half / r_up - scale) < 0.03);
  // A pedal just below the hold threshold is slower still (monotonic in the pedal), and 63 / 127 is below it.
  const double pre_b = level(pre, b0, b1);
  CHECK(pre_b > half_b && pre_b < down_b);
  // Monotonic over the half-pedal zone (0, 63.5): the further down, the longer the release; all shorter than a held note, longer than pedal up.
  {
    const double l32 = level(c32, b0, b1), l40 = half_b, l56 = level(c56, b0, b1), l63 = pre_b;
    CHECK(up_b < l32 && l32 < l40 && l40 < l56 && l56 < l63 && l63 < down_b);
  }
  // A2: a normal pedal lift does not lengthen the release of the held notes: CC 127, note off (held), CC 60, CC 0 -> released at CC 60 at the instrument's own rate.
  {
    std::vector<off::NoteEvent> ev = {cc(0, 64, 127), note(0, true, 60), note(kOff, false, 60), cc(30000, 64, 60), cc(36000, 64, 0)};
    off::WavData out;
    std::string err;
    CHECK(off::render_live_set(ev, opt, out, err, nullptr));
    CHECK(std::fabs(level(out, 28000, 29000) - on_up) < 1e-6);  // held until the lift
    const double ra = level(out, 30000 + 2400, 30000 + 3360), rb = level(out, 30000 + 9600, 30000 + 10560);
    CHECK(std::fabs(std::log(rb / ra) / r_up - 1.0) < 0.03);
  }
  // A binary pedal that goes up and down keeps its behaviour: pedal down at the note off, up at 0.8 s -> the release starts then, at the normal rate.
  {
    std::vector<off::NoteEvent> ev = {cc(0, 64, 127), note(0, true, 60), note(kOff, false, 60), cc(38400, 64, 0)};
    off::WavData out;
    std::string err;
    CHECK(off::render_live_set(ev, opt, out, err, nullptr));
    CHECK(std::fabs(level(out, 30000, 36000) - on_up) < 1e-6);                                    // held while the pedal is down
    const double ra = level(out, 38400 + 2400, 38400 + 3360), rb = level(out, 38400 + 9600, 38400 + 10560);
    CHECK(std::fabs(std::log(rb / ra) / r_up - 1.0) < 0.03);                                      // released at the instrument's own rate
  }
  std::printf("pedal_half_pedal_test: %s\n", g_fail ? "FAILED" : "ok");
  return g_fail ? 1 : 0;
}
