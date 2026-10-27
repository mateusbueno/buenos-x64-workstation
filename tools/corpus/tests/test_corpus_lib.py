"""Offline tests for tools/corpus (Story 5.4): manifest, fetch, hash pinning, safe extraction, exclusions, guard.
No network: the downloader is fed a fake opener serving in-memory archives.   python3 -m unittest discover -s tools/corpus/tests
"""
import contextlib
import hashlib
import io
import json
import os
import sys
import tarfile
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import corpus_lib as cl  # noqa: E402
import corpus  # noqa: E402
import midi_suite  # noqa: E402


def make_tar(members, mode="w:gz"):
    """members: list of (name, bytes|None, kind) kind in file|dir|sym|hard|fifo; returns bytes."""
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode=mode) as t:
        for name, data, kind in members:
            ti = tarfile.TarInfo(name)
            if kind == "dir":
                ti.type = tarfile.DIRTYPE
                t.addfile(ti)
            elif kind == "sym":
                ti.type = tarfile.SYMTYPE
                ti.linkname = data
                t.addfile(ti)
            elif kind == "hard":
                ti.type = tarfile.LNKTYPE
                ti.linkname = data
                t.addfile(ti)
            elif kind == "fifo":
                ti.type = tarfile.FIFOTYPE
                t.addfile(ti)
            else:
                ti.size = len(data)
                t.addfile(ti, io.BytesIO(data))
    return bio.getvalue()


