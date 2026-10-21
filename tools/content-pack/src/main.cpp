// velora-content-pack: turns a source SFZ/SF2 library into an encrypted, signed protected bundle, vaults its content key and keeps the vendor ID register.
// See README.md for the pack description schema, the output layout and the exit codes.
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <span>
#include <set>
#include <string>
#include <vector>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "key_files.hpp"
#include "pack.hpp"
#include "vendor_register.hpp"
#include "velora/format/json_lite.hpp"
#include "velora/format/protected_writer.hpp"

#ifndef VELORA_KEYS_DEV_TRUST_LIST
#define VELORA_KEYS_DEV_TRUST_LIST ""
#endif
#ifndef VELORA_CONTENT_PACK_REGISTER
#define VELORA_CONTENT_PACK_REGISTER "vendor-ids"
#endif

using namespace velora::contentpack;
namespace fs = std::filesystem;
namespace fmt = velora::format;

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: velora-content-pack pack --description FILE --out DIR [--keys-dir DIR] [--signing-key FILE] [--kek FILE]\n"
               "                                [--device-pub FILE]... [--reuse-vault-entry FILE | --content-key-file FILE] [--deterministic-vault]\n"
               "                                [--register --owner NAME] [--register-file FILE] [--dev-trust-list FILE] [--overwrite] [--revision N]\n"
               "       velora-content-pack register --description FILE --owner NAME [--register-file FILE] [--date YYYY-MM-DD]\n"
               "       velora-content-pack verify --bundle FILE --vault-entry FILE --kek FILE --trusted-pub FILE\n"
               "       velora-content-pack check-register [--register-file FILE]\n"
               "exit codes: 0 ok, 2 usage, 3 bad_description, 4 bad_key_file, 5 root_mismatch, 6 library_id_conflict, 7 library_id_unregistered,\n"
               "            8 register_io, 9 import_skipped, 10 import_failed, 11 bundle_failed, 12 vault_failed, 13 output_failed, 14 verify_failed\n");
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
  std::string cmd, description, out, keys_dir, signing_key, kek, reuse_vault, content_key_file, register_file = VELORA_CONTENT_PACK_REGISTER, owner, date;
  std::string bundle, vault_entry, trusted_pub;
  std::string dev_trust_list = VELORA_KEYS_DEV_TRUST_LIST;
  std::vector<std::string> device_pubs;
  bool deterministic_vault = false, register_new = false, overwrite = false;
  std::uint32_t revision = 1;
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
    else if (s == "--device-pub") { a.device_pubs.emplace_back(); if (!val(a.device_pubs.back())) return false; }
    else if (s == "--reuse-vault-entry") { if (!val(a.reuse_vault)) return false; }
    else if (s == "--content-key-file") { if (!val(a.content_key_file)) return false; }
    else if (s == "--register-file") { if (!val(a.register_file)) return false; }
    else if (s == "--owner") { if (!val(a.owner)) return false; }
    else if (s == "--date") { if (!val(a.date)) return false; }
    else if (s == "--dev-trust-list") { if (!val(a.dev_trust_list)) return false; }
    else if (s == "--bundle") { if (!val(a.bundle)) return false; }
    else if (s == "--vault-entry") { if (!val(a.vault_entry)) return false; }
    else if (s == "--trusted-pub") { if (!val(a.trusted_pub)) return false; }
    else if (s == "--revision") {
      std::string v;
      if (!val(v) || v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos || std::stoul(v) == 0) return false;
      a.revision = static_cast<std::uint32_t>(std::stoul(v));
    }
    else if (s == "--deterministic-vault") a.deterministic_vault = true;
    else if (s == "--register") a.register_new = true;
    else if (s == "--overwrite") a.overwrite = true;
    else return false;  // no option takes a key as a value: a key never travels in argv
  }
  return true;
}

