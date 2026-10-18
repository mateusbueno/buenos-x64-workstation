#include <cstring>
#include <string>

#include "test_util.hpp"
#include "velora/format/hash.hpp"

using namespace velora::format;

static std::span<const std::uint8_t> sv(const std::string& s) {
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}
static std::string hex(auto d) { return to_hex(d); }

int main() {
  // SHA-256 known answers (FIPS 180-4 / NIST).
  CHECK(hex(Sha256::hash(sv(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex(Sha256::hash(sv("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hex(Sha256::hash(sv("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  {
    Sha256 s;
    const std::string a(1000, 'a');
    for (int i = 0; i < 1000; ++i) s.update(a.data(), a.size());
    CHECK(hex(s.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  }
  {  // incremental update in odd chunk sizes matches one-shot
    std::string big(1000, 0);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = char(i * 31 + 7);
    Sha256 s;
    for (std::size_t i = 0; i < big.size();) {
      const std::size_t n = std::min<std::size_t>(1 + i % 97, big.size() - i);
      s.update(big.data() + i, n);
      i += n;
    }
    CHECK(s.finish() == Sha256::hash(sv(big)));
  }
  // SHA-1 known answers.
  CHECK(hex(Sha1::hash(sv(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(hex(Sha1::hash(sv("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(hex(Sha1::hash(sv("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  {
    Sha1 s;
    const std::string a(1000, 'a');
    for (int i = 0; i < 1000; ++i) s.update(a.data(), a.size());
    CHECK(hex(s.finish()) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
  }
  // UUIDv5: RFC 4122 DNS namespace, uuid5(DNS, "python.org") (reference value from Python's uuid).
  const Uuid dns = {0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
                    0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
  CHECK(uuid_to_string(uuid_v5(dns, sv("python.org"))) == "886313e1-3b8a-5372-9b90-0c9aee199e5d");
  const Uuid a = library_id_from_fingerprint(sv("fingerprint-a"));
  CHECK(a == library_id_from_fingerprint(sv("fingerprint-a")));
  CHECK(a != library_id_from_fingerprint(sv("fingerprint-b")));
  CHECK((a[6] >> 4) == 5 && (a[8] >> 6) == 2);

  if (g_fail == 0) std::printf("hash_test OK\n");
  return g_fail == 0 ? 0 : 1;
}
