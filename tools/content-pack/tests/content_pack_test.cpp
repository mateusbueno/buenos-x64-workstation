// Story 6.3: tools/content-pack. Pinned vectors of tools/keys/KEY-HIERARCHY.md, the pack description, the vendor ID register, the packing pipeline on a
// generated WAV+SFZ library (decrypt with the vaulted key, reproducibility, no plaintext on disk, offline render bit-identical to the unencrypted import),
// the importer's additive options and the CLI's exit codes.
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "crypto.hpp"
#include "import_fixtures.hpp"
#include "pack.hpp"
#include "pack_description.hpp"
#include "vendor_register.hpp"
#include "velora/format/bundle.hpp"
#include "velora/format/protected_reader.hpp"
#include "velora/format/protected_writer.hpp"
#include "velora/librarian/import.hpp"
#include "velora/librarian/library_index.hpp"

#ifndef CP_EXE
#error "CP_EXE must name velora-content-pack"
#endif
#ifndef RENDER_EXE
#error "RENDER_EXE must name velora-offline-render"
#endif

namespace fs = std::filesystem;
namespace fmt = velora::format;
namespace lib = velora::librarian;
using namespace velora::contentpack;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)
#define CHECK_CODE(f, want)                                                                                      \
  do {                                                                                                           \
    const Failure _f = (f);                                                                                      \
    if (_f.code != (want)) {                                                                                     \
      std::printf("FAIL %s:%d: %s -> %s '%s' (want %s)\n", __FILE__, __LINE__, #f, to_string(_f.code), _f.message.c_str(), to_string(want)); \
      ++g_fail;                                                                                                  \
    }                                                                                                            \
  } while (0)

namespace {

template <std::size_t N>
std::array<std::uint8_t, N> seq(std::uint8_t first) {
  std::array<std::uint8_t, N> a{};
  for (std::size_t i = 0; i < N; ++i) a[i] = static_cast<std::uint8_t>(first + i);
  return a;
}
std::string hex(std::span<const std::uint8_t> d) { return fmt::to_hex(d); }

Bytes read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
std::string read_text(const fs::path& p) {
  const Bytes b = read_all(p);
  return std::string(b.begin(), b.end());
}
void put(const fs::path& p, const std::string& bytes, mode_t mode = 0644) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  ::chmod(p.c_str(), mode);
}
void put_bytes(const fs::path& p, std::span<const std::uint8_t> b, mode_t mode = 0644) {
  put(p, std::string(b.begin(), b.end()), mode);
}

// ---- generated source library ------------------------------------------------------------------------------------------------------------------------

std::string le32(std::uint32_t v) { return std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; }
std::string le16(std::uint32_t v) { return std::string{char(v), char(v >> 8)}; }

// Noise-like integer PCM (so no 32-byte window repeats), `ch` channels, `bits` 16 or 24; returns the WAV file and the raw interleaved little-endian PCM bytes.
std::string make_wav(unsigned ch, unsigned bits, std::uint32_t frames, std::uint32_t seed, std::string* pcm_out) {
  std::string pcm;
  std::uint32_t x = seed * 2654435761u + 12345u;
  for (std::uint32_t i = 0; i < frames * ch; ++i) {
    x = x * 1664525u + 1013904223u;
    const std::int32_t v = static_cast<std::int32_t>((x >> 8) & 0x3fff) - 0x2000;  // +-8192 of 16 bit
    const std::int32_t s = bits == 16 ? v : v * 256 + static_cast<std::int32_t>(x & 0xff);
    pcm.push_back(char(s & 0xff));
    pcm.push_back(char((s >> 8) & 0xff));
    if (bits == 24) pcm.push_back(char((s >> 16) & 0xff));
  }
  const std::uint32_t align = ch * bits / 8;
  const std::string wav = "RIFF" + le32(static_cast<std::uint32_t>(36 + pcm.size())) + "WAVE" + "fmt " + le32(16) + le16(1) + le16(ch) + le32(48000) +
                          le32(48000 * align) + le16(align) + le16(bits) + "data" + le32(static_cast<std::uint32_t>(pcm.size())) + pcm;
  if (pcm_out) *pcm_out = pcm;
  return wav;
}

struct Source {
  std::vector<std::string> pcm;  // raw PCM bytes per sample, for the plaintext scan
};

// <dir>/pack.sfz with three samples in <dir>/samples (stereo 16-bit 9000 frames, mono 24-bit 5000 frames, mono 16-bit 4500 frames).
Source make_library(const fs::path& dir) {
  Source s;
  s.pcm.resize(3);
  put(dir / "samples/a.wav", make_wav(2, 16, 9000, 1, &s.pcm[0]));
  put(dir / "samples/b.wav", make_wav(1, 24, 5000, 2, &s.pcm[1]));
  put(dir / "samples/c.wav", make_wav(1, 16, 4500, 3, &s.pcm[2]));
  put(dir / "pack.sfz",
      "<control> default_path=samples/\n<group> ampeg_release=0.05\n"
      "<region> sample=a.wav lokey=48 hikey=59 pitch_keycenter=54\n"
      "<region> sample=b.wav lokey=60 hikey=71 pitch_keycenter=66\n"
      "<region> sample=c.wav lokey=72 hikey=83 pitch_keycenter=78\n");
  return s;
}

// A short MIDI suite: three notes, one in each region, 240 ticks (0.25 s at 120 bpm / 480 ppq) each.
Bytes make_midi() {
  Bytes trk;
  for (std::uint8_t note : {50, 64, 76}) {
    for (std::uint8_t b : {std::uint8_t(0), std::uint8_t(0x90), note, std::uint8_t(100)}) trk.push_back(b);
    for (std::uint8_t b : {std::uint8_t(0x81), std::uint8_t(0x70), std::uint8_t(0x80), note, std::uint8_t(0)}) trk.push_back(b);
  }
  for (std::uint8_t b : {0x00, 0xff, 0x2f, 0x00}) trk.push_back(static_cast<std::uint8_t>(b));
  Bytes m = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0, 'M', 'T', 'r', 'k', 0, 0, 0, static_cast<std::uint8_t>(trk.size())};
  m.insert(m.end(), trk.begin(), trk.end());
  return m;
}

// ---- keys --------------------------------------------------------------------------------------------------------------------------------------------

struct Keys {
  Key32 seed{}, kek{}, dev_priv{}, dev_pub{};
  fmt::SigningSeed sg;  // == seed; the pointers a PackRequest holds point here, so a Keys must outlive its requests
  fmt::SigningSeed signing() const { return sg; }
  Key32 sign_pub() const { return fmt::ed25519_public_from_seed(sg); }
  std::string sign_id() const { return hex(fmt::key_id_of_public(sign_pub())); }
  std::string kek_id_hex() const { return hex(kek_id(kek)); }
};

// Sidecars as tools/keys writes them (root, dev_only, key_id), with overridable fields for the mislabelled cases.
void write_sidecars(const fs::path& dir, const Keys& k, const char* root, bool dev_only, const std::string& sign_id_override = "", const std::string& kek_id_override = "") {
  auto side = [&](const std::string& id) {
    return std::string("{\"root\": \"") + root + "\", \"dev_only\": " + (dev_only ? "true" : "false") + ", \"key_id\": \"" + id + "\"}\n";
  };
  put(dir / "content-manifest.json", side(sign_id_override.empty() ? k.sign_id() : sign_id_override));
  put(dir / "kek.json", side(kek_id_override.empty() ? k.kek_id_hex() : kek_id_override));
}

// Random test keys in <dir> with the file names of tools/keys (mode 0600) and sidecars saying `root`.
Keys make_keys(const fs::path& dir, const char* root, bool dev_only = false) {
  Keys k;
  random_bytes(k.seed);
  random_bytes(k.kek);
  random_bytes(k.dev_priv);
  k.sg.bytes = k.seed;
  k.dev_pub = x25519_public(k.dev_priv);
  put_bytes(dir / "content-manifest.ed25519.seed", k.seed, 0600);
  put_bytes(dir / "content-manifest.ed25519.pub", k.sign_pub(), 0644);
  put_bytes(dir / "kek.aes256", k.kek, 0600);
  put_bytes(dir / "device.x25519.priv", k.dev_priv, 0600);
  put_bytes(dir / "device.x25519.pub", k.dev_pub, 0644);
  write_sidecars(dir, k, root, dev_only);
  return k;
}

std::string desc_json(const std::string& id, const std::string& name, const std::string& source, const std::string& root,
                      const std::string& extra = "", const std::string& category = "piano") {
  return "{\"schema_version\": 1, \"library_id\": \"" + id + "\", \"name\": \"" + name + "\", \"source\": \"" + source + "\", \"root\": \"" + root +
         "\", \"category\": \"" + category + "\"" + (extra.empty() ? "" : ", " + extra) + "}\n";
}

fs::path g_work;  // the scratch directory of this run

// Runs `cmd`, captures stdout+stderr in a per-process log under <work>/logs (excluded from the write watch) and returns the exit status.
int run_cmd(const std::string& cmd, std::string* out = nullptr) {
  static int n = 0;
  const fs::path log = g_work / "logs" / ("cmd." + std::to_string(::getpid()) + "." + std::to_string(++n) + ".log");
  fs::create_directories(log.parent_path());
  const int rc = std::system((cmd + " > '" + log.string() + "' 2>&1").c_str());
  if (out) *out = read_text(log);
  if (rc == -1) return -1;
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

bool contains(std::span<const std::uint8_t> hay, std::span<const std::uint8_t> needle) {
  return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

// Every regular file under `dir`.
std::vector<fs::path> files_under(const fs::path& dir) {
  std::vector<fs::path> v;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec)) v.push_back(it->path());
  return v;
}

// ---- independent write watch and plaintext scan ----------------------------------------------------------------------------------------------------------

using Snap = std::map<std::string, std::uintmax_t>;
// Every regular file under the scratch directory (which includes this process's TMPDIR, see main) with its size, except the command logs.
Snap snapshot() {
  Snap s;
  for (const fs::path& p : files_under(g_work))
    if (p.string().find("/logs/") == std::string::npos) s[p.string()] = fs::file_size(p);
  return s;
}
// Files that appeared or changed since `before`.
std::vector<std::string> changed_since(const Snap& before) {
  std::vector<std::string> v;
  for (const auto& [path, size] : snapshot()) {
    auto it = before.find(path);
    if (it == before.end() || it->second != size) v.push_back(path);
  }
  return v;
}

