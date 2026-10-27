"""Story 5.4: manifest, fetch (cache + hash verify) and safe extraction for the reference corpus.

Pure standard library, Python 3.9+. Nothing in here executes anything that comes out of an archive.
Trust model: see README.md ("Hash pinning is trust-on-first-use").
"""
import gzip
import hashlib
import json
import os
import re
import shutil
import stat
import tarfile
import unicodedata
import zlib
import urllib.error
import urllib.parse
import urllib.request
import zipfile

SCHEMA = 1
ALLOWED_HOSTS = ("codeload.github.com", "github.com", "raw.githubusercontent.com", "objects.githubusercontent.com")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
AUDIO_EXTS = (".wav", ".flac", ".aif", ".aiff", ".ogg", ".opus", ".mp3", ".wv", ".w64")

# Limits applied while extracting an archive (all overridable per entry as `limits`).
DEFAULT_LIMITS = {
    "max_archive_bytes": 4 * 1024**3,
    "max_total_bytes": 8 * 1024**3,
    "max_file_bytes": 1 * 1024**3,
    "max_members": 200_000,
    "max_ratio": 1000,  # uncompressed bytes / archive bytes, a zip-bomb guard on top of the absolute limits
    "max_path_len": 400,
}


class CorpusError(Exception):
    """Any manifest, fetch, hash or extraction failure. Always fatal for the entry."""


# ---------------------------------------------------------------------------------------------
# manifest

def load_manifest(path):
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    validate_manifest(data)
    return data


def validate_manifest(data):
    """Raises CorpusError on the first structural problem. An entry is either included (full source,
    version, licence, pinned hash) or excluded (reason required); nothing in between."""
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        raise CorpusError("manifest: schema must be %d" % SCHEMA)
    entries = data.get("libraries")
    if not isinstance(entries, list) or not entries:
        raise CorpusError("manifest: libraries must be a non-empty list")
    seen = set()
    for e in entries:
        validate_entry(e)
        if e["id"] in seen:
            raise CorpusError("manifest: duplicate id %r" % e["id"])
        seen.add(e["id"])


