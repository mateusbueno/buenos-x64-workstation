// Story 6.5: the sample-engine null-test SUITE (tests/sample_suite: velocity layers, round robin, random layers, far pitch, volume/pan, offsets, envelopes, loops, release
// triggers, poly layers, keyswitches, CC crossfades, filters; each with its committed MIDI and SFZ) played PROTECTED and UNPROTECTED. For every case the SFZ is imported by the
// librarian's importer in build-only mode (the same code path as the content-pack tool), the content is finalized twice -- as an ordinary bundle and as a protected bundle
// (AES-256-CTR blocks, signed manifest, content key wrapped to a dev device key) -- and the shipped tool renders the case's MIDI from each, with the options of the null test.
// The two WAV files must be BIT-IDENTICAL (and not silent). SoundFont 2 libraries are not rendered here (they have no MIDI suite of their own; they are covered by the decrypt
// and PCM-hash tests of formatlib and the content-pack tool).
#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../../../engine/host/tests/protected_fixtures.hpp"
#include "velora/librarian/import.hpp"

#ifndef SUITE_DIR
#error "SUITE_DIR must point at tools/offline-render/tests/sample_suite"
#endif

namespace fs = std::filesystem;
namespace fmt = velora::format;
namespace lib = velora::librarian;
using pfx::Bytes;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

static std::string q(const std::string& s) { return "'" + s + "'"; }
static int run(const std::string& cmd, std::string* out, const std::string& log) {
  const int rc = std::system((cmd + " > " + q(log) + " 2>&1").c_str());
  if (out) {
    std::ifstream in(log);
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  return rc == -1 ? -1 : (WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
}

int main() {
  const fs::path work = fs::path(OFFLINE_TEST_DIR) / "protected_suite";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work);
  const pfx::Keys keys = pfx::make_keys(5);
  const fs::path keyfile = work / "device.x25519.priv", trustfile = work / "dev.pub-list";
  pfx::write_file(keyfile, Bytes(keys.device_priv.begin(), keys.device_priv.end()));
  { std::ofstream t(trustfile); t << "dev content-manifest " << fmt::to_hex(keys.sign_id) << " " << fmt::to_hex(keys.sign_pub) << "\n"; }
  fmt::ContentKey ck;
  ck.bytes = keys.content;

  std::ifstream cases(std::string(SUITE_DIR) + "/cases.txt");
  std::string line;
  int n = 0;
  while (std::getline(cases, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string name, interp;
    ss >> name >> interp;
    if (name.empty() || (interp != "hermite" && interp != "linear")) continue;

    Bytes plain, prot;
    std::string library_id;
    lib::ImportOptions opt;
    opt.sfz_path = std::string(SUITE_DIR) + "/sfz/" + name + ".sfz";
    opt.base_dir = std::string(SUITE_DIR) + "/sfz";
    opt.bundle_sink = [&](const lib::BuiltBundle& b) {
      auto p = b.writer->finalize();
      if (!p) return p.error();
      plain = p.take();
      auto e = fmt::finalize_protected(*b.writer, ck, keys.seed);
      if (!e) return e.error();
      prot = e.take();
      library_id = b.library_id;
      return fmt::Error::kNone;
    };
    const auto imp = lib::import_file(opt);
    CHECK(imp.ok() && !plain.empty() && !prot.empty());
    if (!imp.ok() || prot.empty()) continue;
    CHECK(plain != prot);

    const fs::path dir = work / name;
    pfx::write_file(dir / "bundle.vlb", plain);
    pfx::write_file(dir / "bundle.vlrp", prot);
    pfx::Key32 eph{};
    for (int i = 0; i < 32; ++i) eph[i] = static_cast<std::uint8_t>(0x33 + i);
    std::array<std::uint8_t, 12> nonce{};
    nonce.fill(0x66);
    pfx::write_file(dir / "wrapped-key.bin", pfx::wrap_content_key(keys.content, library_id, keys.device_pub, eph, nonce));

    const std::string common = q(RENDER_EXE) + " --midi " + q(std::string(SUITE_DIR) + "/midi/" + name + ".mid") + " --engine sample --instrument " + q(name + ".sfz") +
                               " --interpolation " + interp + " --gain-db -7.35 --oracle-block-frames 64 --tail-seconds 1.0 ";
    std::string out_p, out_u;
    CHECK(run(common + "--out " + q((dir / "plain.wav").string()) + " --bundle " + q((dir / "bundle.vlb").string()), &out_u, (dir / "u.log").string()) == 0);
    CHECK(run(common + "--out " + q((dir / "prot.wav").string()) + " --bundle " + q((dir / "bundle.vlrp").string()) + " --dev-device-key " + q(keyfile.string()) + " --wrapped-key " +
                  q((dir / "wrapped-key.bin").string()) + " --trust-list " + q(trustfile.string()),
              &out_p, (dir / "p.log").string()) == 0);
    const Bytes a = pfx::read_file(dir / "plain.wav"), b = pfx::read_file(dir / "prot.wav");
    bool loud = false;
    for (std::size_t i = 44; i < a.size() && !loud; ++i) loud = a[i] != 0;
    const bool same = a.size() > 1000 && a == b && loud;
    if (!same) std::printf("case %s: protected and unprotected renders differ (sizes %zu / %zu, loud %d)\n%s\n%s\n", name.c_str(), a.size(), b.size(), loud, out_u.c_str(), out_p.c_str());
    CHECK(same);
    ++n;
  }
  CHECK(n >= 17);  // every case of the suite
  std::printf("protected_suite_test: %d cases %s\n", n, g_fail ? "FAILED" : "ok");
  return g_fail ? 1 : 0;
}