def make_zip(members):
    bio = io.BytesIO()
    with zipfile.ZipFile(bio, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in members:
            z.writestr(name, data)
    return bio.getvalue()


class FakeResp(io.BytesIO):
    def __init__(self, data, headers=None):
        super().__init__(data)
        self.headers = headers if headers is not None else {"Content-Length": str(len(data))}

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


class FakeOpener:
    def __init__(self, data, headers=None):
        self.data, self.headers, self.calls = data, headers, 0

    def open(self, req, timeout=None):
        self.calls += 1
        return FakeResp(self.data, self.headers)


def entry(**kw):
    e = {"id": "lib-a", "name": "Lib A", "kind": "required", "source_url": "https://codeload.github.com/o/r/tar.gz/abc", "version": "1",
         "license": "CC0 1.0", "license_url": "https://creativecommons.org/publicdomain/zero/1.0/"}
    e.update(kw)
    return e


GOOD = [("root/", None, "dir"), ("root/a.sfz", b"<region> sample=x.wav\n", "file"), ("root/s/x.wav", b"RIFF", "file")]


class ManifestTests(unittest.TestCase):
    def test_shipped_manifest_is_valid(self):
        m = cl.load_manifest(corpus.MANIFEST)
        ids = [e["id"] for e in m["libraries"]]
        for need in ("salamander-grand-piano-v3", "vsco2-community", "sonatina-symphonic-orchestra", "karoryfer-scarypiano", "karoryfer-meatbass",
                     "karoryfer-weresax", "karoryfer-bear-sax"):
            self.assertIn(need, ids)

    def test_exclusion_needs_a_reason(self):
        base = {"schema": 1, "libraries": [{"id": "x", "name": "X", "kind": "pianobook", "excluded": {"reason": ""}}]}
        with self.assertRaises(cl.CorpusError):
            cl.validate_manifest(base)
        base["libraries"][0]["excluded"]["reason"] = "needs a login to download"
        cl.validate_manifest(base)

    def test_exclusions_are_not_fetched_or_selected(self):
        m = {"schema": 1, "libraries": [entry(), {"id": "pb", "name": "PB", "kind": "pianobook", "excluded": {"reason": "login required to download"}}]}
        cl.validate_manifest(m)
        self.assertEqual([e["id"] for e in cl.included(m)], ["lib-a"])
        self.assertEqual([e["id"] for e in cl.excluded(m)], ["pb"])
        with self.assertRaises(cl.CorpusError):
            corpus.selected(m, ["pb"])

    def test_missing_fields_and_bad_urls(self):
        for patch in ({"version": ""}, {"license": None}, {"source_url": "http://codeload.github.com/o/r"},
                      {"source_url": "https://evil.example.com/x.tar.gz"}, {"source_url": "https://user:pw@codeload.github.com/o"},
                      {"sha256": "ABC"}, {"tolerance_db": 3}, {"archive": "rar"}, {"limits": {"nope": 1}},
                      {"skip_files": [{"path": "../x.sfz", "reason": "some long reason here"}]},
                      {"skip_files": [{"path": "a.sfz", "reason": "short"}]}):
            m = {"schema": 1, "libraries": [entry(**patch)]}
            with self.assertRaises(cl.CorpusError, msg=str(patch)):
                cl.validate_manifest(m)

    def test_duplicate_ids(self):
        with self.assertRaises(cl.CorpusError):
            cl.validate_manifest({"schema": 1, "libraries": [entry(), entry()]})


class ExtractTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def extract(self, data, fmt="tar.gz", limits=None):
        arch = os.path.join(self.tmp.name, "a." + fmt.replace(".", "_"))
        with open(arch, "wb") as fh:
            fh.write(data)
        dest = os.path.join(self.tmp.name, "out")
        return cl.safe_extract(arch, fmt, dest, limits), dest

    def test_good_archive_strips_the_root(self):
        (n, files), dest = self.extract(make_tar(GOOD))
        self.assertEqual(files, 2)
        self.assertTrue(os.path.isfile(os.path.join(dest, "a.sfz")))
        self.assertTrue(os.path.isfile(os.path.join(dest, "s", "x.wav")))

    def test_good_zip(self):
        (n, files), dest = self.extract(make_zip([("root/a.sfz", b"x"), ("root/d/b.wav", b"y")]), "zip")
        self.assertEqual(files, 2)
        self.assertTrue(os.path.isfile(os.path.join(dest, "d", "b.wav")))

    def bad(self, data, fmt="tar.gz", limits=None):
        with self.assertRaises(cl.CorpusError):
            self.extract(data, fmt, limits)
        self.assertFalse(os.path.exists(os.path.join(self.tmp.name, "out")), "a refused archive must leave no target")

    def test_path_traversal_rejected(self):
        self.bad(make_tar([("root/a", b"x", "file"), ("root/../../evil", b"x", "file")]))
        self.bad(make_tar([("../evil", b"x", "file")]))
        self.bad(make_zip([("root/../../evil", b"x")]), "zip")
        self.assertFalse(os.path.exists(os.path.join(self.tmp.name, "evil")))

    def test_absolute_and_odd_names_rejected(self):
        self.bad(make_tar([("/etc/evil", b"x", "file")]))
        self.bad(make_tar([("C:/evil", b"x", "file")]))
        self.bad(make_tar([("root\\evil", b"x", "file")]))
        self.bad(make_zip([("/abs", b"x")]), "zip")

    def test_links_and_special_files_rejected(self):
        self.bad(make_tar([("root/a", b"x", "file"), ("root/l", "/etc/passwd", "sym")]))
        self.bad(make_tar([("root/a", b"x", "file"), ("root/l", "a", "sym")]))  # even a harmless link
        self.bad(make_tar([("root/a", b"x", "file"), ("root/h", "a", "hard")]))
        self.bad(make_tar([("root/p", None, "fifo")]))

    def test_symlink_in_zip_rejected(self):
        bio = io.BytesIO()
        with zipfile.ZipFile(bio, "w") as z:
            zi = zipfile.ZipInfo("root/l")
            zi.create_system = 3
            zi.external_attr = (0o120777 << 16)
            z.writestr(zi, "/etc/passwd")
        self.bad(bio.getvalue(), "zip")

    def test_duplicates_and_case_collisions_rejected(self):
        self.bad(make_tar([("r/A.wav", b"1", "file"), ("r/a.wav", b"2", "file")]))
        self.bad(make_tar([("r/a", b"1", "file"), ("r/a", b"2", "file")]))

    def test_unicode_normalisation_collision_rejected(self):
        self.bad(make_tar([("r/caf\u00e9.wav", b"1", "file"), ("r/cafe\u0301.wav", b"2", "file")]))

    def test_truncated_gzip_is_a_corpus_error(self):
        data = make_tar([("r/a", b"x" * 100000, "file")])
        self.bad(data[: len(data) // 2])

    def test_file_below_file_rejected(self):
        self.bad(make_tar([("r/a", b"1", "file"), ("r/a/b", b"2", "file")]))

    def test_size_and_count_limits(self):
        data = make_tar([("r/a", b"x" * 5000, "file")])
        self.bad(data, limits={"max_file_bytes": 1000})
        self.bad(make_tar([("r/a", b"x" * 600, "file"), ("r/b", b"x" * 600, "file")]), limits={"max_total_bytes": 1000})
        self.bad(make_tar([("r/%d" % i, b"x", "file") for i in range(10)]), limits={"max_members": 5})

    def test_zip_bomb_ratio_rejected(self):
        bomb = make_tar([("r/zeros", b"\0" * 5_000_000, "file")])
        self.bad(bomb, limits={"max_ratio": 50})

    def test_decompression_cap_applies_during_listing(self):
        bomb = make_tar([("r/zeros", b"\0" * 3_000_000, "file")])
        with self.assertRaisesRegex(cl.CorpusError, "decompresses beyond"):
            arch = os.path.join(self.tmp.name, "b.tgz")
            with open(arch, "wb") as fh:
                fh.write(bomb)
            list(cl.list_archive(arch, "tar.gz", cap=1_000_000))

    def test_existing_target_refused(self):
        arch = os.path.join(self.tmp.name, "a.tgz")
        with open(arch, "wb") as fh:
            fh.write(make_tar(GOOD))
        dest = os.path.join(self.tmp.name, "taken")
        os.makedirs(dest)
        with self.assertRaises(cl.CorpusError):
            cl.safe_extract(arch, "tar.gz", dest)

    def test_not_an_archive(self):
        self.bad(b"this is not a tarball")

    def test_failure_midway_leaves_no_partial_tree(self):
        # a member whose real size differs from its header cannot be built with tarfile; use the size limit on the second file instead
        self.bad(make_tar([("r/a", b"x", "file"), ("r/b", b"y" * 5000, "file")]), limits={"max_file_bytes": 1000})


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.cache = os.path.join(self.tmp.name, "cache")
        self.data = make_tar(GOOD)
        self.sha = hashlib.sha256(self.data).hexdigest()
        self.manifest = os.path.join(self.tmp.name, "m.json")
        self.write_manifest(entry())
        self.log = []

    def write_manifest(self, e):
        with open(self.manifest, "w") as fh:
            json.dump({"schema": 1, "libraries": [e]}, fh)

    def f(self, e, **kw):
        return cl.fetch(e, self.cache, opener=kw.pop("opener", FakeOpener(self.data)), log=self.log.append, **kw)

    def test_unpinned_refused_without_pin_flag(self):
        with self.assertRaisesRegex(cl.CorpusError, "no sha256 pinned"):
            self.f(entry())
        self.assertFalse(os.path.exists(self.cache))

    def test_tofu_pin_writes_the_manifest_once(self):
        tree, sha = self.f(entry(), pin_manifest=self.manifest)
        self.assertEqual(sha, self.sha)
        self.assertEqual(json.load(open(self.manifest))["libraries"][0]["sha256"], self.sha)
        self.assertTrue(os.path.isfile(os.path.join(tree, "a.sfz")))
        with self.assertRaises(cl.CorpusError):  # never overwrites an existing pin
            cl.pin_hash(self.manifest, "lib-a", "0" * 64)

    def test_hash_mismatch_fails_and_leaves_nothing(self):
        with self.assertRaisesRegex(cl.CorpusError, "HASH MISMATCH"):
            self.f(entry(sha256="0" * 64))
        left = [n for n in os.listdir(os.path.join(self.cache, "lib-a"))] if os.path.isdir(os.path.join(self.cache, "lib-a")) else []
        self.assertEqual(left, [])

    def test_cache_hit_does_not_download(self):
        e = entry(sha256=self.sha)
        self.f(e)
        op = FakeOpener(b"should not be read")
        tree, _ = self.f(e, opener=op)
        self.assertEqual(op.calls, 0)
        self.assertIn("cache hit", self.log[-1])

    def test_cache_tampering_detected(self):
        e = entry(sha256=self.sha)
        tree, _ = self.f(e)
        with open(os.path.join(tree, "a.sfz"), "ab") as fh:  # size change: caught by the cheap listing check
            fh.write(b"x")
        with self.assertRaisesRegex(cl.CorpusError, "changed since extraction"):
            self.f(e)

    def test_deep_check_catches_same_size_edit(self):
        e = entry(sha256=self.sha)
        tree, _ = self.f(e)
        p = os.path.join(tree, "s", "x.wav")
        with open(p, "r+b") as fh:
            fh.write(b"XXXX")  # same size, other bytes
        self.f(e)  # the listing check cannot see it (documented)
        with self.assertRaisesRegex(cl.CorpusError, "deep"):
            self.f(e, deep=True)

    def test_symlink_planted_in_cache_detected(self):
        e = entry(sha256=self.sha)
        tree, _ = self.f(e)
        os.symlink("/etc/passwd", os.path.join(tree, "evil"))
        with self.assertRaisesRegex(cl.CorpusError, "symlink"):
            self.f(e)

    def test_download_limits(self):
        e = entry(sha256=self.sha, limits={"max_archive_bytes": 10})
        with self.assertRaisesRegex(cl.CorpusError, "limit"):
            self.f(e)
        # a server that lies about Content-Length is caught while streaming
        with self.assertRaisesRegex(cl.CorpusError, "exceeded"):
            self.f(e, opener=FakeOpener(self.data, headers={}))

    def test_corrupt_archive_with_matching_hash_still_refused(self):
        junk = b"not an archive"
        e = entry(sha256=hashlib.sha256(junk).hexdigest())
        with self.assertRaises(cl.CorpusError):
            self.f(e, opener=FakeOpener(junk))
        self.assertEqual(os.listdir(os.path.join(self.cache, "lib-a")), [])

    def test_traversal_archive_fails_the_fetch(self):
        evil = make_tar([("r/../../x", b"x", "file")])
        e = entry(sha256=hashlib.sha256(evil).hexdigest())
        with self.assertRaises(cl.CorpusError):
            self.f(e, opener=FakeOpener(evil))
        self.assertFalse(os.path.exists(os.path.join(self.tmp.name, "x")))

    def test_redirect_to_other_host_refused(self):
        with self.assertRaises(cl.CorpusError):
            cl.check_url("https://evil.example.com/x", "redirect")
        with self.assertRaises(cl.CorpusError):
            cl.check_url("http://github.com/x", "redirect")


def quiet_main(argv):
    with contextlib.redirect_stdout(io.StringIO()):
        return corpus.main(argv)


class GuardAndSuiteTests(unittest.TestCase):
    def test_guard_fails_on_corpus_audio_and_passes_on_clean_tree(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(quiet_main(["guard", "--root", d]), 0)
            os.makedirs(os.path.join(d, "tools", "corpus"))
            with open(os.path.join(d, "tools", "corpus", "piano.flac"), "wb") as fh:
                fh.write(b"fLaC")
            self.assertEqual(quiet_main(["guard", "--root", d]), 1)
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "tools", "offline-render", "tests"))
            with open(os.path.join(d, "tools", "offline-render", "tests", "ok.wav"), "wb") as fh:
                fh.write(b"RIFF")
            self.assertEqual(quiet_main(["guard", "--root", d]), 0)
            with open(os.path.join(d, "big.wav"), "wb") as fh:
                fh.write(b"RIFF")
            self.assertEqual(quiet_main(["guard", "--root", d]), 1)

    def test_midi_suite_is_deterministic_and_well_formed(self):
        for n in midi_suite.SUITES:
            a, b = midi_suite.build(n), midi_suite.build(n)
            self.assertEqual(a, b)
            self.assertTrue(a.startswith(b"MThd\0\0\0\x06\0\0\0\x01\x01\xe0MTrk"))
            self.assertTrue(a.endswith(b"\xff\x2f\x00"))
            self.assertEqual(int.from_bytes(a[18:22], "big"), len(a) - 22)
        self.assertEqual(set(midi_suite.SUITES), {"range", "velocity", "round_robin", "keyswitches", "releases", "sustained_loops"})

    def test_markdown_report_lists_exclusions(self):
        rep = {"generated": "t", "manifest": "m", "libraries": [], "excluded": [{"id": "pb", "name": "PB", "reason": "login required"}], "failed": 0}
        self.assertIn("login required", corpus.render_markdown(rep))

    def test_null_output_parser(self):
        r = corpus.parse_null("file: x\n  result: PASS residual -64.2 dB offset 0\n  worst window -61.0 dB\n")
        self.assertEqual(r.get("residual_db"), -64.2)


if __name__ == "__main__":
    unittest.main()