def validate_entry(e):
    if not isinstance(e, dict):
        raise CorpusError("manifest: entry is not an object")
    eid = e.get("id")
    if not isinstance(eid, str) or not ID_RE.match(eid):
        raise CorpusError("manifest: bad id %r" % (eid,))
    where = "manifest[%s]" % eid
    if not isinstance(e.get("name"), str) or not e["name"]:
        raise CorpusError(where + ": name required")
    kind = e.get("kind")
    if kind not in ("required", "karoryfer", "pianobook", "extra"):
        raise CorpusError(where + ": kind must be required|karoryfer|pianobook|extra")
    if "excluded" in e:
        ex = e["excluded"]
        if not isinstance(ex, dict) or not isinstance(ex.get("reason"), str) or len(ex["reason"].strip()) < 10:
            raise CorpusError(where + ": excluded needs a reason of at least 10 characters")
        return
    local = is_local(e)
    if e.get("source_kind", "archive") not in ("archive", "local_dir"):
        raise CorpusError(where + ": source_kind must be archive or local_dir")
    for key in (("local_hint",) if local else ("source_url",)) + ("version", "license", "license_url"):
        if not isinstance(e.get(key), str) or not e[key].strip():
            raise CorpusError("%s: %s required" % (where, key))
    if local:
        hint = e["local_hint"]
        if hint.startswith("/") or "\\" in hint or re.search(r"(^|/)(Users|home)/", hint) or "\x00" in hint:
            raise CorpusError("%s: local_hint must be a machine-independent hint (no absolute path, no home directory); the path comes from VELORA_CORPUS_LOCAL_<ID> or --local" % where)
        if "source_url" in e or "sha256" in e or "archive" in e:
            raise CorpusError(where + ": a local_dir entry has no source_url, archive or sha256 (the pin is expected_tree_sha256)")
        if e.get("source_page"):
            check_url(e["source_page"], where + ".source_page", hosts=None)
    else:
        check_url(e["source_url"], where + ".source_url")
        if "expected_tree_sha256" in e or "local_hint" in e:
            raise CorpusError(where + ": expected_tree_sha256 / local_hint belong to source_kind local_dir")
    check_url(e["license_url"], where + ".license_url", hosts=None)
    fmt = e.get("archive", "tar.gz")
    if fmt not in ("tar.gz", "zip"):
        raise CorpusError(where + ": archive must be tar.gz or zip")
    for key in ("sha256", "expected_tree_sha256"):
        sha = e.get(key)
        if sha is not None and not (isinstance(sha, str) and SHA256_RE.match(sha)):
            raise CorpusError("%s: %s must be 64 lowercase hex characters" % (where, key))
    tol = e.get("tolerance_db")
    if tol is not None and not (isinstance(tol, (int, float)) and -400 <= tol < 0):
        raise CorpusError(where + ": tolerance_db must be a negative number of dB")
    for key in ("skip_files", "null_files"):
        v = e.get(key, [])
        if not isinstance(v, list):
            raise CorpusError("%s: %s must be a list" % (where, key))
        for item in v:
            if key == "skip_files":
                if not (isinstance(item, dict) and isinstance(item.get("path"), str) and isinstance(item.get("reason"), str) and len(item["reason"].strip()) >= 10):
                    raise CorpusError(where + ": every skip_files item needs path and a reason")
                check_rel(item["path"], where + ".skip_files")
            else:
                if not isinstance(item, str):
                    raise CorpusError(where + ": null_files items must be strings")
                check_rel(item, where + ".null_files")
    overrides = e.get("tolerance_overrides", [])
    if not isinstance(overrides, list):
        raise CorpusError(where + ": tolerance_overrides must be a list")
    seen_paths = set()
    for item in overrides:
        if not (isinstance(item, dict) and isinstance(item.get("path"), str) and isinstance(item.get("tolerance_db"), (int, float))
                and not isinstance(item.get("tolerance_db"), bool) and -400 <= item["tolerance_db"] < 0
                and isinstance(item.get("reason"), str) and len(item["reason"].strip()) >= 40):
            raise CorpusError(where + ": every tolerance_overrides item needs path, a negative tolerance_db and a reason of at least 40 characters")
        check_rel(item["path"], where + ".tolerance_overrides")
        if item["path"] in seen_paths:
            raise CorpusError(where + ": tolerance_overrides lists %r twice" % item["path"])
        seen_paths.add(item["path"])
    limits = e.get("limits", {})
    if not isinstance(limits, dict) or any(k not in DEFAULT_LIMITS or not isinstance(v, int) or v <= 0 for k, v in limits.items()):
        raise CorpusError(where + ": limits has an unknown key or a non-positive value")


def check_url(url, where, hosts=ALLOWED_HOSTS):
    u = urllib.parse.urlsplit(url)
    if u.scheme != "https" or not u.hostname or u.username or u.password:
        raise CorpusError("%s: only plain https URLs (no credentials) are allowed: %r" % (where, url))
    if hosts is not None and u.hostname not in hosts:
        raise CorpusError("%s: host %r is not on the allow-list %s" % (where, u.hostname, ", ".join(hosts)))


def check_rel(path, where):
    if not path or path.startswith("/") or "\\" in path or ".." in path.split("/") or "\x00" in path:
        raise CorpusError("%s: unsafe relative path %r" % (where, path))


def effective_limits(entry):
    lim = dict(DEFAULT_LIMITS)
    lim.update(entry.get("limits", {}))
    return lim


def included(manifest):
    return [e for e in manifest["libraries"] if "excluded" not in e]


def excluded(manifest):
    return [e for e in manifest["libraries"] if "excluded" in e]


def is_local(entry):
    return entry.get("source_kind", "archive") == "local_dir"


def pin_key(entry):
    return "expected_tree_sha256" if is_local(entry) else "sha256"


def pin_of(entry):
    return entry.get(pin_key(entry))


