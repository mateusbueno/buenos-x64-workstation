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


def local_entry(**kw):
    e = {"id": "pb-x", "name": "PB X", "kind": "pianobook", "source_kind": "local_dir", "local_hint": "~/Downloads/pb-x/", "version": "1",
         "license": "none stated", "license_url": "https://www.pianobook.co.uk/profile/someone"}
    e.update(kw)
    return e


def make_local(root, extra=None):
    os.makedirs(os.path.join(root, "samples"))
    files = {"x.sfz": b"<region> sample=samples/a.wav\n", "samples/a.wav": b"RIFF-a", "samples/B.WAV": b"RIFF-bb",
             ".DS_Store": b"junk", "readme.txt": b"hi", "cover.jpg": b"jpg", "x.dspreset": b"<xml/>", "samples/._a.wav": b"apple"}
    files.update(extra or {})
    for rel, data in files.items():
        with open(os.path.join(root, *rel.split("/")), "wb") as fh:
            fh.write(data)


class LocalDirTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, ignore_errors=True))
        self.src = os.path.join(self.tmp, "src")
        self.cache = os.path.join(self.tmp, "cache")
        make_local(self.src)
        self.mf = os.path.join(self.tmp, "m.json")
        self.e = local_entry()
        with open(self.mf, "w") as fh:
            json.dump({"schema": 1, "libraries": [self.e]}, fh)
        self.log = lambda *_: None

    def fetch(self, entry=None, **kw):
        return cl.fetch_local(entry or self.e, self.cache, overrides={self.e["id"]: self.src}, log=self.log, **kw)

    def test_manifest_rules(self):
        cl.validate_entry(local_entry())
        for bad in ({"local_hint": "/Users/someone/Downloads/x"}, {"local_hint": "C:\\\\x"}, {"local_hint": "foo/home/x"}, {"source_url": "https://github.com/a/b"},
                    {"sha256": "0" * 64}, {"expected_tree_sha256": "xyz"}):
            with self.assertRaises(cl.CorpusError, msg=str(bad)):
                cl.validate_entry(local_entry(**bad))
        with self.assertRaises(cl.CorpusError):
            cl.validate_entry(entry(local_hint="x"))

    def test_digest_is_deterministic_and_selects_only_sfz_and_audio(self):
        d1, n, c = cl.copy_local(self.src, os.path.join(self.tmp, "c1"))
        d2, _, _ = cl.copy_local(self.src, os.path.join(self.tmp, "c2"))
        self.assertEqual(d1, d2)
        self.assertEqual(c, 3)
        self.assertEqual(sorted(os.listdir(os.path.join(self.tmp, "c1"))), ["samples", "x.sfz"])
        self.assertEqual(sorted(os.listdir(os.path.join(self.tmp, "c1", "samples"))), ["B.WAV", "a.wav"])
        self.assertEqual(cl.local_tree_digest(os.path.join(self.tmp, "c1")), d1)
        self.assertEqual(os.stat(os.path.join(self.tmp, "c1", "x.sfz")).st_mode & 0o777, 0o644)

    def test_digest_changes_with_content_size_and_name(self):
        base, _, _ = cl.copy_local(self.src, os.path.join(self.tmp, "c1"))
        with open(os.path.join(self.src, "samples", "a.wav"), "wb") as fh:
            fh.write(b"RIFF-X")  # same size, other bytes
        d, _, _ = cl.copy_local(self.src, os.path.join(self.tmp, "c2"))
        self.assertNotEqual(base, d)
        os.rename(os.path.join(self.src, "samples", "a.wav"), os.path.join(self.src, "samples", "c.wav"))
        d3, _, _ = cl.copy_local(self.src, os.path.join(self.tmp, "c3"))
        self.assertNotIn(d3, (base, d))

    def test_tofu_pin_then_mismatch_is_hard_failure(self):
        tree, digest = self.fetch(pin_manifest=self.mf)
        with open(self.mf) as fh:
            self.assertEqual(json.load(fh)["libraries"][0]["expected_tree_sha256"], digest)
        self.assertEqual(os.path.basename(os.path.dirname(tree)), digest)
        with open(os.path.join(self.src, "samples", "a.wav"), "ab") as fh:
            fh.write(b"tampered")
        pinned = dict(self.e, expected_tree_sha256=digest)
        other_cache = os.path.join(self.tmp, "cache2")
        with self.assertRaisesRegex(cl.CorpusError, "HASH MISMATCH"):
            cl.fetch_local(pinned, other_cache, overrides={"pb-x": self.src}, log=self.log)
        self.assertEqual(os.listdir(os.path.join(other_cache, "pb-x")), [])  # no stage left behind

    def test_no_pin_without_flag_refuses(self):
        with self.assertRaisesRegex(cl.CorpusError, "no expected_tree_sha256"):
            self.fetch()

    def test_repin_refused(self):
        _, digest = self.fetch(pin_manifest=self.mf)
        with self.assertRaises(cl.CorpusError):
            cl.pin_hash(self.mf, "pb-x", "0" * 64)

    def test_cache_hit_needs_no_source_and_detects_tampering(self):
        tree, digest = self.fetch(pin_manifest=self.mf)
        pinned = dict(self.e, expected_tree_sha256=digest)
        t2, _ = cl.fetch_local(pinned, self.cache, env={}, log=self.log)  # source gone, verified cache is used
        self.assertEqual(t2, tree)
        p = os.path.join(tree, "samples", "a.wav")
        with open(p, "r+b") as fh:
            fh.write(b"RIFF-Z")  # same size
        cl.fetch_local(pinned, self.cache, env={}, log=self.log)  # listing check does not see it
        with self.assertRaisesRegex(cl.CorpusError, "deep"):
            cl.fetch_local(pinned, self.cache, env={}, deep=True, log=self.log)
        os.unlink(p)
        with self.assertRaisesRegex(cl.CorpusError, "changed since copy"):
            cl.fetch_local(pinned, self.cache, env={}, log=self.log)

    def test_missing_source_is_a_hard_failure_with_guidance(self):
        pinned = dict(self.e, expected_tree_sha256="0" * 64)
        with self.assertRaisesRegex(cl.CorpusError, "local source not available.*VELORA_CORPUS_LOCAL_PB_X"):
            cl.fetch_local(pinned, self.cache, env={}, log=self.log)
        with self.assertRaisesRegex(cl.CorpusError, "local source not available"):
            cl.fetch_local(pinned, self.cache, overrides={"pb-x": os.path.join(self.tmp, "nope")}, env={}, log=self.log)

    def test_env_var_and_flag_resolution(self):
        self.assertEqual(cl.local_env_name("pianobook-manthey-klaviano"), "VELORA_CORPUS_LOCAL_PIANOBOOK_MANTHEY_KLAVIANO")
        e = {"id": "pb-x"}
        self.assertIsNone(cl.resolve_local(e, {}, {}))
        self.assertEqual(cl.resolve_local(e, {}, {"VELORA_CORPUS_LOCAL_PB_X": self.src}), self.src)
        self.assertEqual(cl.resolve_local(e, {"pb-x": "/flag"}, {"VELORA_CORPUS_LOCAL_PB_X": self.src}), "/flag")  # flag wins
        self.assertEqual(cl.parse_local_overrides(["a=/b=c"]), {"a": "/b=c"})
        with self.assertRaises(cl.CorpusError):
            cl.parse_local_overrides(["nopath"])

    def test_source_inside_repository_refused(self):
        with self.assertRaisesRegex(cl.CorpusError, "inside the repository"):
            cl.resolve_local({"id": "pb-x"}, {"pb-x": self.src}, {}, repo=self.tmp)
        with self.assertRaisesRegex(cl.CorpusError, "cache directory is inside"):
            cl.fetch_local(dict(self.e, expected_tree_sha256="0" * 64), self.cache, repo=self.tmp, log=self.log)

    def test_symlink_special_and_traversal_refused(self):
        os.symlink(os.path.join(self.src, "x.sfz"), os.path.join(self.src, "samples", "link.wav"))
        with self.assertRaisesRegex(cl.CorpusError, "symlink"):
            cl.copy_local(self.src, os.path.join(self.tmp, "c1"))
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "c1")) and os.listdir(os.path.join(self.tmp, "c1")))
        os.unlink(os.path.join(self.src, "samples", "link.wav"))
        os.symlink(self.tmp, os.path.join(self.src, "dirlink"))  # symlinked directory, even though unneeded
        with self.assertRaisesRegex(cl.CorpusError, "symlink"):
            cl.plan_local(self.src, cl.DEFAULT_LIMITS)
        os.unlink(os.path.join(self.src, "dirlink"))
        os.mkfifo(os.path.join(self.src, "pipe"))
        with self.assertRaisesRegex(cl.CorpusError, "special"):
            cl.plan_local(self.src, cl.DEFAULT_LIMITS)
        os.unlink(os.path.join(self.src, "pipe"))
        link_top = os.path.join(self.tmp, "toplink")
        os.symlink(self.src, link_top)
        with self.assertRaisesRegex(cl.CorpusError, "plain directory"):
            cl.plan_local(link_top, cl.DEFAULT_LIMITS)

    def test_limits_and_case_duplicates(self):
        with self.assertRaisesRegex(cl.CorpusError, "limit"):
            cl.plan_local(self.src, dict(cl.DEFAULT_LIMITS, max_file_bytes=3))
        with self.assertRaisesRegex(cl.CorpusError, "expands beyond"):
            cl.plan_local(self.src, dict(cl.DEFAULT_LIMITS, max_total_bytes=10))
        with self.assertRaisesRegex(cl.CorpusError, "too many"):
            cl.plan_local(self.src, dict(cl.DEFAULT_LIMITS, max_members=1))
        if os.path.exists(os.path.join(self.src, "samples", "A.wav")):
            self.skipTest("case-insensitive filesystem cannot hold both names")
        with open(os.path.join(self.src, "samples", "A.wav"), "wb") as fh:
            fh.write(b"x")
        if len(os.listdir(os.path.join(self.src, "samples"))) == 4:  # case-sensitive filesystem: both names exist
            with self.assertRaisesRegex(cl.CorpusError, "duplicate"):
                cl.plan_local(self.src, cl.DEFAULT_LIMITS)

    def test_source_is_never_modified(self):
        def snap():
            out = {}
            for dp, _, fs in os.walk(self.src):
                for f in fs:
                    p = os.path.join(dp, f)
                    out[p] = (os.path.getsize(p), os.path.getmtime(p), cl.sha256_file(p))
            return out
        before = snap()
        self.fetch(pin_manifest=self.mf)
        self.assertEqual(before, snap())

    def test_check_command_reports_local_entries(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = corpus.main(["--manifest", self.mf, "check"])
        self.assertEqual(rc, 0)
        self.assertIn("source NOT set on this machine", buf.getvalue())
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = corpus.main(["--manifest", self.mf, "check", "--require-pins"])
        self.assertEqual(rc, 1)

    def test_ids_no_local(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            corpus.main(["--manifest", self.mf, "ids", "--no-local"])
        self.assertEqual(buf.getvalue(), "")


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
            self.assertTrue(a.startswith(b"MThd\0\0\0\x06\0\0\0\x01\x5d\xc0MTrk"))
            self.assertTrue(a.endswith(b"\xff\x2f\x00"))
            self.assertEqual(int.from_bytes(a[18:22], "big"), len(a) - 22)
        self.assertEqual(set(midi_suite.SUITES), {"range", "velocity", "round_robin", "keyswitches", "releases", "sustained_loops"})

    def test_midi_suite_events_are_half_a_frame_after_a_frame_boundary(self):
        """Every event time (from the tempo map) is k + 0.5 frames at 48 kHz, never exactly on a frame (sfizz_render fires a tie 1 frame early or late)."""
        for n in midi_suite.SUITES:
            data = midi_suite.build(n)
            pos, tempo, tick, times = 22, None, 0, []
            end = len(data)
            exact = 0  # time in microseconds * 24000 ticks units: frames = units * 48000 / (24000 * 1e6)
            units = 0
            while pos < end:
                delta, shift = 0, 0
                while True:
                    b = data[pos]; pos += 1
                    delta = (delta << 7) | (b & 0x7F)
                    if not b & 0x80:
                        break
                if tempo is not None:
                    units += delta * tempo
                tick += delta
                st = data[pos]
                if st == 0xFF:
                    typ, ln = data[pos + 1], data[pos + 2]
                    if typ == 0x51:
                        tempo = int.from_bytes(data[pos + 3:pos + 6], "big")
                    pos += 3 + ln
                else:
                    pos += 3
                    times.append((tick, units))
            self.assertTrue(times)
            for tick, units in times:
                if tick == 0:
                    continue
                twice_frames = units * 2 * 48000 // (24000 * 1000000)
                self.assertEqual(units * 2 * 48000 % (24000 * 1000000), 0)
                self.assertEqual(twice_frames, 2 * tick + 1, "tick %d is not at tick + 0.5 frames" % tick)

    def test_random_layer_detection_and_null_pick(self):
        with tempfile.TemporaryDirectory() as d:
            for name, body in (("a.sfz", "<group> lorand=0.0 hirand=0.5\n<region> sample=x.wav\n"), ("b.sfz", "<region> sample=x.wav seq_length=2\n"),
                               ("c.sfz", "<region> sample=x.wav xlorand=1\n"),
                               ("d.sfz", "<group> lorand=0.0 hirand=0.5 pitch_random=20 // comment\n<region> sample=x.wav\n"),
                               ("e.sfz", "<group> lorand=0.5 hirand=1 off_by=3\n<region> sample=x.wav\n"),
                               ("f.sfz", "<group> lorand=0.5 hirand=1 sw_last=3 // amp_random=1\n<region> sample=x.wav\n")):
                with open(os.path.join(d, name), "w") as fh:
                    fh.write(body)
            self.assertTrue(corpus.has_random_layers(os.path.join(d, "a.sfz")))
            self.assertFalse(corpus.has_random_layers(os.path.join(d, "b.sfz")))
            self.assertFalse(corpus.has_random_layers(os.path.join(d, "c.sfz")))
            self.assertFalse(corpus.has_random_layers(os.path.join(d, "missing.sfz")))
            # Story 13.10: what the sfizz random replay cannot align (a comment does not count, sw_last does not block)
            self.assertEqual(corpus.replay_blockers(os.path.join(d, "a.sfz")), [])
            self.assertEqual(corpus.replay_blockers(os.path.join(d, "d.sfz")), ["pitch_random"])
            self.assertEqual(corpus.replay_blockers(os.path.join(d, "e.sfz")), ["off_by"])
            self.assertEqual(corpus.replay_blockers(os.path.join(d, "f.sfz")), [])
            self.assertEqual(corpus.replay_blockers(os.path.join(d, "missing.sfz")), ["unreadable"])
            runner = corpus.Runner.__new__(corpus.Runner)
            runner.m = {"max_null_instruments": 3}
            # a plain random-layer instrument is a normal null pick now (rendered with the replay); only a blocked one is left out
            ok = [("a.sfz", {"regions": 1}), ("b.sfz", {"regions": 2}), ("d.sfz", {"regions": 3})]
            res = {"failures": []}
            picked = runner.pick_null({}, ok, res, d)
            self.assertEqual([p[0] for p in picked], ["a.sfz", "b.sfz"])
            self.assertEqual(res["null_excluded_random_layers"], [{"path": "d.sfz", "blockers": ["pitch_random"]}])
            res = {"failures": []}  # nothing else to test: the blocked one is kept (the failure stays honest)
            self.assertEqual([p[0] for p in runner.pick_null({}, [ok[2]], res, d)], ["d.sfz"])
            self.assertNotIn("null_excluded_random_layers", res)
            res = {"failures": []}
            self.assertEqual([p[0] for p in runner.pick_null({}, [ok[0]], res, d)], ["a.sfz"])
            self.assertNotIn("null_excluded_random_layers", res)

    def test_sfz_text_comments_and_includes_are_shared_by_both_scans(self):
        with tempfile.TemporaryDirectory() as d:
            files = {
                "a.sfz": '/* lorand=0.5 */ <region> sample=dir//x.wav\n#include "inc/b.sfz"\n',
                "inc/b.sfz": "<group> lorand=0.0 hirand=0.5 pitch_random=3\n",
                "c.sfz": "<region> sample=a//b.wav // hirand=1 off_by=2\n/* off_by=3\n amp_random=1 */\n",
                "d.sfz": '#include "../a.sfz"\n#include "/etc/passwd"\n<region> sample=x.wav\n',
            }
            for name, body in files.items():
                full = os.path.join(d, *name.split("/"))
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, "w") as fh:
                    fh.write(body)
            a, c, dd = (os.path.join(d, n) for n in ("a.sfz", "c.sfz", "d.sfz"))
            self.assertIn("sample=dir//x.wav", corpus.sfz_text(a))  # // inside a path is not a comment
            self.assertTrue(corpus.has_random_layers(a))  # lorand comes from the include only (the commented one does not count)
            self.assertEqual(corpus.replay_blockers(a), ["pitch_random"])  # also from the include
            self.assertFalse(corpus.has_random_layers(c))  # hirand sits after a // comment start
            self.assertEqual(corpus.replay_blockers(c), [])  # off_by and amp_random are inside comments
            self.assertFalse(corpus.has_random_layers(dd))  # '..' and absolute includes are not followed

    def test_replay_flag_is_appended_only_for_random_layer_files(self):
        with tempfile.TemporaryDirectory() as d:
            for name, body in (("rand.sfz", "<group> lorand=0 hirand=0.5\n<region> sample=x.wav\n"), ("plain.sfz", "<region> sample=x.wav\n")):
                with open(os.path.join(d, name), "w") as fh:
                    fh.write(body)
            runner = corpus.Runner.__new__(corpus.Runner)
            runner.tools = {"render": "R", "nulltest": "N", "oracle_sh": "O", "sfizz": "S"}
            calls = []

            def fake_sh(argv, log, env=None):
                calls.append(list(argv))
                if argv[0] == "O":
                    with open(argv[3], "wb") as fh:
                        fh.write(b"\0" * (44 + 4 * 48000))
                elif argv[0] == "R":
                    with open(argv[argv.index("--out") + 1], "wb") as fh:
                        fh.write(b"\0")
                return 0, "residual: -80.00 dB (tolerance -60.00 dB)\nresult: PASS\n", 0.0

            runner.sh = fake_sh
            work = os.path.join(d, "work")
            os.makedirs(work)
            for rel, expect in (("rand.sfz", True), ("plain.sfz", False)):
                for suite in ("range", "releases"):
                    calls.clear()
                    res = {"failures": []}
                    r = runner.null_one({}, d, rel, "bundle", suite, "x.mid", work, -60, res)
                    render = [c for c in calls if c[0] == "R"][0]
                    self.assertEqual("--sfizz-random-replay" in render, expect)
                    self.assertEqual(bool(r.get("random_replay")), expect)
                    if expect:  # a probe with the sustain pedal records the unaligned deferred releases
                        self.assertEqual(any("pedal" in b for b in r["replay_blockers"]), suite == "releases")

    def test_null_exclude_needs_a_reason_and_keeps_the_instrument_out_of_the_picks(self):
        base = {"id": "x", "name": "X", "kind": "required", "source_url": "https://codeload.github.com/a/b/tar.gz/c", "version": "1", "commit": "c",
                "license": "CC0", "license_url": "https://example.org/l"}
        why = "CC64 moves while a note sounds and drives amplitude_oncc64: sfizz follows it, the engine reads it once"
        cl.validate_entry(dict(base, null_exclude=[{"path": "a/b.sfz", "reason": why}]))
        for bad in ([{"path": "a/b.sfz"}], [{"path": "a/b.sfz", "reason": "too short"}], [{"path": "../b.sfz", "reason": why}], {"path": "a"}):
            with self.assertRaises(cl.CorpusError):
                cl.validate_entry(dict(base, null_exclude=bad))
        with self.assertRaises(cl.CorpusError):
            cl.validate_entry(dict(base, null_files=["a/b.sfz"], null_exclude=[{"path": "a/b.sfz", "reason": why}]))
        runner = corpus.Runner.__new__(corpus.Runner)
        runner.m = {"max_null_instruments": 5}
        ok = [("a.sfz", {"regions": 1}), ("b.sfz", {"regions": 2}), ("c.sfz", {"regions": 3})]
        with tempfile.TemporaryDirectory() as d:
            res = {"failures": []}
            picked = runner.pick_null({"null_exclude": [{"path": "b.sfz", "reason": why}]}, ok, res, d)
            self.assertEqual([p[0] for p in picked], ["a.sfz", "c.sfz"])
            self.assertEqual(res["null_excluded_listed"], [{"path": "b.sfz", "reason": why}])
            res = {"failures": []}  # a stale entry fails the job
            runner.pick_null({"null_exclude": [{"path": "gone.sfz", "reason": why}]}, ok, res, d)
            self.assertTrue(any("gone.sfz" in f for f in res["failures"]))

    def test_tolerance_overrides_need_a_reason_and_a_real_path(self):
        base = {"id": "x", "name": "X", "kind": "required", "source_url": "https://codeload.github.com/a/b/tar.gz/c", "version": "1", "commit": "c",
                "license": "CC0", "license_url": "https://example.org/l"}
        good = {"path": "a/b.sfz", "tolerance_db": -45, "reason": "r" * 50}
        cl.validate_entry(dict(base, tolerance_overrides=[good]))
        for bad in (dict(good, reason="too short"), dict(good, tolerance_db=0), dict(good, tolerance_db=True), dict(good, tolerance_db="-45"),
                    dict(good, path="../b.sfz"), {"path": "a.sfz", "reason": "r" * 50}):
            with self.assertRaises(cl.CorpusError):
                cl.validate_entry(dict(base, tolerance_overrides=[bad]))
        with self.assertRaises(cl.CorpusError):
            cl.validate_entry(dict(base, tolerance_overrides=[good, good]))
        with self.assertRaises(cl.CorpusError):
            cl.validate_entry(dict(base, tolerance_overrides={"a": 1}))

    def test_markdown_report_lists_exclusions(self):
        rep = {"generated": "t", "manifest": "m", "libraries": [], "excluded": [{"id": "pb", "name": "PB", "reason": "login required"}], "failed": 0}
        self.assertIn("login required", corpus.render_markdown(rep))

    def test_null_output_parser(self):
        r = corpus.parse_null("file: x\n  result: PASS residual -64.2 dB offset 0\n  worst window -61.0 dB\n")
        self.assertEqual(r.get("residual_db"), -64.2)


if __name__ == "__main__":
    unittest.main()
