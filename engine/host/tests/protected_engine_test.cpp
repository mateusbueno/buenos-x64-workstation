// Story 6.5: the engine plays PROTECTED (factory) libraries. Dev key provider (wrapped_key_v1 against the pinned vectors and a tamper matrix), key memory
// (locked, dump-excluded, wiped), the resolver's verification of bundle.vlrp, the ProtectedHeadProvider (typed refusals, bit-identical residency), the key attached to
// the generation and wiped when the generation is freed, damaged blocks found by an I/O edge thread (voices fade to silence, content_integrity counted, library
// reported in State.content), and "no plaintext block or key reaches a disk".
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "live_set_fixtures.hpp"
#include "protected_fixtures.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/dev_key_provider.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/protected_head_provider.hpp"
#include "velora/host/secure_memory.hpp"
#include "velora/host/trust_list.hpp"

namespace fs = std::filesystem;
using namespace velora;
namespace ls = host::liveset;
namespace lsp = ls::lsp;
using fixtures::uuid_n;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)
#define CHECK_NEAR(a, b, tol)                                                                                       \
  do {                                                                                                              \
    const double va_ = (a), vb_ = (b);                                                                              \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                                                         \
      std::printf("FAIL %s:%d: %s = %.9g, expected %.9g (+-%g)\n", __FILE__, __LINE__, #a, va_, vb_, (double)(tol)); \
      ++g_fail;                                                                                                     \
    }                                                                                                               \
  } while (0)

