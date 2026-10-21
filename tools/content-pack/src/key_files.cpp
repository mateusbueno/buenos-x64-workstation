#include "key_files.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <vector>

#include "velora/format/json_lite.hpp"
#include "velora/format/protected_writer.hpp"

namespace velora::contentpack {
namespace fs = std::filesystem;
namespace fmt = velora::format;

namespace {
std::string hex_of(std::span<const std::uint8_t> d) { return fmt::to_hex(d); }
}  // namespace

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

std::string default_dev_dir() {
  if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/velora-dev-keys";
  if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.velora-dev-keys";
  return {};
}

namespace {
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

}  // namespace

Failure load_root_keys(Root root, const std::string& keys_dir, const std::string& signing_key, const std::string& kek, const std::string& dev_trust_list,
                       Secret<fmt::SigningSeed>& seed, Secret<Key32>& kek_out) {
  std::string seed_path = signing_key, kek_path = kek;
  std::string dir = keys_dir;
  if (dir.empty() && root == Root::kDev && (seed_path.empty() || kek_path.empty())) dir = default_dev_dir();
  if (seed_path.empty() && !dir.empty()) seed_path = dir + "/content-manifest.ed25519.seed";
  if (kek_path.empty() && !dir.empty()) kek_path = dir + "/kek.aes256";
  if (seed_path.empty() || kek_path.empty())
    return fail(Code::kBadKeyFile, std::string("root ") + to_string(root) + " needs --keys-dir, or both --signing-key and --kek");
  if (Failure f = read_key_file(seed_path, seed->bytes.data(), 32, true, "signing key")) return f;
  if (Failure f = read_key_file(kek_path, kek_out.get().data(), 32, true, "KEK")) return f;
  const std::string sign_id = hex_of(fmt::key_id_of_public(fmt::ed25519_public_from_seed(seed.get())));
  const std::string kid = hex_of(kek_id(kek_out.get()));
  if (Failure f = check_root(seed_path, "content-manifest.json", root, sign_id)) return f;
  if (Failure f = check_root(kek_path, "kek.json", root, kid)) return f;
  if (root != Root::kDev) {  // whatever a sidecar claims: a key of the dev root never serves another root
    const DevIds dv = dev_ids(dev_trust_list, default_dev_dir());
    if (dv.signing.count(sign_id)) return fail(Code::kRootMismatch, "the signing key " + sign_id + " is a dev-root key and cannot sign for root " + to_string(root));
    if (dv.kek.count(kid)) return fail(Code::kRootMismatch, "the KEK " + kid + " is the dev-root KEK and cannot be used for root " + to_string(root));
  }
  return {};
}

}  // namespace velora::contentpack