// What must not reach the disk: the content key (raw and hex), the source's raw PCM, and the blocks of the unprotected bundle.
struct Needles {
  Key32 key{};
  std::vector<Bytes> windows;  // 32-byte windows
};
void add_windows(Needles& n, std::span<const std::uint8_t> data, std::size_t from, std::size_t stride) {
  for (std::size_t off = from; off + 32 <= data.size(); off += stride) n.windows.emplace_back(data.begin() + off, data.begin() + off + 32);
}
bool find_hit(std::span<const std::uint8_t> hay, const Needles& n, std::string* what = nullptr) {
  const bool have_key = n.key != Key32{};  // an unset key (all zero) is not a needle
  const std::string key_hex = hex(n.key);
  if (have_key && contains(hay, n.key)) { if (what) *what = "raw key"; return true; }
  if (have_key && contains(hay, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(key_hex.data()), key_hex.size()))) { if (what) *what = "hex key"; return true; }
  for (const Bytes& w : n.windows)
    if (contains(hay, w)) { if (what) *what = "plaintext window"; return true; }
  return false;
}
// Fails the test for every file in `files` that holds a needle; returns the number of files scanned.
std::size_t scan_files(const std::vector<std::string>& files, const Needles& n) {
  for (const std::string& f : files) {
    std::string what;
    if (find_hit(read_all(f), n, &what)) {
      std::printf("FAIL: %s holds a %s\n", f.c_str(), what.c_str());
      ++g_fail;
    }
  }
  return files.size();
}
// The five (or four) files of a pack directory, relative to `dir`, as the set `changed_since` must report.
std::set<std::string> pack_files(const fs::path& dir, bool wrapped) {
  std::set<std::string> v;
  for (const char* f : {"bundle.vlrp", "content-key.vault", "catalog.meta", "catalog.meta.sig", "pack.json"}) v.insert((dir / f).string());
  if (wrapped)
    for (const fs::path& p : files_under(dir))
      if (p.filename().string().rfind("wrapped-key.", 0) == 0) v.insert(p.string());
  return v;
}

// ---- tests -------------------------------------------------------------------------------------------------------------------------------------------

void test_vectors() {
  const std::string lib_id = "velora.test.library";
  const Key32 content = seq<32>(0xc0);
  // wrapped_key_v1 (KEY-HIERARCHY.md section 6)
  const Key32 dev_priv = seq<32>(0x01), eph_priv = seq<32>(0x21);
  const Key32 dev_pub = x25519_public(dev_priv);
  CHECK(hex(dev_pub) == "07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c");
  std::array<std::uint8_t, kWrappedKeySize> blob{};
  CHECK(wrapped_key_seal(content, lib_id, dev_pub, eph_priv, seq<12>(0xa0), blob));
  CHECK(hex(blob) ==
        "56574b3101aaa8fff703b50b225869aff450549732cbaaed5e5df9b30a6da31cb0e5742bad5ad4a1a768f1a67ba0a1a2a3a4a5a6a7a8a9aaab9eb9ebbe5dd3cc7205c0854c53b12f3"
        "a98da5488b008bb9b068e9230c82bb4809e6004e870a73f77e35234a13d20ce3a");
  Key32 out{};
  CHECK(wrapped_key_open(dev_priv, blob, lib_id, out) && out == content);
  CHECK(!wrapped_key_open(dev_priv, blob, "velora.test.other", out));  // bound to the library
  for (std::size_t i = 0; i < blob.size(); ++i) {                      // every byte is covered: a flipped bit fails
    auto bad = blob;
    bad[i] ^= 0x01;
    CHECK(!wrapped_key_open(dev_priv, bad, lib_id, out));
  }
  CHECK(!wrapped_key_open(seq<32>(0x02), blob, lib_id, out));  // another device
  CHECK(!wrapped_key_open(dev_priv, std::span<const std::uint8_t>(blob).first(104), lib_id, out));
  std::array<std::uint8_t, kWrappedKeySize> z{};
  CHECK(!wrapped_key_seal(content, lib_id, Key32{}, eph_priv, seq<12>(0xa0), z));  // an all-zero shared secret (low-order peer) is refused
  CHECK(wrapped_key_seal_random(content, lib_id, dev_pub, z) && z != blob);
  std::array<std::uint8_t, kWrappedKeySize> z2{};
  CHECK(wrapped_key_seal_random(content, lib_id, dev_pub, z2) && z2 != z);  // fresh ephemeral key and nonce per wrap

  // vault_entry_v1
  const Key32 kek = seq<32>(0x40);
  CHECK(hex(kek_id(kek)) == "9b660e663d66688a");
  const auto entry = vault_entry_seal(kek, content, lib_id, seq<12>(0xb0));
  CHECK(hex(entry) ==
        "564b56319b660e663d66688ab0b1b2b3b4b5b6b7b8b9babbc3c14246058f2d5ef327ed33213b82046ed956fb90c000482b1fbae0556d2c4fee9f6c8b2270d1bca7a76c2f9658b9c4");
  CHECK(vault_entry_open(kek, entry, lib_id, out) && out == content);
  CHECK(!vault_entry_open(kek, entry, "velora.test.other", out));
  CHECK(!vault_entry_open(seq<32>(0x41), entry, lib_id, out));
  for (std::size_t i = 0; i < entry.size(); ++i) {
    auto bad = entry;
    bad[i] ^= 0x01;
    CHECK(!vault_entry_open(kek, bad, lib_id, out));
  }
  {  // vectors with a real UUID-text library_id, generated by tools/keys/tests/wrap_ref.py (the same constants are pinned in tools/keys/tests/test_keys.py)
    const std::string uuid = "3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f";
    std::array<std::uint8_t, kWrappedKeySize> wb{};
    CHECK(wrapped_key_seal(content, uuid, dev_pub, eph_priv, seq<12>(0xa0), wb));
    CHECK(hex(wb) ==
          "56574b3101aaa8fff703b50b225869aff450549732cbaaed5e5df9b30a6da31cb0e5742bad5ad4a1a768f1a67ba0a1a2a3a4a5a6a7a8a9aaabb27952d3c6f1222c0cfd438d34d0430"
          "34a13df431b860479b07ba77078486cc141bd3487091063b292da3d3b6643ee5d");
    const auto ve = vault_entry_seal(kek, content, uuid, seq<12>(0xb0));
    CHECK(hex(ve) ==
          "564b56319b660e663d66688ab0b1b2b3b4b5b6b7b8b9babbc3c14246058f2d5ef327ed33213b82046ed956fb90c000482b1fbae0556d2c4fe2b80d5f28f43d84b73e7b1212714196");
    CHECK(wrapped_key_open(dev_priv, wb, uuid, out) && out == content && vault_entry_open(kek, ve, uuid, out) && out == content);
  }
  CHECK(vault_nonce_deterministic(content, lib_id) == vault_nonce_deterministic(content, lib_id));
  CHECK(vault_nonce_deterministic(content, lib_id) != vault_nonce_deterministic(content, "velora.test.other"));
}

void test_description() {
  const std::string id = "11111111-2222-4333-8444-555555555555";
  PackDescription d;
  CHECK(!parse_pack_description(desc_json(id, "Vendor Piano", "lib/pack.sfz", "factory-test",
                                          "\"localized_names\": {\"pt-BR\": \"Piano do Fabricante\"}, \"instruments\": [{\"key\": \"pack.sfz\", \"category\": \"strings\", \"name\": \"Lead\"}]"),
                                "/base", d));
  CHECK(d.library_id == id && d.name == "Vendor Piano" && d.root == Root::kFactoryTest && d.source == "/base/lib/pack.sfz");
  CHECK(d.localized_names.size() == 1 && d.instruments.size() == 1 && d.instruments[0].category == "strings");
  // upper-case id is normalised
  CHECK(!parse_pack_description(desc_json("AAAAAAAA-2222-4333-8444-555555555555", "X", "a.sfz", "dev"), "", d) && d.library_id == "aaaaaaaa-2222-4333-8444-555555555555");
  const std::vector<std::string> bad = {
      "not json",
      "[]",
      desc_json("nope", "X", "a.sfz", "dev"),
      desc_json("00000000-0000-0000-0000-000000000000", "X", "a.sfz", "dev"),
      desc_json(id, "", "a.sfz", "dev"),
      desc_json(id, "X", "a.sfz", "staging"),
      desc_json(id, "X", "a.sfz", "dev", "", "not-a-category"),
      desc_json(id, "X", "a.sfz", "dev", "", "none"),
      desc_json(id, "X", "a.sfz", "dev", "\"extra\": 1"),
      desc_json(id, "X", "a.sfz", "dev", "\"instruments\": [{\"key\": \"a\"}]"),
      desc_json(id, "X", "a.sfz", "dev", "\"instruments\": [{\"key\": \"a\", \"category\": \"piano\"}, {\"key\": \"a\", \"name\": \"b\"}]"),
      desc_json(id, "X", "a.sfz", "dev", "\"instruments\": [{\"key\": \"a\", \"category\": \"nope\"}]"),
      desc_json(id, "X", "a.sfz", "dev", "\"localized_names\": {\"not a tag\": \"x\"}"),
      "{\"schema_version\": 2, \"library_id\": \"" + id + "\", \"name\": \"X\", \"source\": \"a.sfz\", \"root\": \"dev\", \"category\": \"piano\"}",
      "{\"schema_version\": 1, \"library_id\": \"" + id + "\", \"library_id\": \"" + id + "\", \"name\": \"X\", \"source\": \"a.sfz\", \"root\": \"dev\", \"category\": \"piano\"}",
  };
  for (const auto& b : bad) CHECK_CODE(parse_pack_description(b, "", d), Code::kBadDescription);
  CHECK(normalize_name("  Vendor\t  PIANO ") == "vendor piano" && normalize_name("\n") == "");
  CHECK(has_format_chars("a\xef\xbb\xbf" "b") && has_format_chars("\xc2\xad") && !has_format_chars("Caf\xc3\xa9 \xe2\x80\x93 Piano"));
  CHECK(is_dev_range_id(fmt::Uuid{0, 0, 0, 0, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0, 1}));
  CHECK(!is_dev_range_id(fmt::Uuid{1}));
}

