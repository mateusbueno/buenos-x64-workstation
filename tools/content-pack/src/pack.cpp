#include "pack.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>

#include "velora/format/bundle.hpp"
#include "velora/format/hash.hpp"
#include "velora/format/json_lite.hpp"
#include "velora/format/protected_reader.hpp"
#include "velora/format/protected_writer.hpp"
#include "velora/librarian/import.hpp"
#include "velora/librarian/library_index.hpp"
#include "vendor_register.hpp"

namespace velora::contentpack {
namespace fs = std::filesystem;
namespace fmt = velora::format;
namespace lib = velora::librarian;

namespace {

std::string json_str(const std::string& s) {
  std::string o = "\"";
  static const char* kHex = "0123456789abcdef";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += static_cast<char>(c);
    } else if (c < 0x20 || c == 0x7f) {
      o += "\\u00";
      o += kHex[c >> 4];
      o += kHex[c & 15];
    } else {
      o += static_cast<char>(c);
    }
  }
  return o + "\"";
}

std::string hex(std::span<const std::uint8_t> d) { return fmt::to_hex(d); }

// One file, written through a fresh O_EXCL file, fsynced. The monitor sees the bytes first.
Failure write_file(const PackRequest& req, const std::string& dir, const std::string& name, std::span<const std::uint8_t> bytes, mode_t mode) {
  if (req.write_monitor) req.write_monitor(name, bytes);
  const std::string path = dir + "/" + name;
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
  if (fd < 0) return fail(Code::kOutputFailed, path + ": " + std::strerror(errno));
  std::size_t done = 0;
  bool ok = true;
  while (ok && done < bytes.size()) {
    const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) ok = false;
    else done += static_cast<std::size_t>(n);
  }
  ok = ::fsync(fd) == 0 && ok;
  ok = ::close(fd) == 0 && ok;
  if (!ok) return fail(Code::kOutputFailed, path + ": write failed");
  return {};
}

bool fsync_dir(const std::string& dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

std::span<const std::uint8_t> as_bytes(const std::string& s) { return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}; }

}  // namespace