int cmd_pack(const Args& a) {
  if (a.description.empty() || a.out.empty()) return usage();
  if (!a.reuse_vault.empty() && !a.content_key_file.empty()) return usage();
  if (a.register_new && a.owner.empty()) return usage();
  PackRequest req;
  if (Failure f = load_pack_description(a.description, req.desc)) return report(f);
  const Root root = req.desc.root;

  Secret<fmt::SigningSeed> seed;
  Secret<Key32> kek;
  if (Failure f = load_root_keys(root, a.keys_dir, a.signing_key, a.kek, a.dev_trust_list, seed, kek)) return report(f);

  Secret<fmt::ContentKey> reuse;
  if (!a.reuse_vault.empty()) {
    std::uint8_t entry[kVaultEntrySize];
    if (Failure f = read_key_file(a.reuse_vault, entry, sizeof entry, false, "vault entry")) return report(f);
    Key32 out{};
    if (!vault_entry_open(kek.get(), entry, req.desc.library_id, out)) return report(fail(Code::kVaultFailed, a.reuse_vault + ": does not open with this KEK for library " + req.desc.library_id));
    reuse->bytes = out;
    wipe(out.data(), out.size());
    req.content_key = &reuse.get();
  } else if (!a.content_key_file.empty()) {
    if (root != Root::kDev) return report(fail(Code::kUsage, "--content-key-file is accepted for root dev only (reuse a vault entry with --reuse-vault-entry instead)"));
    if (Failure f = read_key_file(a.content_key_file, reuse->bytes.data(), 32, true, "content key")) return report(f);
    req.content_key = &reuse.get();
  }
  for (const std::string& p : a.device_pubs) {
    Key32 pub{};
    if (Failure f = read_key_file(p, pub.data(), 32, false, "device public key")) return report(f);
    req.device_pubs.push_back(pub);
  }
  req.out_dir = a.out;
  req.overwrite = a.overwrite;
  req.revision = a.revision;
  req.kek = &kek.get();
  req.signing_seed = &seed.get();
  req.deterministic_vault = a.deterministic_vault;
  req.register_path = a.register_file;
  req.register_new = a.register_new;
  req.owner = a.owner;
  req.date = a.date.empty() ? today() : a.date;

  PackReport rep;
  if (Failure f = run_pack(req, rep)) return report(f);
  std::printf("packed library %s (%zu instrument(s), %llu block(s))\n  bundle_id %s\n  signer    %s\n  kek       %s\n  dir       %s\n", rep.library_id.c_str(),
              rep.instruments, static_cast<unsigned long long>(rep.blocks), rep.bundle_id.c_str(), rep.signer_key_id.c_str(), rep.kek_id.c_str(), rep.dir.c_str());
  for (const auto& f : rep.files) std::printf("  wrote     %s\n", f.c_str());
  return 0;
}

int cmd_register(const Args& a) {
  if (a.description.empty() || a.owner.empty()) return usage();
  PackDescription d;
  if (Failure f = load_pack_description(a.description, d)) return report(f);
  if (Failure f = append_register(a.register_file, d, a.owner, a.date.empty() ? today() : a.date)) return report(f);
  std::printf("registered %s as '%s' (owner %s) in %s\n", d.library_id.c_str(), d.name.c_str(), a.owner.c_str(), a.register_file.c_str());
  return 0;
}

int cmd_check_register(const Args& a) {
  VendorRegister reg;
  if (Failure f = load_register(a.register_file, reg, true)) return report(f);
  std::printf("%s: ok, %zu entr%s\n", a.register_file.c_str(), reg.entries.size(), reg.entries.size() == 1 ? "y" : "ies");
  return 0;
}

int cmd_verify(const Args& a) {
  if (a.bundle.empty() || a.vault_entry.empty() || a.kek.empty() || a.trusted_pub.empty()) return usage();
  Secret<Key32> kek;
  Key32 pub{};
  std::uint8_t entry[kVaultEntrySize];
  if (Failure f = read_key_file(a.kek, kek.get().data(), 32, true, "KEK")) return report(f);
  if (Failure f = read_key_file(a.trusted_pub, pub.data(), 32, false, "trusted public key")) return report(f);
  if (Failure f = read_key_file(a.vault_entry, entry, sizeof entry, false, "vault entry")) return report(f);
  std::string summary;
  if (Failure f = verify_pack(a.bundle, entry, kek.get(), pub, &summary)) return report(f);
  std::printf("%s\n", summary.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // No core dumps: the process holds the content key, the KEK and the signing seed.
  struct rlimit rl{0, 0};
  (void)::setrlimit(RLIMIT_CORE, &rl);
#ifdef __linux__
  (void)::prctl(PR_SET_DUMPABLE, 0);
#endif
  Args a;
  if (!parse_args(argc, argv, a)) return usage();
  if (a.cmd == "pack") return cmd_pack(a);
  if (a.cmd == "register") return cmd_register(a);
  if (a.cmd == "verify") return cmd_verify(a);
  if (a.cmd == "core-limit") {  // self-check for the tests: prints the core dump limits the process runs with
    struct rlimit got{1, 1};
    (void)::getrlimit(RLIMIT_CORE, &got);
    std::printf("core_limit=%llu/%llu\n", static_cast<unsigned long long>(got.rlim_cur), static_cast<unsigned long long>(got.rlim_max));
    return 0;
  }
  if (a.cmd == "check-register") return cmd_check_register(a);
  return usage();
}
