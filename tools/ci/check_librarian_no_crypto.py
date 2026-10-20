#!/usr/bin/env python3
"""Story 5.6 / FR27 (user imports are never encrypted): the librarian links no encryption code path.

Checks, and fails if any of them finds a cryptography library or function:
  1. the link line: the library names the librarian targets link against (--link-libs FILE, written by CMake at configure time, one name or path
     per line, collected recursively from the targets' link interfaces) must not name OpenSSL, mbedTLS, libsodium, libgcrypt, nettle, wolfSSL, OP-TEE,
     SQLCipher or Apple's CommonCrypto/Security frameworks;
  2. the symbols of the built binaries and static libraries (--binary / --lib, repeatable): `nm` on every one; defined or undefined symbols whose name is
     an AES / OpenSSL / mbedTLS / libsodium / gcrypt / OP-TEE / CommonCrypto / SQLCipher entry point fail the check.
SHA-256 hashing (bundle ids) is not encryption and is not flagged.

Story 6.2: protected-bundle VERIFICATION (Ed25519 manifest signature, hashes; formatlib target velora_format_verify, which the librarian links from Story
6.4 on) is not encryption either. Exactly these OpenSSL entry points are allowed: EVP_PKEY_new_raw_public_key, EVP_PKEY_free, EVP_MD_CTX_new,
EVP_MD_CTX_free, EVP_DigestVerifyInit and EVP_DigestVerify, and a link to a SHARED libcrypto (`-lcrypto`, `OpenSSL::Crypto`, libcrypto.so /
.dylib). Everything else stays forbidden: every cipher / decrypt entry point (EVP_aes_*, EVP_Decrypt*, EVP_CIPHER_*, AES_*, ...) fails the symbol scan, and a
STATIC libcrypto.a, libssl and the other libraries on the link-line list still fail the link-line check. tools/ci/check_format_no_decrypt.py is the stricter
companion for the verify target itself.

Without `nm` the symbol part is skipped with a clear message (exit 77 = ctest SKIP) after the link-line part ran.
`--self-test` proves the check can fail: it compiles an object that calls EVP_EncryptInit_ex / AES_encrypt (never linked or run) with --cc and checks the
scanner flags it, then checks that a clean object and the pattern list behave.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

LINK_WORDS = ["openssl", "libssl", "libcrypto", "mbedtls", "mbedcrypto", "mbedx509", "libsodium", "gcrypt", "nettle", "hogweed", "wolfssl",
              "sqlcipher", "optee", "commoncrypto", "cryptokit", "libtomcrypt", "libbotan", "cryptopp", "security.framework"]
LINK_PATTERNS = [
    r"(^|[\\/\s])-l(ssl|crypto|sodium|botan[-0-9]*|tomcrypt|teec)(\s|$)",      # -lssl -lcrypto -lsodium ...
    r"(^|[\\/])(lib)?(ssl|crypto|sodium)\.(a|so|dylib|lib)",                      # libssl.a, crypto.lib, sodium.so.26 ...
    r"-framework\s+(Security|CommonCrypto|CryptoKit)\b",
]

# Symbol names (after stripping one leading underscore). C entry points are matched by prefix; C++ symbols by namespace/word.
SYMBOL_PATTERNS = [
    r"^(EVP_|AES_|aes_|OPENSSL_|OpenSSL_|SSL_|TLS_(client_|server_)?method|BIO_|ERR_|RAND_bytes|HMAC_|CRYPTO_|CMAC_|ChaCha|chacha20|poly1305_|X509_|PEM_|RSA_|EC_KEY_|ECDSA_)",
    r"^(mbedtls_|psa_|crypto_(secretbox|aead|box|stream|sign|generichash|pwhash|auth|kdf)|sodium_|randombytes_)",
    r"^(gcry_|nettle_|wc_|wolfSSL_|CCCrypt|CCCryptor|CCKeyDerivation|SecKey|SecRandom|SecItem|TEEC_|TEE_)",
    r"^(sqlite3_key|sqlite3_rekey|sqlcipher_)",
]
SYMBOL_RE = [re.compile(p) for p in SYMBOL_PATTERNS]
# Story 6.2: the verification-only OpenSSL surface (Ed25519 signature check). Exact names; nothing else of OpenSSL is allowed.
VERIFY_ALLOW = {"EVP_PKEY_new_raw_public_key", "EVP_PKEY_free", "EVP_MD_CTX_new", "EVP_MD_CTX_free", "EVP_DigestVerifyInit", "EVP_DigestVerify"}
SHARED_CRYPTO_RE = re.compile(r"(^|[\\/])(lib)?crypto(\.[0-9]+)*\.(so(\.[0-9]+)*|dylib)$|^-lcrypto$|^OpenSSL::Crypto$")
CXX_RE = re.compile(r"(mbedtls|openssl|libsodium|wolfssl|botan|cryptopp)", re.I)


def flag_symbol(name):
    n = name.split("@", 1)[0]  # ELF symbol version suffix: EVP_DigestVerify@OPENSSL_3.0.0
    n = n[1:] if n.startswith("_") and not n.startswith("__") else n
    if n in VERIFY_ALLOW:
        return False
    if any(r.search(n) for r in SYMBOL_RE):
        return True
    # mangled C++: _ZN...mbedtls... / _ZN...openssl...
    return name.startswith("_Z") and bool(CXX_RE.search(name))


def scan_nm_output(text):
    """Returns the sorted list of flagged symbol names in `nm` output (any symbol type: defined or undefined)."""
    bad = set()
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        name = parts[-1]
        if name.endswith(":") or len(parts) == 1 and name.startswith("/"):
            continue
        if flag_symbol(name):
            bad.add(name)
    return sorted(bad)


def check_links(path):
    bad = []
    with open(path) as f:
        for line in f:
            item = line.strip()
            if not item or item.startswith("#"):
                continue
            if item.startswith("UNRESOLVED:"):
                bad.append(item)  # CMake could not resolve this name to a target, path or flag: the link-line check would not cover it
                continue
            if SHARED_CRYPTO_RE.search(item):
                continue  # Story 6.2: a shared libcrypto for signature verification; the symbol scan forbids its cipher entry points
            low = item.lower()
            if any(w in low for w in LINK_WORDS) or any(re.search(p, item, re.I) for p in LINK_PATTERNS):
                bad.append(item)
    return bad


def run_nm(nm, path):
    """(ok, output). nm failing or printing nothing is a FAILURE of that target: an unread binary must never count as clean."""
    r = subprocess.run([nm, path], capture_output=True, text=True)
    return r.returncode == 0 and bool(r.stdout.strip()), r.stdout


def self_test(cc, nm):
    ok = True

    def expect(cond, what):
        nonlocal ok
        if not cond:
            print("self-test FAILED: " + what)
            ok = False

    expect(flag_symbol("_EVP_EncryptInit_ex") and flag_symbol("AES_encrypt") and flag_symbol("mbedtls_aes_setkey_enc"), "known crypto names are flagged")
    expect(flag_symbol("_crypto_secretbox_easy") and flag_symbol("_sqlite3_key") and flag_symbol("_CCCrypt"), "more crypto names are flagged")
    expect(not flag_symbol("_sqlite3_open_v2") and not flag_symbol("_memcpy") and not flag_symbol("_ZN6velora6format6Sha256C1Ev"), "ordinary names pass")
    expect(not flag_symbol("_ZN6velora6format6sha256Ev") and not flag_symbol("_cryptic_name"), "hashing and look-alikes pass")
    expect(all(not flag_symbol(v) for v in ("_EVP_PKEY_new_raw_public_key", "_EVP_PKEY_free", "_EVP_MD_CTX_new", "_EVP_MD_CTX_free", "_EVP_DigestVerifyInit",
                                           "_EVP_DigestVerify", "EVP_DigestVerify@OPENSSL_3.0.0", "EVP_DigestVerifyInit@@OPENSSL_3.0.0")), "the Ed25519 verification entry points are allowed")
    expect(all(flag_symbol(v) for v in ("_EVP_DecryptInit_ex", "_EVP_aes_256_ctr", "_EVP_CIPHER_CTX_new", "_EVP_EncryptUpdate", "_EVP_PKEY_decrypt", "_EVP_DigestSignInit",
                                        "_AES_ctr128_encrypt", "_EVP_PKEY_new_raw_private_key", "_EVP_Digest", "_OPENSSL_cleanse", "_EVP_DigestVerifyFinal", "_ERR_clear_error", "EVP_DecryptInit_ex@OPENSSL_3.0.0")),
           "cipher / decrypt entry points (and every other OpenSSL name) stay forbidden")
    expect(not flag_symbol("_TLS_MODULE_BASE_") and not flag_symbol("__tls_get_addr") and flag_symbol("TLS_method"), "thread-local storage symbols are not TLS the protocol")
    with tempfile.TemporaryDirectory() as d:
        lf = os.path.join(d, "libs.txt")
        with open(lf, "w") as f:
            f.write("/usr/lib/libsqlite3.dylib\n/opt/homebrew/lib/libprotobuf.a\nvelora_format\n")
        expect(check_links(lf) == [], "a clean link list passes")
        with open(lf, "w") as f:
            f.write("/usr/lib/libsqlite3.dylib\n/opt/homebrew/opt/openssl@3/lib/libcrypto.a\n-lmbedtls\n-lssl\n")
        expect(len(check_links(lf)) == 3, "a link list naming libcrypto / mbedtls / -lssl fails: %s" % check_links(lf))
        with open(lf, "w") as f:
            f.write("/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib\n/usr/lib/x86_64-linux-gnu/libcrypto.so.3\n-lcrypto\nOpenSSL::Crypto\n")
        expect(check_links(lf) == [], "a shared libcrypto (verification) passes the link-line check: %s" % check_links(lf))
        with open(lf, "w") as f:
            f.write("/usr/lib/x86_64-linux-gnu/libcrypto.a\n/usr/lib/libssl.so.3\n/opt/homebrew/lib/libcrypto.a\nOpenSSL::SSL\n")
        expect(len(check_links(lf)) == 4, "a static libcrypto, libssl and OpenSSL::SSL still fail: %s" % check_links(lf))
        with open(lf, "w") as f:
            f.write("velora_format\nUNRESOLVED:mystery\n")
        expect(check_links(lf) == ["UNRESOLVED:mystery"], "an unresolved name fails the link-line check")
        if nm:
            expect(run_nm(nm, os.path.join(d, "does-not-exist.o"))[0] is False, "nm failing is not a pass")
        if cc and nm:
            bad_c, ok_c = os.path.join(d, "bad.c"), os.path.join(d, "ok.c")
            with open(bad_c, "w") as f:
                f.write("extern int EVP_EncryptInit_ex(void*, const void*, void*, const unsigned char*, const unsigned char*);\n"
                        "extern void AES_encrypt(const unsigned char*, unsigned char*, const void*);\n"
                        "int use_crypto(void) { AES_encrypt(0, 0, 0); return EVP_EncryptInit_ex(0, 0, 0, 0, 0); }\n")
            with open(ok_c, "w") as f:
                f.write("extern int sqlite3_open_v2(const char*, void**, int, const char*);\nint use_sqlite(void) { return sqlite3_open_v2(0, 0, 0, 0); }\n")
            for src, want in ((bad_c, True), (ok_c, False)):
                obj = src[:-2] + ".o"
                r = subprocess.run([cc, "-c", src, "-o", obj], capture_output=True, text=True)
                if r.returncode != 0:
                    expect(False, "cannot compile the self-test object: " + r.stderr)
                    continue
                found = scan_nm_output(run_nm(nm, obj)[1])
                expect(bool(found) == want, "scanner on %s: %s" % (os.path.basename(src), found))
        else:
            print("self-test: no C compiler or nm; object part skipped")
    print("check_librarian_no_crypto self-test: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--link-libs", help="file listing the libraries the librarian targets link (CMake writes it)")
    ap.add_argument("--binary", action="append", default=[], help="a built binary to scan")
    ap.add_argument("--lib", action="append", default=[], help="a built static library to scan")
    ap.add_argument("--nm", default=None)
    ap.add_argument("--cc", default=None)
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    nm = a.nm or shutil.which("nm")
    if a.self_test:
        return self_test(a.cc, nm)
    failed = False
    if a.link_libs:
        bad = check_links(a.link_libs)
        if bad:
            failed = True
            print("FAIL: the librarian links cryptography libraries: " + ", ".join(bad))
        else:
            print("link line: no cryptography library")
    targets = a.binary + a.lib
    if not targets and not a.link_libs:
        print("nothing to check (give --link-libs, --binary or --lib)")
        return 2
    if not nm:
        print("SKIP: `nm` not found, symbol check not run")
        return 1 if failed else 77
    for t in targets:
        if not os.path.exists(t):
            print("FAIL: %s does not exist" % t)
            failed = True
            continue
        nm_ok, text = run_nm(nm, t)
        if not nm_ok:
            failed = True
            print("FAIL: `nm` could not read %s (non-zero exit or no output): not counted as clean" % t)
            continue
        bad = scan_nm_output(text)
        if bad:
            failed = True
            print("FAIL: %s contains cryptography symbols: %s" % (t, ", ".join(bad[:20])))
        else:
            print("symbols: %s: no cryptography symbol" % os.path.basename(t))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