void test_register(const fs::path& work) {
  const fs::path reg_path = work / "vendor-ids";
  put(reg_path, "# comment\n11111111-2222-4333-8444-555555555555\tVendor Piano\tAcme\t2026-01-02\n");
  VendorRegister reg;
  CHECK(!load_register(reg_path.string(), reg) && reg.entries.size() == 1);
  PackDescription d;
  CHECK(!parse_pack_description(desc_json("11111111-2222-4333-8444-555555555555", "Vendor Piano", "a.sfz", "production"), "", d));
  RegisterState st;
  CHECK(!check_register(reg, d, st) && st == RegisterState::kRegistered);
  // the same id for another library is refused
  PackDescription other = d;
  other.name = "Other Library";
  CHECK_CODE(check_register(reg, other, st), Code::kLibraryIdConflict);
  // the same name under another id is refused
  other = d;
  other.library_id = "99999999-2222-4333-8444-555555555555";
  fmt::uuid_from_string(other.library_id, other.library_uuid);
  CHECK_CODE(check_register(reg, other, st), Code::kLibraryIdConflict);
  // names compare trimmed, space-collapsed and ASCII case-folded: a case or spacing variant of a registered name is the same library
  other = d;
  other.library_id = "99999999-2222-4333-8444-555555555555";
  fmt::uuid_from_string(other.library_id, other.library_uuid);
  other.name = "  VENDOR   piano ";
  CHECK_CODE(check_register(reg, other, st), Code::kLibraryIdConflict);
  other = d;
  other.name = "VENDOR PIANO";  // the registered id with a case variant of its own name: the same library
  CHECK(!check_register(reg, other, st) && st == RegisterState::kRegistered);
  other = d;
  other.library_id = "99999999-2222-4333-8444-555555555555";
  fmt::uuid_from_string(other.library_id, other.library_uuid);
  other.name = "Brand New";
  {
    PackDescription v1 = other;
    v1.library_uuid[6] = 0x13;  // a version 1 id
    v1.library_id = fmt::uuid_to_string(v1.library_uuid);
    CHECK_CODE(append_register(reg_path.string(), v1, "x", "2026-10-20"), Code::kBadDescription);
  }
  other.library_id = "99999999-2222-4333-8444-555555555555";
  fmt::uuid_from_string(other.library_id, other.library_uuid);
  // a new id is new until appended
  other.name = "Brand New";
  CHECK(!check_register(reg, other, st) && st == RegisterState::kNew);
  CHECK(!append_register(reg_path.string(), other, "Newco", "2026-10-20"));
  CHECK(!load_register(reg_path.string(), reg) && reg.entries.size() == 2 && reg.find_id(other.library_id)->owner == "Newco");
  CHECK(!append_register(reg_path.string(), other, "Newco", "2026-10-20"));  // identical entry: no-op
  CHECK(!load_register(reg_path.string(), reg) && reg.entries.size() == 2);
  PackDescription clash = other;
  clash.name = "Taken";
  CHECK_CODE(append_register(reg_path.string(), clash, "x", "2026-10-20"), Code::kLibraryIdConflict);
  CHECK_CODE(append_register(reg_path.string(), other, "bad\towner", "2026-10-20"), Code::kRegisterIo);
  CHECK_CODE(append_register(reg_path.string(), other, "x", "20261020"), Code::kRegisterIo);
  // the dev range: no entry needed, never appended
  PackDescription dev = d;
  dev.library_id = "00000000-0000-4000-8000-000000000042";
  fmt::uuid_from_string(dev.library_id, dev.library_uuid);
  dev.name = "Dev Thing";
  CHECK(!check_register(reg, dev, st) && st == RegisterState::kDevRange);
  CHECK_CODE(append_register(reg_path.string(), dev, "x", "2026-10-20"), Code::kLibraryIdConflict);
  // malformed registers
  for (const char* text : {"onlyone\n", "11111111-2222-4333-8444-555555555555\tA\tB\tnot-a-date\n", "11111111-2222-4333-8444-55555555555X\tA\tB\t2026-01-01\n",
                           "11111111-2222-4333-8444-555555555555\tA\tB\t2026-01-01\n11111111-2222-4333-8444-555555555555\tC\tB\t2026-01-01\n",
                           "11111111-2222-4333-8444-555555555555\tA\tB\t2026-01-01\n22222222-2222-4333-8444-555555555555\tA\tB\t2026-01-01\n",
                           "AAAAAAAA-2222-4333-8444-555555555555\tA\tB\t2026-01-01\n",
                           "00000000-0000-4000-8000-000000000042\tDev\tB\t2026-01-01\n",   // the dev range is never registered
                           "11111111-2222-1333-8444-555555555555\tA\tB\t2026-01-01\n",     // not a version 4 id
                           "11111111-2222-4333-c444-555555555555\tA\tB\t2026-01-01\n",     // not the RFC 4122 variant
                           "11111111-2222-4333-8444-555555555555\tA\tB\t2026-01-01\n22222222-2222-4333-8444-555555555555\t  a \tB\t2026-01-01\n",  // same normalised name
                           "11111111-2222-4333-8444-555555555555\tZero\xe2\x80\x8b" "Width\tB\t2026-01-01\n"}) {
    put(reg_path, text);
    CHECK_CODE(load_register(reg_path.string(), reg), Code::kRegisterIo);
  }
  CHECK_CODE(load_register((work / "missing").string(), reg), Code::kRegisterIo);
  CHECK(!load_register((work / "missing").string(), reg, false) && reg.entries.empty());
  // the committed register parses and holds no dev-range id
  CHECK(!load_register(VELORA_CP_COMMITTED_REGISTER, reg));
  for (const auto& e : reg.entries) {
    fmt::Uuid u{};
    fmt::uuid_from_string(e.library_id, u);
    CHECK(!is_dev_range_id(u));
  }
}

// Reads a sample's frames out of a protected bundle.
std::vector<std::int32_t> read_sample(const fmt::ProtectedBundleReader& r, std::size_t i) {
  const auto& s = r.samples()[i];
  std::vector<std::int32_t> v(s.frame_count * s.format.channels);
  CHECK(r.read_frames(i, 0, s.frame_count, v) == fmt::Error::kNone);
  return v;
}

// Decrypts `bundle` and writes the same content as an UNPROTECTED bundle (same library id, instruments, samples and regions).
// `tweak_sample` >= 0 changes one PCM value of that flat sample (the file stays valid): a negative control for the render comparison.
Bytes rebuild_plain(const fs::path& bundle, const Key32& unwrapped, const fmt::TrustedKeys& trusted, int tweak_sample = -1) {
  fmt::ContentKey ck;
  ck.bytes = unwrapped;
  auto rd = fmt::ProtectedBundleReader::open_file(bundle.string(), trusted, ck);
  CHECK(rd.ok());
  if (!rd) return {};
  fmt::BundleWriter w;
  w.set_library_id(rd.value().info().library_id);
  for (const auto& ins : rd.value().instruments()) {
    auto idx = w.add_instrument(ins.name, ins.key);
    CHECK(idx.ok());
    for (std::size_t k = 0; k < ins.sample_count; ++k) {
      const std::size_t si = ins.first_sample + k;
      auto pcm = read_sample(rd.value(), si);
      if (static_cast<int>(si) == tweak_sample) pcm[100] ^= 0x40;
      const auto& s = rd.value().samples()[si];
      CHECK(w.add_sample(idx.value(), {s.name, s.format, s.frame_count, pcm}) == fmt::Error::kNone);
    }
    for (const auto& reg : ins.regions) CHECK(w.add_region(idx.value(), reg) == fmt::Error::kNone);
  }
  auto bytes = w.finalize();
  CHECK(bytes.ok());
  return bytes ? bytes.value() : Bytes{};
}

void test_importer_options(const fs::path& work, const fs::path& src) {
  // Additive options of the user-import path: a vendor library_id replaces the path-derived one; an unparsable or nil id is refused.
  lib::ImportOptions opt;
  opt.sfz_path = (src / "pack.sfz").string();
  opt.root = (work / "opt_root").string();
  fs::create_directories(opt.root);
  const auto derived = lib::import_sfz(opt);
  CHECK(derived.ok());
  CHECK(derived.library_id == fmt::uuid_to_string(fmt::library_id_from_source(fs::weakly_canonical(src / "pack.sfz").generic_string())));
  opt.library_id = "00000000-0000-4000-8000-0000000000aa";
  opt.root = (work / "opt_root2").string();
  fs::create_directories(opt.root);
  const auto vendor = lib::import_sfz(opt);
  CHECK(vendor.ok() && vendor.library_id == opt.library_id);
  CHECK(fs::exists(fs::path(vendor.bundle_dir) / "bundle.vlb") && vendor.bundle_dir.find(opt.library_id) != std::string::npos);
  CHECK(vendor.bundle_id != derived.bundle_id);  // the id is in the manifest
  for (const char* bad : {"not-a-uuid", "00000000-0000-0000-0000-000000000000", "0000000000004000800000000000000a"}) {
    opt.library_id = bad;
    CHECK(lib::import_sfz(opt).status == lib::Status::kInvalidArgument);
  }
  // build-only mode: nothing is created in a data root that is passed anyway, no root is needed, and the sink sees the verified writer.
  lib::ImportOptions b;
  b.sfz_path = (src / "pack.sfz").string();
  b.root = (work / "untouched_root").string();
  fs::create_directories(b.root);
  b.library_id = "00000000-0000-4000-8000-0000000000bb";
  b.category = "piano";
  b.write_catalog_meta = true;
  int calls = 0;
  b.bundle_sink = [&](const lib::BuiltBundle& built) {
    ++calls;
    CHECK(built.writer && built.library_id == "00000000-0000-4000-8000-0000000000bb" && built.plain_bundle_id.size() == 64);
    lib::CatalogMeta m;
    CHECK(lib::parse_catalog_meta(built.catalog_meta, m) && m.instruments.size() == 1 && m.instruments[0].category == fmt::Category::kPiano);
    return fmt::Error::kNone;
  };
  const auto r = lib::import_file(b);
  CHECK(r.ok() && calls == 1 && r.bundle_dir.empty() && !r.bundle_created && !r.current_flipped && r.timbres_published == 1);
  CHECK(fs::is_empty(b.root));
  b.root.clear();
  CHECK(lib::import_file(b).ok());  // no root at all
  b.bundle_sink = [](const lib::BuiltBundle&) { return fmt::Error::kIo; };
  CHECK(lib::import_file(b).status == lib::Status::kIo);
  // a skipped source is still skipped in build-only mode
  put(src / "bad.sfz", "<region> sample=missing.wav key=60\n");
  b.sfz_path = (src / "bad.sfz").string();
  b.bundle_sink = [&](const lib::BuiltBundle&) { ++calls; return fmt::Error::kNone; };
  const int before = calls;
  const auto sk = lib::import_file(b);
  CHECK(sk.status == lib::Status::kSkipped && calls == before);
  fs::remove(src / "bad.sfz");
}

// Story 6.3 importer fixes: the vendor id decides the `current` lookup of a non-sink SF2 import; build-only SF2 never checkpoints or writes.
void test_importer_sf2(const fs::path& work) {
  const fs::path sf2 = ifx::write_many_sf2(work / "many/many.sf2", 3);
  lib::ImportOptions opt;
  opt.sfz_path = sf2.string();
  opt.root = (work / "many_root").string();
  opt.checkpoint_timbres = 1;
  opt.library_id = "00000000-0000-4000-8000-0000000000cc";
  fs::create_directories(opt.root);
  const auto first = lib::import_sf2(opt);
  CHECK(first.ok() && first.library_id == opt.library_id && first.checkpoints == 2);  // no `current` yet: a checkpoint per timbre
  const auto again = lib::import_sf2(opt);
  // The library (under the vendor id) now has a `current` bundle: Import again must not publish checkpoints (the old bundle stays current until the whole new one verified).
  CHECK(again.ok() && again.library_id == opt.library_id && again.checkpoints == 0);
  // Build-only: a non-empty root is neither used nor touched, nothing is checkpointed.
  lib::ImportOptions b;
  b.sfz_path = sf2.string();
  b.root = (work / "many_untouched").string();
  fs::create_directories(b.root);
  put(fs::path(b.root) / "marker", "x");
  b.checkpoint_timbres = 1;
  b.library_id = "00000000-0000-4000-8000-0000000000dd";
  int calls = 0;
  b.bundle_sink = [&](const lib::BuiltBundle&) { ++calls; return fmt::Error::kNone; };
  const Snap before = snapshot();
  const auto r = lib::import_sf2(b);
  CHECK(r.ok() && calls == 1 && r.checkpoints == 0 && r.timbres_published == 3 && r.bundle_dir.empty());
  CHECK(changed_since(before).empty());  // nothing created or changed anywhere under the scratch directory
  CHECK(std::distance(fs::directory_iterator(b.root), fs::directory_iterator()) == 1);
}

PackRequest base_request(const Keys& keys, const PackDescription& d, const std::string& out, const std::string& register_path) {
  PackRequest req;
  req.desc = d;
  req.out_dir = out;
  req.register_path = register_path;
  req.kek = &keys.kek;
  req.signing_seed = &keys.sg;
  return req;
}

