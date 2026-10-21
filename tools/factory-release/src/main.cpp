// velora-factory-release: packs the founder's factory library and the factory Live Sets into one signed, versioned, reproducible release (Story 6.9).
// See README.md for the release description, the output layout, the founder's delivery layout, the checklist and the exit codes.
#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "key_files.hpp"
#include "release.hpp"
#include "velora/format/protected_verify.hpp"
#include "velora/host/trust_list.hpp"

#ifndef VELORA_KEYS_DEV_TRUST_LIST
#define VELORA_KEYS_DEV_TRUST_LIST ""
#endif
#ifndef VELORA_CONTENT_PACK_REGISTER
#define VELORA_CONTENT_PACK_REGISTER "vendor-ids"
#endif

using namespace velora::factoryrelease;
namespace cp = velora::contentpack;
namespace fmt = velora::format;

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: velora-factory-release build --description FILE --out DIR [--keys-dir DIR] [--signing-key FILE] [--kek FILE] [--reuse-vaults DIR]\n"
               "                                    [--allow-new-keys] [--previous-index FILE] [--register --owner NAME] [--register-file FILE] [--date YYYY-MM-DD] [--dev-trust-list FILE] [--overwrite]\n"
               "       velora-factory-release check --description FILE\n"
               "       velora-factory-release verify RELEASE_DIR (--trust-list FILE | --trusted-pub FILE) [--bundles]\n"
               "exit codes: 0 ok, 2 usage, 3 bad_description, 4 bad_key_file, 5 root_mismatch, 6 library_id_conflict, 7 library_id_unregistered, 8 register_io,\n"
               "            9 import_skipped, 10 import_failed, 11 bundle_failed, 12 vault_failed, 13 output_failed, 14 verify_failed,\n"
               "            20 duplicate_library, 21 duplicate_program_id, 22 missing_category, 23 unresolved_reference, 24 invalid_live_set, 25 program_history, 70 internal\n");
  return static_cast<int>(Code::kUsage);
}

int report(const Failure& f) {
  std::fprintf(stderr, "error: %s: %s\n", to_string(f.code), f.message.c_str());
  return static_cast<int>(f.code);
}

std::string today() {
  const std::time_t t = std::time(nullptr);
  char b[16];
  std::tm tm{};
  localtime_r(&t, &tm);
  std::strftime(b, sizeof b, "%Y-%m-%d", &tm);
  return b;
}

struct Args {
  std::string cmd, description, out, keys_dir, signing_key, kek, reuse_vaults, previous_index, register_file = VELORA_CONTENT_PACK_REGISTER, owner, date, release_dir, trust_list, trusted_pub;
  std::string dev_trust_list = VELORA_KEYS_DEV_TRUST_LIST;
  bool overwrite = false, register_new = false, bundles = false, allow_new_keys = false;
};

bool parse_args(int argc, char** argv, Args& a) {
  if (argc < 2) return false;
  a.cmd = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string s = argv[i];
    auto val = [&](std::string& dst) {
      if (i + 1 >= argc) return false;
      dst = argv[++i];
      return true;
    };
    if (s == "--description") { if (!val(a.description)) return false; }
    else if (s == "--out") { if (!val(a.out)) return false; }
    else if (s == "--keys-dir") { if (!val(a.keys_dir)) return false; }
    else if (s == "--signing-key") { if (!val(a.signing_key)) return false; }
    else if (s == "--kek") { if (!val(a.kek)) return false; }
    else if (s == "--reuse-vaults") { if (!val(a.reuse_vaults)) return false; }
    else if (s == "--register-file") { if (!val(a.register_file)) return false; }
    else if (s == "--owner") { if (!val(a.owner)) return false; }
    else if (s == "--date") { if (!val(a.date)) return false; }
    else if (s == "--dev-trust-list") { if (!val(a.dev_trust_list)) return false; }
    else if (s == "--trust-list") { if (!val(a.trust_list)) return false; }
    else if (s == "--trusted-pub") { if (!val(a.trusted_pub)) return false; }
    else if (s == "--overwrite") a.overwrite = true;
    else if (s == "--register") a.register_new = true;
    else if (s == "--bundles") a.bundles = true;
    else if (s == "--allow-new-keys") a.allow_new_keys = true;
    else if (s == "--previous-index") { if (!val(a.previous_index)) return false; }
    else if (a.cmd == "verify" && a.release_dir.empty() && !s.empty() && s[0] != '-') a.release_dir = s;
    else return false;  // no option takes a key as a value: a key never travels in argv
  }
  return true;
}

