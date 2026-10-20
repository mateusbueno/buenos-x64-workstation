#pragma once
// Story 6.5 test fixtures: protected (factory) libraries on disk exactly as the librarian installs them (Story 6.4):
//   <root>/lib/<library_id>/<bundle_id>/bundle.vlrp, wrapped-key.bin     <root>/lib/<library_id>/current -> <bundle_id>
// plus a vendor-side wrapper of content keys for a device (wrapped_key_v1, tools/keys/KEY-HIERARCHY.md section 6) written with OpenSSL independently of the
// engine's unwrap code, so the dev key provider is tested against the vector and against an implementation that is not its own mirror image.
// Test code only: it links the protected writer (signing + AES-CTR encryption) and OpenSSL, which no shipped target may.
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "velora/format/bundle.hpp"
#include "velora/format/hash.hpp"
#include "velora/format/protected_format.hpp"
#include "velora/format/protected_verify.hpp"
#include "velora/format/protected_writer.hpp"

namespace pfx {

namespace fs = std::filesystem;
namespace fmt = velora::format;
using Bytes = std::vector<std::uint8_t>;
using Key32 = std::array<std::uint8_t, 32>;

// ---- vendor-side wrap (the engine side is DevKeyProvider) -----------------------------------------------------------------------------------------------

struct PkeyDel {
  void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); }
};
inline Key32 x25519_public(const Key32& priv) {
  std::unique_ptr<EVP_PKEY, PkeyDel> k(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), priv.size()));
  Key32 pub{};
  std::size_t len = pub.size();
  if (!k || EVP_PKEY_get_raw_public_key(k.get(), pub.data(), &len) != 1) pub.fill(0);
  return pub;
}
inline Key32 x25519_shared(const Key32& priv, const Key32& peer) {
  std::unique_ptr<EVP_PKEY, PkeyDel> a(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), priv.size()));
  std::unique_ptr<EVP_PKEY, PkeyDel> b(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer.data(), peer.size()));
  Key32 out{};
  std::size_t len = out.size();
  EVP_PKEY_CTX* c = EVP_PKEY_CTX_new(a.get(), nullptr);
  if (!c || EVP_PKEY_derive_init(c) != 1 || EVP_PKEY_derive_set_peer(c, b.get()) != 1 || EVP_PKEY_derive(c, out.data(), &len) != 1) out.fill(0);
  EVP_PKEY_CTX_free(c);
  return out;
}
inline Key32 hkdf32(const Key32& ikm, const Bytes& salt, const Bytes& info) {
  const fmt::Digest256 prk = fmt::hmac_sha256(salt, ikm);
  Bytes t(info);
  t.push_back(0x01);
  return fmt::hmac_sha256(prk, t);
}

// wrapped_key_v1 for `device_pub`, library `library_id` (the UTF-8 text), with the given ephemeral key and GCM nonce (explicit so vectors are reproducible).
inline Bytes wrap_content_key(const Key32& content, const std::string& library_id, const Key32& device_pub, const Key32& eph_priv, const std::array<std::uint8_t, 12>& nonce) {
  const Key32 eph_pub = x25519_public(eph_priv);
  const Key32 shared = x25519_shared(eph_priv, device_pub);
  Bytes salt(eph_pub.begin(), eph_pub.end());
  salt.insert(salt.end(), device_pub.begin(), device_pub.end());
  const std::string tag = "velora/wrapped_key_v1/" + library_id;
  const Key32 wk = hkdf32(shared, salt, Bytes(tag.begin(), tag.end()));
  Bytes blob(105, 0);
  std::copy_n("VWK1", 4, blob.begin());
  blob[4] = 0x01;
  const fmt::KeyId rid = fmt::key_id_of_public(device_pub);
  std::copy(rid.begin(), rid.end(), blob.begin() + 5);
  std::copy(eph_pub.begin(), eph_pub.end(), blob.begin() + 13);
  std::copy(nonce.begin(), nonce.end(), blob.begin() + 45);
  Bytes aad(blob.begin(), blob.begin() + 45);
  aad.insert(aad.end(), library_id.begin(), library_id.end());
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  int l = 0;
  EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
  EVP_EncryptInit_ex(c, nullptr, nullptr, wk.data(), nonce.data());
  EVP_EncryptUpdate(c, nullptr, &l, aad.data(), static_cast<int>(aad.size()));
  EVP_EncryptUpdate(c, blob.data() + 57, &l, content.data(), 32);
  EVP_EncryptFinal_ex(c, blob.data() + 57 + l, &l);
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, blob.data() + 89);
  EVP_CIPHER_CTX_free(c);
  return blob;
}

// ---- keys -----------------------------------------------------------------------------------------------------------------------------------------------

