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

std::string hex_of(std::span<const std::uint8_t> d) { return fmt::to_hex(d); }

int usage() {
  std::fprintf(stderr,
               "usage: velora-content-pack pack --description FILE --out DIR [--keys-dir DIR] [--signing-key FILE] [--kek FILE]\n"
               "                                [--device-pub FILE]... [--reuse-vault-entry FILE | --content-key-file FILE] [--deterministic-vault]\n"
               "                                [--register --owner NAME] [--register-file FILE] [--dev-trust-list FILE] [--overwrite]\n"
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

// Raw key file: a regular file of exactly `n` bytes; a private one must not be readable by group or others.
Failure read_key_file(const std::string& path, std::uint8_t* out, std::size_t n, bool secret, const char* what) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return fail(Code::kBadKeyFile, std::string(what) + " " + path + ": " + std::strerror(errno));
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<std::size_t>(st.st_size) != n) {
    ::close(fd);
    return fail(Code::kBadKeyFile, std::string(what) + " " + path + ": not a regular file of exactly " + std::to_string(n) + " bytes");
  }
  if (secret && (st.st_mode & 077) != 0) {
    ::close(fd);
    return fail(Code::kBadKeyFile, std::string(what) + " " + path + ": permissions are too open (use chmod 600)");
  }
  std::size_t got = 0;
  while (got < n) {
    const ssize_t r = ::read(fd, out + got, n - got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    got += static_cast<std::size_t>(r);
  }
  ::close(fd);
  if (got != n) return fail(Code::kBadKeyFile, std::string(what) + " " + path + ": short read");
  return {};
}

// The root guard. For every key file:
//  * root dev: a sidecar next to it is optional, but when present its root must be dev and its key_id must be the key's real id;
//  * any other root: the sidecar (<name>.json written by tools/keys or the vendor's key ceremony) is REQUIRED and must say the description's root, carry a
//    key_id equal to the key's real id and not be dev_only; a DEV-ONLY.txt marker in the directory is refused.
// A sidecar without a string root is a bad key file. `actual_id` is the lower-case hex id computed from the key itself.
Failure check_root(const std::string& key_path, const char* sidecar, Root want, const std::string& actual_id) {
  const fs::path dir = fs::path(key_path).parent_path();
  std::error_code ec;
  const fs::path side = dir / sidecar;
  const bool has_side = fs::exists(side, ec);
  if (!has_side && want != Root::kDev)
    return fail(Code::kRootMismatch, key_path + ": root " + to_string(want) + " requires " + side.string() + " (root, key_id) next to the key");
  if (has_side) {
    std::string text, err;
    fmt::JsonValue j;
    if (!fmt::read_text_file(side.string(), text, &err) || !fmt::parse_json(text, j, &err) || !j.is_object())
      return fail(Code::kBadKeyFile, side.string() + ": unreadable key sidecar");
    const fmt::JsonValue* r = j.find("root");
    if (!r || !r->is_string()) return fail(Code::kBadKeyFile, side.string() + ": the sidecar has no string root");
    if (r->string != to_string(want))
      return fail(Code::kRootMismatch, key_path + " belongs to root " + r->string + " but the description says root " + to_string(want));
    const fmt::JsonValue* dev = j.find("dev_only");
    if (want != Root::kDev && dev && dev->type == fmt::JsonValue::Type::kBool && dev->boolean)
      return fail(Code::kRootMismatch, key_path + " is marked dev_only");
    const fmt::JsonValue* kid = j.find("key_id");
    if (kid && !kid->is_string()) return fail(Code::kBadKeyFile, side.string() + ": key_id must be a string");
    if (!kid && want != Root::kDev) return fail(Code::kRootMismatch, side.string() + ": root " + to_string(want) + " requires a key_id");
    if (kid && kid->string != actual_id)
      return fail(Code::kRootMismatch, side.string() + ": key_id " + kid->string + " is not the id of " + key_path + " (" + actual_id + ")");
  }
  if (want != Root::kDev && fs::exists(dir / "DEV-ONLY.txt", ec))
    return fail(Code::kRootMismatch, key_path + " is in a DEV-ONLY key directory and cannot be used for root " + to_string(want));
  return {};
}

// Ids of the dev root's keys: the committed trust list (tools/keys/trust/dev.pub-list) and the developer's own dev key directory. A non-dev root never
// accepts a key with one of these ids, whatever its sidecar says.
struct DevIds {
  std::set<std::string> signing, kek;
};
DevIds dev_ids(const std::string& trust_list, const std::string& dev_dir);

std::string default_dev_dir() {
  if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/velora-dev-keys";
  if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.velora-dev-keys";
  return {};
}

DevIds dev_ids(const std::string& trust_list, const std::string& dev_dir) {
  DevIds ids;
  std::string text, err;
  if (!trust_list.empty() && fmt::read_text_file(trust_list, text, &err)) {
    std::size_t pos = 0;
    while (pos < text.size()) {
      std::size_t nl = text.find('\n', pos);
      if (nl == std::string::npos) nl = text.size();
      std::string line = text.substr(pos, nl - pos);
      pos = nl + 1;
      if (const std::size_t h = line.find('#'); h != std::string::npos) line.resize(h);
      std::vector<std::string> t;
      std::size_t b = 0;
      while (b < line.size()) {
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t' || line[b] == '\r')) ++b;
        std::size_t e = b;
        while (e < line.size() && line[e] != ' ' && line[e] != '\t' && line[e] != '\r') ++e;
        if (e > b) t.push_back(line.substr(b, e - b));
        b = e;
      }
      if (t.size() >= 4 && t[0] == "dev") ids.signing.insert(t[2]);
    }
  }
  if (!dev_dir.empty()) {
    Key32 pub{};
    if (!read_key_file(dev_dir + "/content-manifest.ed25519.pub", pub.data(), 32, false, "dev public key")) ids.signing.insert(hex_of(velora::format::key_id_of_public(pub)));
    Secret<Key32> kek;
    if (!read_key_file(dev_dir + "/kek.aes256", kek.get().data(), 32, true, "dev KEK")) ids.kek.insert(hex_of(kek_id(kek.get())));
  }
  return ids;
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

  std::string seed_path = a.signing_key, kek_path = a.kek;
  std::string dir = a.keys_dir;
  if (dir.empty() && root == Root::kDev && (seed_path.empty() || kek_path.empty())) dir = default_dev_dir();
  if (seed_path.empty() && !dir.empty()) seed_path = dir + "/content-manifest.ed25519.seed";
  if (kek_path.empty() && !dir.empty()) kek_path = dir + "/kek.aes256";
  if (seed_path.empty() || kek_path.empty())
    return report(fail(Code::kBadKeyFile, std::string("root ") + to_string(root) + " needs --keys-dir, or both --signing-key and --kek"));
  Secret<fmt::SigningSeed> seed;
  Secret<Key32> kek;
  if (Failure f = read_key_file(seed_path, seed->bytes.data(), 32, true, "signing key")) return report(f);
  if (Failure f = read_key_file(kek_path, kek.get().data(), 32, true, "KEK")) return report(f);
  {
    const std::string sign_id = hex_of(fmt::key_id_of_public(fmt::ed25519_public_from_seed(seed.get())));
    const std::string kid = hex_of(kek_id(kek.get()));
    if (Failure f = check_root(seed_path, "content-manifest.json", root, sign_id)) return report(f);
    if (Failure f = check_root(kek_path, "kek.json", root, kid)) return report(f);
    if (root != Root::kDev) {  // whatever a sidecar claims: a key of the dev root never serves another root
      const DevIds dv = dev_ids(a.dev_trust_list, default_dev_dir());
      if (dv.signing.count(sign_id)) return report(fail(Code::kRootMismatch, "the signing key " + sign_id + " is a dev-root key and cannot sign for root " + to_string(root)));
      if (dv.kek.count(kid)) return report(fail(Code::kRootMismatch, "the KEK " + kid + " is the dev-root KEK and cannot be used for root " + to_string(root)));
    }
  }

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