namespace {

using pfx::Bytes;
using pfx::Key32;
using pfx::TmpDir;
using ports_status = ports::KeyStatus;

Bytes unhex(const std::string& h) {
  Bytes b;
  for (std::size_t i = 0; i + 1 < h.size(); i += 2) b.push_back(static_cast<std::uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
  return b;
}
constexpr const char* kUuid = "3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f";
Key32 seq32(int first) {
  Key32 k{};
  for (int i = 0; i < 32; ++i) k[i] = static_cast<std::uint8_t>(first + i);
  return k;
}

// Wipe events of the content keys a provider hands out.
struct WipeLog {
  std::mutex mu;
  std::vector<host::SecureKeyHandle::WipeEvent> events;
  std::vector<bool> all_zero;
  int count() {
    std::lock_guard<std::mutex> l(mu);
    return static_cast<int>(events.size());
  }
  host::SecureKeyHandle::WipeObserver observer() {
    return [this](const host::SecureKeyHandle::WipeEvent& e) {
      bool zero = true;
      for (std::size_t i = 0; i < e.size; ++i) zero = zero && e.memory[i] == 0;  // looks at the very memory the key lived in, after the wipe
      std::lock_guard<std::mutex> l(mu);
      events.push_back(e);
      all_zero.push_back(zero);
    };
  }
};

std::unique_ptr<host::DevKeyProvider> make_provider(const pfx::Keys& keys, WipeLog* log = nullptr) {
  host::DevKeyProvider::Options o;
  if (log) o.key_memory.on_wipe = log->observer();
  std::string err;
  auto p = host::DevKeyProvider::from_bytes(keys.device_priv, o, err);
  if (!p) std::printf("provider: %s\n", err.c_str());
  return p;
}

// ---- the dev key provider ---------------------------------------------------------------------------------------------------------------------------

void test_pinned_vectors() {
  // KEY-HIERARCHY.md section 6: device private = 0x01.., ephemeral private = 0x21.., nonce = 0xa0.., content key = 0xc0...
  Key32 dev = seq32(0x01), eph = seq32(0x21), content = seq32(0xc0);
  std::array<std::uint8_t, 12> nonce{};
  for (int i = 0; i < 12; ++i) nonce[i] = static_cast<std::uint8_t>(0xa0 + i);
  const Key32 pub = pfx::x25519_public(dev);
  CHECK(pub == Key32{[&] {
    Key32 k{};
    const Bytes b = unhex("07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c");
    std::copy(b.begin(), b.end(), k.begin());
    return k;
  }()});
  const Bytes v1 = unhex(
      "56574b3101aaa8fff703b50b225869aff450549732cbaaed5e5df9b30a6da31cb0e5742bad5ad4a1a768f1a67ba0a1a2a3a4a5a6a7a8a9aaab9eb9ebbe5dd3cc7205c0854c53b12f3a98da5488b008bb9b068e9230c82bb4809e6004e870a73f77e35234a13d20ce3a");
  const Bytes v2 = unhex(
      "56574b3101aaa8fff703b50b225869aff450549732cbaaed5e5df9b30a6da31cb0e5742bad5ad4a1a768f1a67ba0a1a2a3a4a5a6a7a8a9aaabb27952d3c6f1222c0cfd438d34d043034a13df431b860479b07ba77078486cc141bd3487091063b292da3d3b6643ee5d");
  // The vendor-side wrap of the fixtures (written independently of the engine) reproduces both pinned blobs byte for byte.
  CHECK(pfx::wrap_content_key(content, "velora.test.library", pub, eph, nonce) == v1);
  CHECK(pfx::wrap_content_key(content, "3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f", pub, eph, nonce) == v2);

  WipeLog log;
  host::DevKeyProvider::Options o;
  o.key_memory.on_wipe = log.observer();
  o.require_uuid_library_id = false;  // the first pinned vector binds the free-form id "velora.test.library"
  std::string err;
  auto p = host::DevKeyProvider::from_bytes(dev, o, err);
  CHECK(p != nullptr);
  if (!p) return;
  CHECK(p->device_public() == pub);
  {  // the default provider takes only the canonical lower-case UUID text of a library
    auto strict = host::DevKeyProvider::from_bytes(dev, {}, err);
    CHECK(strict->unwrap_content_key("velora.test.library", v1).status == ports_status::kInvalidArgument);
    CHECK(strict->unwrap_content_key("3F1D6A52-9B7E-4C1A-8D2E-5A6B7C8D9E0F", v2).status == ports_status::kInvalidArgument);  // upper case: one spelling per library
    CHECK(strict->unwrap_content_key("3f1d6a529b7e4c1a8d2e5a6b7c8d9e0f", v2).status == ports_status::kInvalidArgument);
    ports::KeyResult ok = strict->unwrap_content_key("3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f", v2);
    CHECK(ok.ok() && std::equal(ok.key->bytes().begin(), ok.key->bytes().end(), content.begin()));
  }
  {
    ports::KeyResult r = p->unwrap_content_key("velora.test.library", v1);
    CHECK(r.ok() && r.status == ports_status::kOk);
    if (r.ok()) {
      CHECK(std::equal(r.key->bytes().begin(), r.key->bytes().end(), content.begin()));
      const auto* h = dynamic_cast<const host::SecureKeyHandle*>(r.key.get());
      CHECK(h && h->locked());  // the key lives in locked memory
#if defined(MADV_DONTDUMP)
      CHECK(h && h->dump_excluded());  // ... and is excluded from core dumps where the OS can do it per region
#endif
    }
  }
  CHECK(log.count() == 1 && log.all_zero[0] && log.events[0].locked);  // released -> wiped, observed in the key's own memory
  {
    ports::KeyResult r = p->unwrap_content_key("3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f", v2);
    CHECK(r.ok() && std::equal(r.key->bytes().begin(), r.key->bytes().end(), content.begin()));
  }
  CHECK(log.count() == 2);
  // The blob is bound to the library: another id, an extra byte, an empty id.
  CHECK(p->unwrap_content_key("velora.test.librarY", v1).status == ports_status::kUnwrapFailed);
  CHECK(p->unwrap_content_key("3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f", v1).status == ports_status::kUnwrapFailed);
  CHECK(p->unwrap_content_key("", v1).status == ports_status::kInvalidArgument);
  CHECK(p->unwrap_content_key("velora.test.library", {}).status == ports_status::kMalformedBlob);
}

void test_unwrap_tamper_matrix() {
  const pfx::Keys keys = pfx::make_keys(3);
  const std::string lib = "3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f";
  WipeLog log;
  auto p = make_provider(keys, &log);
  if (!p) return;
  Key32 eph = seq32(0x30);
  std::array<std::uint8_t, 12> nonce{};
  nonce.fill(0x55);
  const Bytes blob = pfx::wrap_content_key(keys.content, lib, keys.device_pub, eph, nonce);
  CHECK(p->unwrap_content_key(lib, blob).ok());
  // Every single bit of the 105 bytes: refused, never a key. The class of refusal follows the field.
  int refused = 0, wrong_class = 0;
  for (std::size_t i = 0; i < blob.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes b = blob;
      b[i] = static_cast<std::uint8_t>(b[i] ^ (1u << bit));
      const ports::KeyResult r = p->unwrap_content_key(lib, b);
      if (r.ok()) {
        std::printf("FAIL: a flipped bit (byte %zu bit %d) unwrapped\n", i, bit);
        ++g_fail;
        continue;
      }
      ++refused;
      const ports_status want = i < 5 ? ports_status::kMalformedBlob : i < 13 ? ports_status::kWrongDevice : ports_status::kUnwrapFailed;
      if (r.status != want) ++wrong_class;
      CHECK(r.key == nullptr);
    }
  CHECK(refused == 105 * 8);
  CHECK(wrong_class == 0);
  // Sizes.
  for (std::size_t n : {std::size_t(0), std::size_t(1), std::size_t(104)}) CHECK(p->unwrap_content_key(lib, std::span<const std::uint8_t>(blob).first(n)).status == ports_status::kMalformedBlob);
  Bytes longer = blob;
  longer.push_back(0);
  CHECK(p->unwrap_content_key(lib, longer).status == ports_status::kMalformedBlob);
  // Another device: its key id is not the recipient's.
  const pfx::Keys other = pfx::make_keys(9);
  auto q = make_provider(other);
  CHECK(q->unwrap_content_key(lib, blob).status == ports_status::kWrongDevice);
  // A blob made for `other` but carrying our recipient id still does not open (the shared secret differs): a forged recipient field gains nothing.
  Bytes forged = pfx::wrap_content_key(keys.content, lib, other.device_pub, eph, nonce);
  std::copy(blob.begin() + 5, blob.begin() + 13, forged.begin() + 5);
  CHECK(p->unwrap_content_key(lib, forged).status == ports_status::kUnwrapFailed);
  // A low-order ephemeral key (all-zero public key) gives an all-zero shared secret: refused.
  Bytes low = blob;
  std::fill(low.begin() + 13, low.begin() + 45, 0);
  CHECK(p->unwrap_content_key(lib, low).status == ports_status::kUnwrapFailed);
  // Every refusal left no key behind: each key page that was allocated is wiped (the observer saw only zeros) and none is alive.
  {
    std::lock_guard<std::mutex> l(log.mu);
    bool zero = true;
    for (bool z : log.all_zero) zero = zero && z;
    CHECK(zero);
  }
}

void test_key_files_and_lock_failure() {
  TmpDir tmp;
  const pfx::Keys keys = pfx::make_keys(4);
  const fs::path f = fs::path(tmp.path) / "device.x25519.priv";
  pfx::write_file(f, Bytes(keys.device_priv.begin(), keys.device_priv.end()));
  std::string err;
  auto p = host::DevKeyProvider::from_file(f.string(), {}, err);
  CHECK(p && p->device_public() == keys.device_pub);
  // Not 32 bytes, missing, a directory.
  pfx::write_file(fs::path(tmp.path) / "short", Bytes(31, 1));
  pfx::write_file(fs::path(tmp.path) / "long", Bytes(33, 1));
  CHECK(!host::DevKeyProvider::from_file((fs::path(tmp.path) / "short").string(), {}, err) && !err.empty());
  CHECK(!host::DevKeyProvider::from_file((fs::path(tmp.path) / "long").string(), {}, err));
  CHECK(!host::DevKeyProvider::from_file((fs::path(tmp.path) / "nope").string(), {}, err));
  CHECK(!host::DevKeyProvider::from_file(tmp.path, {}, err));
  // The device key is wiped when the provider goes.
  WipeLog dlog;
  {
    host::DevKeyProvider::Options o;
    o.device_key_on_wipe = dlog.observer();
    auto q = host::DevKeyProvider::from_file(f.string(), o, err);
    CHECK(q && dlog.count() == 0);
  }
  CHECK(dlog.count() == 1 && dlog.all_zero[0] && dlog.events[0].locked);
  // A key that cannot be locked in RAM is refused, not kept in swappable memory; nothing is leaked.
  {
    host::DevKeyProvider::Options o;
    int attempts = 0;
    o.key_memory.lock_fn = [&](void*, std::size_t) {
      ++attempts;
      return false;
    };
    CHECK(host::DevKeyProvider::from_file(f.string(), o, err) == nullptr && attempts == 1);
    // The lock works for the device key and fails for the content key.
    host::DevKeyProvider::Options o2;
    std::string e2;
    int locks = 0;
    o2.key_memory.lock_fn = [&](void* p2, std::size_t n) {
      ++locks;
      return locks == 1 ? ::mlock(p2, n) == 0 : false;
    };
    auto s = host::DevKeyProvider::from_bytes(keys.device_priv, o2, e2);
    CHECK(s != nullptr);
    Key32 eph = seq32(1);
    std::array<std::uint8_t, 12> nonce{};
    const Bytes blob = pfx::wrap_content_key(keys.content, kUuid, keys.device_pub, eph, nonce);
    const ports::KeyResult kr = s->unwrap_content_key(kUuid, blob);
    CHECK(kr.status == ports_status::kMemoryLockFailed && kr.key == nullptr);
  }
  // (SecureKeyHandle, the core limit and the wipe helper are tested in protected_keyless_test, which needs no dev key provider.)
}

// ---- the resolver and the head provider ---------------------------------------------------------------------------------------------------------

std::shared_ptr<const format::TrustedKeys> trust_of(const pfx::Keys& k) { return std::make_shared<const format::TrustedKeys>(k.trusted); }

// A byte of bundle.vlrp changed IN PLACE (the resident descriptors see it).
void flip_byte(const fs::path& p, std::uint64_t at) {
  std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
  f.seekg(static_cast<std::streamoff>(at));
  char c = 0;
  f.get(c);
  f.seekp(static_cast<std::streamoff>(at));
  f.put(static_cast<char>(c ^ 0x10));
}

struct HeadRig {
  TmpDir tmp;
  pfx::Keys keys;
  WipeLog log;
  std::unique_ptr<host::DevKeyProvider> provider;
  host::DamageRegistry damage;
  std::shared_ptr<host::FsPort> fs = host::make_posix_fs();
  host::LibraryResolver resolver;
  std::unique_ptr<host::ProtectedHeadProvider> heads;
  explicit HeadRig(std::uint8_t tag = 5) : keys(pfx::make_keys(tag)), resolver(fs, tmp.path) {
    provider = make_provider(keys, &log);
    resolver.set_protected_trust(trust_of(keys));
    heads = std::make_unique<host::ProtectedHeadProvider>(fs, *provider, &damage);
  }
};

void test_head_provider_load() {
  HeadRig rig;
  const std::vector<pfx::Spec> specs = {{"multi.sfz", pfx::Spec::kMulti}};
  const auto prot = pfx::install_library(rig.tmp.path, 1, specs, rig.keys);
  const auto plain = pfx::install_library(rig.tmp.path, 2, specs, rig.keys, {.protect = false});
  // Same content under two library ids: the PCM that a protected load makes resident is the unprotected one, bit for bit.
  auto rp = rig.resolver.resolve(prot.library_id, "multi.sfz");
  auto ru = rig.resolver.resolve(plain.library_id, "multi.sfz");
  CHECK(rp.ok() && ru.ok());
  CHECK(rig.heads->estimate_bytes(rp) == rig.heads->estimate_bytes(ru) && rig.heads->estimate_bytes(rp) > 0);
  std::shared_ptr<const host::BundleInstrument> ip, iu;
  CHECK(rig.heads->load(rp, ip) == host::LoadStatus::kOk);
  CHECK(rig.heads->load(ru, iu) == host::LoadStatus::kOk);  // the same provider passes an unprotected instrument through unchanged
  CHECK(ip && iu && ip->has_damage_maps() && !iu->has_damage_maps());
  if (ip && iu) {
    const auto a = ip->instrument(), b = iu->instrument();
    CHECK(a.samples.size() == 3 && a.samples.size() == b.samples.size() && a.regions.size() == b.regions.size());
    for (std::size_t s = 0; s < a.samples.size() && s < b.samples.size(); ++s) {
      CHECK(a.samples[s].frames == b.samples[s].frames && a.samples[s].channels == b.samples[s].channels);
      const std::size_t n = static_cast<std::size_t>(a.samples[s].frames);
      CHECK(std::memcmp(a.samples[s].left, b.samples[s].left, n * sizeof(float)) == 0);
      if (a.samples[s].channels == 2) CHECK(std::memcmp(a.samples[s].right, b.samples[s].right, n * sizeof(float)) == 0);
      CHECK(a.samples[s].damaged_blocks != nullptr && a.samples[s].damaged_block_frames == 4096 && a.samples[s].damaged_block_count == 3);
      CHECK(b.samples[s].damaged_blocks == nullptr);
    }
  }
  CHECK(rig.heads->live_instruments().size() == 1);
  // The edge thread's decrypt gives the same PCM as the loader's: the streamer path.
  if (ip) {
    host::EdgeBlockReader edge(ip);
    CHECK(edge.valid() && edge.sample_count() == 3);
    std::vector<float> pcm;
    std::uint32_t frames = 0;
    CHECK(edge.read_block_pcm(1, 2, pcm, frames) == format::Error::kNone && frames == 12000 - 8192);  // stereo 24-bit, last (short) block
    const ports::SamplePcm& s1 = ip->instrument().samples[1];
    CHECK(frames * 2 == pcm.size());
    CHECK(std::memcmp(pcm.data(), s1.left + 8192, frames * sizeof(float)) == 0 && std::memcmp(pcm.data() + frames, s1.right + 8192, frames * sizeof(float)) == 0);
    CHECK(edge.read_block_pcm(1, 3, pcm, frames) == format::Error::kOutOfRange && edge.read_block_pcm(7, 0, pcm, frames) == format::Error::kOutOfRange);
    const host::ScrubResult sr = edge.scrub();
    CHECK(sr.blocks_checked == 9 && sr.damaged == 0 && sr.io_errors == 0 && !sr.stopped);
    CHECK(ip->damaged_blocks() == 0 && rig.damage.reports() == 0);
    std::atomic<bool> stop{true};
    CHECK(edge.scrub(&stop).stopped);
  }
}

void test_typed_refusals() {
  const std::vector<pfx::Spec> specs = {{"piano.sfz", pfx::Spec::kConstFloat, 0.1f}};
  const std::uint64_t blocks_bytes = 4096ull * 4 * 2 + (12000 - 8192) * 4;

  {  // A block with one flipped bit: the load is REFUSED with content_integrity, nothing stays resident, no key stays alive.
    HeadRig rig;
    const auto lib = pfx::install_library(rig.tmp.path, 1, specs, rig.keys);
    flip_byte(lib.dir / "bundle.vlrp", lib.bundle.size() - blocks_bytes + 5000);  // inside block 0 of the sample... block 0 spans 16384 bytes
    auto r = rig.resolver.resolve(lib.library_id, "piano.sfz");
    CHECK(r.ok());  // the signed manifest and index are intact: the block is what is damaged
    std::shared_ptr<const host::BundleInstrument> inst;
    CHECK(rig.heads->load(r, inst) == host::LoadStatus::kContentIntegrity && inst == nullptr);
    CHECK(std::string(host::to_string(host::LoadStatus::kContentIntegrity)) == "content_integrity");
    CHECK(rig.log.count() == 1 && rig.log.all_zero[0]);  // the key that was unwrapped for it is already wiped
    CHECK(rig.heads->live_instruments().empty());
  }
  {  // The last block (a short one) too, and the first byte of the first block, and the last byte of the file.
    for (std::uint64_t from_end : {std::uint64_t(1), blocks_bytes, blocks_bytes - 20000}) {
      HeadRig rig;
      const auto lib = pfx::install_library(rig.tmp.path, 1, specs, rig.keys);
      flip_byte(lib.dir / "bundle.vlrp", lib.bundle.size() - from_end);
      auto r = rig.resolver.resolve(lib.library_id, "piano.sfz");
      std::shared_ptr<const host::BundleInstrument> inst;
      CHECK(r.ok() && rig.heads->load(r, inst) == host::LoadStatus::kContentIntegrity);
    }
  }
  {  // A key that does not belong to the bundle (the vendor wrapped another content key): wrong_content_key, never garbage audio.
    HeadRig rig;
    pfx::Keys wrong = rig.keys;
    for (auto& b : wrong.content) b = static_cast<std::uint8_t>(b ^ 0x5a);
    const auto lib = pfx::install_library(rig.tmp.path, 1, specs, rig.keys);
    const Bytes blob = pfx::wrap_content_key(wrong.content, lib.library_id, rig.keys.device_pub, seq32(9), {});
    pfx::write_file(lib.dir / "wrapped-key.bin", blob);
    auto r = rig.resolver.resolve(lib.library_id, "piano.sfz");
    std::shared_ptr<const host::BundleInstrument> inst;
    CHECK(rig.heads->load(r, inst) == host::LoadStatus::kWrongContentKey && inst == nullptr);
    CHECK(std::string(host::to_string(host::LoadStatus::kWrongContentKey)) == "wrong_content_key");
  }
  {  // The key provider refuses: a blob for another device, for another library, modified, missing, not a blob at all.
    HeadRig rig;
    const pfx::Keys other = pfx::make_keys(8);
    const auto lib = pfx::install_library(rig.tmp.path, 1, specs, rig.keys);
    auto r = rig.resolver.resolve(lib.library_id, "piano.sfz");
    std::shared_ptr<const host::BundleInstrument> inst;
    auto try_blob = [&](const Bytes& b, ports_status want) {
      pfx::write_file(lib.dir / "wrapped-key.bin", b);
      const host::LoadStatus st = rig.heads->load(r, inst);
      CHECK(st == host::LoadStatus::kKeyUnavailable && inst == nullptr);
      CHECK(rig.heads->last_key_status() == want);
    };
    try_blob(pfx::wrap_content_key(rig.keys.content, lib.library_id, other.device_pub, seq32(9), {}), ports_status::kWrongDevice);
    try_blob(pfx::wrap_content_key(rig.keys.content, format::uuid_to_string(pfx::library_uuid(99)), rig.keys.device_pub, seq32(9), {}), ports_status::kUnwrapFailed);
    Bytes mod = lib.wrapped;
    mod[80] ^= 1;
    try_blob(mod, ports_status::kUnwrapFailed);
    try_blob(Bytes(50, 7), ports_status::kMalformedBlob);
    try_blob(Bytes(5000, 7), ports_status::kMalformedBlob);  // larger than any blob is not even read
    fs::remove(lib.dir / "wrapped-key.bin");
    CHECK(rig.heads->load(r, inst) == host::LoadStatus::kKeyUnavailable && rig.heads->last_key_status() == ports_status::kUnavailable);
    CHECK(std::string(host::to_string(host::LoadStatus::kKeyUnavailable)) == "key_unavailable");
    // Restored, it loads.
    pfx::write_file(lib.dir / "wrapped-key.bin", lib.wrapped);
    CHECK(rig.heads->load(r, inst) == host::LoadStatus::kOk && inst);
  }
  {  // The signature, through the typed names.
    CHECK(std::string(host::to_string(host::LoadStatus::kSignatureInvalid)) == "signature_invalid");
    CHECK(host::load_status_from(format::Error::kSignatureInvalid) == host::LoadStatus::kSignatureInvalid);
    CHECK(host::load_status_from(format::Error::kHashListMismatch) == host::LoadStatus::kSignatureInvalid);
    CHECK(host::load_status_from(format::Error::kUntrustedKey) == host::LoadStatus::kUntrustedKey);
    CHECK(host::load_status_from(format::Error::kWrongContentKey) == host::LoadStatus::kWrongContentKey);
    CHECK(host::load_status_from(format::Error::kBlockHashMismatch) == host::LoadStatus::kContentIntegrity);
    CHECK(host::load_status_from(format::Error::kCorruptBlock) == host::LoadStatus::kContentIntegrity);
    CHECK(host::load_status_from(format::Error::kIo) == host::LoadStatus::kBundleInconsistent);
  }
  {  // A host with no protected support: the plain provider says so, typed, and decrypts nothing.
    HeadRig rig;
    const auto lib = pfx::install_library(rig.tmp.path, 1, specs, rig.keys);
    auto r = rig.resolver.resolve(lib.library_id, "piano.sfz");
    host::RamHeadProvider ram;
    std::shared_ptr<const host::BundleInstrument> inst;
    CHECK(ram.load(r, inst) == host::LoadStatus::kProtectedUnsupported && inst == nullptr);
    CHECK(ram.estimate_bytes(r) == 4096ull * 4 * 3 - (3 * 4096 - 12000) * 4);
  }
}

// ---- generations: the key lives and dies with the generation -----------------------------------------------------------------------------------

struct GenRig {
  TmpDir tmp;
  pfx::Keys keys = pfx::make_keys(5);
  WipeLog log;
  std::unique_ptr<host::DevKeyProvider> provider = make_provider(keys, &log);
  host::DamageRegistry damage;
  std::shared_ptr<host::FsPort> fs = host::make_posix_fs();
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>(8);
  core::SampleEngine sample;
  core::OrganEngine organ;
  host::ControlEventQueue rt_events;
  host::ControlPlane plane;
  host::LibraryResolver resolver;
  host::ProtectedHeadProvider heads;
  host::LiveSetLoader loader;
  float l[core::kBlockFrames], r[core::kBlockFrames];
  std::vector<std::vector<float>> blocks;

  static core::SampleEngine::Config cfg() {
    core::SampleEngine::Config c;
    c.max_voices = 340;
    return c;
  }
  GenRig()
      : sample(cfg()),
        plane(br->snapshots(), br->last_values(), rt_events, [] { return std::uint64_t{0}; }),
        resolver(fs, tmp.path),
        heads(fs, *provider, &damage),
        loader(*br, plane, resolver, heads, &sample) {
    CHECK(br->bind_engine(sample));
    CHECK(br->bind_engine(organ));
    resolver.set_protected_trust(trust_of(keys));
    plane.set_damage_registry(&damage);
  }
  void run(std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i) {
      br->process_block(l, r);
      blocks.emplace_back(l, l + core::kBlockFrames);
    }
  }
  void note_on(std::uint32_t key) { CHECK(br->submit({core::make_note_on(0, 0, key, 65535), 0})); }
  void note_off(std::uint32_t key) { CHECK(br->submit({core::make_note_off(0, 0, key, 0), 0})); }
  ws::v1::State state() {
    ws::v1::State s;
    plane.fill_state(&s);
    return s;
  }
};

lsp::LiveSet set_with(std::uint64_t seed, const std::string& lib, const std::string& key) {
  lsp::LiveSet s = ls::make_default_live_set(uuid_n(seed), "Protected set", 8);
  lsp::Part* part = s.mutable_parts(0);
  fixtures::timbre(part, lib, key);
  part->set_volume(1.0f);
  return s;
}

void test_key_lives_with_the_generation() {
  GenRig rig;
  const auto prot = pfx::install_library(rig.tmp.path, 1, {{"piano.sfz", pfx::Spec::kConstFloat, 0.1f}}, rig.keys);
  const auto plain = pfx::install_library(rig.tmp.path, 2, {{"pad.sfz", pfx::Spec::kConstFloat, 0.01f}}, rig.keys, {.protect = false});
  const host::LiveSetLoadResult res = rig.loader.load(set_with(10, prot.library_id, "piano.sfz"));
  CHECK(res.ok());
  CHECK(res.parts[0].state == core::PartState::kReady && res.parts[0].resolve == host::ResolveStatus::kOk);
  CHECK(rig.log.count() == 0);  // the key is alive: it is attached to the generation
  CHECK(rig.heads.live_instruments().size() == 1);
  rig.run(2);
  rig.note_on(60);
  rig.run(8);
  CHECK(rig.sample.active_voices() == 1);
  float peak = 0;
  for (float v : rig.blocks.back()) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.05f);  // the protected instrument sounds like any other
  // The next generation replaces it while the note is held: the old one is still referenced by the voice, so the key is NOT wiped.
  CHECK(rig.loader.load(set_with(11, plain.library_id, "pad.sfz")).ok());
  rig.run(4);
  (void)rig.plane.reclaim();
  CHECK(rig.log.count() == 0);
  CHECK(rig.heads.live_instruments().size() == 1);
  // The note ends; once its voice is gone the generation can be freed, and freeing it zeroizes the key.
  rig.note_off(60);
  rig.run(200);
  CHECK(rig.sample.active_voices() == 0);
  std::size_t freed = 0;
  for (int i = 0; i < 5 && rig.log.count() == 0; ++i) {
    freed += rig.plane.reclaim();
    rig.run(2);
  }
  CHECK(freed >= 1);
  CHECK(rig.log.count() == 1);
  if (rig.log.count() == 1) {
    CHECK(rig.log.all_zero[0] && rig.log.events[0].locked && rig.log.events[0].size >= 32);
  }
  CHECK(rig.heads.live_instruments().empty());  // nothing else held the instrument
}