struct Keys {
  fmt::SigningSeed seed;
  fmt::PublicKey sign_pub{};
  fmt::KeyId sign_id{};
  Key32 content{};
  Key32 device_priv{}, device_pub{};
  fmt::TrustedKeys trusted;
};

inline Keys make_keys(std::uint8_t tag) {
  Keys k;
  for (int i = 0; i < 32; ++i) {
    k.seed.bytes[i] = static_cast<std::uint8_t>(tag * 7 + 3 * i + 1);
    k.content[i] = static_cast<std::uint8_t>(tag * 11 + 5 * i + 2);
    k.device_priv[i] = static_cast<std::uint8_t>(tag * 13 + 9 * i + 5);
  }
  k.sign_pub = fmt::ed25519_public_from_seed(k.seed);
  k.sign_id = fmt::key_id_of_public(k.sign_pub);
  k.device_pub = x25519_public(k.device_priv);
  k.trusted = fmt::TrustedKeys({fmt::TrustedKey{k.sign_id, k.sign_pub}});
  return k;
}

// ---- content --------------------------------------------------------------------------------------------------------------------------------------------

// The test library: instruments built deterministically from `Spec`s. `multi` is one instrument with three samples (mono 16-bit FLAC, stereo 24-bit FLAC, stereo
// float32) on three key ranges, several blocks each, with a release time -- what the bit-identical render comparison plays.
struct Spec {
  std::string key;
  enum Kind { kConstFloat, kSineFloat, kMulti, kWide } kind = kConstFloat;
  float level = 0.1f;
  std::uint32_t frames = 12000;  // three 4096-frame blocks
};

inline std::vector<std::int32_t> tone16(std::uint32_t frames, double hz, std::uint32_t seed) {
  std::vector<std::int32_t> v(frames);
  std::uint32_t x = seed * 2654435761u + 1u;
  for (std::uint32_t i = 0; i < frames; ++i) {
    x = x * 1664525u + 1013904223u;
    const double s = 0.5 * std::sin(2.0 * 3.14159265358979323846 * hz * i / 48000.0) + 0.02 * (static_cast<double>((x >> 16) & 0xff) / 255.0 - 0.5);
    v[i] = static_cast<std::int32_t>(std::lround(s * 32767.0));
  }
  return v;
}
inline std::vector<std::int32_t> tone24_stereo(std::uint32_t frames, double hz, std::uint32_t seed) {
  std::vector<std::int32_t> v(static_cast<std::size_t>(frames) * 2);
  std::uint32_t x = seed * 2654435761u + 7u;
  for (std::uint32_t i = 0; i < frames; ++i)
    for (int c = 0; c < 2; ++c) {
      x = x * 1664525u + 1013904223u;
      const double s = 0.4 * std::sin(2.0 * 3.14159265358979323846 * hz * (c ? 1.01 : 1.0) * i / 48000.0) + 0.01 * (static_cast<double>((x >> 16) & 0xff) / 255.0 - 0.5);
      v[static_cast<std::size_t>(i) * 2 + c] = static_cast<std::int32_t>(std::lround(s * 8388607.0));
    }
  return v;
}
inline std::vector<std::int32_t> tone_float_stereo(std::uint32_t frames, double hz) {
  std::vector<std::int32_t> v(static_cast<std::size_t>(frames) * 2);
  for (std::uint32_t i = 0; i < frames; ++i)
    for (int c = 0; c < 2; ++c)
      v[static_cast<std::size_t>(i) * 2 + c] = fmt::float_to_bits(0.3f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * hz * (c ? 0.99 : 1.0) * i / 48000.0)));
  return v;
}

inline fmt::Region region(std::uint32_t sample, int lo, int hi, int center, fmt::LoopMode loop = fmt::LoopMode::kNoLoop) {
  fmt::Region r;
  r.sample = sample;
  r.key_lo = static_cast<std::uint8_t>(lo);
  r.key_hi = static_cast<std::uint8_t>(hi);
  r.key_center = static_cast<std::uint8_t>(center);
  r.loop_mode = loop;
  r.ampeg.release = 0.1f;
  return r;
}