// SoundFont 2 sources go through the same importer: a General-MIDI-style bank packs into a protected bundle with one instrument per preset.
void test_sf2(const fs::path& work) {
  const fs::path sf2 = ifx::write_sf2(work / "sf2src/bank.sf2");
  const Keys keys = make_keys(work / "sf2keys", "dev", true);
  PackDescription d;
  CHECK(!parse_pack_description(desc_json("00000000-0000-4000-8000-000000000003", "GM Bank", sf2.string(), "dev",
                                          "\"instruments\": [{\"key\": \"128:0\", \"category\": \"drums_perc\", \"name\": \"Kit\"}]", "keys"),
                                "", d));
  // the unprotected import of the same bank gives the block windows that must not appear
  lib::ImportOptions plain_opt;
  plain_opt.sfz_path = sf2.string();
  plain_opt.root = (work / "sf2plain").string();
  fs::create_directories(plain_opt.root);
  const auto plain = lib::import_sf2(plain_opt);
  CHECK(plain.ok());
  const Bytes plain_bytes = read_all(fs::path(plain.bundle_dir) / "bundle.vlb");
  Needles needles;
  add_windows(needles, plain_bytes, plain_bytes.size() / 2, 211);

  fs::create_directories(work / "sf2out");
  PackRequest req = base_request(keys, d, (work / "sf2out").string(), (work / "no-register").string());
  PackReport rep;
  const Snap before = snapshot();
  CHECK_CODE(run_pack(req, rep), Code::kOk);
  const auto changed = changed_since(before);
  CHECK(std::set<std::string>(changed.begin(), changed.end()) == pack_files(rep.dir, false) && changed.size() == 5);
  CHECK(rep.instruments == 6);
  lib::CatalogMeta meta;
  CHECK(lib::parse_catalog_meta(read_text(fs::path(rep.dir) / "catalog.meta"), meta));
  std::map<std::string, std::string> names;
  std::map<std::string, fmt::Category> cats;
  for (const auto& m : meta.instruments) {
    names[m.key] = m.name;
    cats[m.key] = m.category;
  }
  CHECK(names["128:0"] == "Kit" && cats["128:0"] == fmt::Category::kDrumsPerc && cats["0:0"] == fmt::Category::kKeys && meta.source_kind == "sf2");
  CHECK(meta.source_path == "bank.sf2");
  Key32 k{};
  CHECK(vault_entry_open(keys.kek, read_all(fs::path(rep.dir) / "content-key.vault"), d.library_id, k));
  needles.key = k;
  CHECK(scan_files(changed, needles) == 5);
  fmt::ContentKey ck;
  ck.bytes = k;
  const fmt::PublicKey pub = keys.sign_pub();
  auto rd = fmt::ProtectedBundleReader::open_file((fs::path(rep.dir) / "bundle.vlrp").string(), fmt::TrustedKeys({fmt::TrustedKey{fmt::key_id_of_public(pub), pub}}), ck);
  CHECK(rd.ok() && rd.value().instruments().size() == 6 && rd.value().verify_all() == fmt::Error::kNone);
  // an unknown instrument key in the description is refused before anything is written
  d.instruments[0].key = "9:9";
  fs::create_directories(work / "sf2out2");
  req = base_request(keys, d, (work / "sf2out2").string(), (work / "no-register").string());
  const Snap before2 = snapshot();
  CHECK_CODE(run_pack(req, rep), Code::kImportFailed);
  CHECK(changed_since(before2).empty());
}

