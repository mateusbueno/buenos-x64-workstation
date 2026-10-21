#include "release.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <set>

#include "pack.hpp"
#include "vendor_register.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/format/category.hpp"
#include "velora/format/hash.hpp"
#include "velora/format/json_lite.hpp"
#include "velora/format/protected_verify.hpp"
#include "velora/format/protected_writer.hpp"
#include "velora/host/factory_live_set.hpp"
#include "velora/host/live_set_store.hpp"
#include "velora/librarian/library_index.hpp"

namespace velora::factoryrelease {
namespace fs = std::filesystem;
namespace fmt = velora::format;
namespace cp = velora::contentpack;
namespace hf = velora::host::factory;
namespace ls = velora::host::liveset;
namespace lib = velora::librarian;

namespace {

std::string hex(std::span<const std::uint8_t> d) { return fmt::to_hex(d); }

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

Failure write_file(const std::string& path, std::string_view bytes, mode_t mode) {
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

bool read_all(const std::string& path, std::string& out, std::size_t max_bytes) {
  std::string err;
  return fmt::read_text_file(path, out, &err, max_bytes);
}

}  // namespace

std::string sha256_file_hex(const std::string& path, std::uint64_t* size_out) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return {};
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return {};
  }
  fmt::Sha256 h;
  std::vector<std::uint8_t> buf(1u << 20);
  std::uint64_t total = 0;
  for (;;) {
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      ::close(fd);
      return {};
    }
    if (n == 0) break;
    h.update(buf.data(), static_cast<std::size_t>(n));
    total += static_cast<std::uint64_t>(n);
  }
  ::close(fd);
  if (size_out) *size_out = total;
  const fmt::Digest256 d = h.finish();
  return hex(d);
}

// ======================================================================================================================================================
// build
// ======================================================================================================================================================
namespace {

// SHA-256 of a factory Live Set entry with the release version of its factory mark left out: a content hash that survives a version bump.
std::string entry_content_sha(const velora::liveset::v1::FactoryLiveSetEntry& entry) {
  velora::liveset::v1::FactoryLiveSetEntry c = entry;
  if (c.live_set().has_factory()) c.mutable_live_set()->mutable_factory()->clear_release_version();
  std::string bytes;
  {
    google::protobuf::io::StringOutputStream sos(&bytes);
    google::protobuf::io::CodedOutputStream cos(&sos);
    cos.SetSerializationDeterministic(true);
    (void)c.SerializeToCodedStream(&cos);
  }
  return hex(fmt::Sha256::hash(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size())));
}

// Engine / master parameters of a factory Live Set against the parameter registry (proto/params.json): an unknown id, a value out of range or a non-integer
// value for an integer parameter is invalid_live_set (the Live Set format itself would only warn about an unknown id and keep it).
Failure check_registry_params(const std::string& where, const std::vector<EngineParam>& list) {
  for (const EngineParam& e : list) {
    const velora::core::params::ParamDef* def = velora::core::params::find(e.param_id);
    if (!def) return fail(Code::kInvalidLiveSet, where + ": parameter id " + std::to_string(e.param_id) + " is not in the registry");
    if (!(e.value >= def->min && e.value <= def->max))
      return fail(Code::kInvalidLiveSet, where + ": parameter " + std::to_string(e.param_id) + " value " + std::to_string(e.value) + " is outside " + std::to_string(def->min) + ".." + std::to_string(def->max));
    if (def->type != velora::core::params::Type::kFloat && e.value != std::floor(e.value))
      return fail(Code::kInvalidLiveSet, where + ": parameter " + std::to_string(e.param_id) + " is not an integer parameter value");
  }
  return {};
}

struct BuiltLibrary {
  LibraryRow row;
  std::string vault_rel, bundle_rel, meta_rel, seal_rel, report_rel;
};

// A part spec applied to a default Live Set.
void apply_part(const PartSpec& ps, velora::liveset::v1::Part* p) {
  namespace lsp = velora::liveset::v1;
  if (ps.has_timbre) {
    p->mutable_timbre()->set_library_id(ps.library_id);
    p->mutable_timbre()->set_instrument_key(ps.instrument_key);
  }
  p->mutable_engine()->set_engine_type_id(ps.engine_type);
  for (const EngineParam& e : ps.params) {
    lsp::ParamEntry* pe = p->mutable_engine()->add_params();
    pe->set_param_id(e.param_id);
    pe->set_value(e.value);
  }
  if (ps.has_low_key) p->set_low_key(ps.low_key);
  if (ps.has_high_key) p->set_high_key(ps.high_key);
  if (ps.has_octave) p->set_octave(ps.octave);
  if (ps.has_transpose) p->set_transpose(ps.transpose);
  if (ps.has_keyboard_on) p->set_keyboard_on(ps.keyboard_on);
  if (ps.has_volume) p->set_volume(ps.volume);
  if (ps.has_send_reverb) p->set_send_reverb(ps.send_reverb);
  if (ps.has_send_delay) p->set_send_delay(ps.send_delay);
  if (ps.has_output) p->set_output(ps.output_aux ? ws::v1::PART_OUTPUT_AUX : ws::v1::PART_OUTPUT_MAIN);
}

}  // namespace