void test_load_failures_publish_nothing() {
  GenRig rig;
  const auto good = pfx::install_library(rig.tmp.path, 2, {{"pad.sfz", pfx::Spec::kConstFloat, 0.01f}}, rig.keys, {.protect = false});
  CHECK(rig.loader.load(set_with(10, good.library_id, "pad.sfz")).ok());
  const std::uint64_t rev = rig.plane.revision();
  const auto prot = pfx::install_library(rig.tmp.path, 1, {{"piano.sfz", pfx::Spec::kConstFloat, 0.1f}}, rig.keys);
  const std::uint64_t blocks_bytes = 4096ull * 4 * 2 + (12000 - 8192) * 4;
  flip_byte(prot.dir / "bundle.vlrp", prot.bundle.size() - blocks_bytes + 9000);
  const host::LiveSetLoadResult res = rig.loader.load(set_with(11, prot.library_id, "piano.sfz"));
  CHECK(res.status == host::LiveSetLoadStatus::kInstrumentLoadFailed && res.load_status == host::LoadStatus::kContentIntegrity);
  CHECK(res.detail.find("content_integrity") != std::string::npos);
  CHECK(rig.plane.revision() == rev);  // the previous generation keeps playing
  CHECK(rig.log.count() == 1);
}

// ---- damage found while playing -----------------------------------------------------------------------------------------------------------------

