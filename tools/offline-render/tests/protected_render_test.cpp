// Story 6.5: the offline renderer plays PROTECTED libraries (replaces the Story 6.3 substitute). A protected bundle (format 3: AES-256-CTR blocks, signed manifest,
// content key wrapped to a dev device key) and an unprotected bundle of the SAME library render the same MIDI suite through the shipped tool
// (velora-offline-render --dev-device-key ... --wrapped-key ... --trust-list ...) to BIT-IDENTICAL WAV files (compared by SHA-256, and byte for byte), over a library with 16-bit
// and 24-bit FLAC blocks, float blocks, a loop and a release. Negative controls: a different library renders differently; every refusal is typed and writes no WAV; the
// Live Set path (render_live_set with the ProtectedHeadProvider) is bit-identical too. And nothing plaintext reaches a disk: the library tree is byte for byte what it was,
// the process temp directory stays empty, and no file in the work tree holds the content key or runs of the decoded PCM.
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../../../engine/host/tests/protected_fixtures.hpp"
#include "render_live_set.hpp"
#include "velora/host/dev_key_provider.hpp"
#include "velora/host/live_set_store.hpp"
#include "velora/host/protected_head_provider.hpp"

#ifndef RENDER_EXE
#error "RENDER_EXE must name velora-offline-render"
#endif
#ifndef OFFLINE_TEST_DIR
#error "OFFLINE_TEST_DIR must name the build directory for scratch files"
#endif

namespace fmt = velora::format;
namespace ls = velora::host::liveset;
namespace off = velora::offline;
namespace host = velora::host;
using pfx::Bytes;

static int g_fail = 0;
#define CHECK(c)                                                 \
  do {                                                           \
    if (!(c)) {                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
      ++g_fail;                                                  \
    }                                                            \
  } while (0)

static std::string q(const std::string& s) { return "'" + s + "'"; }

// Runs a shell command, returns its exit status; stdout+stderr go to `out`.
static int run(const std::string& cmd, std::string* out = nullptr) {
  const std::string log = std::string(OFFLINE_TEST_DIR) + "/protected_render.log";
  const int rc = std::system((cmd + " > " + q(log) + " 2>&1").c_str());
  if (out) {
    std::ifstream in(log);
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  return rc == -1 ? -1 : (WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
}

// The filesystem WRITE MONITOR. `run_nowrite.sh` lowers RLIMIT_FSIZE to 0 (`ulimit -f 0`) and execs the command: from then on the kernel kills the process (SIGXFSZ, or EFBIG)
// the moment it writes a single byte to a REGULAR file anywhere -- a temp file, a cache, a "decrypted copy" of a block -- while pipes, terminals and /dev/null stay writable. The
// protected render below runs under it with its output sent to /dev/null: it can read and decrypt the bundle, it can not leave any plaintext in any file, and surviving proves it
// did not try. The output comes back through a pipe (popen), not through a file. The negative control proves the monitor bites: a process that writes a file under it is killed.
static int run_nowrite(const std::string& script, const std::string& cmd, std::string& out, bool* signalled = nullptr) {
  std::FILE* p = ::popen(("sh " + q(script) + " " + cmd + " 2>&1").c_str(), "r");
  if (!p) return -1;
  char buf[512];
  std::size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
  const int rc = ::pclose(p);
  if (signalled) *signalled = WIFSIGNALED(rc) || (WIFEXITED(rc) && WEXITSTATUS(rc) > 128);
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

// A MIDI suite over the three regions of the test library: single notes, a chord, different velocities, notes long enough for the loop and short enough for the release.
static Bytes make_midi(std::vector<std::uint8_t> notes = {50, 64, 76, 79, 58, 67}) {
  Bytes trk;
  auto vlq = [&](std::uint32_t v) {
    std::vector<std::uint8_t> t;
    t.push_back(v & 0x7f);
    while (v >>= 7) t.push_back(static_cast<std::uint8_t>((v & 0x7f) | 0x80));
    for (auto it = t.rbegin(); it != t.rend(); ++it) trk.push_back(*it);
  };
  struct Ev { std::uint32_t delta; bool on; std::uint8_t note, vel; };
  // notes: groups of two sounding together, three groups (0,1 | 2,3 | 4,5), velocities 100/80/120/60/30/127, held 240 / 720 / 480 ticks
  const std::uint8_t vel[6] = {100, 80, 120, 60, 30, 127};
  const std::uint32_t hold[3] = {240, 720, 480};
  std::vector<Ev> evs;
  for (int g = 0; g < 3; ++g) {
    evs.push_back({0, true, notes[2 * g], vel[2 * g]});
    evs.push_back({0, true, notes[2 * g + 1], vel[2 * g + 1]});
    evs.push_back({hold[g], false, notes[2 * g], 0});
    evs.push_back({0, false, notes[2 * g + 1], 0});
  }
  for (const Ev& e : evs) {
    vlq(e.delta);
    trk.push_back(e.on ? 0x90 : 0x80);
    trk.push_back(e.note);
    trk.push_back(e.vel);
  }
  for (std::uint8_t b : {0x00, 0xff, 0x2f, 0x00}) trk.push_back(b);
  Bytes m = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0, 'M', 'T', 'r', 'k', 0, 0, 0, static_cast<std::uint8_t>(trk.size())};
  m.insert(m.end(), trk.begin(), trk.end());
  return m;
}

static std::map<std::string, Bytes> snapshot(const std::filesystem::path& root) {
  std::map<std::string, Bytes> m;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file()) m[it->path().string()] = pfx::read_file(it->path());
    else m[it->path().string()] = Bytes();
  }
  return m;
}