inline void add_spec(fmt::BundleWriter& w, const Spec& sp) {
  auto ins = w.add_instrument(sp.key, sp.key);
  if (!ins.ok()) std::abort();
  if (sp.kind == Spec::kMulti) {
    const auto a = tone16(sp.frames, 220.0, 1);
    const auto b = tone24_stereo(sp.frames, 330.0, 2);
    const auto c = tone_float_stereo(sp.frames, 440.0);
    if (w.add_sample(ins.value(), {"a", {1, 16, false, 48000}, sp.frames, a}) != fmt::Error::kNone) std::abort();
    if (w.add_sample(ins.value(), {"b", {2, 24, false, 48000}, sp.frames, b}) != fmt::Error::kNone) std::abort();
    if (w.add_sample(ins.value(), {"c", {2, 32, true, 48000}, sp.frames, c}) != fmt::Error::kNone) std::abort();
    (void)w.add_region(ins.value(), region(0, 48, 59, 54));
    (void)w.add_region(ins.value(), region(1, 60, 71, 66));
    (void)w.add_region(ins.value(), region(2, 72, 83, 78, fmt::LoopMode::kLoopContinuous));
    return;
  }
  if (sp.kind == Spec::kWide) {  // the other supported formats: mono 24-bit at 44.1 kHz, stereo 16-bit at 96 kHz, mono 16-bit at 88.2 kHz, stereo 24-bit at 44.1 kHz
    // (formatlib supports 16/24-bit integer and 32-bit float PCM at 44.1/48/88.2/96 kHz; 8-bit and 32-bit integer PCM are not formats of the bundle)
    std::vector<std::int32_t> m24(sp.frames), s16(static_cast<std::size_t>(sp.frames) * 2), m16 = tone16(sp.frames, 261.6, 9), s24 = tone24_stereo(sp.frames, 523.2, 4);
    for (std::uint32_t i = 0; i < sp.frames; ++i) {
      m24[i] = static_cast<std::int32_t>(std::lround(0.45 * std::sin(2.0 * 3.14159265358979323846 * 196.0 * i / 44100.0) * 8388607.0));
      for (int c = 0; c < 2; ++c) s16[static_cast<std::size_t>(i) * 2 + c] = static_cast<std::int32_t>(std::lround(0.35 * std::sin(2.0 * 3.14159265358979323846 * (c ? 392.0 : 400.0) * i / 96000.0) * 32767.0));
    }
    if (w.add_sample(ins.value(), {"m24", {1, 24, false, 44100}, sp.frames, m24}) != fmt::Error::kNone) std::abort();
    if (w.add_sample(ins.value(), {"s16", {2, 16, false, 96000}, sp.frames, s16}) != fmt::Error::kNone) std::abort();
    if (w.add_sample(ins.value(), {"m16", {1, 16, false, 88200}, sp.frames, m16}) != fmt::Error::kNone) std::abort();
    if (w.add_sample(ins.value(), {"s24", {2, 24, false, 44100}, sp.frames, s24}) != fmt::Error::kNone) std::abort();
    (void)w.add_region(ins.value(), region(0, 24, 35, 30));
    (void)w.add_region(ins.value(), region(1, 36, 47, 42, fmt::LoopMode::kLoopContinuous));
    (void)w.add_region(ins.value(), region(2, 84, 95, 90));
    (void)w.add_region(ins.value(), region(3, 96, 107, 102));
    return;
  }
  std::vector<std::int32_t> pcm(sp.frames);
  for (std::size_t i = 0; i < pcm.size(); ++i)
    pcm[i] = fmt::float_to_bits(sp.kind == Spec::kSineFloat ? 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 480.0)) : sp.level);
  if (w.add_sample(ins.value(), {"s", {1, 32, true, 48000}, sp.frames, pcm}) != fmt::Error::kNone) std::abort();
  fmt::Region r = region(0, 0, 127, 60, fmt::LoopMode::kLoopContinuous);
  (void)w.add_region(ins.value(), r);
}

inline fmt::Uuid library_uuid(std::uint32_t index) {
  const std::string fp = "velora-protected-test/" + std::to_string(index);
  return fmt::library_id_from_fingerprint({reinterpret_cast<const std::uint8_t*>(fp.data()), fp.size()});
}

inline void write_file(const fs::path& p, const Bytes& b) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
inline Bytes read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Installed {
  std::string library_id, bundle_id;
  fs::path dir;           // <root>/lib/<library_id>/<bundle_id>
  Bytes bundle;           // the bytes of bundle.vlrp / bundle.vlb as installed
  Bytes wrapped;          // wrapped-key.bin (protected only)
  Bytes plain_pcm_probe;  // see probes()
};

struct InstallOptions {
  bool protect = true;
  bool write_wrapped = true;
  const Keys* wrap_for = nullptr;  // wrap the content key to this device (default: `keys`)
  std::string wrap_library_id;     // the library id the wrap binds (default: the real one)
};