void test_damage_fades_voices_and_reports() {
  GenRig rig;
  const auto lib = pfx::install_library(rig.tmp.path, 1, {{"piano.sfz", pfx::Spec::kConstFloat, 0.1f}}, rig.keys);
  CHECK(rig.loader.load(set_with(10, lib.library_id, "piano.sfz")).ok());
  rig.run(2);
  rig.note_on(60);
  rig.run(8);
  const double base = rig.blocks.back().back();
  CHECK(base > 0.05);
  CHECK(rig.state().content().content_integrity() == 0 && rig.state().content().damaged_library_count() == 0);
  const std::uint64_t rev_before = rig.plane.revision();

  // The sample's block 1 is damaged on disk AFTER the load; an I/O edge thread finds it (hash check + decrypt, off the audio thread) and marks it.
  const std::uint64_t blocks_bytes = 4096ull * 4 * 2 + (12000 - 8192) * 4;
  flip_byte(lib.dir / "bundle.vlrp", lib.bundle.size() - blocks_bytes + 16384 + 100);
  auto insts = rig.heads.live_instruments();
  CHECK(insts.size() == 1);
  host::ScrubResult sr;
  std::thread edge([&] {
    host::EdgeBlockReader reader(insts[0]);
    sr = reader.scrub();
  });
  edge.join();
  CHECK(sr.blocks_checked == 3 && sr.damaged == 1);
  CHECK(insts[0]->block_damaged(0, 1) && !insts[0]->block_damaged(0, 0) && !insts[0]->block_damaged(0, 2) && insts[0]->damaged_blocks() == 1);
  CHECK(rig.damage.library_damaged(lib.library_id) && rig.damage.reports() == 1);
  CHECK(rig.plane.revision() > rev_before);  // subscribers see the change
  const auto ev = rig.damage.events();
  CHECK(ev.size() == 1 && ev[0].instrument_key == "piano.sfz" && ev[0].sample == 0 && ev[0].block == 1 && ev[0].reason == "content_integrity");

  // The voice (playing at frame ~600 of the loop) reaches block 1 at frame 4096: it fades to silence before it, stays silent through the block, and comes back.
  const std::size_t first = rig.blocks.size();
  rig.run(140);  // 8960 frames more: through block 1 (frames 4096..8191) and into block 2
  std::vector<float> out;
  for (std::size_t i = first; i < rig.blocks.size(); ++i) out.insert(out.end(), rig.blocks[i].begin(), rig.blocks[i].end());
  // Find the silent span and check its shape: a monotone fade down, zeros, a monotone fade up; the loop point (frame 12000) is outside this window.
  std::size_t zero_from = out.size(), zero_to = 0;
  for (std::size_t i = 0; i < out.size(); ++i)
    if (std::fabs(out[i]) < 1e-7f) {
      zero_from = std::min(zero_from, i);
      zero_to = i;
    }
  CHECK(zero_from < out.size() && zero_to > zero_from);
  CHECK(zero_to - zero_from + 1 >= 4096 - 70);  // the damaged block is silent for its whole length (give or take the fades)
  bool down_monotone = true, up_monotone = true;
  std::size_t fade_down = 0, fade_up = 0;
  for (std::size_t i = zero_from; i > 0 && std::fabs(out[i - 1]) > 1e-7f && std::fabs(out[i - 1]) < std::fabs(base) * 0.9999; --i) {
    ++fade_down;
    if (out[i - 1] < out[i]) down_monotone = false;
  }
  for (std::size_t i = zero_to + 1; i < out.size() && std::fabs(out[i]) > 1e-7f && std::fabs(out[i]) < std::fabs(base) * 0.9999; ++i) {
    ++fade_up;
    if (out[i] < out[i - 1]) up_monotone = false;
  }
  CHECK(down_monotone && up_monotone);
  CHECK(fade_down >= 30 && fade_down <= 64 && fade_up >= 30 && fade_up <= 64);  // a short fade (core::kIntegrityFadeFrames = 64), not a cut
  CHECK_NEAR(out.back(), base, 1e-6);                                              // and the voice is back at full level in the next, intact block
  CHECK(rig.sample.content_integrity_events() == 1);
  const ws::v1::State st = rig.state();
  CHECK(st.content().content_integrity() == 1 && st.content().damaged_library_count() == 1 && st.content().damaged_library_ids_size() == 1);
  CHECK(st.content().damaged_library_ids(0) == lib.library_id && st.content().damage_reports() == 1);

  // A note that starts later meets the same block: it too fades (+1), and a note whose region starts inside the damaged block starts silent.
  rig.note_off(60);
  for (int i = 0; i < 40 && rig.sample.active_voices() != 0; ++i) rig.run(50);  // the first note finishes its release (it may meet the block again on its loop)
  CHECK(rig.sample.active_voices() == 0);
  const std::uint64_t events_before = rig.sample.content_integrity_events();
  rig.note_on(61);
  rig.run(160);
  CHECK(rig.sample.content_integrity_events() == events_before + 1);
  CHECK(rig.state().content().content_integrity() == events_before + 1);
  rig.note_off(61);
  rig.run(200);
  // Reporting again does not duplicate the library.
  CHECK(rig.damage.damaged_libraries().size() == 1);
}

