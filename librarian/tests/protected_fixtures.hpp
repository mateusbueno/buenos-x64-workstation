#pragma once
// Story 6.4 test fixtures: small PROTECTED bundles with the files a vendor pack carries (bundle.vlrp, catalog.meta, catalog.meta.sig, wrapped-key.<id>.bin,
// plus the files a pack also holds that the librarian must never touch: content-key.vault, pack.json). Test code only: it links the protected writer (signing and
// AES-CTR), which no shipped target may (tools/ci/check_format_no_decrypt.py scans the real binaries).
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "velora/format/bundle.hpp"
#include "velora/format/hash.hpp"
#include "velora/format/protected_verify.hpp"
#include "velora/format/protected_writer.hpp"
#include "velora/librarian/library_index.hpp"

namespace pfx {

namespace fs = std::filesystem;
namespace fmt = velora::format;
using Bytes = std::vector<std::uint8_t>;

struct Keys {
  fmt::SigningSeed seed;
  fmt::PublicKey pub{};
  fmt::KeyId id{};
  fmt::ContentKey content;
  fmt::TrustedKeys trusted;
};

inline Keys make_keys(std::uint8_t tag) {
  Keys k;
  for (int i = 0; i < 32; ++i) {
    k.seed.bytes[i] = static_cast<std::uint8_t>(tag * 7 + 3 * i + 1);
    k.content.bytes[i] = static_cast<std::uint8_t>(tag * 11 + 5 * i + 2);
  }
  k.pub = fmt::ed25519_public_from_seed(k.seed);
  k.id = fmt::key_id_of_public(k.pub);
  k.trusted = fmt::TrustedKeys({fmt::TrustedKey{k.id, k.pub}});
  return k;
}

inline void write_bytes(const fs::path& p, const Bytes& b) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
inline Bytes read_bytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
inline Bytes as_bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

struct PackSpec {
  std::string library_id = "3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f";
  std::string name = "Vendor Grand Piano";
  bool extension = false;       // a keyswitch region: format minor 1
  unsigned instruments = 2;     // 1..3
  std::uint32_t frames = 3000;  // per sample (mono 16-bit unless `big`)
  bool big = false;             // stereo float noise, `frames` frames per instrument: many blocks, a bundle far larger than the copy chunk
  unsigned variant = 0;         // changes the PCM: the same library, a new bundle
  std::uint32_t revision = 1;   // the signed pack_revision (catalog.meta.sig)
  std::string wrapped_name;     // default: wrapped-key.<recipient id>.bin
  std::uint8_t recipient = 0xA1;
};

struct Pack {
  fs::path dir;
  std::string library_id, bundle_id;
  Bytes bundle, meta, seal, wrapped;
  std::string wrapped_name;
  std::uint64_t blocks = 0;
  std::uint32_t minor = 0;
  std::uint32_t revision = 1;
};

inline Bytes make_wrapped_key(std::uint8_t recipient) {
  Bytes w(105, 0);
  std::copy_n("VWK1", 4, w.begin());
  w[4] = 0x01;
  for (int i = 0; i < 8; ++i) w[5 + i] = static_cast<std::uint8_t>(recipient + i);
  for (int i = 0; i < 32; ++i) w[13 + i] = static_cast<std::uint8_t>(0x40 + 3 * i + recipient);
  for (std::size_t i = 45; i < w.size(); ++i) w[i] = static_cast<std::uint8_t>(0x11 * (i % 15) + recipient);
  return w;
}

inline std::vector<std::int32_t> noise16(std::uint32_t frames, std::uint32_t seed) {
  std::vector<std::int32_t> v(frames);
  std::uint32_t x = seed * 2654435761u + 12345u;
  for (auto& s : v) {
    x = x * 1664525u + 1013904223u;
    s = static_cast<std::int32_t>((x >> 16) & 0x3fff) - 0x2000;
  }
  return v;
}
inline std::vector<std::int32_t> noise_float(std::uint32_t frames, std::uint32_t seed) {
  std::vector<std::int32_t> v(static_cast<std::size_t>(frames) * 2);
  std::uint32_t x = seed * 2654435761u + 777u;
  for (auto& s : v) {
    x = x * 1664525u + 1013904223u;
    s = fmt::float_to_bits(static_cast<float>(static_cast<std::int32_t>(x >> 8) - (1 << 23)) / static_cast<float>(1 << 23));
  }
  return v;
}

// Builds the (unprotected) content of a pack into a writer.
inline fmt::BundleWriter make_writer(const PackSpec& sp) {
  fmt::BundleWriter w;
  fmt::Uuid id{};
  fmt::uuid_from_string(sp.library_id, id);
  w.set_library_id(id);
  for (unsigned i = 0; i < sp.instruments; ++i) {
    const std::string nm = "Instrument " + std::to_string(i);
    auto ins = w.add_instrument(nm, "pack/inst" + std::to_string(i) + ".sfz");
    const std::uint32_t seed = 100 + i * 17 + sp.variant * 7919u;
    if (sp.big) {
      const fmt::PcmFormat f{2, 32, true, 48000};
      auto pcm = noise_float(sp.frames, seed);
      (void)w.add_sample(ins.value(), {"s" + std::to_string(i), f, sp.frames, pcm});
    } else {
      const fmt::PcmFormat f{1, 16, false, 48000};
      auto pcm = noise16(sp.frames, seed);
      (void)w.add_sample(ins.value(), {"s" + std::to_string(i), f, sp.frames, pcm});
    }
    fmt::Region r;
    r.sample = 0;
    (void)w.add_region(ins.value(), r);
    if (sp.extension && i == 0) {
      r.sw = {24, 28, 26, -1, 37, -1, 26};
      (void)w.add_region(ins.value(), r);
    }
  }
  return w;
}

// Writes a complete pack directory <parent>/<library_id>/ and returns what is in it.
inline Pack make_pack(const fs::path& parent, const PackSpec& sp, const Keys& keys) {
  Pack p;
  p.library_id = sp.library_id;
  p.dir = parent / sp.library_id;
  fs::create_directories(p.dir);
  fmt::BundleWriter w = make_writer(sp);
  auto b = fmt::finalize_protected(w, keys.content, keys.seed);  // content-derived nonce, like the content-pack tool
  p.bundle = b.ok() ? b.take() : Bytes{};
  auto view = fmt::ProtectedBundleView::open_memory(p.bundle, keys.trusted);
  if (!view.ok()) return p;
  p.bundle_id = fmt::to_hex(view.value().info().bundle_id);
  p.blocks = view.value().total_blocks();
  p.minor = view.value().info().format_minor;
  p.revision = sp.revision;
  velora::librarian::CatalogMeta meta;
  meta.library_name = sp.name;
  meta.source_kind = "sfz";
  meta.source_path = "inst0.sfz";
  for (const auto& ins : view.value().instruments()) meta.instruments.push_back({ins.key, ins.name, fmt::Category::kPiano});
  p.meta = as_bytes(velora::librarian::catalog_meta_text(meta));
  auto seal = fmt::sign_catalog_meta(keys.seed, view.value().info().bundle_id, p.meta, sp.revision);
  if (seal.ok()) p.seal.assign(seal.value().begin(), seal.value().end());
  p.wrapped = make_wrapped_key(sp.recipient);
  p.wrapped_name = sp.wrapped_name.empty() ? "wrapped-key." + fmt::to_hex(std::span<const std::uint8_t>(p.wrapped.data() + 5, 8)) + ".bin" : sp.wrapped_name;
  write_bytes(p.dir / "bundle.vlrp", p.bundle);
  write_bytes(p.dir / "catalog.meta", p.meta);
  write_bytes(p.dir / "catalog.meta.sig", p.seal);
  write_bytes(p.dir / p.wrapped_name, p.wrapped);
  write_bytes(p.dir / "content-key.vault", Bytes(72, 0x5a));  // must never be read or copied by the librarian
  write_bytes(p.dir / "pack.json", as_bytes("{}\n"));
  return p;
}

// Rebuilds a bundle after a deliberate edit and signs it again with `seed` (so the signature is VALID for the edited manifest): `edit` may change the manifest bytes
// (offsets from the manifest start) and the header; the manifest hash, bundle_id and the signature over the statement are recomputed. Used to make bundles that are
// validly signed and still wrong (an inconsistent hash-list root, an unknown version).
inline Bytes resign(Bytes b, const fmt::SigningSeed& seed, const std::function<void(Bytes& manifest, Bytes& header)>& edit) {
  auto rd64 = [&](std::size_t o) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[o + i];
    return v;
  };
  const std::size_t man_off = rd64(8), man_size = rd64(16), idx_off = rd64(24), idx_size = rd64(32), seal_off = idx_off + idx_size;
  Bytes manifest(b.begin() + man_off, b.begin() + man_off + man_size), header(b.begin(), b.begin() + 128);
  edit(manifest, header);
  const fmt::Digest256 mh = fmt::Sha256::hash(manifest);
  std::copy(manifest.begin(), manifest.end(), b.begin() + man_off);
  std::copy(header.begin(), header.begin() + 64, b.begin());
  std::copy(mh.begin(), mh.end(), b.begin() + 64);
  fmt::ManifestStatement st;
  st.version_word = 0;
  for (int i = 3; i >= 0; --i) st.version_word = (st.version_word << 8) | b[4 + i];
  std::copy_n(manifest.begin() + 4, 16, st.library_id.begin());
  st.bundle_id = mh;
  std::copy_n(b.begin() + 96, 32, st.index_sha256.begin());
  std::copy_n(manifest.begin() + 96, 32, st.hash_list_root.begin());
  st.block_count = 0;
  for (int i = 7; i >= 0; --i) st.block_count = (st.block_count << 8) | manifest[28 + i];
  std::copy_n(manifest.begin() + 72, 8, st.nonce.begin());
  std::copy_n(b.begin() + seal_off + 8, 8, st.signer_key_id.begin());
  auto sig = fmt::ed25519_sign(seed, fmt::statement_bytes(st));
  if (sig.ok()) std::copy(sig.value().begin(), sig.value().end(), b.begin() + seal_off + 16);
  return b;
}

// Which files of a directory tree (relative names), for "nothing was written" checks.
inline std::vector<std::string> list_tree(const fs::path& root) {
  std::vector<std::string> out;
  std::error_code ec;
  if (!fs::exists(root, ec)) return out;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec))
    out.push_back(fs::relative(it->path(), root).generic_string());
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace pfx