int cmd_check(const Args& a) {
  if (a.description.empty()) return usage();
  ReleaseDescription d;
  if (const Failure f = load_release_description(a.description, d)) return report(f);
  std::printf("%s %s (root %s): %zu librar%s, %zu factory Live Set(s): ok\n", d.name.c_str(), d.version.c_str(), cp::to_string(d.root), d.libraries.size(),
              d.libraries.size() == 1 ? "y" : "ies", d.live_sets.size());
  return 0;
}

int cmd_build(const Args& a) {
  if (a.description.empty() || a.out.empty()) return usage();
  if (a.register_new && a.owner.empty()) return usage();
  BuildRequest req;
  if (const Failure f = load_release_description(a.description, req.desc)) return report(f);
  cp::Secret<fmt::SigningSeed> seed;
  cp::Secret<cp::Key32> kek;
  if (const cp::Failure f = cp::load_root_keys(req.desc.root, a.keys_dir, a.signing_key, a.kek, a.dev_trust_list, seed, kek)) return report(from_pack(f));
  req.out_dir = a.out;
  req.overwrite = a.overwrite;
  req.kek = &kek.get();
  req.signing_seed = &seed.get();
  req.reuse_vaults_dir = a.reuse_vaults;
  req.allow_new_keys = a.allow_new_keys;
  req.previous_index_path = a.previous_index;
  req.register_path = a.register_file;
  req.register_new = a.register_new;
  req.owner = a.owner;
  req.date = a.date.empty() ? today() : a.date;
  BuildReport rep;
  if (const Failure f = build_release(req, rep)) return report(f);
  std::printf("built %s %s (root %s)\n  dir       %s\n  signer    %s\n  kek       %s\n  index     sha256 %s (%zu file(s))\n", req.desc.name.c_str(), req.desc.version.c_str(),
              cp::to_string(req.desc.root), rep.release_dir.c_str(), rep.signer_key_id.c_str(), rep.kek_id.c_str(), rep.index_sha256.c_str(), rep.files);
  for (const LibraryRow& l : rep.libraries)
    std::printf("  library   %s  %s  revision %u  bundle %s  (%zu instrument(s))\n", l.library_id.c_str(), l.name.c_str(), l.revision, l.bundle_id.c_str(), l.instruments.size());
  for (const ProgramRow& p : rep.programs) std::printf("  program   %u  %s  (%s)\n", p.program_id, p.name.c_str(), p.live_set_id.c_str());
  if (!rep.kept_old.empty()) std::printf("  kept the previous release (other vault keys) as %s\n", rep.kept_old.c_str());
  std::printf("  hand registry-vault/ to the registry; unit/ is the unit image content. Never ship registry-vault/.\n");
  return 0;
}

int cmd_verify(const Args& a) {
  if (a.release_dir.empty() || (a.trust_list.empty() == a.trusted_pub.empty())) return usage();
  fmt::TrustedKeys trusted;
  if (!a.trust_list.empty()) {
    std::string err;
    if (!velora::host::load_trust_list_file(a.trust_list, trusted, err)) return report(fail(Code::kBadKeyFile, err));
  } else {
    cp::Key32 pub{};
    if (const cp::Failure f = cp::read_key_file(a.trusted_pub, pub.data(), 32, false, "trusted public key")) return report(from_pack(f));
    trusted = fmt::TrustedKeys({fmt::TrustedKey{fmt::key_id_of_public(pub), pub}});
  }
  VerifyOptions vo;
  vo.trusted = &trusted;
  vo.check_bundles = a.bundles;
  VerifyReport rep;
  const Failure f = verify_release(a.release_dir, vo, rep);
  for (const Problem& p : rep.problems) std::fprintf(stderr, "problem: %s%s%s%s%s\n", p.code.c_str(), p.path.empty() ? "" : " ", p.path.c_str(), p.detail.empty() ? "" : " -- ", p.detail.c_str());
  if (f) return report(f);
  std::printf("ok: %s %s (root %s), signer %s, %zu file(s) hashed, %zu bundle(s) re-verified, %zu factory Live Set(s)\n", rep.release_name.c_str(), rep.release_version.c_str(),
              rep.root.c_str(), rep.signer_key_id.c_str(), rep.files_checked, rep.bundles_checked, rep.live_sets);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // No core dumps: the process holds the signing seed, the KEK and content keys.
  struct rlimit rl{0, 0};
  (void)::setrlimit(RLIMIT_CORE, &rl);
#ifdef __linux__
  (void)::prctl(PR_SET_DUMPABLE, 0);
#endif
  Args a;
  if (!parse_args(argc, argv, a)) return usage();
  if (a.cmd == "build") return cmd_build(a);
  if (a.cmd == "verify") return cmd_verify(a);
  if (a.cmd == "check") return cmd_check(a);
  return usage();
}
