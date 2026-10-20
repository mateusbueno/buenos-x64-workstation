#!/usr/bin/env python3
"""Story 6.1 tests: dev key tool, trust-list embedding and its refusals. Hermetic: temp dirs, no network, no committed key material."""
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
KEYS = os.path.dirname(HERE)
sys.path.insert(0, KEYS)
sys.path.insert(0, HERE)
import embed_trusted_keys as emb  # noqa: E402
import velora_keys as vk  # noqa: E402
import wrap_ref as wr  # noqa: E402

PY = sys.executable
FIXTURE_LIST = os.path.join(HERE, "fixtures", "dev.pub-list")


def slurp(path, mode="rb"):
    with open(path, mode) as f:
        return f.read()


def spit(path, text):
    with open(path, "w") as f:
        f.write(text)


def run(*args, env=None):
    return subprocess.run([PY, "-I", *args], capture_output=True, text=True, env=env)


class Rfc(unittest.TestCase):
    def test_ed25519_rfc8032(self):
        v = [("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"),
             ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"),
             ]
        for seed, pub in v:
            self.assertEqual(vk.ed25519_public(bytes.fromhex(seed)).hex(), pub)

    def test_x25519_rfc7748(self):
        k = bytes.fromhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4")
        u = bytes.fromhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")
        self.assertEqual(vk.x25519(k, u).hex(), "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552")
        a = bytes.fromhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
        b = bytes.fromhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
        apub, bpub = vk.x25519_public(a), vk.x25519_public(b)
        self.assertEqual(apub.hex(), "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
        self.assertEqual(bpub.hex(), "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
        shared = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"
        self.assertEqual(vk.x25519(a, bpub).hex(), shared)
        self.assertEqual(vk.x25519(b, apub).hex(), shared)

    def test_ed25519_sign_rfc8032(self):
        self.assertEqual(vk.ed25519_sign(bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"), b"").hex(),
                         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")
        self.assertEqual(vk.ed25519_sign(bytes.fromhex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb"), b"\x72").hex(),
                         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00")

    def test_validate_public(self):
        ok = bytes.fromhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
        self.assertIsNone(vk.ed25519_validate_public(ok))
        self.assertIn("small-order", vk.ed25519_validate_public(bytes([1]) + bytes(31)))      # identity
        self.assertIn("small-order", vk.ed25519_validate_public(bytes(32)))                   # order 4
        self.assertIn("non-canonical", vk.ed25519_validate_public(b"\xed" + b"\xff" * 30 + b"\x7f"))  # y = p
        off = next(y for y in range(2, 100) if vk.ed25519_validate_public(y.to_bytes(32, "little")) == "not on the curve")
        self.assertTrue(off)
        self.assertIsNotNone(vk.ed25519_validate_public(b"short"))

    def test_primitives_against_published_vectors(self):
        z = bytes(32)
        self.assertEqual(wr.gcm_encrypt(z, bytes(12), b"", b"")[1].hex(), "530f8afbc74536b9a963b4f1c4cb738b")  # NIST GCM test case 13
        ct, tag = wr.gcm_encrypt(z, bytes(12), bytes(16), b"")                                                    # test case 14
        self.assertEqual((ct.hex(), tag.hex()), ("cea7403d4d606b6e074ec5d3baf39d18", "d0d1c8a799996bf0265b98b5d48ab919"))
        self.assertEqual(wr.aes256_encrypt_block(bytes(range(32)), bytes.fromhex("00112233445566778899aabbccddeeff")).hex(),
                         "8ea2b7ca516745bfeafc49904b496089")                                                      # FIPS 197 C.3
        self.assertEqual(wr.hkdf_sha256(bytes([0x0b] * 22), bytes(range(13)), bytes(range(0xf0, 0xfa)), 42).hex(),
                         "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865")  # RFC 5869 case 1

    def test_key_id(self):
        pub = bytes.fromhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
        self.assertEqual(vk.key_id(pub), hashlib.sha256(pub).digest()[:8])
        self.assertEqual(vk.key_id(pub).hex(), "21fe31dfa154a261")


LIB = "velora.test.library"
DEV_PRIV, EPH_PRIV = bytes(range(1, 33)), bytes(range(33, 65))
NONCE, CK = bytes(range(0xa0, 0xac)), bytes(range(0xc0, 0xe0))
KEK, VNONCE = bytes(range(0x40, 0x60)), bytes(range(0xb0, 0xbc))
WK_BLOB = ("56574b3101aaa8fff703b50b225869aff450549732cbaaed5e5df9b30a6da31cb0e5742bad5ad4a1a768f1a67ba0a1a2a3a4a5a6a7a8a9aaab"
           "9eb9ebbe5dd3cc7205c0854c53b12f3a98da5488b008bb9b068e9230c82bb4809e6004e870a73f77e35234a13d20ce3a")
VAULT_ENTRY = ("564b56319b660e663d66688ab0b1b2b3b4b5b6b7b8b9babbc3c14246058f2d5ef327ed33213b82046ed956fb90c000482b1fbae0556d2c4fee9f6c8b2270d1bca7a76c2f9658b9c4")


class WrapFormats(unittest.TestCase):
    def test_wrapped_key_v1_pinned_vector_and_layout(self):
        dpub = vk.x25519_public(DEV_PRIV)
        blob, p = wr.wrap_key_v1(dpub, EPH_PRIV, NONCE, LIB, CK)
        self.assertEqual(dpub.hex(), "07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c")
        self.assertEqual(p["shared"].hex(), "a84dc7c3c8f058b1b2dc4cd1e9b5dc0a7987f88b6a9564cde3391fc421159e77")
        self.assertEqual(p["wrapping_key"].hex(), "e665ab101a53dad94fd3ac2902d09f512ab0e6400c7631f11b8938559eca08bf")
        self.assertEqual(p["aad"].hex(), "56574b3101aaa8fff703b50b225869aff450549732cbaaed5e5df9b30a6da31cb0e5742bad5ad4a1a768f1a67b76656c6f72612e746573742e6c696272617279")
        self.assertEqual(blob.hex(), WK_BLOB)
        self.assertEqual(len(blob), 105)
        self.assertEqual((blob[:4], blob[4]), (b"VWK1", 1))
        self.assertEqual(blob[5:13], vk.key_id(dpub))
        self.assertEqual(blob[13:45], vk.x25519_public(EPH_PRIV))
        self.assertEqual(blob[45:57], NONCE)
        self.assertEqual(len(blob[57:89]), 32)
        self.assertEqual(len(blob[89:105]), 16)
        self.assertEqual(p["aad"], blob[:45] + LIB.encode())

    def test_wrapped_key_v1_round_trip_and_binding(self):
        blob, _ = wr.wrap_key_v1(vk.x25519_public(DEV_PRIV), EPH_PRIV, NONCE, LIB, CK)
        self.assertEqual(wr.unwrap_key_v1(DEV_PRIV, blob, LIB), CK)
        with self.assertRaises(ValueError):
            wr.unwrap_key_v1(DEV_PRIV, blob, LIB + "x")                     # other library
        with self.assertRaises(ValueError):
            wr.unwrap_key_v1(bytes(range(2, 34)), blob, LIB)                # other device
        for i in range(len(blob)):                                         # every byte is authenticated or checked
            bad = bytearray(blob)
            bad[i] ^= 1
            with self.assertRaises(ValueError, msg="byte %d" % i):
                wr.unwrap_key_v1(DEV_PRIV, bytes(bad), LIB)

    def test_vault_entry_v1_pinned_vector_and_round_trip(self):
        e, q = wr.vault_entry_v1(KEK, VNONCE, LIB, CK)
        self.assertEqual(vk.kek_id(KEK).hex(), "9b660e663d66688a")
        self.assertEqual(q["aad"].hex(), "564b56319b660e663d66688a76656c6f72612f7661756c745f656e7472795f76312f76656c6f72612e746573742e6c696272617279")
        self.assertEqual(e.hex(), VAULT_ENTRY)
        self.assertEqual((len(e), e[:4], e[4:12], e[12:24]), (72, b"VKV1", vk.kek_id(KEK), VNONCE))
        self.assertEqual(len(e[24:56]), 32)
        self.assertEqual(len(e[56:72]), 16)
        self.assertEqual(wr.open_vault_entry_v1(KEK, e, LIB), CK)
        with self.assertRaises(ValueError):
            wr.open_vault_entry_v1(KEK, e, "other.library")
        for i in range(len(e)):
            bad = bytearray(e)
            bad[i] ^= 1
            with self.assertRaises(ValueError, msg="byte %d" % i):
                wr.open_vault_entry_v1(KEK, bytes(bad), LIB)

    def test_doc_pins_the_vectors(self):
        doc = slurp(os.path.join(KEYS, "KEY-HIERARCHY.md"), "r")
        for v in (WK_BLOB, VAULT_ENTRY, "a84dc7c3c8f058b1b2dc4cd1e9b5dc0a7987f88b6a9564cde3391fc421159e77",
                  "e665ab101a53dad94fd3ac2902d09f512ab0e6400c7631f11b8938559eca08bf"):
            self.assertIn(v, doc)

    def test_doc_has_a_row_for_every_key_kind_and_root(self):
        doc = slurp(os.path.join(KEYS, "KEY-HIERARCHY.md"), "r")
        rows = [[c.strip() for c in l.strip().strip("|").split("|")] for l in doc.splitlines() if l.startswith("|")]
        for kind in ("Boot-image signing", "RAUC bundle signing", "Content-manifest signing", "Content-key vault KEK"):
            for root in ("dev", "factory-test", "production"):
                self.assertTrue(any(r[0] == kind and r[1] == root and len(r) >= 5 and all(r[2:5]) for r in rows), "no holder/storage/rotation row for %s / %s" % (kind, root))


class Generate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="velora-keys-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_generate_formats_modes_labels(self):
        out = vk.generate(os.path.join(self.tmp, "keys"))
        self.assertEqual(stat.S_IMODE(os.stat(out).st_mode), 0o700)
        for n in ("content-manifest.ed25519.seed", "content-manifest.ed25519.pub", "kek.aes256", "device.x25519.priv", "device.x25519.pub"):
            p = os.path.join(out, n)
            self.assertEqual(os.path.getsize(p), 32, n)
            self.assertEqual(stat.S_IMODE(os.stat(p).st_mode), 0o600, n)
        rd = lambda n: slurp(os.path.join(out, n))
        self.assertEqual(vk.ed25519_public(rd("content-manifest.ed25519.seed")), rd("content-manifest.ed25519.pub"))
        self.assertEqual(vk.x25519_public(rd("device.x25519.priv")), rd("device.x25519.pub"))
        self.assertNotEqual(rd("content-manifest.ed25519.seed"), rd("kek.aes256"))
        priv = [rd("content-manifest.ed25519.seed"), rd("kek.aes256"), rd("device.x25519.priv")]
        for n in ("content-manifest.json", "kek.json", "device.json"):
            self.assertEqual(stat.S_IMODE(os.stat(os.path.join(out, n)).st_mode), 0o600)
            raw = slurp(os.path.join(out, n))
            side = json.loads(raw)
            self.assertEqual(side["root"], "dev")
            self.assertIs(side["dev_only"], True)
            for p in priv:  # sidecars never carry private material in any encoding
                self.assertNotIn(p.hex().encode(), raw)
        side = json.loads(slurp(os.path.join(out, "content-manifest.json")))
        self.assertEqual(side["key_id"], vk.key_id(rd("content-manifest.ed25519.pub")).hex())
        self.assertEqual(side["algorithm"], "ed25519")
        self.assertIn("DEV-ONLY", slurp(os.path.join(out, "DEV-ONLY.txt"), "r"))

    def test_two_runs_give_different_keys(self):
        a = vk.generate(os.path.join(self.tmp, "a"))
        b = vk.generate(os.path.join(self.tmp, "b"))
        for n in ("kek.aes256", "content-manifest.ed25519.seed", "device.x25519.priv"):
            self.assertNotEqual(slurp(os.path.join(a, n)), slurp(os.path.join(b, n)), n)

    def test_rng_injection_ids_and_sidecars(self):
        stream = iter([bytes([1] * 32), bytes([2] * 32), bytes([3] * 32)])
        out = vk.generate(os.path.join(self.tmp, "fixed"), rng=lambda n: next(stream))
        kek, dpub = bytes([2] * 32), vk.x25519_public(bytes([3] * 32))
        self.assertEqual(slurp(os.path.join(out, "kek.aes256")), kek)
        self.assertEqual(vk.kek_id(kek), hashlib.sha256(b"velora-kek-id:" + kek).digest()[:8])
        self.assertEqual(json.loads(slurp(os.path.join(out, "kek.json")))["key_id"], vk.kek_id(kek).hex())
        d = json.loads(slurp(os.path.join(out, "device.json")))
        self.assertEqual((d["key_id"], d["public_key_hex"]), (vk.key_id(dpub).hex(), dpub.hex()))
        self.assertNotIn("public_key_hex", json.loads(slurp(os.path.join(out, "kek.json"))))

    def test_unsafe_targets_refused(self):
        real = os.path.join(self.tmp, "real")
        os.makedirs(real)
        link = os.path.join(self.tmp, "linkdir")
        os.symlink(real, link)
        with self.assertRaises(vk.KeyToolError):
            vk.generate(link)  # a symlinked output directory is not followed
        busy = os.path.join(self.tmp, "busy")
        os.makedirs(busy, mode=0o755)
        os.chmod(busy, 0o755)
        spit(os.path.join(busy, "other.txt"), "x")
        with self.assertRaises(vk.KeyToolError):
            vk.generate(busy)  # non-empty and world-readable
        self.assertEqual(stat.S_IMODE(os.stat(busy).st_mode), 0o755)  # never chmod-ed
        empty = os.path.join(self.tmp, "empty")
        os.makedirs(empty)
        os.chmod(empty, 0o755)
        vk.generate(empty)
        self.assertEqual(stat.S_IMODE(os.stat(empty).st_mode), 0o755)  # only a directory the tool created gets 0700
        victim = os.path.join(self.tmp, "victim")
        spit(victim, "keep")
        os.remove(os.path.join(empty, "kek.aes256"))
        os.symlink(victim, os.path.join(empty, "kek.aes256"))
        for force in (False, True):
            with self.assertRaises(vk.KeyToolError):
                vk.generate(empty, force=force)  # existing non-regular entry: refused even with --force
        self.assertEqual(slurp(victim, "r"), "keep")
        self.assertEqual([n for n in os.listdir(empty) if n.startswith(".stage-")], [])

    def test_force_replaces_atomically_and_leaves_no_stage(self):
        out = vk.generate(os.path.join(self.tmp, "f"))
        vk.generate(out, force=True)
        self.assertEqual([n for n in os.listdir(out) if n.startswith(".")], [])
        r = run(os.path.join(KEYS, "velora_keys.py"), "generate", "--out", out, "--force")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("unreadable", r.stderr)

    def test_refuses_repo_root_without_git(self):
        self.assertIn("repository", vk.refusal_reason(os.path.join(vk.REPO_ROOT, "never-created")))
        self.assertIsNone(vk.refusal_reason(os.path.join(self.tmp, "x")))

    def test_refuses_git_work_tree(self):
        repo = os.path.join(self.tmp, "repo")
        os.makedirs(os.path.join(repo, ".git"))
        with self.assertRaises(vk.KeyToolError):
            vk.generate(os.path.join(repo, "sub", "keys"))
        self.assertFalse(os.path.exists(os.path.join(repo, "sub")))
        wt = os.path.join(self.tmp, "worktree")  # a linked worktree has a .git FILE
        os.makedirs(wt)
        spit(os.path.join(wt, ".git"), "gitdir: /elsewhere\n")
        with self.assertRaises(vk.KeyToolError):
            vk.generate(os.path.join(wt, "keys"))
        link = os.path.join(self.tmp, "link")  # a symlink into the repo does not bypass the check
        os.symlink(repo, link)
        with self.assertRaises(vk.KeyToolError):
            vk.generate(os.path.join(link, "keys"))
        r = run(os.path.join(KEYS, "velora_keys.py"), "generate", "--out", os.path.join(repo, "k"))
        self.assertEqual(r.returncode, 1)
        self.assertIn("git work tree", r.stderr)

    def test_refuses_overwrite_without_force(self):
        out = vk.generate(os.path.join(self.tmp, "k"))
        before = slurp(os.path.join(out, "kek.aes256"))
        with self.assertRaises(vk.KeyToolError):
            vk.generate(out)
        self.assertEqual(slurp(os.path.join(out, "kek.aes256")), before)
        vk.generate(out, force=True)
        self.assertNotEqual(slurp(os.path.join(out, "kek.aes256")), before)

    def test_default_dir(self):
        self.assertEqual(vk.default_dir({"XDG_DATA_HOME": "/x/share", "HOME": "/h"}), "/x/share/velora-dev-keys")
        self.assertEqual(vk.default_dir({"HOME": "/h"}), "/h/.velora-dev-keys")
        self.assertEqual(vk.default_dir({"XDG_DATA_HOME": "relative", "HOME": "/h"}), "/h/.velora-dev-keys")

    def test_cli_generate_show_trust_entry(self):
        out = os.path.join(self.tmp, "cli")
        env = dict(os.environ, HOME=self.tmp)
        r = run(os.path.join(KEYS, "velora_keys.py"), "generate", "--out", out, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        te = run(os.path.join(KEYS, "velora_keys.py"), "trust-entry", "--dir", out).stdout.strip()
        self.assertTrue(te.startswith("dev content-manifest "))
        self.assertIn(te, r.stdout)
        sh = run(os.path.join(KEYS, "velora_keys.py"), "show", "--dir", out)
        self.assertEqual(sh.returncode, 0)
        seed = slurp(os.path.join(out, "content-manifest.ed25519.seed")).hex()
        self.assertNotIn(seed, sh.stdout + r.stdout)
        # default dir honours XDG_DATA_HOME
        env2 = dict(os.environ, XDG_DATA_HOME=os.path.join(self.tmp, "xdg"))
        self.assertEqual(run(os.path.join(KEYS, "velora_keys.py"), "default-dir", env=env2).stdout.strip(),
                         os.path.join(self.tmp, "xdg", "velora-dev-keys"))


class Embed(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="velora-embed-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.trust = os.path.join(self.tmp, "trust")
        os.makedirs(self.trust)
        self.dev = vk.generate(os.path.join(self.tmp, "devkeys"))
        self.out = os.path.join(self.tmp, "gen", "h.hpp")

    def entry(self, root, pub=None):
        pub = pub or vk.ed25519_public(os.urandom(32))
        return "%s content-manifest %s %s\n" % (root, vk.key_id(pub).hex(), pub.hex()), pub

    def put(self, root, text):
        spit(os.path.join(self.trust, root + ".pub-list"), text)

    def embed(self, variant, *extra):
        return run(os.path.join(KEYS, "embed_trusted_keys.py"), "--variant", variant, "--trust-dir", self.trust, "--output", self.out, *extra)

    def test_dev_trusts_only_dev_keys(self):
        d, dpub = self.entry("dev")
        f, fpub = self.entry("factory-test")
        p, ppub = self.entry("production")
        self.put("dev", "# c\n" + d)
        self.put("factory-test", f)
        self.put("production", p)
        r = self.embed("dev")
        self.assertEqual(r.returncode, 0, r.stderr)
        h = slurp(self.out, "r")
        self.assertIn(", ".join("0x%02x" % b for b in dpub), h)
        self.assertNotIn(", ".join("0x%02x" % b for b in fpub), h)
        self.assertNotIn(", ".join("0x%02x" % b for b in ppub), h)
        self.assertIn('kTrustedRoot[] = "dev"', h)
        self.assertEqual(self.embed("production").returncode, 0)
        h = slurp(self.out, "r")
        self.assertIn(", ".join("0x%02x" % b for b in ppub), h)
        self.assertNotIn(", ".join("0x%02x" % b for b in dpub), h)

    def test_dev_keys_dir_added_to_dev_only(self):
        self.put("dev", "")
        r = self.embed("dev", "--dev-keys-dir", self.dev)
        self.assertEqual(r.returncode, 0, r.stderr)
        pub = slurp(os.path.join(self.dev, "content-manifest.ed25519.pub"))
        self.assertIn(", ".join("0x%02x" % b for b in pub), slurp(self.out, "r"))

    def test_production_refuses_dev_public_key(self):
        pub = slurp(os.path.join(self.dev, "content-manifest.ed25519.pub"))
        d, _ = self.entry("dev", pub)
        p, _ = self.entry("production")
        self.put("production", p)
        r = self.embed("production", "--dev-keys-dir", self.dev)  # dev key dir into production
        self.assertEqual(r.returncode, 1)
        self.assertIn("dev public key", " ".join(r.stderr.split()))
        self.assertFalse(os.path.exists(self.out))
        self.put("production", p + d)  # dev-labelled entry in the production list
        r = self.embed("production")
        self.assertEqual(r.returncode, 1)
        self.assertIn("dev public key into a production variant", r.stderr)
        self.put("production", p + d.replace("dev ", "production ", 1))  # relabelled: same key also in the dev list
        self.put("dev", d)
        r = self.embed("production")
        self.assertEqual(r.returncode, 1)
        self.assertIn("both dev and production", r.stderr)
        self.assertFalse(os.path.exists(self.out))
        self.assertEqual(self.embed("factory-test", "--dev-keys-dir", self.dev).returncode, 1)

    def test_production_without_key_list_refused(self):
        r = self.embed("production")  # no production.pub-list at all
        self.assertEqual(r.returncode, 1)
        self.assertIn("no production key list", r.stderr)
        self.put("production", "# only comments\n")
        self.assertEqual(self.embed("production").returncode, 1)
        self.assertFalse(os.path.exists(self.out))

    def test_bad_lists_refused(self):
        for text in ("dev content-manifest 0000000000000000 " + "ab" * 32 + "\n",       # key id mismatch
                     "dev content-manifest short abc\n",
                     "weird content-manifest " + "0" * 16 + " " + "0" * 64 + "\n",
                     "dev boot-signing " + "0" * 16 + " " + "0" * 64 + "\n",
                     "dev content-manifest " + "21FE31DFA154A261" + " " + "D75A980182B10AB7D54BFED3C964073A0EE172F3DAA62325AF021A68F707511A" + "\n",
                     "factory-test content-manifest 21fe31dfa154a261 d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a\n"):
            self.put("dev", text)
            self.assertEqual(self.embed("dev").returncode, 1, text)

    def test_committed_lists_are_sane(self):
        trust = os.path.join(KEYS, "trust")
        for root in emb.ROOTS:
            for e in emb.read_list(os.path.join(trust, root + ".pub-list")):
                self.assertEqual(e["root"], root)
        r = run(os.path.join(KEYS, "embed_trusted_keys.py"), "--variant", "production", "--output", self.out)
        self.assertEqual(r.returncode, 1, "the committed production list stays empty until the Story 6.10 ceremony")
        self.assertEqual(run(os.path.join(KEYS, "embed_trusted_keys.py"), "--variant", "dev", "--output", self.out).returncode, 0)

    def test_factory_test_positive_and_stale_header_deleted(self):
        f, fpub = self.entry("factory-test")
        self.put("factory-test", f)
        self.assertEqual(self.embed("factory-test").returncode, 0)
        self.assertIn("namespace factory_test", slurp(self.out, "r"))
        self.assertIn(", ".join("0x%02x" % b for b in fpub), slurp(self.out, "r"))
        self.assertTrue(os.path.exists(self.out))
        self.assertEqual(self.embed("production").returncode, 1)  # no production list: stale header must go
        self.assertTrue(os.path.exists(self.out) is False)
        self.assertEqual(self.embed("factory-test").returncode, 0)
        self.put("factory-test", "factory-test content-manifest 0 0\n")
        self.assertEqual(self.embed("factory-test").returncode, 1)
        self.assertFalse(os.path.exists(self.out))

    def test_empty_factory_list_embeds_zero_keys(self):
        self.put("factory-test", "")
        self.assertEqual(self.embed("factory-test").returncode, 0)
        self.assertIn("kTrustedContentKeyCount = 0", slurp(self.out, "r"))
        self.assertIn("kIsProduction = false", slurp(self.out, "r"))

    def test_production_header_flags_production(self):
        p, _ = self.entry("production")
        self.put("production", p)
        self.assertEqual(self.embed("production").returncode, 0)
        self.assertIn("kIsProduction = true", slurp(self.out, "r"))

    def test_relabelled_dev_and_fixture_keys_refused_with_custom_trust_dir(self):
        pub = slurp(os.path.join(self.dev, "content-manifest.ed25519.pub"))
        line = "production content-manifest %s %s\n" % (vk.key_id(pub).hex(), pub.hex())
        self.put("production", line)  # fresh dev key relabelled production; only the dev key directory knows it
        deny = os.path.join(self.tmp, "deny")
        os.makedirs(deny)
        spit(os.path.join(deny, "dev.pub-list"), line.replace("production", "dev", 1))
        r = self.embed("production", "--deny-dir", deny)
        self.assertEqual(r.returncode, 1)
        self.assertIn("dev/fixture key", r.stderr)
        # the committed test fixture key relabelled as production / factory-test, custom trust dir with no dev list at all
        fx = emb.read_list(FIXTURE_LIST)[0]
        for variant in ("production", "factory-test"):
            self.put(variant, "%s content-manifest %s %s\n" % (variant, fx["key_id"], fx["pub"]))
            self.assertEqual(self.embed(variant).returncode, 1, variant)
            self.assertIn("dev/fixture key", self.embed(variant).stderr)
            os.remove(os.path.join(self.trust, variant + ".pub-list"))

    def test_dev_keys_dir_checked_against_other_root_lists(self):
        pub = slurp(os.path.join(self.dev, "content-manifest.ed25519.pub"))
        self.put("production", "production content-manifest %s %s\n" % (vk.key_id(pub).hex(), pub.hex()))
        self.put("dev", "")
        r = self.embed("dev", "--dev-keys-dir", self.dev)
        self.assertEqual(r.returncode, 1)
        self.assertIn("listed under the production root", r.stderr)

    def test_invalid_points_refused(self):
        bad = {"identity": bytes([1]) + bytes(31), "order4": bytes(32), "y>=p": b"\xed" + b"\xff" * 30 + b"\x7f"}
        off = next(y for y in range(2, 100) if vk.ed25519_validate_public(y.to_bytes(32, "little")) == "not on the curve")
        bad["off-curve"] = off.to_bytes(32, "little")
        for name, pub in bad.items():
            self.put("dev", "dev content-manifest %s %s\n" % (vk.key_id(pub).hex(), pub.hex()))
            r = self.embed("dev")
            self.assertEqual(r.returncode, 1, name)
            self.assertIn("invalid Ed25519 public key", r.stderr)

    def test_non_utf8_list_is_a_clean_refusal(self):
        with open(os.path.join(self.trust, "dev.pub-list"), "wb") as f:
            f.write(b"\xff\xfe\x80 not utf-8\n")
        r = self.embed("dev")
        self.assertEqual(r.returncode, 1)
        self.assertIn("REFUSED", r.stderr)
        self.assertNotIn("Traceback", r.stderr)

    def test_fixture_list_embeds(self):
        shutil.copy(FIXTURE_LIST, os.path.join(self.trust, "dev.pub-list"))
        self.assertEqual(self.embed("dev").returncode, 0)
        self.assertIn("kTrustedContentKeyCount = 1", slurp(self.out, "r"))


@unittest.skipUnless(os.environ.get("CMAKE_COMMAND"), "CMAKE_COMMAND not set")
class CMakeFunction(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="velora-cmake-embed-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.trust = os.path.join(self.tmp, "trust")
        os.makedirs(self.trust)
        self.dev = vk.generate(os.path.join(self.tmp, "devkeys"))

    def cmake(self, variant, *extra):
        return subprocess.run([os.environ["CMAKE_COMMAND"], "-DVARIANT=" + variant, "-DTRUST_DIR=" + self.trust, "-DOUTPUT_DIR=" + os.path.join(self.tmp, "out"),
                               *extra, "-P", os.path.join(HERE, "embed_variant.cmake")], capture_output=True, text=True)

    def test_dev_ok_production_refused(self):
        spit(os.path.join(self.trust, "dev.pub-list"), vk.trust_entry(self.dev) + "\n")
        r = self.cmake("dev")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "out", "velora_trusted_keys_dev.hpp")))
        # production with no production list: configure fails
        r = self.cmake("production")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("refused", r.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "out", "velora_trusted_keys_production.hpp")))
        # production with a dev key in its list: configure fails
        spit(os.path.join(self.trust, "production.pub-list"), vk.trust_entry(self.dev) + "\n")
        r = self.cmake("production")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("dev public key", " ".join(r.stderr.split()))
        # production with a dev key dir: configure fails
        os.remove(os.path.join(self.trust, "production.pub-list"))
        r = self.cmake("production", "-DDEV_KEYS_DIR=" + self.dev)
        self.assertNotEqual(r.returncode, 0)
        # production with a real production-labelled key: ok
        pub = vk.ed25519_public(os.urandom(32))
        spit(os.path.join(self.trust, "production.pub-list"), "production content-manifest %s %s\n" % (vk.key_id(pub).hex(), pub.hex()))
        r = self.cmake("production")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "out", "velora_trusted_keys_production.hpp")))


if __name__ == "__main__":
    unittest.main(verbosity=1)