void test_pack(const fs::path& work, const fs::path& src_a, const Source& source) {
  const Keys keys = make_keys(work / "keys", "dev", true);
  const std::string lib_id = "00000000-0000-4000-8000-000000000001";
  const std::string no_register = (work / "no-register").string();
  PackDescription d;
  CHECK(!parse_pack_description(desc_json(lib_id, "Vendor Piano", (src_a / "pack.sfz").string(), "dev"), "", d));
  fs::create_directories(work / "out1");

  // A plain (unprotected) import of the same library: its blocks must not appear in any pack output; it is also the render reference.
  lib::ImportOptions plain_opt;
  plain_opt.sfz_path = (src_a / "pack.sfz").string();
  plain_opt.root = (work / "plain_root").string();
  fs::create_directories(plain_opt.root);
  const auto plain = lib::import_sfz(plain_opt);
  CHECK(plain.ok());
  const Bytes plain_bytes = read_all(fs::path(plain.bundle_dir) / "bundle.vlb");
  Needles needles;
  add_windows(needles, plain_bytes, plain_bytes.size() / 2, 211);
  for (const std::string& pcm : source.pcm) add_windows(needles, {reinterpret_cast<const std::uint8_t*>(pcm.data()), pcm.size()}, 0, 97);

  {  // The scan is not vacuous. It finds block windows in the unprotected bundle, raw PCM in the source WAV, and a planted raw or hex key. (Raw PCM is NOT in the
     // unprotected bundle: its blocks are FLAC coded, so the PCM windows only guard against a tool that writes decoded audio.)
    Needles blocks, pcm, planted;
    add_windows(blocks, plain_bytes, plain_bytes.size() / 2, 211);
    for (const std::string& p : source.pcm) add_windows(pcm, {reinterpret_cast<const std::uint8_t*>(p.data()), p.size()}, 0, 97);
    CHECK(find_hit(plain_bytes, blocks));
    CHECK(find_hit(read_all(src_a / "samples/a.wav"), pcm));
    CHECK(!find_hit(plain_bytes, pcm));
    planted.key = seq<32>(7);
    Bytes raw(planted.key.begin(), planted.key.end());
    const std::string h = hex(planted.key);
    CHECK(find_hit(raw, planted) && find_hit({reinterpret_cast<const std::uint8_t*>(h.data()), h.size()}, planted));
    std::string what;
    Bytes copy = plain_bytes;
    CHECK(find_hit(copy, needles, &what) && what == "plaintext window");  // the very needles used below do fire on a plain bundle
  }

  // Runs run_pack with the independent watch: only the pack's own files appear anywhere under the scratch directory (and so in TMPDIR), and none of them holds a needle.
  auto watched = [&](PackRequest& r, PackReport& rp) {
    const Snap before = snapshot();
    CHECK_CODE(run_pack(r, rp), Code::kOk);
    const auto ch = changed_since(before);
    CHECK(std::set<std::string>(ch.begin(), ch.end()) == pack_files(rp.dir, true) && ch.size() == 5 + r.device_pubs.size());
    Key32 k{};
    CHECK(vault_entry_open(*r.kek, read_all(fs::path(rp.dir) / "content-key.vault"), r.desc.library_id, k));
    needles.key = k;
    CHECK(scan_files(ch, needles) == ch.size());
  };

  // 1. Pack with a fresh random key. The write monitor sees every file.
  std::map<std::string, Bytes> monitored;
  PackRequest req = base_request(keys, d, (work / "out1").string(), no_register);
  req.device_pubs.push_back(keys.dev_pub);
  req.write_monitor = [&](const std::string& name, std::span<const std::uint8_t> b) { monitored[name] = Bytes(b.begin(), b.end()); };
  PackReport rep;
  watched(req, rep);
  const fs::path pack_dir = work / "out1" / lib_id;
  CHECK(rep.dir == pack_dir.string() && rep.library_id == lib_id && rep.instruments == 1 && rep.blocks > 0);
  const std::string wrapped_name = "wrapped-key." + hex(fmt::key_id_of_public(keys.dev_pub)) + ".bin";
  CHECK(fs::exists(pack_dir / wrapped_name) && files_under(pack_dir).size() == 6 && monitored.size() == 6);
  for (const auto& [name, bytes] : monitored) CHECK(read_all(pack_dir / name) == bytes);
  CHECK(std::distance(fs::directory_iterator(work / "out1"), fs::directory_iterator()) == 1);  // no staging leftovers

  // 2. The key is only in the vault entry and the wrapped blob: unwrap both, compare, decrypt the bundle.
  Key32 vaulted{}, unwrapped{};
  const Bytes vault = read_all(pack_dir / "content-key.vault");
  CHECK(vault.size() == kVaultEntrySize && vault_entry_open(keys.kek, vault, lib_id, vaulted));
  const Bytes blob = read_all(pack_dir / wrapped_name);
  CHECK(blob.size() == kWrappedKeySize && wrapped_key_open(keys.dev_priv, blob, lib_id, unwrapped) && unwrapped == vaulted);
  CHECK(vaulted != Key32{});
  for (const auto& [name, b] : monitored) CHECK(!find_hit(b, [&] { Needles n = needles; n.key = vaulted; return n; }()));
  fmt::ContentKey ck;
  ck.bytes = vaulted;
  const fmt::PublicKey pub = keys.sign_pub();
  const fmt::TrustedKeys trusted({fmt::TrustedKey{fmt::key_id_of_public(pub), pub}});
  auto rd = fmt::ProtectedBundleReader::open_file((pack_dir / "bundle.vlrp").string(), trusted, ck);
  CHECK(rd.ok());
  if (!rd) return;
  CHECK(fmt::uuid_to_string(rd.value().info().library_id) == lib_id && hex(rd.value().info().bundle_id) == rep.bundle_id);
  CHECK(rd.value().info().signer_key_id == fmt::key_id_of_public(pub) && hex(rd.value().info().signer_key_id) == rep.signer_key_id);
  CHECK(rd.value().verify_all() == fmt::Error::kNone);
  CHECK(rd.value().samples().size() == 3);
  {  // the decoded PCM is the source WAV's PCM bit for bit
    const std::size_t bits[3] = {16, 24, 16};
    for (std::size_t i = 0; i < 3; ++i) {
      const auto v = read_sample(rd.value(), i);
      std::string raw;
      for (std::int32_t x : v)
        for (std::size_t b = 0; b < bits[i] / 8; ++b) raw.push_back(char((x >> (8 * b)) & 0xff));
      CHECK(raw == source.pcm[i]);
    }
  }
  // wrong KEK, wrong key and an untrusted signer never decrypt
  Key32 junk{};
  CHECK(!vault_entry_open(seq<32>(1), vault, lib_id, junk));
  CHECK(fmt::ProtectedBundleReader::open_file((pack_dir / "bundle.vlrp").string(), trusted, fmt::ContentKey{}).error() == fmt::Error::kWrongContentKey);
  CHECK(fmt::ProtectedBundleReader::open_file((pack_dir / "bundle.vlrp").string(), fmt::TrustedKeys{}, ck).error() == fmt::Error::kUntrustedKey);

  // catalog.meta carries the categories and names, and not the packer's directory layout
  lib::CatalogMeta meta;
  const std::string meta_text = read_text(pack_dir / "catalog.meta");
  CHECK(lib::parse_catalog_meta(meta_text, meta));
  CHECK(meta.library_name == "Vendor Piano" && meta.instruments.size() == 1 && meta.instruments[0].key == "pack.sfz" &&
        meta.instruments[0].category == fmt::Category::kPiano);
  CHECK(meta.source_path == "pack.sfz" && meta_text.find(work.string()) == std::string::npos && meta_text.find("src_a") == std::string::npos);
  {  // Story 6.4: catalog.meta.sig binds the catalog.meta to this bundle with the bundle's signing key; the verifier accepts it and rejects any altered byte
    const Bytes seal = read_all(pack_dir / "catalog.meta.sig");
    const Bytes meta_bytes = read_all(pack_dir / "catalog.meta");
    auto view = fmt::ProtectedBundleView::open_file((pack_dir / "bundle.vlrp").string(), trusted);
    CHECK(view.ok() && seal.size() == fmt::kCatalogMetaSealSize);
    if (view.ok()) {
      const fmt::Digest256 bid = view.value().info().bundle_id;
      std::uint32_t rev = 0;
      CHECK(fmt::verify_catalog_meta_seal(trusted, bid, meta_bytes, seal, nullptr, &rev) == fmt::Error::kNone && rev == 1);  // the default pack revision
      Bytes bad_meta = meta_bytes;
      bad_meta[bad_meta.size() / 2] ^= 1;
      CHECK(fmt::verify_catalog_meta_seal(trusted, bid, bad_meta, seal) == fmt::Error::kSignatureInvalid);
      Bytes bad_seal = seal;
      bad_seal[40] ^= 1;
      CHECK(fmt::verify_catalog_meta_seal(trusted, bid, meta_bytes, bad_seal) == fmt::Error::kSignatureInvalid);
      fmt::Digest256 other = bid;
      other[0] ^= 1;
      CHECK(fmt::verify_catalog_meta_seal(trusted, other, meta_bytes, seal) == fmt::Error::kSignatureInvalid);
      CHECK(fmt::verify_catalog_meta_seal(fmt::TrustedKeys{}, bid, meta_bytes, seal) == fmt::Error::kUntrustedKey);
    }
  }
  const std::string report_json = read_text(pack_dir / "pack.json");
  CHECK(report_json.find(lib_id) != std::string::npos && report_json.find(rep.bundle_id) != std::string::npos && report_json.find("category.piano") != std::string::npos);
  CHECK(report_json.find(hex(vaulted)) == std::string::npos && report_json.find(work.string()) == std::string::npos);

  // 4. Reproducibility: same source, same content key, same signing key => identical bundle bytes, also from a copy of the source in another directory;
  //    with --deterministic-vault the whole directory is identical (wrapped keys excluded: fresh ephemeral key and nonce each time).
  fs::create_directories(work / "out2");
  PackRequest again = base_request(keys, d, (work / "out2").string(), no_register);
  fmt::ContentKey reuse;
  reuse.bytes = vaulted;
  again.content_key = &reuse;
  again.deterministic_vault = true;
  PackReport rep2;
  watched(again, rep2);
  CHECK(read_all(fs::path(rep2.dir) / "bundle.vlrp") == read_all(pack_dir / "bundle.vlrp"));
  CHECK(rep2.bundle_id == rep.bundle_id && rep2.nonce_hex == rep.nonce_hex);
  CHECK(read_all(fs::path(rep2.dir) / "catalog.meta") == read_all(pack_dir / "catalog.meta"));
  CHECK(read_all(fs::path(rep2.dir) / "content-key.vault") != vault);  // the first vault entry used a random nonce (allowed to differ)
  fs::create_directories(work / "out3");
  PackRequest third = again;
  third.out_dir = (work / "out3").string();
  PackReport rep3;
  watched(third, rep3);
  for (const char* f : {"bundle.vlrp", "content-key.vault", "catalog.meta", "catalog.meta.sig", "pack.json"}) CHECK(read_all(fs::path(rep3.dir) / f) == read_all(fs::path(rep2.dir) / f));
  CHECK(read_all(fs::path(rep3.dir) / "bundle.vlrp") == read_all(pack_dir / "bundle.vlrp"));
  {  // Story 6.4: --revision N signs the pack revision into catalog.meta.sig; the bundle itself does not change
    fs::create_directories(work / "out_rev");
    PackRequest r5 = again;
    r5.out_dir = (work / "out_rev").string();
    r5.revision = 5;
    PackReport rep5r;
    watched(r5, rep5r);
    CHECK(read_all(fs::path(rep5r.dir) / "bundle.vlrp") == read_all(pack_dir / "bundle.vlrp"));
    CHECK(read_all(fs::path(rep5r.dir) / "catalog.meta.sig") != read_all(pack_dir / "catalog.meta.sig"));
    auto view = fmt::ProtectedBundleView::open_file((fs::path(rep5r.dir) / "bundle.vlrp").string(), trusted);
    std::uint32_t rev = 0;
    CHECK(view.ok() && fmt::verify_catalog_meta_seal(trusted, view.value().info().bundle_id, read_all(fs::path(rep5r.dir) / "catalog.meta"),
                                                     read_all(fs::path(rep5r.dir) / "catalog.meta.sig"), nullptr, &rev) == fmt::Error::kNone && rev == 5);
    CHECK(read_text(fs::path(rep5r.dir) / "pack.json").find("\"pack_revision\": 5") != std::string::npos);
  }
  {  // a copy of the source tree elsewhere: every file identical
    const fs::path copy = work / "src_copy";
    fs::create_directories(copy);
    fs::copy(src_a, copy, fs::copy_options::recursive);
    PackDescription d2 = d;
    d2.source = (copy / "pack.sfz").string();
    fs::create_directories(work / "out4");
    PackRequest r4 = again;
    r4.desc = d2;
    r4.out_dir = (work / "out4").string();
    PackReport rep4;
    watched(r4, rep4);
    for (const char* f : {"bundle.vlrp", "content-key.vault", "catalog.meta", "catalog.meta.sig", "pack.json"}) CHECK(read_all(fs::path(rep4.dir) / f) == read_all(fs::path(rep2.dir) / f));
  }
  {  // a fresh random key: another bundle
    fs::create_directories(work / "out5");
    PackRequest r5 = base_request(keys, d, (work / "out5").string(), no_register);
    PackReport rep5;
    watched(r5, rep5);
    CHECK(read_all(fs::path(rep5.dir) / "bundle.vlrp") != read_all(pack_dir / "bundle.vlrp") && rep5.bundle_id != rep.bundle_id);
    Key32 k5{};
    CHECK(vault_entry_open(keys.kek, read_all(fs::path(rep5.dir) / "content-key.vault"), lib_id, k5) && k5 != vaulted);
  }
  // the content key must be a real key
  {
    PackRequest r0 = again;
    r0.out_dir = (work / "out_zero").string();
    fs::create_directories(r0.out_dir);
    fmt::ContentKey zero;
    r0.content_key = &zero;
    CHECK_CODE(run_pack(r0, rep3), Code::kBadKeyFile);
    CHECK(fs::is_empty(r0.out_dir));
  }
  // an existing pack needs --overwrite; the same key replaces it in place
  CHECK_CODE(run_pack(again, rep2), Code::kOutputFailed);
  again.overwrite = true;
  CHECK_CODE(run_pack(again, rep2), Code::kOk);
  CHECK(read_all(fs::path(rep2.dir) / "bundle.vlrp") == read_all(pack_dir / "bundle.vlrp"));
  CHECK(rep2.kept_old.empty() && files_under(work / "out2").size() == 5 && std::distance(fs::directory_iterator(work / "out2"), fs::directory_iterator()) == 1);
  // ... but another key must not destroy the only copy of the old one: the old directory is kept, with its vault entry
  {
    PackRequest r6 = base_request(keys, d, (work / "out2").string(), no_register);
    r6.overwrite = true;
    PackReport rep6;
    CHECK_CODE(run_pack(r6, rep6), Code::kOk);
    CHECK(!rep6.kept_old.empty() && fs::is_directory(rep6.kept_old) && fs::path(rep6.kept_old).filename().string().rfind(".old-" + lib_id, 0) == 0);
    Key32 old_key{}, new_key{};
    CHECK(vault_entry_open(keys.kek, read_all(fs::path(rep6.kept_old) / "content-key.vault"), lib_id, old_key) && old_key == vaulted);
    CHECK(vault_entry_open(keys.kek, read_all(fs::path(rep6.dir) / "content-key.vault"), lib_id, new_key) && new_key != vaulted);
    CHECK(fmt::ProtectedBundleReader::open_file((fs::path(rep6.kept_old) / "bundle.vlrp").string(), trusted, ck).ok());  // the old pack is still decryptable
  }

  // 5. Render. HEADLINE EVIDENCE: the library decrypted with the vaulted key and rebuilt without protection is byte-identical to the unprotected bundle the importer
  //    itself makes for the same vendor id (its own finalize() and, on disk, a normal published import with that library_id), so every render of the two
  //    is identical by construction. The renders below confirm it end to end; SF2 sources are covered by the decrypt/PCM tests above but not by a render
  //    comparison. Story 6.5: the engine's protected path now renders STRAIGHT from the pack's bundle.vlrp (step 6 below); the rebuilt bundle remains as the headline
  //    evidence for the SF2-free, key-free comparison.
  const Bytes rebuilt = rebuild_plain(pack_dir / "bundle.vlrp", unwrapped, trusted);
  CHECK(!rebuilt.empty());
  {
    lib::ImportOptions o;
    o.sfz_path = (src_a / "pack.sfz").string();
    o.library_id = lib_id;
    Bytes via_sink;
    o.bundle_sink = [&](const lib::BuiltBundle& b) {
      auto f = b.writer->finalize();
      if (!f) return f.error();
      via_sink = f.value();
      return fmt::Error::kNone;
    };
    CHECK(lib::import_file(o).ok());
    CHECK(via_sink == rebuilt);
    lib::ImportOptions pub_opt;  // the same import, published to a data root: the bytes on disk
    pub_opt.sfz_path = (src_a / "pack.sfz").string();
    pub_opt.library_id = lib_id;
    pub_opt.root = (work / "vendor_root").string();
    fs::create_directories(pub_opt.root);
    const auto published = lib::import_sfz(pub_opt);
    CHECK(published.ok() && published.library_id == lib_id);
    CHECK(read_all(fs::path(published.bundle_dir) / "bundle.vlb") == rebuilt);
  }
  put_bytes(work / "render/rebuilt.vlb", rebuilt);
  put_bytes(work / "render/suite.mid", make_midi());
  const std::string common = std::string("'") + RENDER_EXE + "' --midi '" + (work / "render/suite.mid").string() + "' --engine sample --instrument pack.sfz --tail-seconds 0.5 --out ";
  std::string out;
  CHECK(run_cmd(common + "'" + (work / "render/protected.wav").string() + "' --bundle '" + (work / "render/rebuilt.vlb").string() + "'", &out) == 0);
  CHECK(run_cmd(common + "'" + (work / "render/plain.wav").string() + "' --bundle '" + (fs::path(plain.bundle_dir) / "bundle.vlb").string() + "'", &out) == 0);
  const Bytes wp = read_all(work / "render/protected.wav"), wu = read_all(work / "render/plain.wav");
  CHECK(wp.size() > 1000 && wp == wu);
  bool loud = false;
  for (std::size_t i = 44; i < wp.size() && !loud; ++i) loud = wp[i] != 0;
  CHECK(loud);  // not a vacuous comparison of two silent files
  // Negative control: one altered PCM value in a sample the suite plays (the rebuilt file is still a perfectly valid bundle) must change the render.
  {
    const Bytes altered = rebuild_plain(pack_dir / "bundle.vlrp", unwrapped, trusted, /*tweak_sample=*/0);
    CHECK(!altered.empty() && altered != rebuilt);
    put_bytes(work / "render/altered.vlb", altered);
    CHECK(run_cmd(common + "'" + (work / "render/altered.wav").string() + "' --bundle '" + (work / "render/altered.vlb").string() + "'", &out) == 0);
    CHECK(read_all(work / "render/altered.wav") != wp);
  }

#ifdef RENDER_HAS_DEV_KEYS
  // 6. Story 6.5: the offline renderer plays the pack's PROTECTED bundle directly -- the content key is unwrapped with the dev device key from the pack's wrapped-key blob,
  //    blocks are hash-checked and decrypted in the renderer's process, nothing is rebuilt or written -- and the audio is bit-identical to the render of the unprotected
  //    import of the same library (this replaces the Story 6.3 substitute above as the end-to-end proof).
  {
    const fmt::KeyId sid = fmt::key_id_of_public(pub);
    put(work / "render/trust.pub-list", "dev content-manifest " + hex(sid) + " " + hex(pub) + "\n");
    const std::string flags = " --bundle '" + (pack_dir / "bundle.vlrp").string() + "' --dev-device-key '" + (work / "keys/device.x25519.priv").string() + "' --wrapped-key '" +
                              (pack_dir / wrapped_name).string() + "' --trust-list '" + (work / "render/trust.pub-list").string() + "'";
    const auto before = snapshot();
    CHECK(run_cmd(common + "'" + (work / "render/direct.wav").string() + "'" + flags, &out) == 0);
    CHECK(read_all(work / "render/direct.wav") == wp);
    CHECK(fmt::to_hex(fmt::Sha256::hash(read_all(work / "render/direct.wav"))) == fmt::to_hex(fmt::Sha256::hash(wu)));
    const auto ch = changed_since(before);  // the render wrote its WAV and nothing else: no decrypted block, no key, anywhere under the scratch tree
    CHECK(ch.size() == 1 && ch[0] == (work / "render/direct.wav").string());
    // The pack's wrapped key is for ITS device: another device key cannot play it.
    Key32 other_priv = seq<32>(0x70);
    put_bytes(work / "render/other.priv", other_priv, 0600);
    CHECK(run_cmd(common + "'" + (work / "render/never.wav").string() + "' --bundle '" + (pack_dir / "bundle.vlrp").string() + "' --dev-device-key '" + (work / "render/other.priv").string() +
                      "' --wrapped-key '" + (pack_dir / wrapped_name).string() + "' --trust-list '" + (work / "render/trust.pub-list").string() + "'",
                  &out) == 1);
    CHECK(out.find("key_unavailable") != std::string::npos && !fs::exists(work / "render/never.wav"));
  }
#endif
}