Failure build_release(const BuildRequest& req, BuildReport& rep) {
  rep = BuildReport{};
  const ReleaseDescription& d = req.desc;
  if (!req.kek || !req.signing_seed) return fail(Code::kInternal, "build_release needs a KEK and a signing key");
  std::error_code ec;
  if (!fs::is_directory(req.out_dir, ec)) return fail(Code::kOutputFailed, req.out_dir + ": the output directory does not exist");
  const fs::path out(req.out_dir);
  const fs::path final_dir = out / d.version;
  if (fs::exists(fs::symlink_status(final_dir, ec)) && !req.overwrite) return fail(Code::kOutputFailed, final_dir.string() + " already exists (use --overwrite to replace it)");

  const fmt::PublicKey sign_pub = fmt::ed25519_public_from_seed(*req.signing_seed);
  if (sign_pub == fmt::PublicKey{}) return fail(Code::kBadKeyFile, "the signing seed is not usable");
  const fmt::KeyId signer = fmt::key_id_of_public(sign_pub);

  // Everything that needs no packing is checked before anything is packed or registered: the Live Sets against the parameter registry and the v1 part count, the
  // vault directory the keys come from.
  for (const LiveSetSpec& spec : d.live_sets) {
    const std::string w = "program " + std::to_string(spec.program_id);
    for (const PartSpec& ps : spec.parts) {
      if (ps.part >= ls::kV1PartCount) return fail(Code::kInvalidLiveSet, w + " part " + std::to_string(ps.part) + ": a v1 Live Set has " + std::to_string(ls::kV1PartCount) + " parts");
      if (Failure f = check_registry_params(w + " part " + std::to_string(ps.part), ps.params)) return f;
    }
    if (Failure f = check_registry_params(w + " master", spec.master)) return f;
  }
  if (!req.reuse_vaults_dir.empty() && !req.allow_new_keys && !fs::is_directory(req.reuse_vaults_dir, ec))
    return fail(Code::kVaultFailed, req.reuse_vaults_dir + ": --reuse-vaults names a directory that does not exist (use --allow-new-keys for a first release)");

  const fs::path stage = out / (".release-" + d.version + "." + std::to_string(::getpid()));
  fs::remove_all(stage, ec);
  fs::create_directories(stage / "packs", ec);
  fs::create_directories(stage / kLibrariesDir, ec);
  fs::create_directories(stage / kRegistryVaultDir, ec);
  if (ec) return fail(Code::kOutputFailed, stage.string() + ": " + ec.message());
  auto cleanup = [&] {
    std::error_code e;
    fs::remove_all(stage, e);
  };
  auto bail = [&](Failure f) {
    cleanup();
    return f;
  };

  // 1. Pack every library with content-pack's run_pack under the release root's keys.
  std::vector<BuiltLibrary> built;
  for (const LibraryEntry& le : d.libraries) {
    cp::PackRequest pr;
    pr.desc = le.pack;
    pr.out_dir = (stage / "packs").string();
    pr.revision = le.revision;
    pr.kek = req.kek;
    pr.signing_seed = req.signing_seed;
    pr.deterministic_vault = true;
    // The pack runs against a COPY of the register in the staging area; the real register is only touched by the final commit, once everything is validated.
    pr.register_path = (stage / "packs" / "vendor-ids").string();
    if (!req.register_path.empty() && fs::exists(req.register_path, ec) && !fs::exists(pr.register_path, ec)) fs::copy_file(req.register_path, pr.register_path, ec);
    pr.register_new = req.register_new && !cp::is_dev_range_id(le.pack.library_uuid);  // the dev/test id range is never registered
    pr.owner = req.owner;
    pr.date = req.date;
    cp::Secret<fmt::ContentKey> reuse;
    if (!req.reuse_vaults_dir.empty()) {
      const fs::path vp = fs::path(req.reuse_vaults_dir) / le.pack.library_id / cp::kVaultEntryFileName;
      if (fs::exists(vp, ec)) {
        std::string entry;
        if (!read_all(vp.string(), entry, cp::kVaultEntrySize) || entry.size() != cp::kVaultEntrySize)
          return bail(fail(Code::kVaultFailed, vp.string() + ": not a vault entry (72 bytes)"));
        cp::Key32 k{};
        if (!cp::vault_entry_open(*req.kek, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(entry.data()), entry.size()), le.pack.library_id, k))
          return bail(fail(Code::kVaultFailed, vp.string() + ": does not open with this KEK for library " + le.pack.library_id));
        reuse->bytes = k;
        cp::wipe(k.data(), k.size());
        pr.content_key = &reuse.get();
      } else if (!req.allow_new_keys) {
        return bail(fail(Code::kVaultFailed, vp.string() + ": no vault entry for library " + le.pack.library_id + " (use --allow-new-keys to give a new library a fresh key)"));
      }
    }
    cp::PackReport prep;
    if (const cp::Failure f = cp::run_pack(pr, prep)) return bail(Failure{static_cast<Code>(static_cast<int>(f.code)), le.pack.name + " (" + le.pack.library_id + "): " + f.message});

    BuiltLibrary b;
    b.row.library_id = le.pack.library_id;
    b.row.name = le.pack.name;
    b.row.bundle_id = prep.bundle_id;
    b.row.revision = le.revision;
    std::string meta_text;
    if (!read_all(prep.dir + "/" + cp::kCatalogMetaFileName, meta_text, 4u << 20)) return bail(fail(Code::kInternal, "cannot read back catalog.meta of " + le.pack.library_id));
    lib::CatalogMeta meta;
    if (!lib::parse_catalog_meta(meta_text, meta)) return bail(fail(Code::kMissingCategory, le.pack.library_id + ": an instrument has no assignable category"));
    if (meta.instruments.empty()) return bail(fail(Code::kImportSkipped, le.pack.library_id + ": the library has no instrument"));
    for (const auto& mi : meta.instruments) {
      if (mi.category == fmt::Category::kNone) return bail(fail(Code::kMissingCategory, le.pack.library_id + ": instrument '" + mi.key + "' has no category"));
      b.row.instruments.push_back({mi.key, mi.name, std::string(fmt::key(mi.category))});
    }
    const std::string lid = le.pack.library_id;
    const fs::path ldir = stage / kLibrariesDir / lid;
    fs::create_directories(ldir, ec);
    fs::create_directories(stage / kRegistryVaultDir / lid, ec);
    const struct {
      const char* name;
      std::string* rel;
    } unit_files[] = {{cp::kBundleFileName, &b.bundle_rel},
                      {cp::kCatalogMetaFileName, &b.meta_rel},
                      {fmt::kCatalogMetaSealFileName, &b.seal_rel},
                      {cp::kPackReportFileName, &b.report_rel}};
    for (const auto& uf : unit_files) {
      fs::copy_file(fs::path(prep.dir) / uf.name, ldir / uf.name, fs::copy_options::none, ec);
      if (ec) return bail(fail(Code::kOutputFailed, std::string(uf.name) + ": " + ec.message()));
      ::chmod((ldir / uf.name).c_str(), 0644);
      *uf.rel = std::string(kLibrariesDir) + "/" + lid + "/" + uf.name;
    }
    fs::copy_file(fs::path(prep.dir) / cp::kVaultEntryFileName, stage / kRegistryVaultDir / lid / cp::kVaultEntryFileName, fs::copy_options::none, ec);
    if (ec) return bail(fail(Code::kOutputFailed, std::string(cp::kVaultEntryFileName) + ": " + ec.message()));
    ::chmod((stage / kRegistryVaultDir / lid / cp::kVaultEntryFileName).c_str(), 0600);
    b.vault_rel = std::string(kRegistryVaultDir) + "/" + lid + "/" + cp::kVaultEntryFileName;
    built.push_back(std::move(b));
    rep.signer_key_id = prep.signer_key_id;
    rep.kek_id = prep.kek_id;
  }
  {
    std::error_code e;
    fs::remove_all(stage / "packs", e);  // the packs' own copies of the vault entries must not outlive the build
  }

  // 2. The factory Live Sets: author, validate against the packed libraries, sign.
  std::string live_sets_rel;
  if (!d.live_sets.empty()) {
    namespace lsp = velora::liveset::v1;
    std::map<std::string, const BuiltLibrary*> by_id;
    for (const BuiltLibrary& b : built) by_id[b.row.library_id] = &b;
    lsp::FactoryLiveSets payload;
    payload.set_format_version(hf::kFormatVersion);
    payload.set_release_name(d.name);
    payload.set_release_version(d.version);
    payload.set_root(cp::to_string(d.root));
    for (const LiveSetSpec& spec : d.live_sets) {
      lsp::LiveSet l = ls::make_default_live_set(hf::derive_live_set_id(spec.program_id), spec.name, ls::kV1PartCount);
      l.set_tempo(spec.tempo);
      for (const PartSpec& ps : spec.parts) {
        const std::string where = "program " + std::to_string(spec.program_id) + " part " + std::to_string(ps.part);
        if (ps.part >= ls::kV1PartCount) return bail(fail(Code::kInvalidLiveSet, where + ": a v1 Live Set has " + std::to_string(ls::kV1PartCount) + " parts"));
        if (ps.has_timbre) {
          auto it = by_id.find(ps.library_id);
          if (it == by_id.end()) return bail(fail(Code::kUnresolvedReference, where + ": library " + ps.library_id + " is not part of the release"));
          bool found = false;
          for (const InstrumentRow& ir : it->second->row.instruments) found = found || ir.key == ps.instrument_key;
          if (!found) return bail(fail(Code::kUnresolvedReference, where + ": library " + ps.library_id + " has no instrument '" + ps.instrument_key + "'"));
        }
        apply_part(ps, l.mutable_parts(static_cast<int>(ps.part)));
      }
      for (const EngineParam& e : spec.master) {
        lsp::ParamEntry* pe = l.add_master();
        pe->set_param_id(e.param_id);
        pe->set_value(e.value);
      }
      l.mutable_factory()->set_program_id(spec.program_id);
      l.mutable_factory()->set_release_version(d.version);
      ls::normalize(l);
      const ls::ValidationReport vr = ls::validate(l, ls::kV1PartCount);
      if (!vr.ok()) {
        const ls::Issue& is = vr.errors.front();
        return bail(fail(Code::kInvalidLiveSet, "program " + std::to_string(spec.program_id) + " (" + spec.name + "): issue " + std::to_string(static_cast<int>(is.code)) +
                                                    " (part " + std::to_string(is.part) + ", index " + std::to_string(is.index) + ") -- see engine/host/README.md, Live Set validation"));
      }
      lsp::FactoryLiveSetEntry* e = payload.add_entries();
      e->set_program_id(spec.program_id);
      *e->mutable_live_set() = l;
      rep.programs.push_back({spec.program_id, spec.name, l.id(), entry_content_sha(*e)});
    }
    std::string bytes;
    if (!hf::serialize_payload(payload, bytes)) return bail(fail(Code::kInternal, "cannot serialise the factory Live Sets"));
    if (bytes.size() > hf::kMaxPayloadBytes) return bail(fail(Code::kInvalidLiveSet, "the factory Live Sets are larger than the container allows"));
    const std::vector<std::uint8_t> stmt = hf::signing_statement(signer, bytes);
    auto sig = fmt::ed25519_sign(*req.signing_seed, stmt);
    if (!sig) return bail(fail(Code::kBundleFailed, "cannot sign the factory Live Sets"));
    const std::string file = hf::assemble_container(signer, sig.value(), bytes);
    live_sets_rel = std::string(kUnitDir) + "/" + d.live_set_file;
    if (Failure f = write_file((stage / live_sets_rel).string(), file, 0644)) return bail(f);
  }

  // 3. The index: every file of the release (not the index and its signature) with size and SHA-256, the libraries, the program id table.
  struct FileRow {
    std::string path;
    std::uint64_t size;
    std::string sha;
  };
  std::vector<FileRow> files;
  for (fs::recursive_directory_iterator it(stage, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    FileRow r;
    r.path = it->path().lexically_relative(stage).generic_string();
    r.sha = sha256_file_hex(it->path().string(), &r.size);
    if (r.sha.empty()) return bail(fail(Code::kOutputFailed, r.path + ": cannot be hashed"));
    files.push_back(std::move(r));
  }
  if (ec) return bail(fail(Code::kOutputFailed, stage.string() + ": " + ec.message()));
  std::sort(files.begin(), files.end(), [](const FileRow& a, const FileRow& b) { return a.path < b.path; });

  std::string j = "{\n  \"schema\": " + json_str(kIndexSchema) + ",\n  \"tool\": " + json_str(std::string(kToolName) + " " + kToolVersion) + ",\n  \"release\": {\n    \"name\": " +
                  json_str(d.name) + ",\n    \"version\": " + json_str(d.version) + ",\n    \"root\": " + json_str(cp::to_string(d.root)) + "\n  },\n  \"signer_key_id\": " +
                  json_str(hex(signer)) + ",\n  \"kek_id\": " + json_str(rep.kek_id) + ",\n  \"libraries\": [";
  for (std::size_t i = 0; i < built.size(); ++i) {
    const BuiltLibrary& b = built[i];
    j += std::string(i ? "," : "") + "\n    {\n      \"library_id\": " + json_str(b.row.library_id) + ",\n      \"name\": " + json_str(b.row.name) +
         ",\n      \"revision\": " + std::to_string(b.row.revision) + ",\n      \"bundle_id\": " + json_str(b.row.bundle_id) + ",\n      \"files\": {\n        \"bundle\": " +
         json_str(b.bundle_rel) + ",\n        \"catalog_meta\": " + json_str(b.meta_rel) + ",\n        \"catalog_meta_seal\": " + json_str(b.seal_rel) +
         ",\n        \"report\": " + json_str(b.report_rel) + ",\n        \"vault\": " + json_str(b.vault_rel) + "\n      },\n      \"instruments\": [";
    for (std::size_t k = 0; k < b.row.instruments.size(); ++k) {
      const InstrumentRow& ir = b.row.instruments[k];
      j += std::string(k ? "," : "") + "\n        {\"key\": " + json_str(ir.key) + ", \"name\": " + json_str(ir.name) + ", \"category\": " + json_str(ir.category) + "}";
    }
    j += "\n      ]\n    }";
  }
  j += "\n  ],\n  \"live_sets\": ";
  if (live_sets_rel.empty()) {
    j += "null";
  } else {
    j += "{\n    \"file\": " + json_str(live_sets_rel) + ",\n    \"programs\": [";
    for (std::size_t i = 0; i < rep.programs.size(); ++i)
      j += std::string(i ? "," : "") + "\n      {\"program_id\": " + std::to_string(rep.programs[i].program_id) + ", \"name\": " + json_str(rep.programs[i].name) +
           ", \"live_set_id\": " + json_str(rep.programs[i].live_set_id) + ", \"sha256\": " + json_str(rep.programs[i].sha256) + "}";
    j += "\n    ]\n  }";
  }
  j += ",\n  \"files\": [";
  for (std::size_t i = 0; i < files.size(); ++i)
    j += std::string(i ? "," : "") + "\n    {\"path\": " + json_str(files[i].path) + ", \"size\": " + std::to_string(files[i].size) + ", \"sha256\": " + json_str(files[i].sha) + "}";
  j += "\n  ]\n}\n";

  const fmt::Digest256 idx_hash = fmt::Sha256::hash(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(j.data()), j.size()));
  std::vector<LibraryRow> built_rows;
  for (const BuiltLibrary& b : built) built_rows.push_back(b.row);
  // Release history: the previous release's index (optional) and an existing release of the same version.
  if (!req.previous_index_path.empty()) {
    std::string ptext, perr;
    fmt::JsonValue prev;
    if (!fmt::read_text_file(req.previous_index_path, ptext, &perr) || !fmt::parse_json(ptext, prev, &perr) || !prev.is_object() || !prev.find("release") || !prev.find("release")->is_object() ||
        !prev.find("release")->find("version") || !prev.find("release")->find("version")->is_string() || !hf::valid_release_version(prev.find("release")->find("version")->string))
      return bail(fail(Code::kBadDescription, req.previous_index_path + ": not a release index"));
    const std::string pver = prev.find("release")->find("version")->string;
    const bool newer = hf::compare_release_versions(d.version, pver) > 0;
    if (hf::compare_release_versions(d.version, pver) < 0) return bail(fail(Code::kProgramHistory, "release " + d.version + " is older than the previous release " + pver));
    std::map<std::uint32_t, std::string> prev_programs;
    if (const fmt::JsonValue* lsj = prev.find("live_sets"); lsj && lsj->is_object() && lsj->find("programs") && lsj->find("programs")->is_array())
      for (const fmt::JsonValue& pv : lsj->find("programs")->array)
        if (pv.find("program_id") && pv.find("program_id")->is_number()) prev_programs[static_cast<std::uint32_t>(pv.find("program_id")->number)] = pv.find("sha256") && pv.find("sha256")->is_string() ? pv.find("sha256")->string : "";
    for (const auto& [pid, psha] : prev_programs) {
      const ProgramRow* now = nullptr;
      for (const ProgramRow& r : rep.programs) now = r.program_id == pid ? &r : now;
      if (!now) return bail(fail(Code::kProgramHistory, "program id " + std::to_string(pid) + " of release " + pver + " is missing: a program id is never removed or reused"));
      if (!psha.empty() && psha != now->sha256 && !newer) return bail(fail(Code::kProgramHistory, "program " + std::to_string(pid) + " changed but the release version was not raised above " + pver));
    }
    if (const fmt::JsonValue* pl = prev.find("libraries"); pl && pl->is_array()) {
      for (const fmt::JsonValue& pv : pl->array) {
        const fmt::JsonValue* id = pv.find("library_id");
        const fmt::JsonValue* rv = pv.find("revision");
        const fmt::JsonValue* bid = pv.find("bundle_id");
        if (!id || !id->is_string() || !rv || !rv->is_number() || !bid || !bid->is_string()) continue;
        for (const LibraryRow& l : built_rows)
          if (l.library_id == id->string && l.bundle_id != bid->string && l.revision <= static_cast<std::uint32_t>(rv->number))
            return bail(fail(Code::kProgramHistory, "library " + l.library_id + " changed but its revision (" + std::to_string(l.revision) + ") was not raised above " + std::to_string(static_cast<std::uint32_t>(rv->number))));
      }
    }
  }
  if (fs::exists(final_dir / kIndexFileName, ec)) {
    const std::string old_sha = sha256_file_hex((final_dir / kIndexFileName).string());
    if (old_sha != hex(idx_hash)) return bail(fail(Code::kOutputFailed, final_dir.string() + ": a release of this version exists with a different index; raise release.version instead of overwriting it"));
  }
  std::vector<std::uint8_t> msg;
  msg.insert(msg.end(), kIndexSigDomain, kIndexSigDomain + std::strlen(kIndexSigDomain));
  msg.push_back(0);
  msg.insert(msg.end(), signer.begin(), signer.end());
  msg.insert(msg.end(), idx_hash.begin(), idx_hash.end());
  auto isig = fmt::ed25519_sign(*req.signing_seed, msg);
  if (!isig) return bail(fail(Code::kBundleFailed, "cannot sign the release index"));
  std::string sigfile = "VRIS";
  sigfile.push_back(1);
  sigfile.push_back(1);
  sigfile.push_back(0);
  sigfile.push_back(0);
  sigfile.append(reinterpret_cast<const char*>(signer.data()), signer.size());
  sigfile.append(reinterpret_cast<const char*>(isig.value().data()), isig.value().size());
  if (Failure f = write_file((stage / kIndexFileName).string(), j, 0644)) return bail(f);
  if (Failure f = write_file((stage / kIndexSigFileName).string(), sigfile, 0644)) return bail(f);
  for (const char* sub : {"unit/libraries", "unit", "registry-vault", "."})
    if (!fsync_dir((stage / sub).string())) return bail(fail(Code::kOutputFailed, std::string(sub) + ": fsync failed"));
  rep.files = files.size();
  rep.index_sha256 = hex(idx_hash);

  // 3b. The register: only now, with everything validated and written, do the new ids take their entries (a failure here publishes nothing; ids appended before
  //     a failure stay assigned -- the register is append-only and an id once handed out is never reused).
  if (req.register_new) {
    for (const LibraryEntry& le : d.libraries) {
      if (cp::is_dev_range_id(le.pack.library_uuid)) continue;
      if (const cp::Failure rf = cp::append_register(req.register_path, le.pack, req.owner, req.date)) return bail(from_pack(rf));
    }
  }

  // 4. Publish: an existing release directory is replaced only when every vault entry in it is byte-identical to the new one (the keys survive); otherwise it is
  //    kept as .old-<version>.<time>.<pid>, because the only copy of a content key must never be deleted by a rebuild.
  std::string aside;
  bool keep_aside = false;
  if (fs::exists(fs::symlink_status(final_dir, ec))) {
    for (const BuiltLibrary& b : built) {
      const fs::path oldv = final_dir / b.vault_rel;
      if (!fs::exists(oldv, ec)) continue;
      if (sha256_file_hex(oldv.string()) != sha256_file_hex((stage / b.vault_rel).string())) keep_aside = true;
    }
    {  // a vault of a library that this release no longer has is also a key that must not be lost
      std::set<std::string> now;
      for (const BuiltLibrary& b : built) now.insert(b.row.library_id);
      fs::path ov = final_dir / kRegistryVaultDir;
      if (fs::is_directory(ov, ec))
        for (const auto& e : fs::directory_iterator(ov, ec))
          if (!now.count(e.path().filename().string())) keep_aside = true;
    }
    aside = (out / (keep_aside ? ".old-" + d.version + "." + std::to_string(std::time(nullptr)) + "." + std::to_string(::getpid()) : ".old-" + d.version + "." + std::to_string(::getpid()))).string();
    if (std::rename(final_dir.c_str(), aside.c_str()) != 0) return bail(fail(Code::kOutputFailed, final_dir.string() + ": cannot move the previous release aside"));
    rep.kept_old = keep_aside ? aside : std::string();
  }
  if (std::rename(stage.c_str(), final_dir.c_str()) != 0) {
    const bool restored = aside.empty() || std::rename(aside.c_str(), final_dir.c_str()) == 0;
    cleanup();
    return fail(Code::kOutputFailed, final_dir.string() + ": cannot move the release into place" + (restored ? "" : "; ALSO could not restore the previous release from " + aside));
  }
  if (!fsync_dir(out.string())) {
    std::error_code e;
    fs::remove_all(final_dir, e);
    const bool restored = aside.empty() || std::rename(aside.c_str(), final_dir.c_str()) == 0;
    return fail(Code::kOutputFailed, out.string() + ": fsync failed after publishing the release" + (restored ? "" : "; ALSO could not restore the previous release from " + aside));
  }
  if (!aside.empty() && !keep_aside) fs::remove_all(aside, ec);
  rep.release_dir = final_dir.string();
  for (BuiltLibrary& b : built) rep.libraries.push_back(std::move(b.row));
  return {};
}