def pin_hash(manifest_path, entry_id, sha256):
    """Trust-on-first-use: writes `sha256` into the entry only when none is pinned. Never overwrites.
    The field is `sha256` (archive hash) or, for a local_dir entry, `expected_tree_sha256` (tree digest)."""
    with open(manifest_path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    for e in data["libraries"]:
        if e["id"] == entry_id:
            if pin_of(e):
                raise CorpusError("%s: a hash is already pinned; refusing to overwrite it" % entry_id)
            e[pin_key(e)] = sha256
            tmp = manifest_path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as fh:
                json.dump(data, fh, indent=2, ensure_ascii=False)
                fh.write("\n")
            os.replace(tmp, manifest_path)
            return
    raise CorpusError("no such entry %r" % entry_id)


# ---------------------------------------------------------------------------------------------
# hashing and download

def sha256_file(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            b = fh.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


class _SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        check_url(newurl, "redirect")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def default_opener():
    return urllib.request.build_opener(_SafeRedirect)


def download(url, dest, max_bytes, opener=None, chunk=1 << 20):
    """Streams `url` to `dest` (a new file, written through `dest + '.part'`), enforcing the size limit
    while it streams. Returns (sha256, size). No resume: a partial file is deleted."""
    check_url(url, "download")
    opener = opener or default_opener()
    part = dest + ".part"
    h = hashlib.sha256()
    size = 0
    req = urllib.request.Request(url, headers={"User-Agent": "velora-corpus/1"})
    try:
        with opener.open(req, timeout=60) as resp, open(part, "xb") as out:
            declared = resp.headers.get("Content-Length")
            if declared and declared.isdigit() and int(declared) > max_bytes:
                raise CorpusError("download: server announces %s bytes, limit is %d" % (declared, max_bytes))
            while True:
                b = resp.read(chunk)
                if not b:
                    break
                size += len(b)
                if size > max_bytes:
                    raise CorpusError("download: exceeded %d bytes" % max_bytes)
                h.update(b)
                out.write(b)
    except (urllib.error.URLError, OSError) as exc:
        _rm(part)
        raise CorpusError("download of %s failed: %s" % (url, exc))
    except CorpusError:
        _rm(part)
        raise
    os.replace(part, dest)
    return h.hexdigest(), size


def _rm(p):
    try:
        os.unlink(p)
    except OSError:
        pass


# ---------------------------------------------------------------------------------------------
# safe extraction

def _safe_member_path(name, limits):
    if not name or len(name) > limits["max_path_len"] or "\x00" in name or "\\" in name:
        raise CorpusError("unsafe member name %r" % name[:80])
    if name.startswith("/") or re.match(r"^[A-Za-z]:", name):
        raise CorpusError("absolute member path %r" % name)
    parts = [p for p in name.split("/") if p not in ("", ".")]
    if not parts or any(p == ".." for p in parts):
        raise CorpusError("path traversal in member %r" % name)
    return "/".join(parts)


def _plan_members(members, limits, archive_bytes):
    """members: iterable of (name, kind, size) with kind 'file' | 'dir' | other string. Returns the
    normalised plan [(safe_name, kind, size)] or raises. Rejects every link, device and duplicate."""
    plan = []
    seen = {}
    total = 0
    for name, kind, size in members:
        if len(plan) >= limits["max_members"]:
            raise CorpusError("too many members (limit %d)" % limits["max_members"])
        safe = _safe_member_path(name, limits)
        if kind not in ("file", "dir"):
            raise CorpusError("member %r is a %s; links and special files are refused" % (safe, kind))
        key = unicodedata.normalize("NFC", safe).casefold()  # case- and normalisation-insensitive filesystems (macOS) must not merge two members
        if key in seen:
            raise CorpusError("duplicate member %r (also %r)" % (safe, seen[key]))
        seen[key] = safe
        if kind == "file":
            if size > limits["max_file_bytes"]:
                raise CorpusError("member %r is %d bytes, limit %d" % (safe, size, limits["max_file_bytes"]))
            total += size
            if total > limits["max_total_bytes"]:
                raise CorpusError("archive expands beyond %d bytes" % limits["max_total_bytes"])
        plan.append((safe, kind, size))
    if archive_bytes and total > archive_bytes * limits["max_ratio"]:
        raise CorpusError("compression ratio above %d: refusing" % limits["max_ratio"])
    # a file may not also be a directory prefix of another member
    files = {n for n, k, _ in plan if k == "file"}
    for n, _, _ in plan:
        parent = n.rsplit("/", 1)[0] if "/" in n else ""
        while parent:
            if parent in files:
                raise CorpusError("member %r lies below file %r" % (n, parent))
            parent = parent.rsplit("/", 1)[0] if "/" in parent else ""
    return plan, total


class _Capped:
    """File object over a decompressed stream that refuses to go past `cap` bytes: the listing pass of a tar.gz
    decompresses the whole stream (tar has no index), so a gzip bomb is stopped here, before any limit on headers matters."""

    def __init__(self, f, cap):
        self.f, self.cap = f, cap

    def _chk(self):
        if self.f.tell() > self.cap:
            raise CorpusError("archive decompresses beyond %d bytes: refusing (zip-bomb guard)" % self.cap)

    def read(self, n=-1):
        d = self.f.read(n)
        self._chk()
        return d

    def seek(self, *a):
        r = self.f.seek(*a)
        self._chk()
        return r

    def tell(self):
        return self.f.tell()


def list_archive(archive_path, fmt, cap=None):
    """Yields (name, kind, size) without extracting. Opening an archive reads only its directory/headers."""
    if fmt == "zip":
        with zipfile.ZipFile(archive_path) as z:
            for i in z.infolist():
                mode = (i.external_attr >> 16) & 0xFFFF
                if i.is_dir():
                    kind = "dir"
                elif stat.S_ISLNK(mode):
                    kind = "symlink"
                elif (mode & 0o170000) and not stat.S_ISREG(mode):
                    kind = "special"
                else:
                    kind = "file"
                yield i.filename, kind, i.file_size
    else:
        raw = gzip.open(archive_path, "rb")
        with tarfile.open(fileobj=_Capped(raw, cap) if cap else raw, mode="r:") as t:
            for m in t:
                if m.isdir():
                    kind = "dir"
                elif m.isreg():
                    kind = "file"
                elif m.issym():
                    kind = "symlink"
                elif m.islnk():
                    kind = "hardlink"
                else:
                    kind = "special"
                yield m.name, kind, m.size


def strip_common_root(plan):
    """GitHub archives wrap everything in one top directory; drop it when every member shares it."""
    roots = {n.split("/", 1)[0] for n, k, _ in plan if n}
    if len(roots) == 1 and all(("/" in n or k == "dir") for n, k, _ in plan):
        root = roots.pop()
        out = []
        for n, k, s in plan:
            m = n[len(root) + 1:] if "/" in n else ""
            if m:
                out.append((m, k, s))
        return root, out
    return None, plan


def safe_extract(archive_path, fmt, dest_dir, limits=None):
    """Extracts into `dest_dir`, which must not exist yet. Two passes: the whole member list is validated
    first (nothing is written when anything is wrong), then every file is written with O_EXCL under a
    directory that is created by us, so no pre-existing path can be followed and no symlink can exist to
    be followed (links are refused). Permissions are normalised (0644 / 0755) and setuid bits never kept.
    Returns the number of bytes written and the file count. Never executes anything."""
    limits = dict(DEFAULT_LIMITS, **(limits or {}))
    if os.path.lexists(dest_dir):
        raise CorpusError("extraction target already exists: %s" % dest_dir)
    archive_bytes = os.path.getsize(archive_path)
    try:
        members = list(list_archive(archive_path, fmt, limits["max_total_bytes"] + (64 << 20)))
    except (tarfile.TarError, zipfile.BadZipFile, EOFError, OSError, gzip.BadGzipFile) as exc:
        raise CorpusError("unreadable archive: %s" % exc)
    plan, _total = _plan_members(members, limits, archive_bytes)
    # Map the validated, root-stripped names back to the archive's names.
    raw_to_safe = {}
    for (raw, kind, _), (safe, _, _) in zip(members, plan):
        raw_to_safe[raw] = safe
    root, stripped = strip_common_root(plan)
    strip_map = {}
    if root is not None:
        for (safe, _, _) in plan:
            strip_map[safe] = safe[len(root) + 1:] if "/" in safe else ""
    os.makedirs(dest_dir)
    written = 0
    nfiles = 0
    real_dest = os.path.realpath(dest_dir)
    try:
        opener = _open_zip if fmt == "zip" else _open_tar
        with opener(archive_path) as (iterate):
            for raw, kind, size, reader in iterate():
                safe = raw_to_safe.get(raw)
                if safe is None:
                    raise CorpusError("member %r changed between listing and extraction" % raw)
                rel = strip_map.get(safe, safe) if root is not None else safe
                if not rel:
                    continue
                target = os.path.join(dest_dir, *rel.split("/"))
                if os.path.commonpath([os.path.realpath(os.path.dirname(target)) if os.path.isdir(os.path.dirname(target)) else real_dest, real_dest]) != real_dest:
                    raise CorpusError("member %r escapes the target" % raw)
                if kind == "dir":
                    os.makedirs(target, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(target), exist_ok=True)
                fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o644)
                n = 0
                with os.fdopen(fd, "wb") as out:
                    while True:
                        b = reader.read(1 << 20)
                        if not b:
                            break
                        n += len(b)
                        if n > limits["max_file_bytes"] or written + n > limits["max_total_bytes"]:
                            raise CorpusError("member %r is larger than announced (zip-bomb guard)" % raw)
                        out.write(b)
                if n != size:
                    raise CorpusError("member %r: size %d differs from the header's %d" % (raw, n, size))
                written += n
                nfiles += 1
    except CorpusError:
        shutil.rmtree(dest_dir, ignore_errors=True)
        raise
    except (tarfile.TarError, zipfile.BadZipFile, zlib.error, EOFError, OSError, RuntimeError, NotImplementedError, gzip.BadGzipFile) as exc:
        shutil.rmtree(dest_dir, ignore_errors=True)  # a corrupt or encrypted member: refused like any other bad archive
        raise CorpusError("extraction failed: %s" % exc)
    except BaseException:
        shutil.rmtree(dest_dir, ignore_errors=True)
        raise
    return written, nfiles


class _open_tar:
    def __init__(self, p):
        self.t = tarfile.open(fileobj=gzip.open(p, "rb"), mode="r:")

    def __enter__(self):
        t = self.t

        def it():
            for m in t:
                if m.isdir():
                    yield m.name, "dir", 0, None
                elif m.isreg():
                    yield m.name, "file", m.size, t.extractfile(m)
        return it

    def __exit__(self, *a):
        self.t.close()


class _open_zip:
    def __init__(self, p):
        self.z = zipfile.ZipFile(p)

    def __enter__(self):
        z = self.z

        def it():
            for i in z.infolist():
                if i.is_dir():
                    yield i.filename, "dir", 0, None
                else:
                    with z.open(i) as r:
                        yield i.filename, "file", i.file_size, r
        return it

    def __exit__(self, *a):
        self.z.close()


# ---------------------------------------------------------------------------------------------
# cache

def tree_listing_digest(root):
    """Cheap integrity stamp of an extracted tree: sha256 over sorted (relative path, size)."""
    h = hashlib.sha256()
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort()
        for f in sorted(filenames):
            p = os.path.join(dirpath, f)
            if os.path.islink(p):
                raise CorpusError("symlink appeared in the cache: %s" % p)
            h.update(("%s\0%d\n" % (os.path.relpath(p, root), os.path.getsize(p))).encode("utf-8", "surrogateescape"))
    return h.hexdigest()


def tree_content_digest(root):
    h = hashlib.sha256()
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort()
        for f in sorted(filenames):
            p = os.path.join(dirpath, f)
            if os.path.islink(p):
                raise CorpusError("symlink appeared in the cache: %s" % p)
            h.update(os.path.relpath(p, root).encode("utf-8", "surrogateescape") + b"\0" + sha256_file(p).encode() + b"\n")
    return h.hexdigest()


def fetch(entry, cache_dir, pin_manifest=None, opener=None, deep=False, log=print):
    """Returns the directory holding the extracted library. Reuses a cached, verified copy.

    * the archive hash must equal the manifest's pin; without a pin the fetch refuses unless
      `pin_manifest` (path) is given, which pins the first downloaded hash (trust on first use);
    * the extracted tree sits at <cache>/<id>/<sha256>/tree and is only ever created by an atomic rename
      of a staging directory, so a crash leaves no half tree that looks complete;
    * on reuse the tree is re-stamped (path+size listing, or all bytes with deep=True) against the stamp
      written at extraction: a file added, removed, truncated or replaced behind our back refuses reuse.
    """
    eid = entry["id"]
    limits = effective_limits(entry)
    lib_dir = os.path.join(cache_dir, eid)
    pinned = entry.get("sha256")
    if pinned:
        final = os.path.join(lib_dir, pinned)
        if os.path.isfile(os.path.join(final, "stamp.json")):
            stamp = _read_stamp(final)
            tree = os.path.join(final, "tree")
            if stamp.get("archive_sha256") != pinned or not os.path.isdir(tree):
                raise CorpusError("%s: cache stamp does not match the pin; delete %s" % (eid, final))
            digest = tree_content_digest(tree) if deep else tree_listing_digest(tree)
            if digest != stamp["content_digest" if deep else "listing_digest"]:
                raise CorpusError("%s: cached tree changed since extraction (%s check); delete %s" % (eid, "deep" if deep else "listing", final))
            log("%s: cache hit (%s)" % (eid, "deep verified" if deep else "listing verified"))
            return tree, pinned
    elif pin_manifest is None:
        raise CorpusError("%s: no sha256 pinned in the manifest; run `corpus.py fetch --pin` once to pin the first download (trust on first use)" % eid)
    os.makedirs(lib_dir, exist_ok=True)
    fmt = entry.get("archive", "tar.gz")
    stage = os.path.join(lib_dir, ".stage-%d" % os.getpid())
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    try:
        arch = os.path.join(stage, "archive." + fmt.replace(".", "_"))
        log("%s: downloading %s" % (eid, entry["source_url"]))
        digest, size = download(entry["source_url"], arch, limits["max_archive_bytes"], opener=opener)
        if pinned and digest != pinned:
            raise CorpusError("%s: HASH MISMATCH: manifest pins %s, downloaded %s" % (eid, pinned, digest))
        if not pinned:
            log("%s: pinning first download %s (trust on first use)" % (eid, digest))
        tree = os.path.join(stage, "tree")
        written, nfiles = safe_extract(arch, fmt, tree, limits)
        stamp = {"archive_sha256": digest, "archive_bytes": size, "bytes": written, "files": nfiles,
                 "listing_digest": tree_listing_digest(tree), "content_digest": tree_content_digest(tree)}
        with open(os.path.join(stage, "stamp.json"), "w", encoding="utf-8") as fh:
            json.dump(stamp, fh, indent=1)
        os.unlink(arch)
        final = os.path.join(lib_dir, digest)
        if os.path.lexists(final):
            raise CorpusError("%s: %s exists but has no valid stamp; delete it" % (eid, final))
        try:
            os.rename(stage, final)
        except OSError as exc:  # another process published the same archive first: refuse rather than guess
            raise CorpusError("%s: could not publish %s (%s); a concurrent fetch? rerun" % (eid, final, exc))
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    if not pinned:
        pin_hash(pin_manifest, eid, digest)
    return os.path.join(final, "tree"), digest


def _read_stamp(final):
    with open(os.path.join(final, "stamp.json"), "r", encoding="utf-8") as fh:
        return json.load(fh)


# ---------------------------------------------------------------------------------------------
# local_dir sources (supplied by the founder from local disk; see README "Local sources")

LOCAL_EXTS = (".sfz",) + AUDIO_EXTS  # the only files copied into the cache: SFZ text and audio, nothing else (no images, dspreset, .DS_Store, readmes)


def local_env_name(entry_id):
    return "VELORA_CORPUS_LOCAL_" + re.sub(r"[^A-Z0-9]", "_", entry_id.upper())


def parse_local_overrides(items):
    out = {}
    for it in items or []:
        if "=" not in it:
            raise CorpusError("--local expects ID=PATH, got %r" % it)
        k, v = it.split("=", 1)
        out[k] = v
    return out


def in_repository(path, repo):
    p, r = os.path.realpath(path), os.path.realpath(repo)
    return os.path.commonpath([p, r]) == r


def resolve_local(entry, overrides=None, env=None, repo=None):
    """The directory for a local_dir entry: `--local ID=PATH`, else $VELORA_CORPUS_LOCAL_<ID>. Returns None when neither is set.
    A path inside the repository tree is refused (corpus audio must never sit in the checkout); so is a symlinked top directory."""
    env = os.environ if env is None else env
    path = (overrides or {}).get(entry["id"]) or env.get(local_env_name(entry["id"]))
    if not path:
        return None
    path = os.path.abspath(os.path.expanduser(path))
    if repo and in_repository(path, repo):
        raise CorpusError("%s: local source %s is inside the repository tree; corpus audio must stay outside it" % (entry["id"], path))
    return path


def local_tree_digest(root):
    """Deterministic pin of a (copied) local tree: sha256 over lines `relpath NUL size NUL sha256-of-file LF`, sorted by relative path
    (byte order of the UTF-8 name). Independent of mtimes, modes, inode order and of where the tree sits."""
    rows = []
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        for f in filenames:
            p = os.path.join(dirpath, f)
            if os.path.islink(p):
                raise CorpusError("symlink in tree: %s" % p)
            rows.append((os.path.relpath(p, root).replace(os.sep, "/"), os.path.getsize(p), sha256_file(p)))
    return _digest_rows(rows)


def _digest_rows(rows):
    h = hashlib.sha256()
    for rel, size, sha in sorted(rows, key=lambda r: r[0].encode("utf-8", "surrogateescape")):
        h.update(rel.encode("utf-8", "surrogateescape") + b"\0" + str(size).encode() + b"\0" + sha.encode() + b"\n")
    return h.hexdigest()


def plan_local(src, limits):
    """Walks `src` without following anything and returns [(relpath, size)] of the files to copy. Refuses a symlinked top directory,
    any symlink or special file anywhere below it (even an unneeded one), unsafe or duplicate names and any size limit breach.
    Only LOCAL_EXTS files that are not dot-files are selected."""
    try:
        st = os.lstat(src)
    except OSError as exc:
        raise CorpusError("local source not readable: %s" % exc)
    if not stat.S_ISDIR(st.st_mode):
        raise CorpusError("local source %s is not a plain directory (a symlink or a file is refused)" % src)
    members = []
    for dirpath, dirnames, filenames in os.walk(src, followlinks=False):
        for name in dirnames + filenames:
            p = os.path.join(dirpath, name)
            m = os.lstat(p).st_mode
            if stat.S_ISLNK(m):
                raise CorpusError("symlink in local source: %s (refused)" % p)
            if not (stat.S_ISDIR(m) or stat.S_ISREG(m)):
                raise CorpusError("special file in local source: %s (refused)" % p)
        for name in filenames:
            if name.startswith(".") or not name.lower().endswith(LOCAL_EXTS):
                continue
            p = os.path.join(dirpath, name)
            members.append((os.path.relpath(p, src).replace(os.sep, "/"), "file", os.lstat(p).st_size))
    if not members:
        raise CorpusError("local source %s holds no .sfz or audio file" % src)
    plan, _ = _plan_members(members, limits, 0)
    return [(n, s) for n, _, s in plan]


def copy_local(src, dest_dir, limits=None):
    """Copies the plan_local() files into `dest_dir` (must not exist), hardened like archive extraction: every source file is opened with
    O_NOFOLLOW and must still be a regular file of the planned size, every target is created with O_EXCL | O_NOFOLLOW, mode 0644, sizes
    re-checked while streaming. The source is only ever read. Returns (tree digest, bytes, file count)."""
    limits = dict(DEFAULT_LIMITS, **(limits or {}))
    if os.path.lexists(dest_dir):
        raise CorpusError("copy target already exists: %s" % dest_dir)
    plan = plan_local(src, limits)
    os.makedirs(dest_dir)
    rows, written = [], 0
    try:
        for rel, size in plan:
            s_path = os.path.join(src, *rel.split("/"))
            target = os.path.join(dest_dir, *rel.split("/"))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            try:
                sfd = os.open(s_path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
            except OSError as exc:
                raise CorpusError("cannot open %s: %s" % (s_path, exc))
            with os.fdopen(sfd, "rb") as inp:
                if not stat.S_ISREG(os.fstat(inp.fileno()).st_mode):
                    raise CorpusError("%s is not a regular file" % s_path)
                tfd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o644)
                h, n = hashlib.sha256(), 0
                with os.fdopen(tfd, "wb") as out:
                    while True:
                        b = inp.read(1 << 20)
                        if not b:
                            break
                        n += len(b)
                        if n > limits["max_file_bytes"] or written + n > limits["max_total_bytes"]:
                            raise CorpusError("%s grew beyond the limits while copying" % rel)
                        h.update(b)
                        out.write(b)
            if n != size:
                raise CorpusError("%s changed while copying (%d bytes, planned %d)" % (rel, n, size))
            written += n
            rows.append((rel, n, h.hexdigest()))
    except BaseException:
        shutil.rmtree(dest_dir, ignore_errors=True)
        raise
    return _digest_rows(rows), written, len(rows)


def fetch_local(entry, cache_dir, pin_manifest=None, deep=False, overrides=None, env=None, repo=None, log=print):
    """local_dir counterpart of fetch(): returns (tree dir, tree digest). Cache layout <cache>/<id>/<tree digest>/{tree,stamp.json}.
    * The pin is `expected_tree_sha256`; the copy must hash to it (HASH MISMATCH otherwise). Without a pin the fetch refuses
      unless `pin_manifest` is given (trust on first use of what the founder supplied).
    * A verified cached copy (listing check, or every byte with deep=True, which also recomputes the pin digest) is reused, so
      the source folder is not needed again; with no cached copy and no source the entry fails with 'local source not available'.
    * The source folder is never written to."""
    eid = entry["id"]
    if repo and in_repository(cache_dir, repo):
        raise CorpusError("%s: the cache directory is inside the repository tree" % eid)
    limits = effective_limits(entry)
    lib_dir = os.path.join(cache_dir, eid)
    pinned = pin_of(entry)
    if pinned:
        final = os.path.join(lib_dir, pinned)
        if os.path.isfile(os.path.join(final, "stamp.json")):
            stamp = _read_stamp(final)
            tree = os.path.join(final, "tree")
            if stamp.get("tree_sha256") != pinned or not os.path.isdir(tree):
                raise CorpusError("%s: cache stamp does not match the pin; delete %s" % (eid, final))
            if deep and local_tree_digest(tree) != pinned:
                raise CorpusError("%s: cached tree no longer hashes to the pin (deep check); delete %s" % (eid, final))
            digest = tree_content_digest(tree) if deep else tree_listing_digest(tree)
            if digest != stamp["content_digest" if deep else "listing_digest"]:
                raise CorpusError("%s: cached tree changed since copy (%s check); delete %s" % (eid, "deep" if deep else "listing", final))
            log("%s: cache hit (%s)" % (eid, "deep verified" if deep else "listing verified"))
            return tree, pinned
    elif pin_manifest is None:
        raise CorpusError("%s: no expected_tree_sha256 pinned in the manifest; run `corpus.py fetch --pin --local %s=PATH` once to pin the first copy (trust on first use)" % (eid, eid))
    src = resolve_local(entry, overrides, env, repo)
    if src is None:
        raise CorpusError("%s: local source not available on this machine (set %s or pass --local %s=PATH; hint: %s). Not skipped: list the entry under `excluded` with a reason to leave it out" % (eid, local_env_name(eid), eid, entry["local_hint"]))
    if not os.path.isdir(src):
        raise CorpusError("%s: local source not available: %s is not a directory" % (eid, src))
    os.makedirs(lib_dir, exist_ok=True)
    stage = os.path.join(lib_dir, ".stage-%d" % os.getpid())
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    try:
        tree = os.path.join(stage, "tree")
        log("%s: copying local source (hint: %s)" % (eid, entry["local_hint"]))
        digest, written, nfiles = copy_local(src, tree, limits)
        if pinned and digest != pinned:
            raise CorpusError("%s: HASH MISMATCH: manifest pins tree %s, local source hashes to %s" % (eid, pinned, digest))
        if not pinned:
            log("%s: pinning first copy %s (trust on first use)" % (eid, digest))
        stamp = {"tree_sha256": digest, "bytes": written, "files": nfiles, "listing_digest": tree_listing_digest(tree), "content_digest": tree_content_digest(tree)}
        with open(os.path.join(stage, "stamp.json"), "w", encoding="utf-8") as fh:
            json.dump(stamp, fh, indent=1)
        final = os.path.join(lib_dir, digest)
        if os.path.lexists(final):
            raise CorpusError("%s: %s exists but has no valid stamp; delete it" % (eid, final))
        try:
            os.rename(stage, final)
        except OSError as exc:
            raise CorpusError("%s: could not publish %s (%s); a concurrent fetch? rerun" % (eid, final, exc))
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    if not pinned:
        pin_hash(pin_manifest, eid, digest)
    return os.path.join(final, "tree"), digest


def fetch_entry(entry, cache_dir, pin_manifest=None, opener=None, deep=False, overrides=None, env=None, repo=None, log=print):
    if is_local(entry):
        return fetch_local(entry, cache_dir, pin_manifest, deep, overrides, env, repo, log)
    return fetch(entry, cache_dir, pin_manifest, opener, deep, log)


def find_audio_in_tree(root, skip_dirs=()):
    """Paths below `root` that look like corpus audio (used by the repository guard)."""
    hits = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in skip_dirs]
        for f in filenames:
            if f.lower().endswith(AUDIO_EXTS):
                hits.append(os.path.join(dirpath, f))
    return hits
