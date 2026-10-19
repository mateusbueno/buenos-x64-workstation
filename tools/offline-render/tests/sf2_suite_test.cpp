// Render comparison of the SF2 import against FluidSynth (Story 5.3), through the shipped tools:
//   SF2 built in code  ->  librarian import (in process)  ->  velora-offline-render --engine sample  ->
//   velora-null-test --case against the committed FluidSynth render.
// Usage: sf2_suite_test <case> [--negative gain|lag|strip]
//        sf2_suite_test --emit <dir>        write <case>.sf2 and <case>.mid for the oracle script
//        sf2_suite_test cli-errors           exercises the SF2 paths of the renderer CLI
//   no flag      the render must pass the case's null test (tolerance in golden/<case>.case)
//   --negative   a deliberately wrong render must FAIL the same null test on residual:
//                  gain   a 1 dB gain error
//                  lag    the MIDI one block (64 frames) later than it should be
//                  strip  the SF2 imported without its envelopes, filters, pans and attenuations
// Needs no FluidSynth: the goldens are committed. Regenerating them needs tools/oracle (see its README).
#include <sys/stat.h>
#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include "sf2_suite/sf2_fixtures.hpp"
#include "velora/librarian/import.hpp"

#ifndef SUITE_DIR
#error "SUITE_DIR must point at tools/offline-render/tests/sf2_suite"
#endif

static int g_fail = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
      ++g_fail;                                                        \
    }                                                                  \
  } while (0)

static std::string q(const std::string& s) { return "\"" + s + "\""; }