// ---- which thread decrypts -------------------------------------------------------------------------------------------------------------------

std::mutex g_obs_mu;
std::vector<std::thread::id> g_obs_ids;
void observe_decrypt(std::uint64_t) {
  std::lock_guard<std::mutex> l(g_obs_mu);
  g_obs_ids.push_back(std::this_thread::get_id());
}
std::vector<std::thread::id> observed() {
  std::lock_guard<std::mutex> l(g_obs_mu);
  return g_obs_ids;
}

// Thread identity: every block is decrypted by the loading thread (the head loader) or by an I/O edge thread -- never by the audio thread. The audio thread renders
// with notes sounding (process_block over a protected instrument) while no edge thread runs: the number of decrypts does not move; then an edge thread scrubs and
// the new decrypts are all its own.
void test_decrypt_threads() {
  GenRig rig;
  const auto lib = pfx::install_library(rig.tmp.path, 1, {{"piano.sfz", pfx::Spec::kConstFloat, 0.1f}}, rig.keys);
  format::ProtectedBundleReader::set_decrypt_observer(&observe_decrypt);
  const std::thread::id loader_id = std::this_thread::get_id();
  CHECK(rig.loader.load(set_with(10, lib.library_id, "piano.sfz")).ok());
  const auto after_load = observed();
  CHECK(after_load.size() == 3);  // the three blocks of the sample, decrypted once each
  for (const auto& id : after_load) CHECK(id == loader_id);

  std::thread::id audio_id;
  std::atomic<std::uint64_t> voices_seen{0};
  std::atomic<bool> rendered{false}, release{false};
  std::thread audio([&] {
    audio_id = std::this_thread::get_id();
    rig.run(2);
    rig.note_on(60);
    rig.note_on(67);
    for (int i = 0; i < 400; ++i) {
      rig.run(1);
      voices_seen.store(rig.sample.active_voices());
    }
    rendered.store(true);
    while (!release.load()) std::this_thread::yield();  // stays alive: its thread id cannot be reused by the edge thread below
  });
  while (!rendered.load()) std::this_thread::yield();
  CHECK(voices_seen.load() == 2);
  CHECK(audio_id != loader_id);
  CHECK(observed().size() == after_load.size());  // process_block decrypted nothing, on any thread

  std::thread::id edge_id;
  std::thread edge([&] {
    edge_id = std::this_thread::get_id();
    host::EdgeBlockReader reader(rig.heads.live_instruments().front());
    CHECK(reader.scrub().blocks_checked == 3);
  });
  edge.join();
  const auto all = observed();
  CHECK(all.size() == after_load.size() + 3);
  for (std::size_t i = after_load.size(); i < all.size(); ++i) CHECK(all[i] == edge_id);
  CHECK(edge_id != audio_id && edge_id != loader_id);
  release.store(true);
  audio.join();
  format::ProtectedBundleReader::set_decrypt_observer(nullptr);
}

