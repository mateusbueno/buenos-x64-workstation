#include "velora/host/trust_list.hpp"

#include <array>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

namespace velora::host {

namespace fmt = velora::format;

bool load_trust_list_file(const std::string& path, fmt::TrustedKeys& out, std::string& err) {
  std::ifstream in(path);
  if (!in) {
    err = path + ": cannot be read";
    return false;
  }
  auto unhex = [](const std::string& h, std::uint8_t* dst, std::size_t n) {
    if (h.size() != 2 * n) return false;
    for (std::size_t i = 0; i < n; ++i) {
      int v = 0;
      for (int k = 0; k < 2; ++k) {
        const char c = h[2 * i + static_cast<std::size_t>(k)];
        const int x = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (x < 0) return false;
        v = v * 16 + x;
      }
      dst[i] = static_cast<std::uint8_t>(v);
    }
    return true;
  };
  std::vector<fmt::TrustedKey> keys;
  std::set<std::array<std::uint8_t, 8>> ids;
  std::string line;
  int ln = 0;
  while (std::getline(in, line)) {
    ++ln;
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    std::istringstream ss(line);
    std::vector<std::string> f;
    for (std::string t; ss >> t;) f.push_back(t);
    if (f.empty()) continue;
    auto bad = [&](const char* why) {
      err = path + ":" + std::to_string(ln) + ": " + why;
      return false;
    };
    if (f.size() != 4) return bad("expected `<root> <role> <key_id> <public_key>`");
    if (f[1] != "content-manifest") return bad("unknown role (only content-manifest)");
    fmt::TrustedKey k;
    if (!unhex(f[2], k.id.data(), k.id.size()) || !unhex(f[3], k.pub.data(), k.pub.size())) return bad("key id or public key is not hex of the right length");
    if (fmt::key_id_of_public(k.pub) != k.id) return bad("the key id is not the first 8 bytes of SHA-256(public key)");
    if (!ids.insert(k.id).second) return bad("the key id is listed twice");
    keys.push_back(k);
  }
  out = fmt::TrustedKeys(std::move(keys));
  return true;
}

}  // namespace velora::host
