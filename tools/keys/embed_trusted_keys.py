#!/usr/bin/env python3
"""Generates the trusted content-signing public-key header for one image variant (Story 6.1, AR34/NFR23).

  embed_trusted_keys.py --variant dev|factory-test|production --output FILE [--trust-dir DIR] [--dev-keys-dir DIR] [--deny-dir DIR]

Rules (any violation exits 1 and writes nothing):
  * a variant embeds only keys of its own root: dev images trust only dev keys, factory-test only factory-test, production only production;
  * the build REFUSES to embed a dev public key into a production variant (from the production list, from --dev-keys-dir, from anywhere);
  * a production variant with no production key list, or an empty one, is refused;
  * a key must be a canonical, on-curve, non-small-order Ed25519 public key;
  * for non-dev variants the committed trust/dev.pub-list and tests/fixtures/*.pub-list (and --deny-dir DIR/*.pub-list) are a denylist: relabelling
    a dev or fixture key as factory-test/production fails even with a custom --trust-dir;
  * a refusal deletes any existing output header, so a stale one can never be built against;
  * every entry's key id must equal the first 8 bytes of SHA-256(public key), and no public key may appear under two roots.
Trust list format (tools/keys/trust/<root>.pub-list, public keys only, committed): one entry per line,
  `<root> <role> <key_id hex16> <public_key hex64>`, `#` starts a comment. The only role so far is `content-manifest` (Ed25519).
--dev-keys-dir (dev variant only) additionally trusts the content-manifest public key of a local dev key directory made by velora_keys.py.
"""
import argparse
import glob
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import velora_keys  # noqa: E402

ROOTS = ("dev", "factory-test", "production")
ROLES = ("content-manifest",)
HEX = re.compile(r"^[0-9a-f]+$")


class EmbedError(Exception):
    pass


def _entry(line, origin):
    parts = line.split()
    if len(parts) != 4:
        raise EmbedError("%s: expected `<root> <role> <key_id> <public_key>`, got: %s" % (origin, line))
    root, role, kid, pub = parts
    if root not in ROOTS:
        raise EmbedError("%s: unknown root '%s'" % (origin, root))
    if role not in ROLES:
        raise EmbedError("%s: unknown role '%s'" % (origin, role))
    if not (HEX.match(kid) and len(kid) == 16 and HEX.match(pub) and len(pub) == 64):
        raise EmbedError("%s: key id must be 16 and public key 64 lowercase hex digits" % origin)
    if hashlib.sha256(bytes.fromhex(pub)).digest()[:8].hex() != kid:
        raise EmbedError("%s: key id %s is not the first 8 bytes of SHA-256(public key)" % (origin, kid))
    bad = velora_keys.ed25519_validate_public(bytes.fromhex(pub))
    if bad:
        raise EmbedError("%s: invalid Ed25519 public key %s: %s" % (origin, kid, bad))
    return {"root": root, "role": role, "key_id": kid, "pub": pub, "origin": origin}


def read_list(path):
    out = []
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if line:
                out.append(_entry(line, "%s:%d" % (path, n)))
    return out


def denylist(extra_dirs=()):
    """Public keys that must never appear in a non-dev variant: the committed dev list and every committed test fixture list."""
    paths = [os.path.join(HERE, "trust", "dev.pub-list")] + sorted(glob.glob(os.path.join(HERE, "tests", "fixtures", "*.pub-list")))
    for d in extra_dirs:
        paths += sorted(glob.glob(os.path.join(d, "*.pub-list")))
    deny = {}
    for p in paths:
        if os.path.exists(p):
            for e in read_list(p):
                deny[e["pub"]] = p
    return deny