void test_cli(const fs::path& work, const fs::path& src_a, const Source& source) {
  const std::string exe = std::string("'") + CP_EXE + "'";
  const std::string env = "TMPDIR='" + (work / "tmp").string() + "' ";
  const Keys dev = make_keys(work / "cli_keys_dev", "dev", true);
  const Keys ft = make_keys(work / "cli_keys_ft", "factory-test");
  const fs::path reg = work / "cli-vendor-ids";
  put(reg, "# test register\naaaaaaaa-2222-4333-8444-555555555555\tTaken Name\tAcme\t2026-01-02\n");
  const std::string src = (src_a / "pack.sfz").string();
  std::string out;
  auto desc = [&](const char* file, const std::string& id, const std::string& name, const std::string& root, const std::string& extra = "") {
    put(work / file, desc_json(id, name, src, root, extra));
    return "'" + (work / file).string() + "'";
  };
  const std::string o = " --out '" + (work / "cli_out").string() + "'";
  fs::create_directories(work / "cli_out");
  const std::string regarg = " --register-file '" + reg.string() + "'";
  const std::string devkeys = " --keys-dir '" + (work / "cli_keys_dev").string() + "'";
  const std::string ftkeys = " --keys-dir '" + (work / "cli_keys_ft").string() + "'";
  const std::string no_dev_list = " --dev-trust-list '" + (work / "no-such-list").string() + "'";

  // The process hardening is real: no core dumps.
  CHECK(run_cmd(exe + " core-limit", &out) == 0 && out.find("core_limit=0/0") != std::string::npos);

  // usage
  CHECK(run_cmd(exe) == 2);
  CHECK(run_cmd(exe + " pack") == 2);
  CHECK(run_cmd(exe + " pack --content-key 00") == 2);  // a key never comes through argv
  CHECK(run_cmd(exe + " bogus") == 2);
  CHECK(run_cmd(exe + " pack --description x --out y --revision 0") == 2 && run_cmd(exe + " pack --description x --out y --revision abc") == 2);  // revisions start at 1
  // bad description
  put(work / "bad.json", "{ nope");
  CHECK(run_cmd(env + exe + " pack --description '" + (work / "bad.json").string() + "'" + o + devkeys + regarg, &out) == 3);
  CHECK(out.find("bad_description") != std::string::npos);

  // ok: dev root, dev-range id, no register entry needed. Watched: only the pack's files appear, none holds the key, and the key is not in the tool's output.
  const std::string dev_id = "00000000-0000-4000-8000-000000000007";
  const std::string dev_desc = desc("dev.json", dev_id, "Dev Pack", "dev");
  Needles needles;
  {
    lib::ImportOptions po;
    po.sfz_path = src;
    po.root = (work / "cli_plain").string();
    fs::create_directories(po.root);
    const auto pl = lib::import_sfz(po);
    CHECK(pl.ok());
    const Bytes pb = read_all(fs::path(pl.bundle_dir) / "bundle.vlb");
    add_windows(needles, pb, pb.size() / 2, 211);
    for (const std::string& p : source.pcm) add_windows(needles, {reinterpret_cast<const std::uint8_t*>(p.data()), p.size()}, 0, 97);
  }
  const fs::path pack = work / "cli_out" / dev_id;
  auto watched_pack = [&](const std::string& args, int want_rc, const fs::path& dir, std::string* output = nullptr) {
    const Snap before = snapshot();
    std::string text;
    const int rc = run_cmd(env + exe + " pack " + args, &text);
    CHECK(rc == want_rc);
    if (output) *output = text;
    if (rc != 0) return;
    const auto ch = changed_since(before);
    std::size_t outside = 0;
    for (const std::string& f : ch)
      if (f.rfind(dir.string(), 0) != 0 && f.find("vendor-ids") == std::string::npos && f.find("cli-vendor-ids") == std::string::npos) ++outside;
    CHECK(outside == 0);  // nothing else anywhere: no temp file, no staging copy, no log (TMPDIR is under the scratch directory too)
    Key32 k{};
    const Keys& kk = args.find("cli_keys_ft") != std::string::npos ? ft : dev;
    CHECK(vault_entry_open(kk.kek, read_all(dir / "content-key.vault"), dir.filename().string(), k));
    needles.key = k;
    scan_files(ch, needles);
    CHECK(text.find(hex(k)) == std::string::npos);  // stdout and stderr of the run never carry the key
  };
  watched_pack("--description " + dev_desc + o + devkeys + regarg + " --device-pub '" + (work / "cli_keys_dev/device.x25519.pub").string() + "'", 0, pack, &out);
  CHECK(out.find("packed library " + dev_id) != std::string::npos);
  CHECK(fs::exists(pack / "bundle.vlrp"));
  // the same again needs --overwrite (output_failed 13)
  CHECK(run_cmd(env + exe + " pack --description " + dev_desc + o + devkeys + regarg, &out) == 13);
  // a rebuild with another key keeps the old directory (it holds the only copy of the old key); with the vaulted key it replaces it
  const Bytes first_vault = read_all(pack / "content-key.vault");
  CHECK(run_cmd(env + exe + " pack --description " + dev_desc + o + devkeys + regarg + " --overwrite", &out) == 0);
  std::size_t olds = 0;
  for (const auto& e : fs::directory_iterator(work / "cli_out"))
    if (e.path().filename().string().rfind(".old-" + dev_id, 0) == 0) {
      ++olds;
      CHECK(read_all(e.path() / "content-key.vault") == first_vault);
    }
  CHECK(olds == 1);
  CHECK(run_cmd(env + exe + " pack --description " + dev_desc + o + devkeys + regarg + " --overwrite --reuse-vault-entry '" + (pack / "content-key.vault").string() + "'", &out) == 0);
  olds = 0;
  for (const auto& e : fs::directory_iterator(work / "cli_out"))
    if (e.path().filename().string().rfind(".old-" + dev_id, 0) == 0) ++olds;
  CHECK(olds == 1);  // the rebuild with the same key left no further copy

  // reproducibility through the CLI: reuse the vaulted key, deterministic vault => every file identical except the wrapped keys; also from a second copy of the source
  {
    const fs::path copy = work / "cli_src_copy";
    fs::create_directories(copy);
    fs::copy(src_a, copy, fs::copy_options::recursive);
    put(work / "dev_copy.json", desc_json(dev_id, "Dev Pack", (copy / "pack.sfz").string(), "dev"));
    const std::string reuse = " --reuse-vault-entry '" + (pack / "content-key.vault").string() + "' --deterministic-vault --device-pub '" + (work / "cli_keys_dev/device.x25519.pub").string() + "'";
    fs::create_directories(work / "repro_a");
    fs::create_directories(work / "repro_b");
    CHECK(run_cmd(env + exe + " pack --description " + dev_desc + " --out '" + (work / "repro_a").string() + "'" + devkeys + regarg + reuse, &out) == 0);
    CHECK(run_cmd(env + exe + " pack --description '" + (work / "dev_copy.json").string() + "' --out '" + (work / "repro_b").string() + "'" + devkeys + regarg + reuse, &out) == 0);
    std::size_t same = 0;
    for (const char* f : {"bundle.vlrp", "content-key.vault", "catalog.meta", "catalog.meta.sig", "pack.json"}) {
      CHECK(read_all(work / "repro_a" / dev_id / f) == read_all(work / "repro_b" / dev_id / f));
      ++same;
    }
    CHECK(same == 5 && read_all(work / "repro_a" / dev_id / "bundle.vlrp") == read_all(pack / "bundle.vlrp"));
    // the wrapped keys differ (fresh ephemeral key and nonce per wrap) and both unwrap to the same key
    const std::string wname = "wrapped-key." + hex(fmt::key_id_of_public(dev.dev_pub)) + ".bin";
    CHECK(read_all(work / "repro_a" / dev_id / wname) != read_all(work / "repro_b" / dev_id / wname));
    Key32 ka{}, kb{};
    CHECK(wrapped_key_open(dev.dev_priv, read_all(work / "repro_a" / dev_id / wname), dev_id, ka) && wrapped_key_open(dev.dev_priv, read_all(work / "repro_b" / dev_id / wname), dev_id, kb) && ka == kb);
  }

  // verify
  const std::string vparts = " --bundle '" + (pack / "bundle.vlrp").string() + "' --vault-entry '" + (pack / "content-key.vault").string() + "' --kek '" +
                             (work / "cli_keys_dev/kek.aes256").string() + "' --trusted-pub '";
  put_bytes(work / "dev.pub", dev.sign_pub());
  CHECK(run_cmd(exe + " verify" + vparts + (work / "dev.pub").string() + "'", &out) == 0);
  CHECK(out.find("ok: library " + dev_id) != std::string::npos);
  put_bytes(work / "other.pub", ft.sign_pub());
  CHECK(run_cmd(exe + " verify" + vparts + (work / "other.pub").string() + "'") == 14);  // untrusted signer
  CHECK(run_cmd(exe + " verify --bundle '" + (pack / "bundle.vlrp").string() + "' --vault-entry '" + (pack / "content-key.vault").string() + "' --kek '" +
                (work / "cli_keys_ft/kek.aes256").string() + "' --trusted-pub '" + (work / "dev.pub").string() + "'") == 14);  // wrong KEK
  CHECK(run_cmd(exe + " verify --bundle '" + (work / "missing.vlrp").string() + "' --vault-entry '" + (pack / "content-key.vault").string() + "' --kek '" +
                (work / "cli_keys_dev/kek.aes256").string() + "' --trusted-pub '" + (work / "dev.pub").string() + "'") == 14);  // missing bundle
  CHECK(run_cmd(exe + " verify --bundle '" + (pack / "bundle.vlrp").string() + "' --vault-entry '" + (work / "missing.vault").string() + "' --kek '" +
                (work / "cli_keys_dev/kek.aes256").string() + "' --trusted-pub '" + (work / "dev.pub").string() + "'") == 4);  // missing vault entry (a key file error)
  {
    Bytes b = read_all(pack / "bundle.vlrp");
    b[b.size() - 3] ^= 0x10;  // a flipped bit in the last block
    put_bytes(work / "tampered.vlrp", b);
    CHECK(run_cmd(exe + " verify --bundle '" + (work / "tampered.vlrp").string() + "' --vault-entry '" + (pack / "content-key.vault").string() + "' --kek '" +
                  (work / "cli_keys_dev/kek.aes256").string() + "' --trusted-pub '" + (work / "dev.pub").string() + "'") == 14);
  }

  // ---- the root guard -------------------------------------------------------------------------------------------------------------------------------------
  const std::string ft_dev_id = desc("ft_dev_id.json", "00000000-0000-4000-8000-000000000008", "Dev Range In FT", "factory-test");
  CHECK(run_cmd(env + exe + " pack --description " + ft_dev_id + o + ftkeys + regarg, &out) == 7);  // the dev-range id is refused for every other root
  const std::string mism = desc("mism.json", "bbbbbbbb-2222-4333-8444-555555555555", "Mismatch", "factory-test");
  CHECK(run_cmd(env + exe + " pack --description " + mism + o + devkeys + regarg, &out) == 5);  // sidecars say dev
  CHECK(out.find("root_mismatch") != std::string::npos);
  {
    auto expect = [&](const std::string& keys_args, int rc, const char* why) {
      const int got = run_cmd(env + exe + " pack --description " + mism + o + keys_args + regarg + no_dev_list, &out);
      if (got != rc) std::printf("  (%s: got %d want %d: %s)\n", why, got, rc, out.c_str());
      CHECK(got == rc);
    };
    // a good factory-test key set packs (registered below), so the refusals are about the guard and nothing else
    // 1. a bare key without sidecars under a non-dev root
    make_keys(work / "g_bare", "factory-test");
    fs::remove(work / "g_bare/content-manifest.json");
    fs::remove(work / "g_bare/kek.json");
    expect(" --keys-dir '" + (work / "g_bare").string() + "'", 5, "bare key, no sidecar");
    // one sidecar missing
    make_keys(work / "g_halfbare", "factory-test");
    fs::remove(work / "g_halfbare/kek.json");
    expect(" --keys-dir '" + (work / "g_halfbare").string() + "'", 5, "KEK without sidecar");
    // 2. mislabelled key_id
    const Keys mis = make_keys(work / "g_mis", "factory-test");
    write_sidecars(work / "g_mis", mis, "factory-test", false, "0011223344556677");
    expect(" --keys-dir '" + (work / "g_mis").string() + "'", 5, "wrong signing key_id");
    write_sidecars(work / "g_mis", mis, "factory-test", false, "", "0011223344556677");
    expect(" --keys-dir '" + (work / "g_mis").string() + "'", 5, "wrong KEK key_id");
    put(work / "g_mis/kek.json", "{\"root\": \"factory-test\", \"dev_only\": false}\n");  // no key_id at all
    expect(" --keys-dir '" + (work / "g_mis").string() + "'", 5, "sidecar without key_id");
    // 3. mixed directories: the signing key from a factory-test directory, the KEK from the dev directory (and the reverse)
    expect(" --signing-key '" + (work / "cli_keys_ft/content-manifest.ed25519.seed").string() + "' --kek '" + (work / "cli_keys_dev/kek.aes256").string() + "'", 5, "dev KEK");
    expect(" --signing-key '" + (work / "cli_keys_dev/content-manifest.ed25519.seed").string() + "' --kek '" + (work / "cli_keys_ft/kek.aes256").string() + "'", 5, "dev signing key");
    // 4. the DEV-ONLY marker
    make_keys(work / "g_marker", "factory-test");
    put(work / "g_marker/DEV-ONLY.txt", "DEV-ONLY\n");
    expect(" --keys-dir '" + (work / "g_marker").string() + "'", 5, "DEV-ONLY.txt");
    // 5. sidecar with a missing / non-string root
    const Keys nr = make_keys(work / "g_noroot", "factory-test");
    put(work / "g_noroot/content-manifest.json", "{\"key_id\": \"" + nr.sign_id() + "\"}\n");
    expect(" --keys-dir '" + (work / "g_noroot").string() + "'", 4, "no root");
    put(work / "g_noroot/content-manifest.json", "{\"root\": 3, \"key_id\": \"" + nr.sign_id() + "\"}\n");
    expect(" --keys-dir '" + (work / "g_noroot").string() + "'", 4, "root not a string");
    put(work / "g_noroot/content-manifest.json", "not json");
    expect(" --keys-dir '" + (work / "g_noroot").string() + "'", 4, "sidecar not JSON");
    // 6. dev_only true under a non-dev root
    make_keys(work / "g_devonly", "factory-test", /*dev_only=*/true);
    expect(" --keys-dir '" + (work / "g_devonly").string() + "'", 5, "dev_only");
    // 7. a key of the dev root relabelled as factory-test: its id is on the dev trust list / in the developer's dev key directory
    const Keys relabel = make_keys(work / "g_relabel", "factory-test");
    put(work / "dev.pub-list", "# fixture\ndev content-manifest " + relabel.sign_id() + " " + hex(relabel.sign_pub()) + "\n");
    {
      const int got = run_cmd(env + exe + " pack --description " + mism + o + " --keys-dir '" + (work / "g_relabel").string() + "'" + regarg + " --dev-trust-list '" + (work / "dev.pub-list").string() + "'", &out);
      CHECK(got == 5 && out.find("dev-root key") != std::string::npos);
    }
    fs::create_directories(work / "xdg/velora-dev-keys");
    put_bytes(work / "xdg/velora-dev-keys/kek.aes256", relabel.kek, 0600);
    {
      const int got = run_cmd("XDG_DATA_HOME='" + (work / "xdg").string() + "' " + env + exe + " pack --description " + mism + o + " --keys-dir '" + (work / "g_relabel").string() + "'" + regarg + no_dev_list, &out);
      CHECK(got == 5 && out.find("dev-root KEK") != std::string::npos);
    }
    // the same key set is fine when nothing on the dev lists matches (it gets as far as the register: unregistered, 7)
    CHECK(run_cmd(env + exe + " pack --description " + mism + o + " --keys-dir '" + (work / "g_relabel").string() + "'" + regarg + no_dev_list, &out) == 7);
  }

  // ---- key resolution -------------------------------------------------------------------------------------------------------------------------------------
  {  // --signing-key / --kek override a --keys-dir file; the default dev key directory is used for root dev when nothing is given
    const std::string id = "00000000-0000-4000-8000-000000000009";
    const std::string d9 = desc("d9.json", id, "Key Resolution", "dev");
    const Keys other = make_keys(work / "cli_keys_dev2", "dev", true);
    fs::create_directories(work / "res_out");
    CHECK(run_cmd(env + exe + " pack --description " + d9 + " --out '" + (work / "res_out").string() + "'" + devkeys + " --signing-key '" +
                  (work / "cli_keys_dev2/content-manifest.ed25519.seed").string() + "'" + regarg, &out) == 0);
    auto pubinfo = read_text(work / "res_out" / id / "pack.json");
    CHECK(pubinfo.find(other.sign_id()) != std::string::npos && pubinfo.find(dev.kek_id_hex()) != std::string::npos);  // other's signer, dev's KEK
    CHECK(run_cmd(env + exe + " pack --description " + d9 + " --out '" + (work / "res_out").string() + "'" + devkeys + " --kek '" +
                  (work / "cli_keys_dev2/kek.aes256").string() + "'" + regarg + " --overwrite", &out) == 0);
    pubinfo = read_text(work / "res_out" / id / "pack.json");
    CHECK(pubinfo.find(dev.sign_id()) != std::string::npos && pubinfo.find(other.kek_id_hex()) != std::string::npos);
    // default dev dir: $XDG_DATA_HOME/velora-dev-keys
    fs::create_directories(work / "xdg2");
    fs::copy(work / "cli_keys_dev2", work / "xdg2/velora-dev-keys", fs::copy_options::recursive);
    CHECK(run_cmd("XDG_DATA_HOME='" + (work / "xdg2").string() + "' " + env + exe + " pack --description " + d9 + " --out '" + (work / "res_out").string() + "'" + regarg + " --overwrite", &out) == 0);
    pubinfo = read_text(work / "res_out" / id / "pack.json");
    CHECK(pubinfo.find(other.sign_id()) != std::string::npos && pubinfo.find(other.kek_id_hex()) != std::string::npos);
    // no directory and no files for a non-dev root
    const std::string f1 = desc("f1.json", "cccccccc-2222-4333-8444-555555555555", "Fresh Library", "factory-test");
    CHECK(run_cmd("XDG_DATA_HOME='" + (work / "xdg2").string() + "' " + env + exe + " pack --description " + f1 + o + regarg, &out) == 4);
  }

  // ---- the vendor ID register -----------------------------------------------------------------------------------------------------------------------------
  const std::string conflict = desc("conflict.json", "aaaaaaaa-2222-4333-8444-555555555555", "Another Library", "factory-test");
  CHECK(run_cmd(env + exe + " pack --description " + conflict + o + ftkeys + regarg + no_dev_list, &out) == 6);
  CHECK(out.find("library_id_conflict") != std::string::npos && out.find("Taken Name") != std::string::npos);
  CHECK(!fs::exists(work / "cli_out/aaaaaaaa-2222-4333-8444-555555555555"));
  const std::string taken_case = desc("taken_case.json", "aaaaaaaa-2222-4333-8444-555555555555", "  TAKEN   name ", "factory-test");  // the registered library under a spelling variant: allowed
  CHECK(run_cmd(env + exe + " pack --description " + taken_case + o + ftkeys + regarg + no_dev_list, &out) == 0);
  const std::string fresh = desc("fresh.json", "cccccccc-2222-4333-8444-555555555555", "Fresh Library", "factory-test");
  CHECK(run_cmd(env + exe + " pack --description " + fresh + o + ftkeys + regarg + no_dev_list, &out) == 7);
  CHECK(!fs::exists(work / "cli_out/cccccccc-2222-4333-8444-555555555555") && read_text(reg).find("cccccccc") == std::string::npos);
  CHECK(run_cmd(env + exe + " pack --description " + fresh + o + ftkeys + regarg + no_dev_list + " --register", &out) == 2);  // --register needs --owner
  watched_pack("--description " + fresh + o + ftkeys + regarg + no_dev_list + " --register --owner Founder", 0, work / "cli_out/cccccccc-2222-4333-8444-555555555555");
  CHECK(read_text(reg).find("cccccccc-2222-4333-8444-555555555555\tFresh Library\tFounder\t") != std::string::npos);
  CHECK(run_cmd(env + exe + " pack --description " + fresh + o + ftkeys + regarg + no_dev_list + " --overwrite", &out) == 0);  // registered now
  CHECK(run_cmd(exe + " check-register" + regarg, &out) == 0);
  // the explicit register step; the dev range is never registered (6); a bad owner or date is a register_io error (8)
  const std::string other_prod = desc("other.json", "dddddddd-2222-4333-8444-555555555555", "Other Library", "production");
  CHECK(run_cmd(exe + " register --description " + other_prod + " --owner Founder" + regarg, &out) == 0);
  CHECK(run_cmd(exe + " register --description " + conflict + " --owner Founder" + regarg, &out) == 6);
  CHECK(run_cmd(exe + " register --description " + dev_desc + " --owner Founder" + regarg, &out) == 6);
  CHECK(run_cmd(exe + " register --description " + desc("r1.json", "12121212-2222-4333-8444-555555555555", "Owner Check", "production") + " --owner 'bad\ttab'" + regarg, &out) == 8);
  CHECK(run_cmd(exe + " register --description " + desc("r1.json", "12121212-2222-4333-8444-555555555555", "Owner Check", "production") + " --owner Founder --date 2026-13-45" + regarg, &out) == 8);
  CHECK(read_text(reg).find("12121212") == std::string::npos);
  // register problems for a non-dev id: malformed and missing registers are register_io (8)
  {
    const std::string bad_reg = " --register-file '" + (work / "bad-vendor-ids").string() + "'";
    put(work / "bad-vendor-ids", "this is not a register\n");
    CHECK(run_cmd(env + exe + " pack --description " + fresh + o + ftkeys + bad_reg + no_dev_list + " --overwrite", &out) == 8);
    CHECK(run_cmd(env + exe + " pack --description " + fresh + o + ftkeys + " --register-file '" + (work / "absent-vendor-ids").string() + "'" + no_dev_list + " --overwrite", &out) == 8);
    CHECK(run_cmd(env + exe + " check-register" + bad_reg, &out) == 8);
  }
  // a failed pack does not take the id (skipped source, 9), nor does an unknown instrument key (10, SFZ)
  put(src_a / "unsup.sfz", "<region> sample=samples/a.wav key=60 amp_veltrack=50\n");
  put(work / "unsup.json", desc_json("eeeeeeee-2222-4333-8444-555555555555", "Unsupported", (src_a / "unsup.sfz").string(), "factory-test"));
  CHECK(run_cmd(env + exe + " pack --description '" + (work / "unsup.json").string() + "'" + o + ftkeys + regarg + no_dev_list + " --register --owner Founder", &out) == 9);
  CHECK(out.find("import_skipped") != std::string::npos && read_text(reg).find("eeeeeeee") == std::string::npos);
  fs::remove(src_a / "unsup.sfz");
  const std::string badkey = desc("badkey.json", "ffffffff-2222-4333-8444-555555555555", "Bad Instrument Key", "factory-test", "\"instruments\": [{\"key\": \"nope.sfz\", \"category\": \"strings\"}]");
  CHECK(run_cmd(env + exe + " pack --description " + badkey + o + ftkeys + regarg + no_dev_list + " --register --owner Founder", &out) == 10);
  CHECK(out.find("unknown_instrument_key") != std::string::npos && read_text(reg).find("ffffffff") == std::string::npos);
  CHECK(!fs::exists(work / "cli_out/ffffffff-2222-4333-8444-555555555555"));

  // a failed register update withdraws the pack: the register's temp name is taken by a directory, so the append fails (8) and no pack directory is left
  {
    const fs::path rr = work / "rollback-ids";
    put(rr, "# empty\n");
    fs::create_directories(work / "rollback-ids.tmp");
    const std::string rid = "13131313-2222-4333-8444-555555555555";
    const std::string dd = desc("rb.json", rid, "Rollback Library", "factory-test");
    fs::create_directories(work / "rb_out");
    CHECK(run_cmd(env + exe + " pack --description " + dd + " --out '" + (work / "rb_out").string() + "'" + ftkeys + " --register-file '" + rr.string() + "'" + no_dev_list + " --register --owner Founder", &out) == 8);
    CHECK(out.find("withdrawn") != std::string::npos && fs::is_empty(work / "rb_out") && read_text(rr).find(rid) == std::string::npos);
  }
  // two processes race for one id under two different names: one wins, the other is refused (6) and leaves no published pack
  {
    const fs::path rr = work / "race-ids";
    put(rr, "# empty\n");
    const std::string rid = "14141414-2222-4333-8444-555555555555";
    const std::string da = desc("race_a.json", rid, "Race Library A", "factory-test");
    const std::string db = desc("race_b.json", rid, "Race Library B", "factory-test");
    fs::create_directories(work / "race_a");
    fs::create_directories(work / "race_b");
    const std::string base = env + exe + " pack --out ";
    const std::string common = ftkeys + " --register-file '" + rr.string() + "'" + no_dev_list + " --register --owner Founder";
    const std::string script = "(" + base + "'" + (work / "race_a").string() + "' --description " + da + common + " >/dev/null 2>&1; echo $? > '" + (work / "race_a.rc").string() + "') & (" + base + "'" +
                               (work / "race_b").string() + "' --description " + db + common + " >/dev/null 2>&1; echo $? > '" + (work / "race_b.rc").string() + "') & wait";
    CHECK(run_cmd("/bin/sh -c \"" + [&] { std::string e; for (char c : script) { if (c == '"' || c == '\\' || c == '$') e += '\\'; e += c; } return e; }() + "\"") == 0);
    const int ra = std::atoi(read_text(work / "race_a.rc").c_str()), rb = std::atoi(read_text(work / "race_b.rc").c_str());
    CHECK((ra == 0 && rb == 6) || (ra == 6 && rb == 0));
    const bool a_won = ra == 0;
    CHECK(fs::exists(work / (a_won ? "race_a" : "race_b") / rid / "bundle.vlrp"));
    CHECK(fs::is_empty(work / (a_won ? "race_b" : "race_a")));  // the loser published nothing
    VendorRegister rg;
    CHECK(!load_register(rr.string(), rg) && rg.entries.size() == 1 && rg.entries[0].name == (a_won ? "Race Library A" : "Race Library B"));
  }

  // key files: too-open permissions, wrong size, missing
  {
    const fs::path k = work / "cli_keys_open";
    make_keys(k, "dev");
    ::chmod((k / "kek.aes256").c_str(), 0644);
    CHECK(run_cmd(env + exe + " pack --description " + dev_desc + o + " --keys-dir '" + k.string() + "'" + regarg + " --overwrite", &out) == 4);
    CHECK(out.find("permissions") != std::string::npos);
    ::chmod((k / "kek.aes256").c_str(), 0600);
    put(k / "kek.aes256", "short", 0600);
    CHECK(run_cmd(env + exe + " pack --description " + dev_desc + o + " --keys-dir '" + k.string() + "'" + regarg + " --overwrite") == 4);
    CHECK(run_cmd(env + exe + " pack --description " + dev_desc + o + " --keys-dir '" + (work / "nonexistent").string() + "'" + regarg + " --overwrite") == 4);
  }
  // reusing a vault entry: wrong KEK or another library is vault_failed (12)
  {
    const std::string vault = " --reuse-vault-entry '" + (pack / "content-key.vault").string() + "'";
    CHECK(run_cmd(env + exe + " pack --description " + dev_desc + o + " --keys-dir '" + (work / "cli_keys_dev2").string() + "'" + regarg + " --overwrite" + vault, &out) == 12);
    CHECK(run_cmd(env + exe + " pack --description " + desc("d10.json", "00000000-0000-4000-8000-00000000000a", "Other Dev Pack", "dev") + o + devkeys + regarg + vault, &out) == 12);
    CHECK(!fs::exists(work / "cli_out/00000000-0000-4000-8000-00000000000a"));
  }
  // --content-key-file: dev root only, never an all-zero key, 0600
  {
    put_bytes(work / "ck.bin", seq<32>(9), 0600);
    const std::string ck = " --content-key-file '" + (work / "ck.bin").string() + "'";
    CHECK(run_cmd(env + exe + " pack --description " + fresh + o + ftkeys + regarg + no_dev_list + " --overwrite" + ck, &out) == 2);  // factory-test: refused
    make_keys(work / "cli_keys_prod", "production");
    CHECK(run_cmd(env + exe + " pack --description " + other_prod + o + " --keys-dir '" + (work / "cli_keys_prod").string() + "'" + regarg + no_dev_list + ck, &out) == 2);  // production: refused
    const std::string id = "00000000-0000-4000-8000-00000000000b";
    fs::create_directories(work / "ck_out");
    CHECK(run_cmd(env + exe + " pack --description " + desc("d11.json", id, "Content Key File", "dev") + " --out '" + (work / "ck_out").string() + "'" + devkeys + regarg + ck, &out) == 0);
    Key32 got{};
    CHECK(vault_entry_open(dev.kek, read_all(work / "ck_out" / id / "content-key.vault"), id, got) && got == seq<32>(9));
    put_bytes(work / "zero.bin", Key32{}, 0600);
    CHECK(run_cmd(env + exe + " pack --description " + desc("d12.json", "00000000-0000-4000-8000-00000000000c", "Zero Key", "dev") + " --out '" + (work / "ck_out").string() + "'" + devkeys + regarg +
                  " --content-key-file '" + (work / "zero.bin").string() + "'", &out) == 4);
    CHECK(!fs::exists(work / "ck_out/00000000-0000-4000-8000-00000000000c"));
  }
  // the TMPDIR the CLI ran with stayed empty
  CHECK(files_under(work / "tmp").empty());
  {
    Key32 k{};
    CHECK(vault_entry_open(dev.kek, read_all(pack / "content-key.vault"), dev_id, k));
    Key32 w{};
    for (const fs::path& p : files_under(pack))
      if (p.filename().string().rfind("wrapped-key.", 0) == 0) CHECK(wrapped_key_open(dev.dev_priv, read_all(p), dev_id, w) && w == k);
  }
}

}  // namespace

int main() {
  g_work = fs::path(CP_TEST_DIR) / ("work." + std::to_string(::getpid()));
  const fs::path work = g_work;
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work / "tmp");
  // Everything this process (and the CLI it runs with the same TMPDIR) creates in a temp location lands under the scratch directory, where the write watch sees it.
  ::setenv("TMPDIR", (work / "tmp").c_str(), 1);
  test_vectors();
  test_description();
  test_register(work);
  const Source source = make_library(work / "src_a");
  test_importer_options(work, work / "src_a");
  test_importer_sf2(work);
  test_pack(work, work / "src_a", source);
  test_sf2(work);
  test_cli(work, work / "src_a", source);
  if (g_fail == 0) fs::remove_all(work, ec);
  std::printf(g_fail ? "content_pack_test: %d FAILED\n" : "content_pack_test: ok\n", g_fail);
  return g_fail ? 1 : 0;
}