static int run(const std::string& cmd, std::string& out, const std::string& log) {
  const int rc = std::system((cmd + " > " + q(log) + " 2>&1").c_str());
  std::ifstream in(log);
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (rc == -1) return -1;
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void mkdirs(const std::string& p) {
  std::string cur;
  for (std::size_t i = 0; i <= p.size(); ++i) {
    if (i == p.size() || p[i] == '/') {
      if (!cur.empty()) ::mkdir(cur.c_str(), 0755);
    }
    if (i < p.size()) cur += p[i];
  }
}

static void write_bytes(const std::string& path, const std::vector<std::uint8_t>& b) {
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

static long counter(const std::string& out, const std::string& label) {
  const std::size_t at = out.find(" " + label);
  if (at == std::string::npos) return -1;
  std::size_t b = at;
  while (b > 0 && out[b - 1] >= '0' && out[b - 1] <= '9') --b;
  if (b == at) return -1;
  return std::strtol(out.substr(b, at - b).c_str(), nullptr, 10);
}

// Removes the generators the `strip` negative drops: filter (8, 9), pan (17), the volume envelope
// (33..38) and the initial attenuation (48), from every instrument and preset zone.
static void strip_sounding_generators(sf2b::Builder& b) {
  auto drop = [](std::vector<sf2b::Zone>& zones) {
    for (auto& z : zones) {
      auto it = std::remove_if(z.gens.begin(), z.gens.end(), [](const sf2b::Gen& g) {
        return g.oper == 8 || g.oper == 9 || g.oper == 17 || (g.oper >= 33 && g.oper <= 38) || g.oper == 48 ||
               g.oper == 51 || g.oper == 52;
      });
      z.gens.erase(it, z.gens.end());
    }
  };
  for (auto& i : b.instruments) drop(i.zones);
  for (auto& p : b.presets) drop(p.zones);
}

static int cli_errors(const std::string& work) {
  std::string out;
  const std::string exe = q(RENDER_EXE);
  const std::string log = work + "/cli.log";
  const sf2suite::Files files = sf2suite::make_files();
  const auto& c = files.cases[0];
  const std::string mid = work + "/c.mid";
  write_bytes(mid, c.midi.smf(sf2suite::kOracleLagTicks, c.tail_ticks));
  sf2b::Builder bank = files.sf2[0].second;
  mkdirs(work + "/root");
  write_bytes(work + "/c.sf2", bank.build());
  velora::librarian::ImportOptions opt;
  opt.sfz_path = work + "/c.sf2";
  opt.root = work + "/root";
  const auto res = velora::librarian::import_sf2(opt);
  CHECK(res.ok());
  if (!res.ok()) return 1;
  const std::string bundle = q(res.bundle_dir + "/bundle.vlb");
  // The instrument key of a preset is bank:program; the SFZ-style names do not exist in an SF2 bundle.
  CHECK(run(exe + " --midi " + q(mid) + " --out " + q(work + "/x.wav") + " --engine sample --bundle " + bundle + " --instrument 0:99", out, log) == 1);
  CHECK(out.find("instrument not found") != std::string::npos);
  CHECK(run(exe + " --midi " + q(mid) + " --out " + q(work + "/x.wav") + " --engine sample --bundle " + bundle + " --instrument Keys.sf2", out, log) == 1);
  CHECK(run(exe + " --midi " + q(mid) + " --out " + q(work + "/x.wav") + " --engine sample --bundle " + bundle + " --instrument 0:0", out, log) == 0);
  CHECK(counter(out, "unplayable regions") == 0 && counter(out, "pool exhausted") == 0);
  // Every preset of the bank renders (percussion bank 128 included).
  for (const char* key : {"0:0", "0:1", "0:2", "0:3", "0:4", "0:5", "0:6", "128:0"}) {
    CHECK(run(exe + " --midi " + q(mid) + " --out " + q(work + "/y.wav") + " --engine sample --bundle " + bundle + " --instrument " + key, out, log) == 0);
    CHECK(counter(out, "unplayable regions") == 0);
  }
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const std::string name = argv[1];
  const sf2suite::Files files = sf2suite::make_files();

  if (name == "--emit") {
    if (argc != 3) return 2;
    const std::string dir = argv[2];
    mkdirs(dir);
    std::string list;
    for (const auto& c : files.cases) {
      for (const auto& f : files.sf2) {
        if (f.first != c.file) continue;
        sf2b::Builder b = f.second;
        sf2suite::for_oracle(b);  // FluidSynth's 0.4 attenuation factor is undone in the oracle's copy of the SF2
        write_bytes(dir + "/" + c.name + ".sf2", b.build());
      }
      write_bytes(dir + "/" + c.name + ".mid", c.midi.smf(0, c.tail_ticks));  // the oracle's MIDI: no shift
      list += c.name + "\n";
    }
    std::ofstream(dir + "/cases.txt") << list;
    return 0;
  }

  std::string negative;
  if (argc > 2) {
    if (argc != 4 || std::string(argv[2]) != "--negative" ||
        (std::string(argv[3]) != "gain" && std::string(argv[3]) != "lag" && std::string(argv[3]) != "strip")) {
      std::fprintf(stderr, "usage: sf2_suite_test <case> [--negative gain|lag|strip]\n");
      return 2;
    }
    negative = argv[3];
  }
  // One work directory per case and variant, so that `ctest -j` cannot make two tests share one.
  const std::string work = std::string(OFFLINE_TEST_DIR) + "/sf2_suite/" + name + (negative.empty() ? "" : "_neg_" + negative);
  std::string out;
  std::system(("rm -rf " + q(work)).c_str());
  mkdirs(work);
  if (name == "cli-errors") return cli_errors(work);

  const sf2suite::Case* cs = nullptr;
  for (const auto& c : files.cases)
    if (c.name == name) cs = &c;
  if (!cs) {
    std::fprintf(stderr, "unknown case %s\n", name.c_str());
    return 2;
  }

  // 1. The SF2 file of the case, imported with the librarian into a scratch data root.
  sf2b::Builder b;
  for (const auto& f : files.sf2)
    if (f.first == cs->file) b = f.second;
  if (negative == "strip") strip_sounding_generators(b);
  const std::string sf2 = work + "/" + cs->file + ".sf2";
  write_bytes(sf2, b.build());
  velora::librarian::ImportOptions opt;
  opt.sfz_path = sf2;
  opt.root = work + "/root";
  mkdirs(opt.root);
  const auto res = velora::librarian::import_sf2(opt);
  if (!res.ok()) {
    std::printf("import failed:\n");
    for (const auto& r : res.reasons) std::printf("  %s\n", r.c_str());
    return 1;
  }

  // 2. The engine render. FluidSynth applies every MIDI event at the start of the next 64-frame block, so
  //    the engine's MIDI is the oracle's shifted by one tick (= 64 frames); `lag` shifts it once more.
  const std::uint32_t shift = sf2suite::kOracleLagTicks + (negative == "lag" ? 1u : 0u);
  const std::string mid = work + "/engine.mid";
  write_bytes(mid, cs->midi.smf(shift, cs->tail_ticks));
  const std::string gain = negative == "gain" ? "1.0" : "0";
  const std::string cand = work + "/candidate.wav";
  const std::string key = std::to_string(cs->bank) + ":" + std::to_string(cs->program);
  const std::string render = q(RENDER_EXE) + " --midi " + q(mid) + " --out " + q(cand) + " --engine sample --bundle " +
                             q(res.bundle_dir + "/bundle.vlb") + " --instrument " + key + " --gain-db " + gain +
                             " --tail-seconds 1.0";
  int rc = run(render, out, work + "/render.log");
  std::printf("%s", out.c_str());
  CHECK(rc == 0);
  CHECK(counter(out, "unplayable regions") == 0);
  CHECK(counter(out, "failed)") == 0);
  CHECK(counter(out, "pool exhausted") == 0);

  // 3. Null test against the committed FluidSynth render.
  rc = run(q(NULLTEST_EXE) + " --case " + q(std::string(SUITE_DIR) + "/golden/" + name + ".case") + " --candidate " + q(cand),
           out, work + "/null.log");
  std::printf("%s", out.c_str());
  if (negative.empty()) {
    CHECK(rc == 0);
    CHECK(out.find("overall: PASS") != std::string::npos);
  } else {
    CHECK(rc == 1);
    CHECK(out.find("result: FAIL (residual above tolerance)") != std::string::npos);
  }
  return g_fail ? 1 : 0;
}