// ---- scrub vs generations ------------------------------------------------------------------------------------------------------------------------

// A scrub in progress never keeps a generation alive: the generation ends, `reclaim()` frees it from another thread, the key is wiped at once and the scrub stops.
void test_reclaim_during_scrub() {
  GenRig rig;
  const auto prot = pfx::install_library(rig.tmp.path, 1, {{"piano.sfz", pfx::Spec::kConstFloat, 0.1f, 300000}}, rig.keys);  // ~74 blocks per pass
  const auto plain = pfx::install_library(rig.tmp.path, 2, {{"pad.sfz", pfx::Spec::kConstFloat, 0.01f}}, rig.keys, {.protect = false});
  CHECK(rig.loader.load(set_with(10, prot.library_id, "piano.sfz")).ok());
  rig.run(2);
  auto weak = rig.heads.live_weak();
  CHECK(weak.size() == 1);
  std::atomic<bool> started{false};
  host::ScrubResult last;
  std::uint64_t passes = 0;
  std::thread edge([&] {
    host::EdgeBlockReader reader(weak.front());
    started.store(true);
    for (;;) {  // scrubs again and again until its generation is gone
      last = reader.scrub();
      ++passes;
      if (last.generation_gone) break;
      if (passes > 100000) break;
    }
  });
  while (!started.load()) std::this_thread::yield();
  CHECK(rig.loader.load(set_with(11, plain.library_id, "pad.sfz")).ok());  // the protected generation is superseded; no note holds it
  bool wiped = false;
  for (int i = 0; i < 20000 && !wiped; ++i) {
    rig.run(1);
    (void)rig.plane.reclaim();
    wiped = rig.log.count() == 1;
    if (!wiped) std::this_thread::yield();
  }
  edge.join();
  CHECK(wiped);                     // the key was wiped while the scrub thread was still running: it held no reference between blocks
  CHECK(last.generation_gone);      // ... and the scrub noticed and stopped
  CHECK(passes >= 1);
  CHECK(rig.log.count() == 1 && rig.log.all_zero[0]);
  CHECK(rig.heads.live_instruments().empty());
}

// An FsPort that can start failing reads (an I/O error says nothing about a block).
class FlakyFs final : public host::FsPort {
 public:
  explicit FlakyFs(std::shared_ptr<host::FsPort> in) : in_(std::move(in)) {}
  host::FsStat lstat(const std::string& p) override { return in_->lstat(p); }
  host::FsStatus readlink(const std::string& p, std::string& t) override { return in_->readlink(p, t); }
  host::FsStatus open_read(const std::string& p, int& fd, std::uint64_t& size) override { return in_->open_read(p, fd, size); }
  host::FsStatus pread(int fd, std::uint64_t off, void* dst, std::size_t n) override { return fail.load() ? host::FsStatus::kIo : in_->pread(fd, off, dst, n); }
  void close(int fd) override { in_->close(fd); }
  host::FsStatus list_dir(const std::string& p, std::vector<std::string>& n) override { return in_->list_dir(p, n); }
  std::atomic<bool> fail{false};

 private:
  std::shared_ptr<host::FsPort> in_;
};