// Installs library `index` with `specs` under `root` the way the librarian does and (protected) wraps its content key for the device of `keys`.
inline Installed install_library(const std::string& root, std::uint32_t index, const std::vector<Spec>& specs, const Keys& keys, const InstallOptions& io = {}) {
  fmt::BundleWriter w;
  const fmt::Uuid lib = library_uuid(index);
  w.set_library_id(lib);
  for (const Spec& sp : specs) add_spec(w, sp);
  Installed out;
  out.library_id = fmt::uuid_to_string(lib);
  if (io.protect) {
    fmt::ContentKey ck;
    ck.bytes = keys.content;
    auto b = fmt::finalize_protected(w, ck, keys.seed);
    if (!b.ok()) std::abort();
    out.bundle = b.take();
    auto view = fmt::ProtectedBundleView::open_memory(out.bundle, keys.trusted);
    if (!view.ok()) std::abort();
    out.bundle_id = fmt::to_hex(view.value().info().bundle_id);
  } else {
    auto b = w.finalize();
    if (!b.ok()) std::abort();
    out.bundle = b.take();
    auto r = fmt::BundleReader::open_memory(out.bundle);
    if (!r.ok()) std::abort();
    out.bundle_id = fmt::to_hex(r.value().bundle_id());
  }
  out.dir = fs::path(root) / "lib" / out.library_id / out.bundle_id;
  write_file(out.dir / (io.protect ? "bundle.vlrp" : "bundle.vlb"), out.bundle);
  if (io.protect && io.write_wrapped) {
    const Keys& to = io.wrap_for ? *io.wrap_for : keys;
    Key32 eph{};
    for (int i = 0; i < 32; ++i) eph[i] = static_cast<std::uint8_t>(0x21 + i + index);
    std::array<std::uint8_t, 12> nonce{};
    for (int i = 0; i < 12; ++i) nonce[i] = static_cast<std::uint8_t>(0xa0 + i);
    out.wrapped = wrap_content_key(keys.content, io.wrap_library_id.empty() ? out.library_id : io.wrap_library_id, to.device_pub, eph, nonce);
    write_file(out.dir / "wrapped-key.bin", out.wrapped);
  }
  fs::create_symlink(out.bundle_id, fs::path(root) / "lib" / out.library_id / "current");
  return out;
}

// All the PLAINTEXT fingerprints that must never reach a disk: the content key and runs of the raw (little-endian) PCM the test library holds.
inline std::vector<Bytes> plaintext_needles(const Keys& keys) {
  std::vector<Bytes> n;
  n.emplace_back(keys.content.begin(), keys.content.end());
  Bytes dev(keys.device_priv.begin(), keys.device_priv.end());
  n.push_back(dev);
  auto from16 = [&](const std::vector<std::int32_t>& v, std::size_t at) {
    Bytes b;
    for (std::size_t i = at; i < at + 24 && i < v.size(); ++i) {
      b.push_back(static_cast<std::uint8_t>(v[i] & 0xff));
      b.push_back(static_cast<std::uint8_t>((v[i] >> 8) & 0xff));
    }
    return b;
  };
  const auto a = tone16(12000, 220.0, 1);
  for (std::size_t at : {std::size_t(100), std::size_t(4500), std::size_t(9000)}) n.push_back(from16(a, at));
  auto from_words = [&](const std::vector<std::int32_t>& v, std::size_t at, int bytes) {
    Bytes b;
    for (std::size_t i = at; i < at + 12 && i < v.size(); ++i)
      for (int k = 0; k < bytes; ++k) b.push_back(static_cast<std::uint8_t>((v[i] >> (8 * k)) & 0xff));
    return b;
  };
  const auto b24 = tone24_stereo(12000, 330.0, 2);
  for (std::size_t at : {std::size_t(300), std::size_t(9000)}) n.push_back(from_words(b24, at, 3));
  const auto cf = tone_float_stereo(12000, 440.0);
  for (std::size_t at : {std::size_t(300), std::size_t(9000)}) n.push_back(from_words(cf, at, 4));
  return n;
}

inline bool contains(const Bytes& hay, const Bytes& needle) {
  if (needle.empty() || hay.size() < needle.size()) return false;
  return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

// Every regular file under `dir` (recursively) that holds one of `needles`; returns the paths. An unreadable file counts as a find (it cannot be vouched for).
inline std::vector<std::string> scan_for(const fs::path& dir, const std::vector<Bytes>& needles) {
  std::vector<std::string> hits;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    const Bytes b = read_file(it->path());
    for (const Bytes& n : needles)
      if (contains(b, n)) {
        hits.push_back(it->path().string());
        break;
      }
  }
  return hits;
}

struct TmpDir {
  std::string path;
  TmpDir() {
    path = (fs::temp_directory_path() / "velora-protected-XXXXXX").string();
    if (!mkdtemp(path.data())) {
      std::printf("mkdtemp failed\n");
      std::exit(1);
    }
  }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TmpDir(const TmpDir&) = delete;
  TmpDir& operator=(const TmpDir&) = delete;
};

}  // namespace pfx
