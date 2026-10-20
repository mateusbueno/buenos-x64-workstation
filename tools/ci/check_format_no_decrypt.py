#!/usr/bin/env python3
"""Story 6.2 (AR27, NFR13): the desktop-side protected-bundle verification target contains no decrypt path.

`velora_format_verify` (Ed25519 manifest signature + hash-list + ciphertext block-hash verification) is what the desktop build and the librarian link.
Decryption (`velora_format_decrypt`: AES-CTR and the decrypting reader) is for the engine, offline-render and test targets only. This check proves
that, from the symbols (`nm`, defined or undefined) of the built static libraries and binaries given with --lib / --binary:

  * no OpenSSL cipher entry point: EVP_aes_* / EVP_Decrypt* / EVP_Encrypt* / EVP_Cipher* / EVP_CIPHER_* / EVP_PKEY_decrypt, AES_*, aesni/vpaes/bsaes,
    CRYPTO_ctr/cbc/cfb/ofb/gcm/ccm/xts, ChaCha, DES, ...;
  * none of the project's decrypt symbols: anything named decrypt, aes256 / aes_ctr, ProtectedBundleReader or ContentKey (C or mangled C++).

Two layers of rules apply to every scanned file:
  * the DENYLIST above (cipher and decrypt names of OpenSSL, CommonCrypto, mbedTLS, libsodium, wolfSSL, and the project's decrypt symbols);
  * an ALLOWLIST for crypto-library symbols: any undefined symbol that starts with EVP_ / OPENSSL_ / CRYPTO_ / ERR_ / BIO_ / CC / Sec / mbedtls / crypto_ must be one
    of the exact names in ALLOWED_CRYPTO (the Ed25519 verification calls: EVP_PKEY_new_raw_public_key, EVP_PKEY_free, EVP_MD_CTX_new, EVP_MD_CTX_free,
    EVP_DigestVerifyInit, EVP_DigestVerify). A new crypto call in the verify target therefore fails the check until it is reviewed and added here.
Symbol version suffixes (`EVP_DigestVerify@OPENSSL_3.0.0`) are stripped before the lookup.

What "no decrypt path" means: the scanned code never CALLS the cipher API, i.e. no cipher / decrypt entry point is imported or defined by it. The shared libcrypto
that the verify target is linked against still contains AES (it is a system library); this check does not and cannot say otherwise. `counter_block`, `hmac_sha256`,
`key_check_value` and `hash_list_root` live in the core: they are plain data layout and SHA-256, no cipher call.

Signature verification stays available: EVP_PKEY_new_raw_public_key, EVP_MD_CTX_*, EVP_DigestVerify* are NOT flagged. `--require-defined NAME` (repeatable)
fails when no scanned object defines a symbol containing NAME, so a scan of the wrong or an empty file cannot pass (the verify library must define
`ed25519_verify_strict`).

`--expect-flagged NAME` (repeatable) inverts the check for a proof of non-vacuity: exit 0 only when every NAME is flagged (denylist) in the scanned files.
Without `nm` the symbol part is skipped (exit 77 = ctest SKIP). `--self-test` proves the scanner can fail: it compiles an object that calls
EVP_DecryptInit_ex / EVP_aes_256_ctr (never linked or run) with --cc and checks it is flagged, that a verify-only object is clean, and the name rules.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

CIPHER_PATTERNS = [
    r"^EVP_(aes|Decrypt|Encrypt|Cipher|CIPHER|des|chacha|camellia|bf_|cast5|rc[2-5]|sm4|aria|seed|get_cipher|OpenInit|SealInit)",
    r"^EVP_PKEY_(en|de)crypt",
    r"^(AES_|aes_|ossl_aes|ossl_cipher|aesni_|vpaes_|bsaes_|DES_|BF_|CAST_|ChaCha|chacha20)",
    r"^CRYPTO_(ctr|cbc|cfb|ofb|gcm|ccm|xts|ocb)",
    r"^BIO_(f_cipher|set_cipher)",
    r"^(CCCrypt|CCCryptor|CCKeyDerivation)",
    r"^(mbedtls_|crypto_(stream|secretbox|aead|box)|wc_Aes|wolfSSL_)",
]
# Allowlist: undefined crypto-library symbols must be exactly one of these (the Ed25519 verification calls).
ALLOWED_CRYPTO = {"EVP_PKEY_new_raw_public_key", "EVP_PKEY_free", "EVP_MD_CTX_new", "EVP_MD_CTX_free", "EVP_DigestVerifyInit", "EVP_DigestVerify"}
CRYPTO_PREFIX_RE = re.compile(r"^(EVP_|OPENSSL_|CRYPTO_|ERR_|BIO_|CC|Sec|mbedtls|crypto_)")
CIPHER_RE = [re.compile(p) for p in CIPHER_PATTERNS]
# Project symbols that exist only on the decrypt side (C names, and substrings of mangled C++ names).
PROJECT_RE = re.compile(r"decrypt|aes_?256|aes_?ctr|ProtectedBundleReader|ContentKey", re.I)


def normalize(name):
    """Strips one leading underscore (Mach-O C names) and a @VERSION / @@VERSION suffix (ELF)."""
    n = name.split("@", 1)[0]
    return n[1:] if n.startswith("_") and not n.startswith("__") else n


def flag_symbol(name):
    """Denylist: cipher / decrypt names."""
    n = normalize(name)
    if any(r.search(n) for r in CIPHER_RE):
        return True
    return bool(PROJECT_RE.search(name))


def unexpected_crypto(name):
    """Allowlist: a crypto-library name that is not one of the reviewed verification calls."""
    n = normalize(name)
    return bool(CRYPTO_PREFIX_RE.match(n)) and n not in ALLOWED_CRYPTO


def parse_nm(text):
    """(all symbol names, defined symbol names) from `nm` output."""
    names, defined = [], []
    for line in text.splitlines():
        parts = line.split()
        if not parts or line.rstrip().endswith(":"):
            continue
        name = parts[-1]
        names.append(name)
        if len(parts) >= 3 and parts[-2] not in ("U", "u", "w", "v"):  # "addr type name"
            defined.append(name)
    return names, defined


def run_nm(nm, path):
    """(ok, output). nm failing or printing nothing is a FAILURE of that target: an unread binary must never count as clean."""
    r = subprocess.run([nm, path], capture_output=True, text=True)
    return r.returncode == 0 and bool(r.stdout.strip()), r.stdout


def scan(nm, path, require):
    """Returns (problems, defined names, flagged names)."""
    if not os.path.exists(path):
        return ["%s does not exist" % path], [], []
    ok, text = run_nm(nm, path)
    if not ok:
        return ["`nm` could not read %s (non-zero exit or no output): not counted as clean" % path], [], []
    names, defined = parse_nm(text)
    bad = sorted({n for n in names if flag_symbol(n)})
    # Allowlist applies to UNDEFINED crypto symbols (what the target imports); defined ones are the project's own names.
    undefined = [n for n in names if n not in set(defined)]
    unknown = sorted({n for n in undefined if not flag_symbol(n) and unexpected_crypto(n)})
    problems = []
    if bad:
        problems.append("%s contains decrypt/cipher symbols: %s" % (path, ", ".join(bad[:20])))
    if unknown:
        problems.append("%s imports crypto-library symbols outside the verification allowlist: %s" % (path, ", ".join(unknown[:20])))
    return problems, defined, bad


def self_test(cc, nm):
    ok = True

    def expect(cond, what):
        nonlocal ok
        if not cond:
            print("self-test FAILED: " + what)
            ok = False

    for bad in ("_EVP_DecryptInit_ex", "EVP_aes_256_ctr", "_EVP_CIPHER_CTX_new", "_EVP_EncryptUpdate", "_AES_encrypt", "_aesni_ctr32_encrypt_blocks",
                "_EVP_CipherFinal_ex", "_EVP_PKEY_decrypt", "_CRYPTO_ctr128_encrypt", "_ChaCha20_ctr32",
                "_ZN6velora6format21ProtectedBundleReader10read_blockEmmNSt3__14spanIiLm18446744073709551615EEERNS0_12BlockDecoderE",
                "_ZN6velora6format12aes256_ctr_xorERKNS0_10ContentKeyE", "velora_decrypt_block", "_ZN6velora6format13decrypt_blockEv"):
        expect(flag_symbol(bad), "decrypt/cipher name is flagged: " + bad)
    for good in ("_EVP_PKEY_new_raw_public_key", "_EVP_MD_CTX_new", "_EVP_MD_CTX_free", "_EVP_DigestVerifyInit", "_EVP_DigestVerify", "_EVP_PKEY_free",
                 "_ZN6velora6format21ed25519_verify_strictERKNSt3__15arrayIhLm32EEENS1_4spanIKhLm18446744073709551615EEERKNS2_IhLm64EEE",
                 "_ZN6velora6format19ProtectedBundleView9open_fileERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEERKNS0_11TrustedKeysE",
                 "_ZN6velora6format6Sha256C1Ev", "_memcpy", "_ZN6velora6format15hash_list_rootENSt3__14spanIKNS1_5arrayIhLm32EEELm18446744073709551615EEE",
                 "_ZN6velora6format15key_check_valueE"):
        expect(not flag_symbol(good), "verify-side name passes: " + good)
    for bad in ("_CCCrypt", "_CCCryptorCreate", "_BIO_f_cipher", "_BIO_set_cipher", "_mbedtls_aes_setkey_dec", "_crypto_stream_xor", "_crypto_secretbox_open_easy",
                "wc_AesSetKey", "EVP_DecryptInit_ex@OPENSSL_3.0.0", "EVP_aes_256_ctr@@OPENSSL_3.0.0"):
        expect(flag_symbol(bad), "extended denylist name is flagged: " + bad)
    for allowed in ("_EVP_DigestVerify", "EVP_DigestVerify@OPENSSL_3.0.0", "EVP_DigestVerifyInit@@OPENSSL_3.0.0", "_EVP_PKEY_new_raw_public_key", "_ZN6velora4fooEv", "_memcpy"):
        expect(not unexpected_crypto(allowed), "allowlisted or non-crypto name passes: " + allowed)
    for unknown in ("_EVP_Digest", "EVP_DigestSign@OPENSSL_3.0.0", "_OPENSSL_cleanse", "_CRYPTO_malloc", "ERR_clear_error", "_ERR_get_error", "_BIO_new", "_SecRandomCopyBytes",
                    "_CCHmac", "_mbedtls_sha256", "_crypto_sign_open", "_EVP_PKEY_new_raw_private_key"):
        expect(unexpected_crypto(unknown), "unreviewed crypto-library name fails the allowlist: " + unknown)
    expect(normalize("EVP_DigestVerify@OPENSSL_3.0.0") == "EVP_DigestVerify" and normalize("_EVP_DigestVerify") == "EVP_DigestVerify", "version suffix is stripped")
    names, defined = parse_nm("                 U _EVP_DecryptInit_ex\n0000000000001000 T _ed25519_verify_strict\nlib.a(x.o):\n")
    expect(names == ["_EVP_DecryptInit_ex", "_ed25519_verify_strict"] and defined == ["_ed25519_verify_strict"], "nm parsing: %s %s" % (names, defined))
    with tempfile.TemporaryDirectory() as d:
        if nm:
            expect(run_nm(nm, os.path.join(d, "does-not-exist.o"))[0] is False, "nm failing is not a pass")
            empty = os.path.join(d, "empty.o")
            open(empty, "w").close()
            expect(scan(nm, empty, [])[0] != [], "an unreadable/empty object is not clean")
        if cc and nm:
            bad_c, ok_c = os.path.join(d, "bad.c"), os.path.join(d, "ok.c")
            with open(bad_c, "w") as f:
                f.write("extern const void* EVP_aes_256_ctr(void);\n"
                        "extern void* EVP_CIPHER_CTX_new(void);\n"
                        "extern int EVP_DecryptInit_ex(void*, const void*, void*, const unsigned char*, const unsigned char*);\n"
                        "int use_decrypt(void) { void* c = EVP_CIPHER_CTX_new(); return EVP_DecryptInit_ex(c, EVP_aes_256_ctr(), 0, 0, 0); }\n")
            with open(ok_c, "w") as f:
                f.write("extern void* EVP_PKEY_new_raw_public_key(int, void*, const unsigned char*, unsigned long);\n"
                        "extern void* EVP_MD_CTX_new(void);\n"
                        "extern int EVP_DigestVerify(void*, const unsigned char*, unsigned long, const unsigned char*, unsigned long);\n"
                        "int ed25519_verify_strict(void) { void* k = EVP_PKEY_new_raw_public_key(1087, 0, 0, 32); return EVP_DigestVerify(EVP_MD_CTX_new(), 0, 64, 0, 0) + (k != 0); }\n")
            unk_c = os.path.join(d, "unk.c")
            with open(unk_c, "w") as f:
                f.write("extern int EVP_Digest(const void*, unsigned long, unsigned char*, unsigned*, const void*, void*);\n"
                        "int use_digest(void) { return EVP_Digest(0, 0, 0, 0, 0, 0); }\n")
            for src, want_bad in ((bad_c, True), (ok_c, False), (unk_c, True)):
                obj = src[:-2] + ".o"
                r = subprocess.run([cc, "-c", src, "-o", obj], capture_output=True, text=True)
                if r.returncode != 0:
                    expect(False, "cannot compile the self-test object: " + r.stderr)
                    continue
                problems, defined, flagged = scan(nm, obj, [])
                expect(bool(problems) == want_bad, "scanner on %s: %s" % (os.path.basename(src), problems))
                if src == bad_c:
                    expect(any("EVP_DecryptInit_ex" in n for n in flagged), "flagged names are reported for --expect-flagged")
                if not want_bad:
                    expect(any("ed25519_verify_strict" in n for n in defined), "defined symbols are reported")
        elif os.environ.get("CI"):
            expect(False, "no C compiler or nm in CI: the object part of the self-test must run there")
        else:
            print("self-test: no C compiler or nm; object part skipped")
    print("check_format_no_decrypt self-test: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", action="append", default=[], help="a built binary to scan")
    ap.add_argument("--lib", action="append", default=[], help="a built static library to scan")
    ap.add_argument("--require-defined", action="append", default=[], help="a symbol name fragment that some scanned file must define")
    ap.add_argument("--expect-flagged", action="append", default=[], help="inverted proof of non-vacuity: succeed only if every NAME is flagged")
    ap.add_argument("--nm", default=None)
    ap.add_argument("--cc", default=None)
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    nm = a.nm or shutil.which("nm")
    if a.self_test:
        return self_test(a.cc or shutil.which("cc"), nm)
    targets = a.binary + a.lib
    if not targets:
        print("nothing to check (give --binary or --lib)")
        return 2
    if not nm:
        print("SKIP: `nm` not found, symbol check not run")
        return 77
    if a.expect_flagged:
        flagged_all = set()
        for t in targets:
            problems, defined, flagged = scan(nm, t, [])
            if not os.path.exists(t) or (problems and not flagged and not any("allowlist" in p for p in problems)):
                print("FAIL: cannot scan %s: %s" % (t, "; ".join(problems)))
                return 1
            flagged_all |= {normalize(n) for n in flagged}
        missing = [n for n in a.expect_flagged if normalize(n) not in flagged_all]
        for n in missing:
            print("FAIL: expected %s to be flagged but it is not in the scanned files" % n)
        if not missing:
            print("expected symbols are flagged: " + ", ".join(a.expect_flagged))
        return 1 if missing else 0
    failed = False
    all_defined = []
    for t in targets:
        problems, defined, _ = scan(nm, t, a.require_defined)
        all_defined += defined
        if problems:
            failed = True
            for p in problems:
                print("FAIL: " + p)
        else:
            print("symbols: %s: no decrypt or cipher symbol" % os.path.basename(t))
    for need in a.require_defined:
        if not any(need in n for n in all_defined):
            failed = True
            print("FAIL: no scanned file defines a symbol containing %r (wrong or empty target?)" % need)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