Failure run_pack(const PackRequest& req, PackReport& report) {
  report = PackReport{};
  const PackDescription& d = req.desc;
  if (!req.kek || !req.signing_seed) return fail(Code::kInternal, "run_pack needs a KEK and a signing key");

  // 1. Vendor ID register (before any work). The lock is taken before the check and held until the pack is published and the id appended, so two packs of
  //    one id cannot both pass the check.
  const bool dev_range = is_dev_range_id(d.library_uuid);
  if (dev_range && d.root != Root::kDev)
    return fail(Code::kLibraryIdUnregistered, "library_id " + d.library_id + " is in the dev/test range, which only root dev may use");
  if (dev_range && req.register_new) return fail(Code::kLibraryIdConflict, "the dev/test id range is never registered");
  if (req.register_new && !is_random_v4(d.library_uuid))
    return fail(Code::kBadDescription, "only a random (version 4) UUID can be registered as a vendor library_id");
  RegisterLock reg_lock;
  bool registered = false;
  if (!dev_range) {
    if (Failure f = reg_lock.acquire(req.register_path)) return f;
    VendorRegister reg;
    if (Failure f = load_register(req.register_path, reg, true)) return f;
    RegisterState st;
    if (Failure f = check_register(reg, d, st)) return f;
    registered = st == RegisterState::kRegistered;
    if (st == RegisterState::kNew && !req.register_new)
      return fail(Code::kLibraryIdUnregistered, "library_id " + d.library_id + " is not in the vendor ID register " + req.register_path +
                                                    "; add it with `content-pack register` or pass --register --owner NAME");
  } else {
    VendorRegister reg;  // a dev-range id needs no entry, but a register that exists must still be well formed
    std::error_code rec;
    if (std::filesystem::exists(req.register_path, rec))
      if (Failure f = load_register(req.register_path, reg, true)) return f;
  }

  // 2. Output location (before any work).
  std::error_code ec;
  const std::string final_dir = (fs::path(req.out_dir) / d.library_id).string();
  if (!fs::is_directory(req.out_dir, ec)) return fail(Code::kOutputFailed, req.out_dir + ": the output directory does not exist");
  if (fs::exists(fs::symlink_status(final_dir, ec)) && !req.overwrite)
    return fail(Code::kOutputFailed, final_dir + " already exists (use --overwrite to replace it)");

  // 3. The content key: fresh from the OS CSPRNG unless one is supplied. It lives in locked, wiped memory only.
  Secret<fmt::ContentKey> key;
  if (req.content_key) {
    key.get() = *req.content_key;
  } else if (!random_bytes(key->bytes)) {
    return fail(Code::kVaultFailed, "the OS random number generator failed");
  }
  if (key->bytes == decltype(key->bytes){}) return fail(Code::kBadKeyFile, "the content key is all zero");
  const fmt::PublicKey sign_pub = fmt::ed25519_public_from_seed(*req.signing_seed);
  if (sign_pub == fmt::PublicKey{}) return fail(Code::kBadKeyFile, "the signing seed is not usable");

  // 4. Import through the librarian importer, in build-only mode: verified in memory, nothing written, the writer goes to the sink.
  lib::ImportOptions opt;
  opt.sfz_path = d.source;
  opt.category = d.category;
  opt.library_name = d.name;
  opt.write_catalog_meta = true;
  opt.library_id = d.library_id;
  for (const auto& o : d.instruments) opt.instrument_meta.push_back({o.key, o.name, o.category});
  Bytes protected_bytes;
  std::string catalog_meta;
  Failure sink_failure;
  opt.bundle_sink = [&](const lib::BuiltBundle& b) -> fmt::Error {
    catalog_meta = b.catalog_meta;
    // The nonce is the content-derived one (BundleWriter::derive_nonce) from the digest the importer already computed.
    auto r = fmt::finalize_protected(*b.writer, key.get(), fmt::BundleWriter::nonce_from_plain_digest(b.plain_sha256), *req.signing_seed);
    if (!r) {
      sink_failure = fail(Code::kBundleFailed, std::string("cannot build the protected bundle: ") + fmt::to_string(r.error()));
      return r.error();
    }
    protected_bytes = r.take();
    return fmt::Error::kNone;
  };
  const lib::ImportResult res = lib::import_file(opt);
  if (!res.ok()) {
    if (sink_failure) return sink_failure;
    std::string why = std::string(lib::to_string(res.status));
    if (res.status == lib::Status::kSkipped) why += std::string(" (") + lib::to_string(res.reason_code) + ")";
    for (const auto& r : res.reasons) why += "\n  " + r;
    return fail(res.status == lib::Status::kSkipped ? Code::kImportSkipped : Code::kImportFailed, d.source + ": " + why);
  }
  if (res.library_id != d.library_id) return fail(Code::kInternal, "the importer returned another library_id than the description");

  // 5. Verify the protected bundle by opening it as a device would (signature, hash list, every block, decrypt, decode, PCM hashes).
  fmt::TrustedKeys trusted({fmt::TrustedKey{fmt::key_id_of_public(sign_pub), sign_pub}});
  fmt::ProtectedInfo info;
  std::uint64_t blocks = 0;
  {
    auto rd = fmt::ProtectedBundleReader::open_memory(protected_bytes, trusted, key.get());
    if (!rd) return fail(Code::kBundleFailed, std::string("the protected bundle does not open: ") + fmt::to_string(rd.error()));
    if (fmt::Error e = rd.value().verify_all(); e != fmt::Error::kNone)
      return fail(Code::kBundleFailed, std::string("the protected bundle does not verify: ") + fmt::to_string(e));
    info = rd.value().info();
    blocks = rd.value().view().total_blocks();
    if (info.library_id != d.library_uuid) return fail(Code::kBundleFailed, "the protected bundle carries another library_id");
  }

  // 6. The content key leaves memory only as vault_entry_v1 (and wrapped_key_v1 for given devices).
  const Nonce12 vnonce = [&] {
    Nonce12 n{};
    if (req.deterministic_vault) return vault_nonce_deterministic(key->bytes, d.library_id);
    if (!random_bytes(n)) n.fill(0);
    return n;
  }();
  if (!req.deterministic_vault && vnonce == Nonce12{}) return fail(Code::kVaultFailed, "the OS random number generator failed");
  const auto vault = vault_entry_seal(*req.kek, key->bytes, d.library_id, vnonce);
  {
    Secret<Key32> check;
    if (!vault_entry_open(*req.kek, vault, d.library_id, check.get()) || !fmt::ct_equal(check.get(), key->bytes))
      return fail(Code::kVaultFailed, "the vault entry does not open again");
  }
  struct Wrapped {
    std::string name, recipient;
    std::array<std::uint8_t, kWrappedKeySize> blob;
  };
  std::vector<Wrapped> wrapped;
  for (const Key32& pub : req.device_pubs) {
    Wrapped w;
    w.recipient = hex(fmt::key_id_of_public(pub));
    w.name = "wrapped-key." + w.recipient + ".bin";
    for (const auto& o : wrapped)
      if (o.name == w.name) return fail(Code::kUsage, "the same device public key was given twice");
    if (!wrapped_key_seal_random(key->bytes, d.library_id, pub, w.blob)) return fail(Code::kVaultFailed, "cannot wrap the content key for device " + w.recipient + " (invalid public key?)");
    wrapped.push_back(std::move(w));
  }

  // 6b. Story 6.4: catalog.meta is not in the signed manifest, so it gets a detached seal (catalog.meta.sig) bound to this bundle_id with the same key.
  auto meta_seal = fmt::sign_catalog_meta(*req.signing_seed, info.bundle_id, as_bytes(catalog_meta), req.revision);
  if (!meta_seal) return fail(Code::kBundleFailed, std::string("cannot seal catalog.meta: ") + fmt::to_string(meta_seal.error()));

  // 7. Report.
  lib::CatalogMeta meta;
  if (!lib::parse_catalog_meta(catalog_meta, meta)) return fail(Code::kInternal, "the importer's catalog.meta does not parse");
  report.library_id = d.library_id;
  report.bundle_id = hex(info.bundle_id);
  report.plain_bundle_id = res.bundle_id;
  report.nonce_hex = hex(info.nonce);
  report.signer_key_id = hex(info.signer_key_id);
  report.kek_id = hex(kek_id(*req.kek));
  report.instruments = meta.instruments.size();
  report.blocks = blocks;
  const fmt::Digest256 file_hash = fmt::Sha256::hash(protected_bytes);
  std::string json = "{\n  \"schema\": \"velora.content-pack-report/1\",\n  \"tool\": " + json_str(std::string(kToolName) + " " + kToolVersion) +
                     ",\n  \"library_id\": " + json_str(d.library_id) + ",\n  \"name\": " + json_str(d.name) + ",\n  \"localized_names\": {";
  {
    bool first = true;
    for (const auto& [tag, nm] : d.localized_names) {
      json += std::string(first ? "\n    " : ",\n    ") + json_str(tag) + ": " + json_str(nm);
      first = false;
    }
    json += first ? "}" : "\n  }";
  }
  json += ",\n  \"pack_revision\": " + std::to_string(req.revision) + ",\n  \"root\": " + json_str(to_string(d.root)) + ",\n  \"source\": " + json_str(fs::path(d.source).filename().string()) +
          ",\n  \"bundle\": {\n    \"file\": " + json_str(kBundleFileName) + ",\n    \"bundle_id\": " + json_str(report.bundle_id) +
          ",\n    \"unprotected_bundle_id\": " + json_str(report.plain_bundle_id) + ",\n    \"format_version\": " + std::to_string(info.format_version) +
          ",\n    \"nonce\": " + json_str(report.nonce_hex) + ",\n    \"signer_key_id\": " + json_str(report.signer_key_id) +
          ",\n    \"blocks\": " + std::to_string(blocks) + ",\n    \"size\": " + std::to_string(protected_bytes.size()) +
          ",\n    \"sha256\": " + json_str(hex(file_hash)) + "\n  },\n  \"vault_entry\": {\n    \"file\": " + json_str(kVaultEntryFileName) +
          ",\n    \"kek_id\": " + json_str(report.kek_id) + "\n  },\n  \"wrapped_keys\": [";
  for (std::size_t i = 0; i < wrapped.size(); ++i)
    json += std::string(i ? "," : "") + "\n    {\"file\": " + json_str(wrapped[i].name) + ", \"recipient_key_id\": " + json_str(wrapped[i].recipient) + "}";
  json += wrapped.empty() ? "],\n  \"instruments\": [" : "\n  ],\n  \"instruments\": [";
  for (std::size_t i = 0; i < meta.instruments.size(); ++i) {
    const auto& m = meta.instruments[i];
    json += std::string(i ? "," : "") + "\n    {\"key\": " + json_str(m.key) + ", \"name\": " + json_str(m.name) +
            ", \"category\": " + json_str(std::string(fmt::key(m.category))) + "}";
  }
  json += "\n  ]\n}\n";

  // 8. Write into a staging directory next to the final one, then rename into place.
  const std::string stage = (fs::path(req.out_dir) / (".pack-" + d.library_id + "." + std::to_string(::getpid()))).string();
  fs::remove_all(stage, ec);
  if (::mkdir(stage.c_str(), 0755) != 0) return fail(Code::kOutputFailed, stage + ": " + std::strerror(errno));
  auto cleanup = [&] {
    std::error_code e;
    fs::remove_all(stage, e);
  };
  auto put = [&](const std::string& name, std::span<const std::uint8_t> b, mode_t mode) -> Failure {
    if (Failure f = write_file(req, stage, name, b, mode)) return f;
    report.files.push_back(name);
    return {};
  };
  Failure f = put(kBundleFileName, protected_bytes, 0644);
  if (!f) f = put(kVaultEntryFileName, vault, 0600);
  for (std::size_t i = 0; !f && i < wrapped.size(); ++i) f = put(wrapped[i].name, wrapped[i].blob, 0600);
  if (!f) f = put(kCatalogMetaFileName, as_bytes(catalog_meta), 0644);
  if (!f) f = put(fmt::kCatalogMetaSealFileName, meta_seal.value(), 0644);
  if (!f) f = put(kPackReportFileName, as_bytes(json), 0644);
  if (!f && !fsync_dir(stage)) f = fail(Code::kOutputFailed, stage + ": fsync failed");
  if (f) {
    cleanup();
    return f;
  }
  // An existing pack is replaced only when its vault entry holds the same content key (the key survives in the new vault entry); otherwise the old
  // directory is kept as .old-<id>.<time>.<pid> - the only copy of a different key must never be deleted by a rebuild.
  std::string aside;
  bool keep_aside = false;
  if (fs::exists(fs::symlink_status(final_dir, ec))) {
    std::error_code vec;
    const fs::path old_vault = fs::path(final_dir) / kVaultEntryFileName;
    if (fs::exists(old_vault, vec)) {
      Key32 old_key{};
      std::uint8_t entry[kVaultEntrySize];
      std::FILE* fp = std::fopen(old_vault.c_str(), "rb");
      const bool read = fp && std::fread(entry, 1, sizeof entry, fp) == sizeof entry;
      if (fp) std::fclose(fp);
      const bool same = read && vault_entry_open(*req.kek, entry, d.library_id, old_key) && fmt::ct_equal(old_key, key->bytes);
      wipe(old_key.data(), old_key.size());
      keep_aside = !same;
    }
    aside = (fs::path(req.out_dir) / (keep_aside ? ".old-" + d.library_id + "." + std::to_string(std::time(nullptr)) + "." + std::to_string(::getpid())
                                                 : ".old-" + d.library_id + "." + std::to_string(::getpid()))).string();
    if (std::rename(final_dir.c_str(), aside.c_str()) != 0) {
      cleanup();
      return fail(Code::kOutputFailed, final_dir + ": cannot move the previous pack aside");
    }
    report.kept_old = keep_aside ? aside : std::string();
  }
  auto roll_back = [&](const std::string& why) -> Failure {
    // Put things as they were: the new pack leaves, the previous one returns.
    std::error_code rbe;
    fs::remove_all(final_dir, rbe);
    bool restored = true;
    if (!aside.empty()) restored = std::rename(aside.c_str(), final_dir.c_str()) == 0;
    report = PackReport{};
    return fail(Code::kOutputFailed, why + (restored ? "" : "; ALSO could not restore the previous pack from " + aside));
  };
  if (std::rename(stage.c_str(), final_dir.c_str()) != 0) {
    std::error_code rbe;
    fs::remove_all(stage, rbe);
    bool restored = aside.empty() || std::rename(aside.c_str(), final_dir.c_str()) == 0;
    return fail(Code::kOutputFailed, final_dir + ": cannot move the pack into place" + (restored ? "" : "; ALSO could not restore the previous pack from " + aside));
  }
  if (!fsync_dir(req.out_dir)) return roll_back(req.out_dir + ": fsync failed after publishing the pack");
  report.dir = final_dir;

  // 9. Only a finished pack takes the id in the register (lock still held). A failed append withdraws the pack again.
  if (req.register_new && !registered) {
    if (Failure rf = append_register_locked(req.register_path, d, req.owner, req.date)) {
      Failure out = roll_back("the pack was withdrawn because the register could not be updated: " + rf.message);
      out.code = rf.code;
      return out;
    }
  }
  if (!aside.empty() && !keep_aside) fs::remove_all(aside, ec);
  return {};
}