void test_repeated_scrub_and_io_errors() {
  TmpDir tmp;
  const pfx::Keys keys = pfx::make_keys(5);
  WipeLog log;
  auto provider = make_provider(keys, &log);
  auto flaky = std::make_shared<FlakyFs>(host::make_posix_fs());
  host::LibraryResolver resolver(flaky, tmp.path);
  resolver.set_protected_trust(trust_of(keys));
  host::DamageRegistry damage;
  host::ProtectedHeadProvider heads(flaky, *provider, &damage);
  const auto lib = pfx::install_library(tmp.path, 1, {{"piano.sfz", pfx::Spec::kConstFloat, 0.1f}}, keys);
  auto rr = resolver.resolve(lib.library_id, "piano.sfz");
  std::shared_ptr<const host::BundleInstrument> inst;
  CHECK(rr.ok() && heads.load(rr, inst) == host::LoadStatus::kOk);
  int pushes = 0;
  damage.set_listener([&] { ++pushes; });
  host::EdgeBlockReader reader(inst);

  // An I/O failure: counted and shown, nothing marked damaged, no library reported.
  flaky->fail.store(true);
  host::ScrubResult r = reader.scrub();
  CHECK(r.io_errors == 3 && r.damaged == 0 && r.blocks_checked == 3);
  CHECK(damage.io_errors() == 3 && damage.reports() == 0 && damage.damaged_libraries().empty() && inst->damaged_blocks() == 0);
  flaky->fail.store(false);
  CHECK(reader.scrub().io_errors == 0 && reader.scrub().damaged == 0);

  // Real damage, scrubbed three times: found each time, reported ONCE (the 0 -> 1 transition), one State push for it.
  const std::uint64_t blocks_bytes = 4096ull * 4 * 2 + (12000 - 8192) * 4;
  flip_byte(lib.dir / "bundle.vlrp", lib.bundle.size() - blocks_bytes + 16384 + 7);
  const int pushes_before = pushes;
  for (int pass = 0; pass < 3; ++pass) {
    r = reader.scrub();
    CHECK(r.damaged == 1 && r.blocks_checked == 3);
    CHECK(damage.reports() == 1 && inst->damaged_blocks() == 1 && damage.io_errors() == 3);
  }
  CHECK(pushes == pushes_before + 1);
  // A control plane sees it as exactly one change, and the counters it shows are stable across the repeated passes.
  core::BlockRenderer br(8);
  host::ControlEventQueue q;
  host::ControlPlane plane(br.snapshots(), br.last_values(), q, [] { return std::uint64_t{0}; });
  plane.set_damage_registry(&damage);
  CHECK(plane.take_content_changed());
  CHECK(!plane.take_content_changed());
  (void)reader.scrub();
  CHECK(!plane.take_content_changed());
  ws::v1::State st;
  plane.fill_state(&st);
  CHECK(st.content().damage_reports() == 1 && st.content().io_errors() == 3 && st.content().damaged_library_count() == 1);
  // Reloaded cleanly (the file repaired): no longer reported damaged, a push.
  flip_byte(lib.dir / "bundle.vlrp", lib.bundle.size() - blocks_bytes + 16384 + 7);  // the same flip again restores the byte
  std::shared_ptr<const host::BundleInstrument> again;
  CHECK(heads.load(rr, again) == host::LoadStatus::kOk);
  CHECK(!damage.library_damaged(lib.library_id) && plane.take_content_changed());
  plane.set_damage_registry(nullptr);
}

// ---- the typed cause reaches the Live Set load ---------------------------------------------------------------------------------------------------

void test_liveset_typed_refusals() {
  const std::vector<pfx::Spec> specs = {{"piano.sfz", pfx::Spec::kConstFloat, 0.1f}};
  const std::uint64_t blocks_bytes = 4096ull * 4 * 2 + (12000 - 8192) * 4;
  struct Case {
    const char* what;
    std::function<void(GenRig&, const pfx::Installed&)> damage;
    host::LoadStatus status;
    format::Error error;
  };
  const Case cases[] = {
      {"signature", [&](GenRig&, const pfx::Installed& l) { flip_byte(l.dir / "bundle.vlrp", l.bundle.size() - blocks_bytes - 128 + 30); }, host::LoadStatus::kSignatureInvalid, format::Error::kSignatureInvalid},
      {"untrusted signer", [&](GenRig&, const pfx::Installed& l) { flip_byte(l.dir / "bundle.vlrp", l.bundle.size() - blocks_bytes - 128 + 9); }, host::LoadStatus::kUntrustedKey, format::Error::kUntrustedKey},
      {"manifest", [&](GenRig&, const pfx::Installed& l) { flip_byte(l.dir / "bundle.vlrp", 200); }, host::LoadStatus::kSignatureInvalid, format::Error::kCorruptManifest},
      {"planted bundle.vlb", [&](GenRig& g, const pfx::Installed& l) {
         format::BundleWriter w;
         w.set_library_id(pfx::library_uuid(1));
         pfx::add_spec(w, specs[0]);
         pfx::write_file(l.dir / "bundle.vlb", w.finalize().take());
         (void)g;
       }, host::LoadStatus::kProtectedDowngrade, format::Error::kNone},
  };
  for (const Case& c : cases) {
    GenRig rig;
    const auto good = pfx::install_library(rig.tmp.path, 2, {{"pad.sfz", pfx::Spec::kConstFloat, 0.01f}}, rig.keys, {.protect = false});
    CHECK(rig.loader.load(set_with(10, good.library_id, "pad.sfz")).ok());
    const std::uint64_t rev = rig.plane.revision();
    const auto lib = pfx::install_library(rig.tmp.path, 1, specs, rig.keys);
    c.damage(rig, lib);
    const host::LiveSetLoadResult res = rig.loader.load(set_with(11, lib.library_id, "piano.sfz"));
    if (res.status != host::LiveSetLoadStatus::kInstrumentLoadFailed || res.load_status != c.status || res.bundle_error != c.error || res.failed_part != 0) {
      std::printf("FAIL: %s: status %s load_status %s bundle_error %s\n", c.what, host::to_string(res.status), host::to_string(res.load_status), format::to_string(res.bundle_error));
      ++g_fail;
    }
    CHECK(res.detail.find(host::to_string(c.status)) != std::string::npos);
    CHECK(rig.plane.revision() == rev && rig.log.count() == 0);  // the previous generation keeps playing; no key was ever unwrapped
  }
  {  // A host with no trusted keys at all plays no protected library: the load succeeds and the part is silent with Missing library (not an error of the library).
    GenRig rig;
    const auto lib = pfx::install_library(rig.tmp.path, 1, specs, rig.keys);
    rig.resolver.set_protected_trust(nullptr);
    const host::LiveSetLoadResult res = rig.loader.load(set_with(10, lib.library_id, "piano.sfz"));
    CHECK(res.ok() && res.parts[0].state == core::PartState::kMissingLibrary && res.parts[0].resolve == host::ResolveStatus::kProtectedUnsupported);
  }
}

// ---- no plaintext block or key reaches a disk ---------------------------------------------------------------------------------------------------

// Every file under `root` with its bytes, to prove nothing was written, changed or added.
std::map<std::string, Bytes> snapshot(const fs::path& root) {
  std::map<std::string, Bytes> m;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (it->is_symlink()) m[it->path().string()] = Bytes();
    else if (it->is_regular_file()) m[it->path().string()] = pfx::read_file(it->path());
    else m[it->path().string()] = Bytes();
  }
  return m;
}