def collect(variant, trust_dir, dev_keys_dir=None, deny_dirs=()):
    if variant not in ROOTS:
        raise EmbedError("unknown variant '%s' (expected one of %s)" % (variant, ", ".join(ROOTS)))
    # every list is parsed and cross-checked, so one public key can never hide under two roots
    seen, own = {}, []
    for root in ROOTS:
        p = os.path.join(trust_dir, root + ".pub-list")
        if not os.path.exists(p):
            continue
        for e in read_list(p):
            if e["root"] != root:
                raise EmbedError("%s: entry labelled root '%s' in the %s list%s" % (e["origin"], e["root"], root,
                                 " (refusing to embed a dev public key into a production variant)" if e["root"] == "dev" and variant == "production" else ""))
            if e["pub"] in seen and seen[e["pub"]] != root:
                raise EmbedError("%s: public key %s is listed under both %s and %s" % (e["origin"], e["key_id"], seen[e["pub"]], root))
            seen[e["pub"]] = root
            if root == variant:
                own.append(e)
    if dev_keys_dir:
        if variant != "dev":
            raise EmbedError("refusing to embed a dev public key (--dev-keys-dir) into the %s variant" % variant)
        with open(os.path.join(dev_keys_dir, "content-manifest.ed25519.pub"), "rb") as f:
            pub = f.read()
        if len(pub) != 32:
            raise EmbedError("%s: content-manifest.ed25519.pub is not 32 bytes" % dev_keys_dir)
        e = _entry("dev content-manifest %s %s" % (hashlib.sha256(pub).digest()[:8].hex(), pub.hex()), dev_keys_dir)
        if e["pub"] in seen and seen[e["pub"]] != "dev":
            raise EmbedError("%s: the local dev public key %s is listed under the %s root" % (dev_keys_dir, e["key_id"], seen[e["pub"]]))
        own.append(e)
    if variant != "dev":
        deny = denylist(deny_dirs)
        for e in own:
            if e["pub"] in deny:
                raise EmbedError("%s: public key %s is a dev/fixture key (%s): refusing to embed it into the %s variant" % (
                    e["origin"], e["key_id"], deny[e["pub"]], variant))
    uniq = {}
    for e in own:
        uniq[e["pub"]] = e
    keys = sorted(uniq.values(), key=lambda e: e["key_id"])
    if variant == "production" and not keys:
        raise EmbedError("production variant has no production key list entries in %s (production.pub-list missing or empty): refusing" % trust_dir)
    return keys


def render(variant, keys):
    ident = re.sub(r"[^A-Za-z0-9]", "_", variant)
    def arr(hexs):
        b = bytes.fromhex(hexs)
        return "{" + ", ".join("0x%02x" % x for x in b) + "}"
    lines = ["// Generated by tools/keys/embed_trusted_keys.py for the '%s' image variant. Do not edit." % variant,
             "// Public keys only. Trusted root: %s. Everything lives in namespace velora::keys::%s so several variants can be included together." % (variant, ident),
             "#pragma once", "#include <array>", "#include <cstddef>", "#include <cstdint>", "",
             "namespace velora::keys {", "",
             "namespace %s {" % ident,
             "struct TrustedKey {", "  std::array<std::uint8_t, 8> id;    // first 8 bytes of SHA-256(public key)",
             "  std::array<std::uint8_t, 32> pub;  // Ed25519 public key (content-manifest signing)", "};", "",
             'inline constexpr char kTrustedRoot[] = "%s";' % variant,
             "inline constexpr bool kIsProduction = %s;" % ("true" if variant == "production" else "false"),
             "inline constexpr std::size_t kTrustedContentKeyCount = %d;" % len(keys),
             "inline constexpr std::array<TrustedKey, %d> kTrustedContentKeys = {{" % len(keys)]
    for e in keys:
        lines.append("    {std::array<std::uint8_t, 8>%s, std::array<std::uint8_t, 32>%s}," % (arr(e["key_id"]), arr(e["pub"])))
    lines += ["}};", "}  // namespace %s" % ident, "", "}  // namespace velora::keys", ""]
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--variant", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--trust-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "trust"))
    ap.add_argument("--dev-keys-dir", default=None)
    ap.add_argument("--deny-dir", action="append", default=[], help="extra directory whose *.pub-list keys are denied in non-dev variants")
    a = ap.parse_args(argv)
    try:
        keys = collect(a.variant, a.trust_dir, a.dev_keys_dir, a.deny_dir)
    except (EmbedError, OSError, UnicodeDecodeError, ValueError) as e:
        try:
            os.remove(a.output)  # never leave a stale header behind a refusal
        except OSError:
            pass
        print("embed_trusted_keys: REFUSED: %s" % e, file=sys.stderr)
        return 1
    if not keys:
        print("embed_trusted_keys: warning: no trusted content keys for the %s variant" % a.variant, file=sys.stderr)
    text = render(a.variant, keys)
    os.makedirs(os.path.dirname(os.path.abspath(a.output)), exist_ok=True)
    try:
        with open(a.output, encoding="utf-8") as f:
            if f.read() == text:
                return 0
    except OSError:
        pass
    with open(a.output, "w", encoding="utf-8") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