// ======================================================================================================================================================
// verify
// ======================================================================================================================================================
namespace {

constexpr std::size_t kMaxProblems = 200;

struct Ctx {
  VerifyReport& rep;
  void add(std::string code, std::string path, std::string detail = {}) {
    if (rep.problems.size() < kMaxProblems) rep.problems.push_back({std::move(code), std::move(path), std::move(detail)});
  }
};

const fmt::JsonValue* member(const fmt::JsonValue& o, const char* k, fmt::JsonValue::Type t) {
  const fmt::JsonValue* v = o.is_object() ? o.find(k) : nullptr;
  return v && v->type == t ? v : nullptr;
}

bool safe_rel_path(const std::string& p) {
  if (p.empty() || p.size() > 512 || p[0] == '/' || p.back() == '/') return false;
  std::size_t start = 0;
  while (start <= p.size()) {
    std::size_t e = p.find('/', start);
    if (e == std::string::npos) e = p.size();
    const std::string comp = p.substr(start, e - start);
    if (comp.empty() || comp == "." || comp == "..") return false;
    start = e + 1;
  }
  for (unsigned char c : p)
    if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '/')) return false;
  return true;
}

}  // namespace

Failure verify_release(const std::string& release_dir, const VerifyOptions& opt, VerifyReport& report) {
  report = VerifyReport{};
  Ctx cx{report};
  if (!opt.trusted) return fail(Code::kInternal, "verify_release needs trusted keys");
  const fs::path root(release_dir);
  auto done = [&]() -> Failure {
    if (report.ok()) return {};
    return fail(Code::kVerifyFailed, report.problems.front().code + (report.problems.front().path.empty() ? "" : ": " + report.problems.front().path) +
                                         (report.problems.front().detail.empty() ? "" : " (" + report.problems.front().detail + ")") +
                                         (report.problems.size() > 1 ? " [+" + std::to_string(report.problems.size() - 1) + " more]" : ""));
  };

  // 1. The index and its signature.
  std::string index_text, sig_bytes;
  if (!read_all((root / kIndexFileName).string(), index_text, 4u << 20)) {
    cx.add("index_missing", kIndexFileName);
    return done();
  }
  if (!read_all((root / kIndexSigFileName).string(), sig_bytes, 1024)) {
    cx.add("index_missing", kIndexSigFileName);
    return done();
  }
  if (sig_bytes.size() != kIndexSigSize || std::memcmp(sig_bytes.data(), "VRIS", 4) != 0 || sig_bytes[4] != 1 || sig_bytes[5] != 1 || sig_bytes[6] != 0 || sig_bytes[7] != 0) {
    cx.add("index_sig_malformed", kIndexSigFileName);
    return done();
  }
  fmt::KeyId signer{};
  fmt::Signature sig{};
  std::memcpy(signer.data(), sig_bytes.data() + 8, 8);
  std::memcpy(sig.data(), sig_bytes.data() + 16, 64);
  report.signer_key_id = hex(signer);
  const fmt::TrustedKey* tk = opt.trusted->find(signer);
  if (!tk) {
    cx.add("index_untrusted_key", kIndexSigFileName, "signer " + report.signer_key_id);
    return done();
  }
  {
    const fmt::Digest256 h = fmt::Sha256::hash(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(index_text.data()), index_text.size()));
    std::vector<std::uint8_t> msg;
    msg.insert(msg.end(), kIndexSigDomain, kIndexSigDomain + std::strlen(kIndexSigDomain));
    msg.push_back(0);
    msg.insert(msg.end(), signer.begin(), signer.end());
    msg.insert(msg.end(), h.begin(), h.end());
    if (!fmt::ed25519_verify_strict(tk->pub, msg, sig)) {
      cx.add("index_signature_invalid", kIndexFileName);
      return done();
    }
  }
  fmt::JsonValue idx;
  std::string jerr;
  if (!fmt::parse_json(index_text, idx, &jerr) || !idx.is_object()) {
    cx.add("index_malformed", kIndexFileName, jerr);
    return done();
  }
  const fmt::JsonValue* schema = member(idx, "schema", fmt::JsonValue::Type::kString);
  const fmt::JsonValue* rel = member(idx, "release", fmt::JsonValue::Type::kObject);
  const fmt::JsonValue* files = member(idx, "files", fmt::JsonValue::Type::kArray);
  const fmt::JsonValue* libs = member(idx, "libraries", fmt::JsonValue::Type::kArray);
  const fmt::JsonValue* idx_signer = member(idx, "signer_key_id", fmt::JsonValue::Type::kString);
  if (!schema || schema->string != kIndexSchema || !rel || !files || !libs || !idx_signer) {
    cx.add("index_malformed", kIndexFileName, "missing or unknown fields");
    return done();
  }
  if (idx_signer->string != report.signer_key_id) cx.add("index_malformed", kIndexFileName, "signer_key_id is not the key that signed the index");
  if (const auto* n = member(*rel, "name", fmt::JsonValue::Type::kString)) report.release_name = n->string;
  if (const auto* n = member(*rel, "version", fmt::JsonValue::Type::kString)) report.release_version = n->string;
  if (const auto* n = member(*rel, "root", fmt::JsonValue::Type::kString)) report.root = n->string;
  if (!tk->root.empty() && report.root != tk->root) cx.add("index_root_mismatch", kIndexFileName, "the signer is trusted for root " + tk->root + ", the release says " + report.root);

  // 2. Every file of the index: present, a regular file, size, SHA-256. Then nothing else may be in the directory.
  std::set<std::string> listed;
  std::map<std::string, std::string> sha_of;
  for (const fmt::JsonValue& f : files->array) {
    const auto* p = member(f, "path", fmt::JsonValue::Type::kString);
    const auto* sz = member(f, "size", fmt::JsonValue::Type::kNumber);
    const auto* sh = member(f, "sha256", fmt::JsonValue::Type::kString);
    if (!p || !sz || !sh) {
      cx.add("index_malformed", kIndexFileName, "a files entry lacks path, size or sha256");
      continue;
    }
    if (!safe_rel_path(p->string)) {
      cx.add("bad_path", p->string);
      continue;
    }
    {
      const std::string& q = p->string;
      const bool in_unit = q.rfind("unit/", 0) == 0, in_vault = q.rfind("registry-vault/", 0) == 0;
      const bool vault_name = q.size() >= 18 && q.compare(q.size() - 18, 18, "/content-key.vault") == 0;
      if (!in_unit && !in_vault) cx.add("layout_invalid", q, "a release holds only unit/ and registry-vault/");
      else if (in_unit && (vault_name || q == "unit/content-key.vault")) cx.add("layout_invalid", q, "a vault entry under unit/ would ship with the image");
      else if (in_vault) {
        fmt::Uuid u{};
        const std::string rest = q.substr(15);
        const std::size_t sl = rest.find('/');
        if (sl == std::string::npos || rest.substr(sl) != "/content-key.vault" || !fmt::uuid_from_string(rest.substr(0, sl), u) || fmt::uuid_to_string(u) != rest.substr(0, sl))
          cx.add("layout_invalid", q, "registry-vault/ holds <library_id>/content-key.vault only");
      }
    }
    if (!listed.insert(p->string).second) {
      cx.add("index_malformed", p->string, "listed twice");
      continue;
    }
    sha_of[p->string] = sh->string;
    const fs::path fp = root / p->string;
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(fp, ec);
    if (!fs::exists(st)) {
      cx.add("file_missing", p->string);
      continue;
    }
    if (!fs::is_regular_file(st)) {
      cx.add("file_not_regular", p->string);
      continue;
    }
    std::uint64_t size = 0;
    const std::string h = sha256_file_hex(fp.string(), &size);
    ++report.files_checked;
    if (h.empty()) cx.add("file_unreadable", p->string);
    else if (static_cast<double>(size) != sz->number) cx.add("size_mismatch", p->string, std::to_string(size) + " bytes, the index says " + std::to_string(static_cast<std::uint64_t>(sz->number)));
    else if (h != sh->string) cx.add("hash_mismatch", p->string);
  }
  {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string rp = it->path().lexically_relative(root).generic_string();
      const fs::file_status st = it->symlink_status(ec);
      if (fs::is_directory(st)) continue;
      if (listed.count(rp)) continue;  // an indexed path that is not a regular file was reported above (file_not_regular)
      if (!fs::is_regular_file(st)) {
        cx.add("unexpected_entry", rp);
        continue;
      }
      if (rp == kIndexFileName || rp == kIndexSigFileName) continue;
      if (!listed.count(rp)) cx.add("unexpected_file", rp);
    }
  }

  // 3. Libraries: their files are in the index; optionally re-verify each bundle and its catalog seal with the verify-only library.
  std::set<std::string> lib_ids;
  std::map<std::string, std::string> bundle_ids;
  for (const fmt::JsonValue& l : libs->array) {
    const auto* id = member(l, "library_id", fmt::JsonValue::Type::kString);
    const auto* bid = member(l, "bundle_id", fmt::JsonValue::Type::kString);
    const auto* revn = member(l, "revision", fmt::JsonValue::Type::kNumber);
    const auto* lf = member(l, "files", fmt::JsonValue::Type::kObject);
    const auto* insts = member(l, "instruments", fmt::JsonValue::Type::kArray);
    if (!id || !bid || !revn || !lf || !insts) {
      cx.add("index_malformed", kIndexFileName, "a libraries entry lacks library_id, bundle_id, revision, files or instruments");
      continue;
    }
    lib_ids.insert(id->string);
    bundle_ids[id->string] = bid->string;
    std::string paths[5];
    const char* names[5] = {"bundle", "catalog_meta", "catalog_meta_seal", "report", "vault"};
    bool complete = true;
    const std::string expect[5] = {"unit/libraries/" + id->string + "/bundle.vlrp", "unit/libraries/" + id->string + "/catalog.meta", "unit/libraries/" + id->string + "/catalog.meta.sig",
                                   "unit/libraries/" + id->string + "/pack.json", "registry-vault/" + id->string + "/content-key.vault"};
    for (int k = 0; k < 5; ++k) {
      const auto* pv = member(*lf, names[k], fmt::JsonValue::Type::kString);
      if (!pv || !listed.count(pv->string)) {
        cx.add("library_file_not_in_index", id->string, names[k]);
        complete = false;
      } else {
        paths[k] = pv->string;
        if (pv->string != expect[k]) cx.add("layout_invalid", pv->string, std::string(names[k]) + " of library " + id->string + " must be " + expect[k]);
      }
    }
    if (!complete || !opt.check_bundles) continue;
    const std::string bpath = (root / paths[0]).string();
    auto view = fmt::ProtectedBundleView::open_file(bpath, *opt.trusted);
    if (!view) {
      cx.add("bundle_invalid", paths[0], fmt::to_string(view.error()));
      continue;
    }
    ++report.bundles_checked;
    const fmt::ProtectedInfo& info = view.value().info();
    if (fmt::uuid_to_string(info.library_id) != id->string) cx.add("bundle_identity_mismatch", paths[0], "library_id");
    if (hex(info.bundle_id) != bid->string) cx.add("bundle_identity_mismatch", paths[0], "bundle_id");
    if (const fmt::Error e = view.value().verify_all_blocks(); e != fmt::Error::kNone) cx.add("bundle_block_invalid", paths[0], fmt::to_string(e));
    std::set<std::string> want, got;
    for (const fmt::JsonValue& iv : insts->array)
      if (const auto* k = member(iv, "key", fmt::JsonValue::Type::kString)) want.insert(k->string);
    for (const fmt::InstrumentInfo& ii : view.value().instruments()) got.insert(ii.key);
    if (want != got) cx.add("bundle_instruments_mismatch", paths[0]);
    std::string meta, seal;
    if (!read_all((root / paths[1]).string(), meta, 4u << 20) || !read_all((root / paths[2]).string(), seal, 1024)) {
      cx.add("bundle_meta_unreadable", paths[1]);
      continue;
    }
    std::uint32_t rv = 0;
    fmt::KeyId seal_signer{};
    const fmt::Error se = fmt::verify_catalog_meta_seal(*opt.trusted, info.bundle_id, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(meta.data()), meta.size()),
                                                        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(seal.data()), seal.size()), &seal_signer, &rv);
    if (se != fmt::Error::kNone) cx.add("bundle_meta_seal_invalid", paths[2], fmt::to_string(se));
    else if (static_cast<double>(rv) != revn->number) cx.add("bundle_revision_mismatch", paths[2]);
    else if (seal_signer != signer) cx.add("bundle_meta_seal_invalid", paths[2], "another signer than the index");
  }

  // 4. The factory Live Sets: the container verifies against the trusted keys and says what the index says.
  const fmt::JsonValue* ls_obj = idx.find("live_sets");
  if (ls_obj && ls_obj->is_object()) {
    const auto* lf = member(*ls_obj, "file", fmt::JsonValue::Type::kString);
    const auto* progs = member(*ls_obj, "programs", fmt::JsonValue::Type::kArray);
    if (!lf || !progs || !listed.count(lf->string)) {
      cx.add("live_sets_index_malformed", kIndexFileName);
    } else {
      std::string bytes;
      if (!read_all((root / lf->string).string(), bytes, hf::kMaxFileBytes)) {
        cx.add("live_sets_unreadable", lf->string);
      } else {
        velora::liveset::v1::FactoryLiveSets sets;
        fmt::KeyId csigner{};
        const hf::ContainerStatus cs = hf::verify_container(bytes, *opt.trusted, sets, &csigner);
        if (cs != hf::ContainerStatus::kOk) {
          cx.add("live_sets_invalid", lf->string, hf::to_string(cs));
        } else {
          report.live_sets = static_cast<std::size_t>(sets.entries_size());
          if (csigner != signer) cx.add("live_sets_invalid", lf->string, "another signer than the index");
          if (sets.release_version() != report.release_version || sets.release_name() != report.release_name || sets.root() != report.root)
            cx.add("live_sets_invalid", lf->string, "the release identity differs from the index");
          std::map<std::uint32_t, std::string> want, want_sha;
          for (const fmt::JsonValue& pv : progs->array) {
            const auto* pid = member(pv, "program_id", fmt::JsonValue::Type::kNumber);
            const auto* lid = member(pv, "live_set_id", fmt::JsonValue::Type::kString);
            if (pid && lid) want[static_cast<std::uint32_t>(pid->number)] = lid->string;
            if (pid && member(pv, "sha256", fmt::JsonValue::Type::kString)) want_sha[static_cast<std::uint32_t>(pid->number)] = pv.find("sha256")->string;
          }
          std::map<std::uint32_t, std::string> have;
          for (const auto& e : sets.entries()) have[e.program_id()] = e.live_set().id();
          if (want != have) cx.add("live_sets_invalid", lf->string, "the program id table differs from the index");
          for (const auto& e : sets.entries())
            if (want_sha.count(e.program_id()) && want_sha[e.program_id()] != entry_content_sha(e)) cx.add("live_sets_invalid", lf->string, "program " + std::to_string(e.program_id()) + " differs from the index");
          // every reference is to a library of the release
          for (const auto& e : sets.entries())
            for (const auto& p : e.live_set().parts())
              if (p.has_timbre() && !lib_ids.count(p.timbre().library_id()))
                cx.add("live_sets_invalid", lf->string, "program " + std::to_string(e.program_id()) + " names a library that is not in the release");
        }
      }
    }
  }
  return done();
}

}  // namespace velora::factoryrelease
