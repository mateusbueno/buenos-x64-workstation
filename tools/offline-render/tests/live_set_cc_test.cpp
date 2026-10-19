// Story 5.2: the Live Set renderer passes raw controllers through to the sample parts. A Live Set with one sample part whose region crossfades
// in on CC1 (xfin_locc1=0 xfin_hicc1=127, gain curve, a looped constant) is rendered with a CC1 sweep, in process through render_live_set(), the
// same code path as `velora-offline-render --live-set`; the levels are compared with the analytic crossfade gain. A MIDI file that holds a
// controller after the last note must extend the render, as a note would. Library and Live Set are built here at run time (no golden).
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

// Mean of the left channel over frames [from, to).
static double mean_left(const off::WavData& w, std::size_t from, std::size_t to) {
  double s = 0;
  for (std::size_t i = from; i < to; ++i) s += w.samples[i * w.channels];
  return s / static_cast<double>(to - from);
}

int main() {
  const std::string work = std::string(OFFLINE_TEST_DIR) + "/live_set_cc";
  std::system(("rm -rf '" + work + "'").c_str());
  const std::string root = work + "/root";
  const fmt::Uuid lib = fmt::library_id_from_fingerprint({reinterpret_cast<const std::uint8_t*>("live-set-cc-test"), 16});
  fmt::BundleWriter w;
  w.set_library_id(lib);
  auto ins = w.add_instrument("dc/fade.sfz", "dc/fade.sfz");
  CHECK(ins.ok());
  std::vector<std::int32_t> pcm(4800, 10000);
  CHECK(w.add_sample(ins.value(), {"s", {1, 16, false, 48000}, 4800, pcm}) == fmt::Error::kNone);
  fmt::Region r;
  r.key_lo = 0;
  r.key_hi = 127;
  r.key_center = 60;
  r.loop_mode = fmt::LoopMode::kLoopContinuous;
  r.xf.in_count = 1;
  r.xf.in[0] = {1, 0, 127};
  r.xf.curve = fmt::XfCurve::kGain;
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

  velora::host::liveset::lsp::LiveSet s = ls::make_default_live_set("2e6b1f40-7c3a-4d58-9b21-6a0c5e8d4f17", "CC pass-through", 8);
  s.mutable_parts(0)->mutable_timbre()->set_library_id(lib_id);
  s.mutable_parts(0)->mutable_timbre()->set_instrument_key("dc/fade.sfz");
  CHECK(ls::validate(s, 8).ok());
  std::string ls_bytes;
  CHECK(ls::serialize(s, ls_bytes) == ls::Status::kOk);
  const std::string ls_path = work + "/cc.liveset";
  { std::ofstream f(ls_path, std::ios::binary | std::ios::trunc); f.write(ls_bytes.data(), static_cast<std::streamsize>(ls_bytes.size())); }

  off::LiveSetRenderOptions opt;
  opt.live_set_path = ls_path;
  opt.library_root = root;
  opt.tail_seconds = 0.2;

  // CC1 = 0 for the first second, 64 for the second, 127 for the third; one note held through all of it.
  std::vector<off::NoteEvent> ev = {cc(0, 1, 0), note(0, true, 60), cc(48000, 1, 64), cc(96000, 1, 127), note(150000, false, 60)};
  off::WavData out;
  std::string err;
  off::LiveSetRenderReport rep;
  CHECK(off::render_live_set(ev, opt, out, err, &rep));
  if (g_fail) { std::printf("%s\n", err.c_str()); return 1; }
  const double l0 = mean_left(out, 20000, 40000), l64 = mean_left(out, 70000, 90000), l127 = mean_left(out, 120000, 140000);
  CHECK(std::fabs(l0) < 1e-6);                       // the wheel at 0: the crossfade region is silent
  CHECK(l127 > 0.05);                                // ... and fully in at 127
  CHECK(std::fabs(l64 / l127 - 64.0 / 126.0) < 1e-3);  // at 64 the gain curve gives 64 / 126 of it
  CHECK(rep.pool_exhausted == 0);

  // A controller after the last note extends the render, as a note would: frames up to 2 s plus the tail.
  std::vector<off::NoteEvent> only_cc = {cc(0, 1, 0), cc(96000, 1, 127)};
  off::WavData out2;
  CHECK(off::render_live_set(only_cc, opt, out2, err, nullptr));
  CHECK(out2.frames() >= 96000 + 9600);
  std::printf("live_set_cc_test: %s\n", g_fail ? "FAILED" : "ok");
  return g_fail ? 1 : 0;
}
