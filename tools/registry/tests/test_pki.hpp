#pragma once
// Story 6.8 test support: a throw-away PKI built with OpenSSL's API inside the test process (no openssl CLI, no files in the repository). The "factory root" CA,
// station / admin client certificates under it, a server certificate, an expired certificate and a certificate under another CA. Keys and certificates are written
// only to the test's own temporary directory (outside the work tree) and removed at the end.
#include <fcntl.h>
#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>

namespace pki {

struct PkeyDel { void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); } };
struct X509Del { void operator()(X509* x) const { X509_free(x); } };
using Pkey = std::unique_ptr<EVP_PKEY, PkeyDel>;
using Cert = std::unique_ptr<X509, X509Del>;

struct Ident {
  Pkey key;
  Cert cert;
};

inline Pkey new_key() { return Pkey(EVP_EC_gen("P-256")); }

// `issuer` null = self-signed. nb / na: validity start / end relative to now, in seconds.
inline Ident make_ident(const char* cn, const char* ou, const Ident* issuer, bool ca, long nb, long na, const char* eku, const char* san_ip) {
  Ident id;
  id.key = new_key();
  id.cert.reset(X509_new());
  X509* x = id.cert.get();
  X509_set_version(x, 2);
  ASN1_INTEGER* serial = X509_get_serialNumber(x);
  unsigned char r[8];
  RAND_bytes(r, sizeof r);
  r[0] &= 0x7f;
  BIGNUM* bn = BN_bin2bn(r, sizeof r, nullptr);
  BN_to_ASN1_INTEGER(bn, serial);
  BN_free(bn);
  X509_gmtime_adj(X509_getm_notBefore(x), nb);
  X509_gmtime_adj(X509_getm_notAfter(x), na);
  X509_set_pubkey(x, id.key.get());
  X509_NAME* name = X509_get_subject_name(x);
  X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("Velora Test"), -1, -1, 0);
  if (ou) X509_NAME_add_entry_by_txt(name, "OU", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(ou), -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
  X509_set_issuer_name(x, issuer ? X509_get_subject_name(issuer->cert.get()) : name);
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, issuer ? issuer->cert.get() : x, x, nullptr, nullptr, 0);
  auto add = [&](int nid, const char* value) {
    X509_EXTENSION* e = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (e) {
      X509_add_ext(x, e, -1);
      X509_EXTENSION_free(e);
    }
  };
  add(NID_basic_constraints, ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
  add(NID_key_usage, ca ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature");
  add(NID_subject_key_identifier, "hash");
  if (issuer) add(NID_authority_key_identifier, "keyid");
  if (eku) add(NID_ext_key_usage, eku);
  if (san_ip) add(NID_subject_alt_name, (std::string("IP:") + san_ip).c_str());
  X509_sign(x, issuer ? issuer->key.get() : id.key.get(), EVP_sha256());
  return id;
}

inline bool write_cert(const Ident& id, const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = PEM_write_X509(f, id.cert.get()) == 1;
  std::fclose(f);
  return ok;
}

inline bool write_key(const Ident& id, const std::string& path, mode_t mode = 0600) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) return false;
  ::fchmod(fd, mode);
  FILE* f = ::fdopen(fd, "wb");
  if (!f) return false;
  const bool ok = PEM_write_PrivateKey(f, id.key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1;
  std::fclose(f);
  return ok;
}

}  // namespace pki