// Counts every FsPort call the engine makes and refuses nothing: the port has no write operation at all, which is the structural half of the claim.
class AuditFs final : public host::FsPort {
 public:
  explicit AuditFs(std::shared_ptr<host::FsPort> in) : in_(std::move(in)) {}
  host::FsStat lstat(const std::string& p) override { return in_->lstat(p); }
  host::FsStatus readlink(const std::string& p, std::string& t) override { return in_->readlink(p, t); }
  host::FsStatus open_read(const std::string& p, int& fd, std::uint64_t& size) override {
    opened.push_back(p);
    return in_->open_read(p, fd, size);
  }
  host::FsStatus pread(int fd, std::uint64_t off, void* dst, std::size_t n) override { return in_->pread(fd, off, dst, n); }
  void close(int fd) override { in_->close(fd); }
  host::FsStatus list_dir(const std::string& p, std::vector<std::string>& n) override { return in_->list_dir(p, n); }
  std::vector<std::string> opened;

 private:
  std::shared_ptr<host::FsPort> in_;
};

void test_no_plaintext_on_disk(const fs::path& process_tmp) {
  TmpDir tmp;
  const pfx::Keys keys = pfx::make_keys(5);
  WipeLog log;
  const auto lib = pfx::install_library(tmp.path, 1, {{"multi.sfz", pfx::Spec::kMulti}}, keys);
  const auto needles = pfx::plaintext_needles(keys);
  // The needles are real: the same content stored UNPROTECTED holds them (else this test would pass on anything).
  {
    TmpDir plain_dir;
    pfx::install_library(plain_dir.path, 2, {{"multi.sfz", pfx::Spec::kMulti}}, keys, {.protect = false});
    CHECK(!pfx::scan_for(plain_dir.path, needles).empty());
  }
  // And the protected files hold none of them.
  CHECK(pfx::scan_for(tmp.path, needles).empty());
  const auto before = snapshot(tmp.path);
  const auto tmp_before = snapshot(process_tmp);

  auto audit = std::make_shared<AuditFs>(host::make_posix_fs());
  const fs::path keyfile = process_tmp / "device.x25519.priv";  // the device key file is read, never copied
  pfx::write_file(keyfile, Bytes(keys.device_priv.begin(), keys.device_priv.end()));
  std::string err;
  host::DevKeyProvider::Options o;
  o.key_memory.on_wipe = log.observer();
  auto provider = host::DevKeyProvider::from_file(keyfile.string(), o, err);
  CHECK(provider != nullptr);
  const auto tmp_with_key = snapshot(process_tmp);
  host::LibraryResolver resolver(audit, tmp.path);
  resolver.set_protected_trust(trust_of(keys));
  host::DamageRegistry damage;
  host::ProtectedHeadProvider heads(audit, *provider, &damage);
  auto r = resolver.resolve(lib.library_id, "multi.sfz");
  std::shared_ptr<const host::BundleInstrument> inst;
  CHECK(r.ok() && heads.load(r, inst) == host::LoadStatus::kOk && inst);
  host::EdgeBlockReader edge(inst);
  CHECK(edge.scrub().damaged == 0);
  std::vector<float> pcm;
  std::uint32_t frames = 0;
  CHECK(edge.read_block_pcm(0, 1, pcm, frames) == format::Error::kNone);
  // The strongest needles: windows of the RESIDENT decoded float arrays themselves (every sample, left and right, several offsets), as raw 32-bit float bytes.
  std::vector<Bytes> resident;
  {
    const ports::InstrumentView iv = inst->instrument();
    for (const ports::SamplePcm& sp : iv.samples)
      for (const float* ch : {sp.left, sp.right}) {
        if (!ch) continue;
        for (std::uint64_t at : {std::uint64_t(150), sp.frames / 3, sp.frames / 2 + 17, sp.frames - 40}) {
          Bytes b(12 * sizeof(float));
          std::memcpy(b.data(), ch + at, b.size());
          resident.push_back(std::move(b));
        }
      }
    CHECK(resident.size() == 4 * 5);  // three samples: mono, stereo, stereo
    // not vacuous: the float sample (the third) is stored RAW and interleaved in an UNPROTECTED bundle of the same content, so frames of the resident arrays, interleaved,
    // are found there
    {
      const ports::SamplePcm& f = iv.samples[2];
      Bytes inter;
      for (std::uint64_t i = 500; i < 506; ++i)
        for (const float* ch : {f.left, f.right}) {
          const auto* b = reinterpret_cast<const std::uint8_t*>(ch + i);
          inter.insert(inter.end(), b, b + sizeof(float));
        }
      TmpDir plain_dir;
      pfx::install_library(plain_dir.path, 2, {{"multi.sfz", pfx::Spec::kMulti}}, keys, {.protect = false});
      CHECK(!pfx::scan_for(plain_dir.path, {inter}).empty());
      resident.push_back(inter);
    }
  }
  // Decoded PCM exists in RAM only: the library tree is byte for byte what it was, the process temp directory gained nothing (the key file we put there is ours).
  CHECK(snapshot(tmp.path) == before);
  CHECK(snapshot(process_tmp) == tmp_with_key);
  CHECK(pfx::scan_for(tmp.path, needles).empty());
  CHECK(pfx::scan_for(tmp.path, resident).empty());
  CHECK(pfx::scan_for(process_tmp, resident).empty());
  CHECK(pfx::scan_for(process_tmp, std::vector<Bytes>(needles.begin(), needles.begin() + 1)).empty());  // the content key is in no file there
  // Everything the engine opened was inside the library tree and read through the read-only port.
  CHECK(!audit->opened.empty());
  for (const std::string& p : audit->opened) CHECK(p.rfind(tmp.path, 0) == 0);
  inst.reset();
  r = host::ResolveResult{};
  fs::remove(keyfile);
}

}  // namespace

int main() {
  // Everything that asks for a temporary file lands in one directory we own and can audit.
  const fs::path process_tmp = fs::path(::getenv("TMPDIR") && *::getenv("TMPDIR") ? ::getenv("TMPDIR") : "/tmp") / ("velora-protected-engine-" + std::to_string(::getpid()));
  fs::create_directories(process_tmp);
  ::setenv("TMPDIR", process_tmp.string().c_str(), 1);

  test_pinned_vectors();
  test_unwrap_tamper_matrix();
  test_key_files_and_lock_failure();
  test_head_provider_load();
  test_typed_refusals();
  test_key_lives_with_the_generation();
  test_load_failures_publish_nothing();
  test_damage_fades_voices_and_reports();
  test_decrypt_threads();
  test_reclaim_during_scrub();
  test_repeated_scrub_and_io_errors();
  test_liveset_typed_refusals();
  test_no_plaintext_on_disk(process_tmp);

  std::error_code ec;
  fs::remove_all(process_tmp, ec);
  if (g_fail == 0) std::printf("protected_engine_test: ok\n");
  return g_fail == 0 ? 0 : 1;
}