int main() {
  namespace fs = std::filesystem;
  // The write monitor relies on SIGXFSZ keeping its default action (kill): it must not be ignored or handled here (children inherit an ignored disposition).
  {
    struct sigaction sa;
    CHECK(::sigaction(SIGXFSZ, nullptr, &sa) == 0 && sa.sa_handler == SIG_DFL);
  }
  const fs::path work = fs::path(OFFLINE_TEST_DIR) / "protected_render";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work / "tmp");  // the CLI runs with TMPDIR here: it must stay empty
  const fs::path root = work / "root";
  const pfx::Keys keys = pfx::make_keys(5);
  const std::vector<pfx::Spec> specs = {{"multi.sfz", pfx::Spec::kMulti, 0.1f, 20000}, {"wide.sfz", pfx::Spec::kWide, 0.1f, 20000}};

  // The same library twice: protected, and unprotected under the same vendor library id.
  const pfx::Installed prot = pfx::install_library(root.string(), 1, specs, keys);  // one library, two instruments
  const fs::path plain_root = work / "plain_root";
  const pfx::Installed plain = pfx::install_library(plain_root.string(), 1, specs, keys, {.protect = false});
  CHECK(prot.library_id == plain.library_id && prot.bundle_id != plain.bundle_id);
  const fs::path keyfile = work / "device.x25519.priv", trustfile = work / "dev.pub-list", midi = work / "suite.mid";
  pfx::write_file(keyfile, Bytes(keys.device_priv.begin(), keys.device_priv.end()));
  ::chmod(keyfile.c_str(), 0600);
  {
    std::ofstream t(trustfile);
    t << "# test trust list\ndev content-manifest " << fmt::to_hex(keys.sign_id) << " " << fmt::to_hex(keys.sign_pub) << "\n";
  }
  pfx::write_file(midi, make_midi());
  const fs::path bundle_p = prot.dir / "bundle.vlrp", wrapped = prot.dir / "wrapped-key.bin", bundle_u = plain.dir / "bundle.vlb";
  const std::string base = q(RENDER_EXE) + " --midi " + q(midi.string()) + " --engine sample --instrument multi.sfz --tail-seconds 0.5 ";
  const std::string prot_flags = " --bundle " + q(bundle_p.string()) + " --dev-device-key " + q(keyfile.string()) + " --wrapped-key " + q(wrapped.string()) + " --trust-list " + q(trustfile.string());
  const std::string env = "TMPDIR=" + q((work / "tmp").string()) + " ";
  const auto tree_before = snapshot(root);

  // 1. The headline: same library, protected and unprotected, same MIDI suite -> bit-identical output (both interpolations, and with a short voice pool).
  for (const char* extra : {"", " --interpolation linear", " --voices 4"}) {
    std::string out;
    const fs::path wp = work / "protected.wav", wu = work / "plain.wav";
    CHECK(run(env + base + extra + " --out " + q(wp.string()) + prot_flags, &out) == 0);
    CHECK(run(env + base + extra + " --out " + q(wu.string()) + " --bundle " + q(bundle_u.string()), &out) == 0);
    const Bytes a = pfx::read_file(wp), b = pfx::read_file(wu);
    CHECK(a.size() > 1000);
    CHECK(fmt::to_hex(fmt::Sha256::hash(a)) == fmt::to_hex(fmt::Sha256::hash(b)));
    CHECK(a == b);
    bool loud = false;
    for (std::size_t i = 44; i < a.size() && !loud; ++i) loud = a[i] != 0;
    CHECK(loud);  // not a comparison of two silent files
  }

  // 1b. The other formats of the library (mono 24-bit at 44.1 kHz, stereo 16-bit at 96 kHz, mono 16-bit at 88.2 kHz, stereo 24-bit at 44.1 kHz; a loop): the wide instrument of the
  //     same bundle, with a MIDI suite of its own. (8-bit and 32-bit integer PCM are not formats of the bundle: formatlib supports 16/24-bit integer and 32-bit float.)
  {
    const fs::path midi2 = work / "wide.mid";
    pfx::write_file(midi2, make_midi({30, 42, 90, 102, 38, 44}));
    const std::string base2 = q(RENDER_EXE) + " --midi " + q(midi2.string()) + " --engine sample --instrument wide.sfz --tail-seconds 0.5 ";
    for (const char* extra : {"", " --interpolation linear"}) {
      std::string out;
      const fs::path wp = work / "protected_wide.wav", wu = work / "plain_wide.wav";
      CHECK(run(env + base2 + extra + " --out " + q(wp.string()) + prot_flags, &out) == 0);
      CHECK(run(env + base2 + extra + " --out " + q(wu.string()) + " --bundle " + q(bundle_u.string()), &out) == 0);
      const Bytes a = pfx::read_file(wp), b = pfx::read_file(wu);
      bool loud = false;
      for (std::size_t i = 44; i < a.size() && !loud; ++i) loud = a[i] != 0;
      CHECK(a.size() > 1000 && a == b && loud);
    }
  }

  // 2. Negative control: other content under the same library id renders differently (the comparison can fail).
  {
    const fs::path other_root = work / "other_root";
    pfx::Spec altered = specs[0];
    altered.frames = 20001;  // one more frame of the same tones: a different library revision
    const pfx::Installed alt = pfx::install_library(other_root.string(), 1, {altered}, keys);
    const std::string flags = " --bundle " + q((alt.dir / "bundle.vlrp").string()) + " --dev-device-key " + q(keyfile.string()) + " --wrapped-key " + q((alt.dir / "wrapped-key.bin").string()) +
                              " --trust-list " + q(trustfile.string());
    CHECK(run(env + base + " --out " + q((work / "alt.wav").string()) + flags) == 0);
    const Bytes a = pfx::read_file(work / "alt.wav"), b = pfx::read_file(work / "plain.wav");
    CHECK(a.size() > 1000 && a != b);
  }

  // 3. Typed refusals through the shipped tool: nothing is written, the exit status is 1, the cause is named.
  {
    std::string out;
    const fs::path none = work / "none.wav";
    // wrong device key
    const pfx::Keys other = pfx::make_keys(9);
    const fs::path other_key = work / "other.priv";
    pfx::write_file(other_key, Bytes(other.device_priv.begin(), other.device_priv.end()));
    CHECK(run(env + base + " --out " + q(none.string()) + " --bundle " + q(bundle_p.string()) + " --dev-device-key " + q(other_key.string()) + " --wrapped-key " + q(wrapped.string()) +
                  " --trust-list " + q(trustfile.string()),
              &out) == 1);
    CHECK(out.find("key_unavailable") != std::string::npos);
    // an untrusted signer
    {
      const fs::path untrusted = work / "untrusted.pub-list";
      std::ofstream t(untrusted);
      t << "dev content-manifest " << fmt::to_hex(other.sign_id) << " " << fmt::to_hex(other.sign_pub) << "\n";
      t.close();
      CHECK(run(env + base + " --out " + q(none.string()) + " --bundle " + q(bundle_p.string()) + " --dev-device-key " + q(keyfile.string()) + " --wrapped-key " + q(wrapped.string()) +
                    " --trust-list " + q(untrusted.string()),
                &out) == 1);
      CHECK(out.find("cannot open protected bundle") != std::string::npos && out.find("untrusted") != std::string::npos);
    }
    // a damaged block -> content_integrity; the file is a copy so the tree stays comparable
    {
      const fs::path dmg_root = work / "dmg_root";
      const pfx::Installed d = pfx::install_library(dmg_root.string(), 1, specs, keys);
      {
        std::fstream f(d.dir / "bundle.vlrp", std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(static_cast<std::streamoff>(d.bundle.size() - 1000));  // a block of the LAST instrument (wide.sfz): the render of that instrument refuses
        f.put('\x42');
      }
      const std::string base_wide = q(RENDER_EXE) + " --midi " + q((work / "wide.mid").string()) + " --engine sample --instrument wide.sfz --tail-seconds 0.5 ";
      CHECK(run(env + base_wide + " --out " + q(none.string()) + " --bundle " + q((d.dir / "bundle.vlrp").string()) + " --dev-device-key " + q(keyfile.string()) + " --wrapped-key " +
                    q((d.dir / "wrapped-key.bin").string()) + " --trust-list " + q(trustfile.string()),
                &out) == 1);
      CHECK(out.find("content_integrity") != std::string::npos);
    }
    // a signature that does not verify
    {
      const fs::path sig_root = work / "sig_root";
      const pfx::Installed d = pfx::install_library(sig_root.string(), 1, specs, keys);
      {
        std::fstream f(d.dir / "bundle.vlrp", std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(40);
        f.put('\x7f');
      }
      CHECK(run(env + base + " --out " + q(none.string()) + " --bundle " + q((d.dir / "bundle.vlrp").string()) + " --dev-device-key " + q(keyfile.string()) + " --wrapped-key " +
                    q((d.dir / "wrapped-key.bin").string()) + " --trust-list " + q(trustfile.string()),
                &out) == 1);
      CHECK(out.find("cannot open protected bundle") != std::string::npos);
    }
    // missing pieces and misuse
    CHECK(run(env + base + " --out " + q(none.string()) + " --bundle " + q(bundle_p.string()) + " --dev-device-key " + q(keyfile.string()) + " --trust-list " + q(trustfile.string()), &out) == 2);
    CHECK(out.find("needs --wrapped-key") != std::string::npos);
    CHECK(run(env + base + " --out " + q(none.string()) + " --bundle " + q(bundle_p.string()) + " --wrapped-key " + q(wrapped.string()), &out) == 2);
    CHECK(run(env + base + " --out " + q(none.string()) + " --bundle " + q(bundle_p.string()) + " --dev-device-key " + q((work / "nope").string()) + " --wrapped-key " + q(wrapped.string()) +
                  " --trust-list " + q(trustfile.string()),
              &out) == 1);
    CHECK(run(env + base + " --out " + q(none.string()) + " --bundle " + q(bundle_p.string()), &out) == 1);  // a protected bundle without the flags is not a format-2 bundle
    CHECK(!fs::exists(none));
  }

  // 4. The Live Set path: the same Live Set over the protected and over the unprotected library renders identical audio.
  {
    ls::lsp::LiveSet s = ls::make_default_live_set("2e6b1f40-7c3a-4d58-9b21-6a0c5e8d4f17", "Protected render", 8);
    s.mutable_parts(0)->mutable_timbre()->set_library_id(prot.library_id);
    s.mutable_parts(0)->mutable_timbre()->set_instrument_key("multi.sfz");
    CHECK(ls::validate(s, 8).ok());
    std::string bytes;
    CHECK(ls::serialize(s, bytes) == ls::Status::kOk);
    const fs::path lsp = work / "p.liveset";
    { std::ofstream f(lsp, std::ios::binary | std::ios::trunc); f.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
    std::vector<off::NoteEvent> ev;
    for (std::uint8_t k : {50, 64, 76}) {
      off::NoteEvent on;
      on.frame = 4800u * (k % 7);
      on.on = true;
      on.channel = 0;
      on.note = k;
      on.velocity16 = 52000;
      off::NoteEvent offv = on;
      offv.frame = on.frame + 24000;
      offv.on = false;
      ev.push_back(on);
      ev.push_back(offv);
    }
    std::sort(ev.begin(), ev.end(), [](const off::NoteEvent& a, const off::NoteEvent& b) { return a.frame < b.frame; });
    std::string err;
    off::LiveSetRenderOptions plain_opt;
    plain_opt.live_set_path = lsp.string();
    plain_opt.library_root = plain_root.string();
    plain_opt.tail_seconds = 0.5;
    off::WavData wu, wp;
    CHECK(off::render_live_set(ev, plain_opt, wu, err, nullptr));
    // Protected: the dev key provider, the trusted keys and the protected head provider, as the CLI builds them.
    std::string kerr;
    auto provider = host::DevKeyProvider::from_file(keyfile.string(), {}, kerr);
    CHECK(provider != nullptr);
    if (provider) {
      host::DamageRegistry damage;
      host::ProtectedHeadProvider heads(host::make_posix_fs(), *provider, &damage);
      off::LiveSetRenderOptions opt = plain_opt;
      opt.library_root = root.string();
      opt.heads = &heads;
      opt.protected_trust = std::make_shared<const velora::format::TrustedKeys>(keys.trusted);
      CHECK(off::render_live_set(ev, opt, wp, err, nullptr));
      CHECK(wu.frames() == wp.frames() && wu.frames() > 1000);
      CHECK(wu.samples == wp.samples);
      bool loud = false;
      for (float v : wp.samples) loud = loud || v != 0.0f;
      CHECK(loud);
      // Without the head provider the same Live Set leaves the part silent with Missing library (and nothing is decrypted).
      off::LiveSetRenderOptions bare = plain_opt;
      bare.library_root = root.string();
      off::WavData wb;
      off::LiveSetRenderReport rep;
      CHECK(off::render_live_set(ev, bare, wb, err, &rep));
      CHECK(rep.part_states.size() == 8 && rep.part_states[0] == velora::core::PartState::kMissingLibrary);
      for (float v : wb.samples) CHECK(v == 0.0f);
    }
  }

  // 5a. The write monitor: the protected render, run with every regular-file write forbidden, still renders (to /dev/null) -- the same number of frames as the normal run.
  {
    const std::string script = (work / "run_nowrite.sh").string();
    { std::ofstream f(script); f << "ulimit -f 0 && exec \"$@\"\n"; }
    std::string out_mon, out_ref;
    bool sig = false;
    CHECK(run(env + base + " --out " + q((work / "ref.wav").string()) + prot_flags, &out_ref) == 0);
    CHECK(run_nowrite(script, "env " + env + base + " --out /dev/null" + prot_flags, out_mon, &sig) == 0 && !sig);
    auto frames_of = [](const std::string& o) {
      const std::size_t at = o.find("rendered ");
      const std::size_t open = o.find('(', at);
      return at == std::string::npos || open == std::string::npos ? std::string() : o.substr(open, o.find(" frames", open) - open);
    };
    CHECK(!frames_of(out_ref).empty() && frames_of(out_ref) == frames_of(out_mon));
    CHECK(out_mon.find("rendered ") != std::string::npos);
    CHECK(out_mon.find("core_limit=0/0") != std::string::npos && out_ref.find("core_limit=0/0") != std::string::npos);  // the tool turned core dumps off before it held a key
    // The Live Set path of the CLI under the same monitor (library root, Live Set file, each library's wrapped-key.bin):
    {
      ls::lsp::LiveSet s2 = ls::make_default_live_set("2e6b1f40-7c3a-4d58-9b21-6a0c5e8d4f18", "Protected CLI", 8);
      s2.mutable_parts(0)->mutable_timbre()->set_library_id(prot.library_id);
      s2.mutable_parts(0)->mutable_timbre()->set_instrument_key("multi.sfz");
      std::string lbytes;
      CHECK(ls::serialize(s2, lbytes) == ls::Status::kOk);
      const fs::path lsf = work / "cli.liveset";
      { std::ofstream f(lsf, std::ios::binary | std::ios::trunc); f.write(lbytes.data(), static_cast<std::streamsize>(lbytes.size())); }
      const std::string live = q(RENDER_EXE) + " --live-set " + q(lsf.string()) + " --library-root " + q(root.string()) + " --midi " + q(midi.string()) + " --tail-seconds 0.5 --dev-device-key " +
                               q(keyfile.string()) + " --trust-list " + q(trustfile.string());
      std::string out_live_ref, out_live_mon;
      CHECK(run(env + live + " --out " + q((work / "live_ref.wav").string()), &out_live_ref) == 0);
      bool sig2 = false;
      CHECK(run_nowrite(script, "env " + env + live + " --out /dev/null", out_live_mon, &sig2) == 0 && !sig2);
      CHECK(!frames_of(out_live_ref).empty() && frames_of(out_live_ref) == frames_of(out_live_mon));
      CHECK(out_live_mon.find("part 0: ready") != std::string::npos && out_live_mon.find("core_limit=0/0") != std::string::npos);
    }
    // The monitor bites: under it, writing even one byte to a regular file kills the writer.
    const fs::path victim = work / "victim.txt";
    std::string out_neg;
    bool killed = false;
    const int rc_neg = run_nowrite(script, "sh -c 'echo plaintext > \"$0\"' " + q(victim.string()), out_neg, &killed);
    CHECK(rc_neg != 0 || killed);
    CHECK(!fs::exists(victim) || fs::file_size(victim) == 0);
    // ... and the render itself, pointed at a regular file, dies under it (the WAV writer is a writer too).
    bool killed2 = false;
    const int rc_wav = run_nowrite(script, base + " --out " + q((work / "must_not_exist.wav").string()) + prot_flags, out_neg, &killed2);
    CHECK(rc_wav != 0 || killed2);
  }

  // 5. No plaintext block or key reached a disk: the library tree is what it was, the CLI's temp directory is empty, and no file anywhere in the work tree holds
  //    the content key, the device key or runs of the decoded PCM -- except the unprotected comparison library, which is plaintext by definition (FR24) and is excluded.
  CHECK(snapshot(root) == tree_before);
  CHECK(fs::is_empty(work / "tmp"));
  const auto needles = pfx::plaintext_needles(keys);
  {
    auto hits = pfx::scan_for(work, needles);
    for (const std::string& h : hits) {
      const bool allowed = h.rfind(plain_root.string(), 0) == 0 || h == keyfile.string();  // the unprotected library; the device key file itself (it is the dev key input)
      if (!allowed) std::printf("plaintext found in %s\n", h.c_str());
      CHECK(allowed);
    }
    CHECK(!hits.empty());  // the scan is not vacuous: it does find the unprotected library's PCM
  }
  std::printf("protected_render_test: %s\n", g_fail ? "FAILED" : "ok");
  return g_fail ? 1 : 0;
}