Failure verify_pack(const std::string& bundle_path, std::span<const std::uint8_t> vault_entry, const Key32& kek, const Key32& trusted_pub,
                    std::string* summary) {
  // The library id is needed to open the vault entry (it is bound as AAD): read it from the bundle without a key first.
  fmt::TrustedKeys trusted({fmt::TrustedKey{fmt::key_id_of_public(trusted_pub), trusted_pub}});
  auto view = fmt::ProtectedBundleView::open_file(bundle_path, trusted);
  if (!view) return fail(Code::kVerifyFailed, std::string(bundle_path) + ": " + fmt::to_string(view.error()));
  const std::string library_id = fmt::uuid_to_string(view.value().info().library_id);
  Secret<fmt::ContentKey> key;
  if (!vault_entry_open(kek, vault_entry, library_id, key->bytes))
    return fail(Code::kVerifyFailed, "the vault entry does not open with this KEK for library " + library_id);
  auto rd = fmt::ProtectedBundleReader::open_file(bundle_path, trusted, key.get());
  if (!rd) return fail(Code::kVerifyFailed, std::string("cannot open the bundle with the vaulted key: ") + fmt::to_string(rd.error()));
  if (fmt::Error e = rd.value().verify_all(); e != fmt::Error::kNone) return fail(Code::kVerifyFailed, std::string("verification failed: ") + fmt::to_string(e));
  if (summary)
    *summary = "ok: library " + library_id + ", bundle " + hex(rd.value().info().bundle_id) + ", " + std::to_string(rd.value().instruments().size()) +
               " instrument(s), " + std::to_string(rd.value().samples().size()) + " sample(s), " + std::to_string(rd.value().view().total_blocks()) + " block(s)";
  return {};
}

}  // namespace velora::contentpack
