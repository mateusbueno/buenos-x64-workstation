#!/usr/bin/env python3
"""Story 5.4 reference corpus: manifest check, fetch, run, repository guard. See README.md.

  corpus.py check                       validate the manifest (offline)
  corpus.py fetch [--pin] [--deep] [--only ID ...] [--local ID=PATH ...]
  corpus.py run   [--only ID ...] [--out DIR] [--pin] [--keep-work]
  corpus.py guard [--root DIR]          fail if corpus audio is anywhere in the repository tree
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import struct
import time
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import corpus_lib as cl  # noqa: E402
import midi_suite  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
MANIFEST = os.path.join(HERE, "manifest.json")
DEFAULT_CACHE = os.path.expanduser("~/.cache/velora-corpus")
# Block size of the sfizz oracle and of the engine's oracle-block event model (the Story 1.15 / 5.2 suite uses 64). sfizz accumulates a voice's
# sample position per block in float32 (Voice::fillWithData: cumsum of the per-frame jumps, indices from the float sum), so the rounding of
# that sum makes its playback speed about 0.6 ppm off for a 44.1 kHz sample at 48 kHz (measured and simulated, see the 5.4 spec): 0.27 frames
# of drift after 10 s, a null of only -53 dB on a 10 s bell at block 64, -61 at 16, -72 at 1. At block size 1 the sum is exact, and the
# block-relative note delay that sfizz mishandles with ampeg_dynamic (ADSREnvelope dynamic mode overwrites the start delay) is always 0.
ORACLE_BLOCK_FRAMES = 1


def sfz_text(path, _seen=None, _depth=0):
    """Text of an SFZ for the opcode scans: comments removed identically for every scan (`/* ... */`, and `//` to the end of the line only where it starts a
    comment: at the start of a line or after whitespace, so `sample=a//b.wav` keeps its path), `#include "file"` followed (relative to the including file,
    no absolute or `..` paths, depth 8, each file once). Raises OSError for an unreadable top file."""
    seen = set() if _seen is None else _seen
    real = os.path.realpath(path)
    if real in seen or _depth > 8:
        return ""
    seen.add(real)
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"(?m)(^|(?<=\s))//[^\n]*", "", text)
    out = [text]
    for m in re.finditer(r'#include[ \t]+"([^"\n]+)"', text):
        inc = m.group(1).replace("\\", "/")
        if inc.startswith("/") or ".." in inc.split("/"):
            continue
        try:
            out.append(sfz_text(os.path.join(os.path.dirname(path), *inc.split("/")), seen, _depth + 1))
        except OSError:
            pass
    return "\n".join(out)


def has_random_layers(path):
    """True when the SFZ (with its includes, comments removed) selects layers with lorand/hirand; an unreadable file counts as not random."""
    try:
        return re.search(r"(?<![A-Za-z0-9_])(lorand|hirand)[ \t]*=", sfz_text(path)) is not None
    except OSError:
        return False


# Story 13.10: opcodes whose presence stops the sfizz random replay from being exact. The replay gives the engine the number sfizz uses for the layer
# choice of each note event (one draw per note on, note off and controller from Synth.cpp's generator). What else changes that count or draws from the
# same generator breaks the correspondence: `off_by` (a choke group makes sfizz run an extra note-off dispatch, which draws once more) and the
# `*_random` opcodes (`pitch_random`, `amp_random`, `offset_random`, ... draw per voice in sfizz from other generators, with other counts than the engine's).
# `sw_last` is NOT a blocker: sfizz then skips only its MidiState draws, which come from a different generator copy (every translation unit has its own `static`).
REPLAY_BLOCKERS = re.compile(r"(?<![A-Za-z0-9_])(off_by|[a-z_0-9]*random)[ \t]*=")


def replay_blockers(path):
    """Opcodes of the SFZ (same text as has_random_layers) that the sfizz random replay cannot align; empty list = no such opcode."""
    try:
        text = sfz_text(path)
    except OSError:
        return ["unreadable"]
    return sorted({m.group(1) for m in REPLAY_BLOCKERS.finditer(text)})


def wav_seconds(path, rate=48000):
    """Length of a 16-bit stereo PCM WAV written by sfizz_render (44 byte header)."""
    return max(0, os.path.getsize(path) - 44) / 4.0 / rate


def sf2_presets(path):
    """[(bank, program, name)] of a SoundFont, sorted, read from its `phdr` chunk (RIFF `sfbk` > LIST `pdta` > `phdr`, 38 bytes per header, the last one is the
    terminal record). Pure parsing with bounds checks; raises CorpusError for a file that is not a SoundFont. The importer is the judge of validity: this only names
    the presets the runner renders (their instrument key is `bank:program`)."""
    with open(path, "rb") as fh:
        head = fh.read(12)
        if len(head) < 12 or head[:4] != b"RIFF" or head[8:12] != b"sfbk":
            raise cl.CorpusError("%s is not a SoundFont (no RIFF sfbk header)" % path)
        end = min(8 + struct.unpack("<I", head[4:8])[0], os.fstat(fh.fileno()).st_size)
        pos = 12
        while pos + 12 <= end:
            fh.seek(pos)
            ck = fh.read(12)
            cid, size = ck[:4], struct.unpack("<I", ck[4:8])[0]
            if cid == b"LIST" and ck[8:12] == b"pdta":
                p, stop = pos + 12, min(pos + 8 + size, end)
                while p + 8 <= stop:
                    fh.seek(p)
                    sub = fh.read(8)
                    sid, ssize = sub[:4], struct.unpack("<I", sub[4:8])[0]
                    if sid == b"phdr":
                        if ssize % 38 or p + 8 + ssize > stop:
                            raise cl.CorpusError("%s: damaged phdr chunk" % path)
                        data = fh.read(ssize)
                        out = []
                        for i in range(0, ssize - 38, 38):  # the last record is the terminal one
                            name = data[i:i + 20].split(b"\0")[0].decode("latin-1")
                            program, bank = struct.unpack("<HH", data[i + 20:i + 24])
                            out.append((bank, program, name))
                        return sorted(out)
                    p += 8 + ssize + (ssize & 1)
                raise cl.CorpusError("%s: no phdr chunk" % path)
            pos += 8 + size + (size & 1)
    raise cl.CorpusError("%s: no pdta list" % path)


def write_silence(path, seconds):
    """Replaces `path` with 16-bit stereo 48 kHz digital silence of `seconds`."""
    with wave.open(path, "wb") as o:
        o.setnchannels(2)
        o.setsampwidth(2)
        o.setframerate(48000)
        o.writeframes(b"\0\0\0\0" * int(round(seconds * 48000)))


def pad_silence(path, seconds):
    """Appends digital silence to the oracle render (16-bit stereo 48 kHz) until it is `seconds` long."""
    with wave.open(path, "rb") as w:
        n, data = w.getnframes(), w.readframes(w.getnframes())
    want = int(round(seconds * 48000))
    with wave.open(path + ".trim", "wb") as o:
        o.setnchannels(2)
        o.setsampwidth(2)
        o.setframerate(48000)
        o.writeframes(data + b"\0\0\0\0" * max(0, want - n))
    os.replace(path + ".trim", path)


def trim_oracle_wav(path, keep_frames=4800):
    """FluidSynth's player appends 2 s of silence after the last voice: cut the render after its last non-zero frame plus `keep_frames` (100 ms), as
    tools/offline-render/tests/sf2_suite/regenerate_goldens.sh does for the Story 5.3 goldens. Returns the length in seconds, or None for a render
    that is entirely silent (left untouched). 16-bit PCM only (the oracle's own format)."""
    with wave.open(path, "rb") as w:
        ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        data = w.readframes(n)
    if sw != 2 or ch != 2 or rate != 48000:
        raise cl.CorpusError("oracle render %s is not 16-bit stereo 48 kHz (%s ch, %s bytes, %s Hz)" % (path, ch, sw, rate))
    frame = ch * sw
    last = n
    zero = b"\0" * frame
    while last > 0 and data[(last - 1) * frame:last * frame] == zero:
        last -= 1
    if last == 0:
        return None
    keep = min(n, last + keep_frames)
    with wave.open(path + ".trim", "wb") as o:
        o.setnchannels(ch)
        o.setsampwidth(sw)
        o.setframerate(rate)
        o.writeframes(data[:keep * frame])
    os.replace(path + ".trim", path)
    return keep / float(rate)


def tool_paths(build_dir):
    b = os.path.abspath(build_dir)
    return {
        "librarian": os.path.join(b, "librarian", "librarian"),
        "render": os.path.join(b, "tools", "offline-render", "velora-offline-render"),
        "nulltest": os.path.join(b, "tools", "offline-render", "velora-null-test"),
        "oracle_sh": os.path.join(REPO, "tools", "oracle", "render-oracle.sh"),
        "sfizz": os.environ.get("VELORA_SFIZZ_RENDER") or os.path.join(REPO, "tools", "oracle", ".work", "build", "library", "bin", "sfizz_render"),
        # Story 13.11: the FluidSynth oracle of Story 5.3 (2.6.1, tools/oracle/build-fluidsynth.sh); no fallback to another fluidsynth on the PATH
        "fluid_sh": os.path.join(REPO, "tools", "oracle", "render-fluidsynth.sh"),
        "fluidsynth": os.environ.get("VELORA_FLUIDSYNTH") or os.path.join(REPO, "tools", "oracle", ".work", "fluidsynth-build", "src", "fluidsynth"),
    }


def cmd_check(args):
    m = cl.load_manifest(args.manifest)
    inc, exc = cl.included(m), cl.excluded(m)
    unpinned = [e["id"] for e in inc if not cl.pin_of(e)]
    print("manifest ok: %d included, %d excluded, %d without a pinned hash" % (len(inc), len(exc), len(unpinned)))
    overrides = cl.parse_local_overrides(getattr(args, "local", []))
    for e in inc:
        if cl.is_local(e):
            src = cl.resolve_local(e, overrides, repo=REPO)  # refuses a path inside the repository tree
            print("  local_dir %s: %s (%s)" % (e["id"], src if src else "source NOT set on this machine; set %s or --local" % cl.local_env_name(e["id"]), e["local_hint"]))
    for e in exc:
        print("  excluded %s: %s" % (e["id"], e["excluded"]["reason"][:100]))
    if unpinned and args.require_pins:
        print("FAIL: unpinned: " + ", ".join(unpinned))
        return 1
    return 0


def selected(m, only):
    inc = cl.included(m)
    if only:
        ids = {e["id"] for e in inc}
        bad = [o for o in only if o not in ids]
        if bad:
            raise cl.CorpusError("unknown or excluded id(s): " + ", ".join(bad))
        inc = [e for e in inc if e["id"] in only]
    return inc


def cmd_ids(args):
    skipped = []
    for e in cl.included(cl.load_manifest(args.manifest)):
        if args.no_local and e.get("format", "sfz") == "sf2":
            skipped.append(e["id"])  # the nightly workflow builds only the sfizz oracle (no FluidSynth step), so an SF2 entry cannot run there
        elif not (args.no_local and cl.is_local(e)):
            print(e["id"])
    if skipped:
        print("::notice::SF2 entries not listed (the workflow has no FluidSynth oracle step): " + ", ".join(skipped), file=sys.stderr)
    return 0


def cmd_fetch(args):
    m = cl.load_manifest(args.manifest)
    rc = 0
    for e in selected(m, args.only):
        try:
            tree, sha = cl.fetch_entry(e, args.cache, pin_manifest=args.manifest if args.pin else None, deep=args.deep,
                                       overrides=cl.parse_local_overrides(args.local), repo=REPO)
            print("%s: ok %s %s" % (e["id"], sha, tree))
            if args.pin and not cl.pin_of(e):
                e[cl.pin_key(e)] = sha
        except cl.CorpusError as exc:
            print("%s: FAIL %s" % (e["id"], exc))
            rc = 1
    return rc


# ---------------------------------------------------------------------------------------------
# run

class Runner:
    def __init__(self, args, manifest):
        self.args = args
        self.m = manifest
        self.tools = tool_paths(args.build)
        self.timeout = args.timeout

    def sh(self, argv, log, env=None, cwd=None):
        e = dict(os.environ)
        if env:
            e.update(env)
        t0 = time.time()
        with open(log, "wb") as fh:
            try:
                p = subprocess.run(argv, stdout=fh, stderr=subprocess.STDOUT, env=e, cwd=cwd, timeout=self.timeout)
                rc = p.returncode
            except subprocess.TimeoutExpired:
                rc = -999
        with open(log, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        return rc, text, time.time() - t0

    def preflight(self, entries=None):
        """Every tool the selected entries need must exist (a missing tool is a failure, never a skip). The sfizz oracle is needed by SFZ entries, FluidSynth by
        SF2 entries (Story 13.11); with no selection given both are required."""
        formats = {e.get("format", "sfz") for e in entries} if entries is not None else {"sfz", "sf2"}
        need = ["librarian", "render", "nulltest"] + (["oracle_sh", "sfizz"] if "sfz" in formats else []) + (["fluid_sh", "fluidsynth"] if "sf2" in formats else [])
        missing = [k for k in need if not os.path.isfile(self.tools[k])]
        if missing:
            raise cl.CorpusError("missing tool(s): %s. Build with cmake (target librarian, velora-offline-render, velora-null-test), tools/oracle/build-sfizz-render.sh (SFZ) "
                                 "and tools/oracle/build-fluidsynth.sh (SF2, or VELORA_FLUIDSYNTH)" % ", ".join("%s=%s" % (k, self.tools[k]) for k in missing))

    def library(self, entry, tree, work):
        if entry.get("format", "sfz") == "sf2":
            return self.library_sf2(entry, tree, work)
        res = {"id": entry["id"], "name": entry["name"], "kind": entry["kind"], "version": entry["version"], "license": entry["license"],
               "license_url": entry["license_url"], "source_url": entry.get("source_url") or ("local_dir: " + entry["local_hint"]), "sha256": cl.pin_of(entry),
               "status": "pass", "failures": [], "imported": 0, "import_failed": 0, "skipped_listed": [], "instruments": []}
        tree = os.path.abspath(tree)
        tol = entry.get("tolerance_db", self.m.get("default_tolerance_db", -60))
        res["tolerance_db"] = tol
        skip = {s["path"]: s["reason"] for s in entry.get("skip_files", [])}
        sfz = []
        for dirpath, dirnames, files in os.walk(tree):
            dirnames.sort()
            for f in sorted(files):
                if f.lower().endswith(".sfz"):
                    sfz.append(os.path.relpath(os.path.join(dirpath, f), tree).replace(os.sep, "/"))
        sfz.sort()
        res["sfz_found"] = res["files_found"] = len(sfz)
        res["format"] = "sfz"
        if not sfz:
            res["failures"].append("no .sfz file found in the library")
            res["status"] = "fail"
            return res
        todo = []
        for rel in sfz:
            if rel in skip:
                res["skipped_listed"].append({"path": rel, "reason": skip[rel]})
            else:
                todo.append(rel)
        for p in skip:
            if p not in sfz:
                res["failures"].append("skip_files lists %r, which is not in the library (stale manifest)" % p)
        for ov in entry.get("tolerance_overrides", []):
            if ov["path"] not in sfz:
                res["failures"].append("tolerance_overrides lists %r, which is not in the library (stale manifest)" % ov["path"])
        root = os.path.join(work, "root")
        os.makedirs(root)
        report = os.path.join(work, "import.jsonl")
        t0 = time.time()
        argv = [self.tools["librarian"], "import"] + [os.path.join(tree, *r.split("/")) for r in todo] + ["--root", root, "--base", tree, "--report", report]
        rc, text, _ = self.sh(argv, os.path.join(work, "import.log"))
        res["import_seconds"] = round(time.time() - t0, 1)
        files = {}
        if os.path.isfile(report):
            with open(report, "r", encoding="utf-8") as fh:
                for line in fh:
                    o = json.loads(line)
                    if o.get("type") == "file":
                        files[os.path.relpath(os.path.abspath(o["file"]), tree).replace(os.sep, "/")] = o
        if rc == -999:
            res["failures"].append("import timed out")
        for rel in todo:
            o = files.get(rel)
            if o is None:
                res["import_failed"] += 1
                res["failures"].append("import: no report line for %s" % rel)
            elif o["status"] in ("imported", "unchanged"):
                res["imported"] += 1
            else:
                res["import_failed"] += 1
                res["failures"].append("import %s: %s (%s) %s" % (rel, o["status"], o.get("reason"), o.get("failing_path") or o.get("detail") or ""))
        hist = {}
        for m in re.finditer(r"unsupported (opcode|header) '?([^' \n]+)'?|invalid value '([^']*)' for opcode '([^']+)'", text):
            k = ("%s %s" % (m.group(1), m.group(2))) if m.group(1) else ("invalid value for %s" % m.group(4))
            hist[k] = hist.get(k, 0) + 1
        res["unsupported_histogram"] = dict(sorted(hist.items(), key=lambda kv: (-kv[1], kv[0]))[:25])
        res["import_warning_files"] = sum(1 for o in files.values() if o.get("warning_count"))
        res["regions_total"] = sum(o.get("regions", 0) for o in files.values())
        ok = [(rel, files[rel]) for rel in todo if rel in files and files[rel]["status"] in ("imported", "unchanged")]
        if not ok:
            res["status"] = "fail"
            return res
        pick = self.pick_null(entry, ok, res, tree)
        suites = midi_suite.write_all(os.path.join(work, "midi"))
        for rel, o in pick:
            bundle = os.path.join(root, "lib", o["library_id"], o["bundle_id"], "bundle.vlb")
            inst = {"file": rel, "regions": o.get("regions"), "samples": o.get("samples"), "suites": []}
            res["instruments"].append(inst)
            itol = tol
            for ov in entry.get("tolerance_overrides", []):
                if ov["path"] == rel:
                    itol = ov["tolerance_db"]
                    inst["tolerance_db"] = itol
                    inst["tolerance_reason"] = ov["reason"]
            for name in midi_suite.SUITES:
                inst["suites"].append(self.null_one(entry, tree, rel, bundle, name, suites[name], work, itol, res))
        if res["failures"]:
            res["status"] = "fail"
        return res

    # -----------------------------------------------------------------------------------------
    # Story 13.11: SoundFont libraries (format sf2)

    def library_sf2(self, entry, tree, work):
        """Imports every `.sf2` of the library with `librarian import` (the SF2 reader of Story 5.3) and null-tests up to `max_null_instruments` presets against the
        FluidSynth oracle with the six probes of midi_suite (FluidSynth grid). Same failure rules as the SFZ path: an `.sf2` that does not import fails the
        entry unless it is listed in `skip_files`, a stale manifest entry fails, a missing tool fails."""
        res = {"id": entry["id"], "name": entry["name"], "kind": entry["kind"], "format": "sf2", "version": entry["version"], "license": entry["license"],
               "license_url": entry["license_url"], "source_url": entry.get("source_url") or ("local_dir: " + entry["local_hint"]), "sha256": cl.pin_of(entry),
               "status": "pass", "failures": [], "imported": 0, "import_failed": 0, "skipped_listed": [], "instruments": [], "notes": []}
        tree = os.path.abspath(tree)
        tol = entry["tolerance_db"]
        res["tolerance_db"] = tol
        skip = {x["path"]: x["reason"] for x in entry.get("skip_files", [])}
        found = []
        for dirpath, dirnames, files in os.walk(tree):
            dirnames.sort()
            for f in sorted(files):
                if f.lower().endswith(".sf2"):
                    found.append(os.path.relpath(os.path.join(dirpath, f), tree).replace(os.sep, "/"))
        found.sort()
        res["files_found"] = len(found)
        if not found:
            res["failures"].append("no .sf2 file found in the library")
            res["status"] = "fail"
            return res
        todo = []
        for rel in found:
            if rel in skip:
                res["skipped_listed"].append({"path": rel, "reason": skip[rel]})
            else:
                todo.append(rel)
        for key in ("skip_files", "tolerance_overrides", "null_exclude"):
            for item in entry.get(key, []):
                if item["path"] not in found:
                    res["failures"].append("%s lists %r, which is not in the library (stale manifest)" % (key, item["path"]))
        if not todo:
            res["failures"].append("every .sf2 of the library is listed in skip_files: nothing is imported or null-tested")
            res["status"] = "fail"
            return res
        root = os.path.join(work, "root")
        os.makedirs(root)
        report = os.path.join(work, "import.jsonl")
        t0 = time.time()
        rc, text, _ = self.sh([self.tools["librarian"], "import"] + [os.path.join(tree, *r.split("/")) for r in todo] + ["--root", root, "--base", tree, "--report", report],
                              os.path.join(work, "import.log"))
        res["import_seconds"] = round(time.time() - t0, 1)
        info = {}
        if os.path.isfile(report):
            with open(report, "r", encoding="utf-8") as fh:
                for line in fh:
                    o = json.loads(line)
                    if o.get("type") == "file":
                        info[os.path.relpath(os.path.abspath(o["file"]), tree).replace(os.sep, "/")] = o
        if rc == -999:
            res["failures"].append("import timed out")
        elif not info:
            res["failures"].append("import: the librarian wrote no report line (exit code %s)" % rc)
        for rel in todo:
            o = info.get(rel)
            if o is None:
                res["import_failed"] += 1
                res["failures"].append("import: no report line for %s" % rel)
            elif o["status"] in ("imported", "unchanged"):
                res["imported"] += 1
            else:
                res["import_failed"] += 1
                res["failures"].append("import %s: %s (%s) %s" % (rel, o["status"], o.get("reason"), "; ".join(str(m).replace(tree + os.sep, "")[:200] for m in o.get("messages", [])[:2]) or o.get("failing_path") or o.get("detail") or ""))
        res["import_warning_files"] = sum(1 for o in info.values() if o.get("warning_count"))
        res["regions_total"] = sum(o.get("regions", 0) for o in info.values())
        res["presets_total"] = sum(o.get("presets", 0) for o in info.values())
        res["ignored_generators"] = sum(o.get("ignored_generators", 0) for o in info.values())
        ok = [(rel, info[rel]) for rel in todo if rel in info and info[rel]["status"] in ("imported", "unchanged")]
        if not ok:
            res["status"] = "fail"
            return res
        picks = self.pick_presets(entry, ok, res, tree)
        for rel, o, bank, program, pname in picks:
            bundle = os.path.join(root, "lib", o["library_id"], o["bundle_id"], "bundle.vlb")
            inst = {"file": rel, "preset": "%d:%d" % (bank, program), "preset_name": pname, "regions": o.get("regions"), "samples": o.get("samples"), "suites": []}
            res["instruments"].append(inst)
            itol = tol
            for ov in entry.get("tolerance_overrides", []):
                if ov["path"] == rel:
                    itol = ov["tolerance_db"]
                    inst["tolerance_db"] = itol
                    inst["tolerance_reason"] = ov["reason"]
            midis = midi_suite.write_fluid(os.path.join(work, "midi"), bank, program)
            for name in midi_suite.SUITES:
                inst["suites"].append(self.null_one_sf2(tree, rel, bundle, bank, program, name, midis[name], work, itol, res))
            if all(x.get("oracle_silent") for x in inst["suites"]):
                res["failures"].append("%s %s: the oracle rendered silence for every probe (the stimulus does not reach the preset's zones); nothing was compared" % (rel, inst["preset"]))
        if not res["instruments"]:
            res["failures"].append("no preset was null-tested (no preset to pick, or every candidate was filtered out)")
        if res["failures"]:
            res["status"] = "fail"
        return res

    def pick_presets(self, entry, ok, res, tree):
        """[(file, import report line, bank, program, name)] to null-test. `null_presets` ('file.sf2#bank:program') names them exactly; otherwise up to
        `max_null_instruments` presets spread evenly over the sorted (file, bank, program) list of `null_files` (default: every imported file)."""
        by_rel = dict(ok)
        allp = {}
        for rel, _o in ok:
            try:
                allp[rel] = sf2_presets(os.path.join(tree, *rel.split("/")))
            except (cl.CorpusError, OSError) as exc:
                res["failures"].append("%s: cannot list the presets: %s" % (rel, exc))
        for rel in list(allp):
            keys = [(b, p) for b, p, _n in allp[rel]]
            for b, p in sorted({k for k in keys if keys.count(k) > 1}):
                res["failures"].append("%s: preset key %d:%d appears more than once (an instrument key must be unique)" % (rel, b, p))
            for b, p, n in allp[rel]:
                if b > 16383 or p > 127:
                    res["failures"].append("%s: preset %r has bank %d / program %d outside 0..16383 / 0..127" % (rel, n, b, p))
            allp[rel] = [x for x in allp[rel] if x[0] <= 16383 and x[1] <= 127]
        for rel, o in ok:
            if rel in allp and o.get("presets") is not None and o["presets"] != len(allp[rel]):
                res["notes"].append("%s: the importer reports %s presets, the file header lists %d (a preset the importer drops fails its render)" % (rel, o["presets"], len(allp[rel])))
        wanted = entry.get("null_presets")
        if wanted:
            out = []
            for item in sorted(set(wanted)):
                m = cl.PRESET_RE.match(item)
                rel, bank, program = m.group(1), int(m.group(2)), int(m.group(3))
                names = {(b, p): n for b, p, n in allp.get(rel, [])}
                if rel not in by_rel or (bank, program) not in names:
                    res["failures"].append("null_presets lists %r, which is not an imported preset" % item)
                else:
                    out.append((rel, by_rel[rel], bank, program, names[(bank, program)]))
            return out
        files = entry.get("null_files") or [rel for rel, _o in ok]
        for rel in files:
            if rel not in by_rel:
                res["failures"].append("null_files lists %r, which did not import" % rel)
        listed = {x["path"]: x["reason"] for x in entry.get("null_exclude", [])}
        if listed:
            res["null_excluded_listed"] = [{"path": rel, "reason": why} for rel, why in listed.items() if rel in by_rel]
        cand = [(rel, by_rel[rel], b, p, n) for rel in sorted(files) if rel in by_rel and rel in allp and rel not in listed for b, p, n in allp[rel]]
        if not cand:  # nothing else to test: the listed ones stay in
            cand = [(rel, by_rel[rel], b, p, n) for rel in sorted(files) if rel in by_rel and rel in allp for b, p, n in allp[rel]]
            res.pop("null_excluded_listed", None)
        n = int(self.m.get("max_null_instruments", 3))
        if len(cand) <= n:
            return cand
        idx = sorted({round(i * (len(cand) - 1) / max(1, n - 1)) for i in range(n)})
        return [cand[i] for i in idx]

    def null_one_sf2(self, tree, rel, bundle, bank, program, suite, midis, work, tol, res):
        key = "%d:%d" % (bank, program)  # the instrument key of an SF2 preset
        tag = "%s__%s__%s" % (re.sub(r"[^A-Za-z0-9_.-]", "_", rel), key.replace(":", "_"), suite)
        mid_oracle, mid_engine = midis
        ref, cand = os.path.join(work, tag + ".oracle.wav"), os.path.join(work, tag + ".engine.wav")
        r = {"suite": suite, "file": rel, "preset": key, "status": "fail"}

        def fail(msg):
            r["error"] = msg
            res["failures"].append("%s %s %s: %s" % (rel, key, suite, msg))
            return r

        try:
            rc, text, sec = self.sh([self.tools["fluid_sh"], os.path.join(tree, *rel.split("/")), mid_oracle, ref], os.path.join(work, tag + ".oracle.log"),
                                    env={"VELORA_FLUIDSYNTH": self.tools["fluidsynth"]})
            r["oracle_seconds"] = round(sec, 1)
            if rc != 0 or not os.path.isfile(ref):
                return fail("oracle render failed (rc %s): %s" % (rc, text[-300:].strip()))
            last_engine = midi_suite.last_event_seconds_fluid(suite, bank, program, engine=True)
            try:
                ref_seconds = trim_oracle_wav(ref)
                if ref_seconds is None:
                    # Digital silence: nothing to compare, but the engine must be silent too. The player's 2 s of grace would make the reference longer than the
                    # engine render, so the reference is rewritten as silence of the engine's length. A preset needs at least one probe that is not silent (checked by the caller).
                    r["oracle_silent"] = True
                    ref_seconds = last_engine + 0.1
                    write_silence(ref, ref_seconds)
                elif ref_seconds < last_engine + 0.01:
                    # the sound died before the last event (a note off after the decay): pad with silence so both files end at the engine's last event + 10 ms
                    pad_silence(ref, last_engine + 0.01)
                    ref_seconds = last_engine + 0.01
            except (cl.CorpusError, wave.Error, OSError, EOFError) as exc:
                return fail("oracle output unusable: %s" % exc)
            t = max(0.01, min(600.0, ref_seconds - last_engine))
            r["tail_seconds"] = round(t, 2)
            rc, text, sec = self.sh([self.tools["render"], "--midi", mid_engine, "--out", cand, "--engine", "sample", "--bundle", bundle, "--instrument", key,
                                     "--gain-db", "0", "--tail-seconds", str(t)], os.path.join(work, tag + ".render.log"))
            r["render_seconds"] = round(sec, 1)
            r["render_report"] = text.strip().splitlines()[-1][:300] if text.strip() else ""
            if rc != 0 or not os.path.isfile(cand):
                return fail("engine render failed (rc %s): %s" % (rc, text[-300:].strip()))
            rc, text, _ = self.sh([self.tools["nulltest"], "--reference", ref, "--candidate", cand, "--tolerance-db", str(tol), "--max-offset-frames", "1", "--check-excess"],
                                  os.path.join(work, tag + ".null.log"))
            r.update(parse_null(text))
            r["status"] = "pass" if rc == 0 else "fail"
            if rc != 0:
                r["error"] = "null test failed: " + " | ".join(l.strip() for l in text.strip().splitlines()[-4:])
                res["failures"].append("%s %s %s: residual %s dB vs tolerance %s dB%s" % (rel, key, suite, r.get("residual_db"), tol, " (oracle silent)" if r.get("oracle_silent") else ""))
            return r
        finally:  # the renders are large: they go away on every exit path (the logs stay)
            for p in (ref, cand, ref + ".trim"):
                try:
                    os.unlink(p)
                except OSError:
                    pass

    def pick_null(self, entry, ok, res, tree=None):
        wanted = entry.get("null_files")
        by_rel = dict(ok)
        if wanted:
            out = []
            for rel in wanted:
                if rel in by_rel:
                    out.append((rel, by_rel[rel]))
                else:
                    res["failures"].append("null_files lists %r, which did not import" % rel)
            return out
        n = int(self.m.get("max_null_instruments", 3))
        # Story 13.5: instruments the manifest keeps out of the automatic picks, with the reason (reported, never silent).
        listed = {x["path"]: x["reason"] for x in entry.get("null_exclude", [])}
        if listed:
            for rel in listed:
                if rel not in by_rel:
                    res["failures"].append("null_exclude lists %r, which did not import" % rel)
            kept = [kv for kv in ok if kv[0] not in listed]
            res["null_excluded_listed"] = [{"path": rel, "reason": why} for rel, why in listed.items() if rel in by_rel]
            if kept:
                ok = kept
            else:  # nothing else to test: the listed ones stay in (the failure, if any, stays honest)
                res.pop("null_excluded_listed", None)
        # Story 13.10: an instrument that picks its layer with lorand/hirand is null-tested with the sfizz random replay (the engine is given the numbers
        # sfizz draws, see tools/offline-render/src/sfizz_random_replay.hpp). Only an instrument whose SFZ uses an opcode the replay cannot align
        # (replay_blockers) stays out of the automatic picks while another instrument exists; the report lists it with the opcodes.
        blocked = {}
        for rel, _ in ok:
            full = os.path.join(tree, *rel.split("/"))
            if has_random_layers(full):
                b = replay_blockers(full)
                if b:
                    blocked[rel] = b
        det = [kv for kv in ok if kv[0] not in blocked]
        if det and blocked:
            res["null_excluded_random_layers"] = [{"path": rel, "blockers": b} for rel, b in sorted(blocked.items())]
            ok = det
        ordered = sorted(ok, key=lambda kv: (kv[1].get("regions", 0), kv[0]))
        if len(ordered) <= n:
            return ordered
        idx = sorted({round(i * (len(ordered) - 1) / max(1, n - 1)) for i in range(n)})  # n picks spread from the smallest to the largest instrument
        return [ordered[i] for i in idx]

    def null_one(self, entry, tree, rel, bundle, suite, midi, work, tol, res):
        key = rel  # --base is the library root, so the instrument_key is the relative path
        tag = "%s__%s" % (re.sub(r"[^A-Za-z0-9_.-]", "_", rel), suite)
        ref, cand = os.path.join(work, tag + ".oracle.wav"), os.path.join(work, tag + ".engine.wav")
        r = {"suite": suite, "file": rel, "status": "fail"}
        rc, text, sec = self.sh([self.tools["oracle_sh"], os.path.join(tree, *rel.split("/")), midi, ref, "2"], os.path.join(work, tag + ".oracle.log"),
                                env={"VELORA_SFIZZ_RENDER": self.tools["sfizz"], "VELORA_SFIZZ_BLOCKSIZE": str(ORACLE_BLOCK_FRAMES)})
        r["oracle_seconds"] = round(sec, 1)
        if rc != 0 or not os.path.isfile(ref):
            r["error"] = "oracle render failed (rc %s): %s" % (rc, text[-300:].strip())
            res["failures"].append("%s %s: %s" % (rel, suite, r["error"]))
            return r
        # sfizz_render keeps rendering after the last MIDI event until a block is silent (a 15 s ampeg_release rings 15 s): the engine render
        # is given exactly that tail, so the harness compares the same span (a shorter one truncates the tail, a longer one counts the
        # engine's decay beyond the point where sfizz stopped, at block size 1 the first near-zero frame, as excess energy).
        t = max(0.01, min(600.0, wav_seconds(ref) - midi_suite.last_event_seconds(suite)))
        r["tail_seconds"] = round(t, 2)
        argv = [self.tools["render"], "--midi", midi, "--out", cand, "--engine", "sample", "--bundle", bundle, "--instrument", key,
                "--gain-db", "-7.35", "--oracle-block-frames", str(ORACLE_BLOCK_FRAMES), "--tail-seconds", str(t)]
        # Story 13.10: a lorand/hirand instrument is rendered with the sfizz random replay (test only); every other render is the unchanged command line.
        sfz_path = os.path.join(tree, *rel.split("/"))
        if has_random_layers(sfz_path):
            argv.append("--sfizz-random-replay")
            r["random_replay"] = True
            r["replay_blockers"] = replay_blockers(sfz_path)
            if midi_suite.uses_pedal(suite):
                # the probe holds notes with the sustain pedal: the engine draws a deferred note off's number when the pedal lifts, sfizz at the key up
                # (and sfizz makes extra dispatches for the delayed releases), so a note-off number (release-trigger regions) is NOT aligned. Recorded, no exactness claimed.
                r["replay_blockers"] = r["replay_blockers"] + ["sustain pedal (CC64) deferred releases"]
        rc, text, sec = self.sh(argv, os.path.join(work, tag + ".render.log"))
        r["render_seconds"] = round(sec, 1)
        r["render_report"] = text.strip().splitlines()[-1][:300] if text.strip() else ""
        if rc != 0 or not os.path.isfile(cand):
            r["error"] = "engine render failed (rc %s): %s" % (rc, text[-300:].strip())
            res["failures"].append("%s %s: %s" % (rel, suite, r["error"]))
            return r
        rc, text, _ = self.sh([self.tools["nulltest"], "--reference", ref, "--candidate", cand, "--tolerance-db", str(tol), "--max-offset-frames", "0",
                               "--check-excess", "--reference-decode", "sfizz-s16", "--candidate-quantize", "sfizz-s16"], os.path.join(work, tag + ".null.log"))
        r.update(parse_null(text))
        r["status"] = "pass" if rc == 0 else "fail"
        if rc != 0:
            r["error"] = "null test failed: " + " | ".join(l.strip() for l in text.strip().splitlines()[-4:])
            res["failures"].append("%s %s: residual %s dB vs tolerance %s dB" % (rel, suite, r.get("residual_db"), tol))
        for p in (ref, cand):  # the renders are large; the logs stay
            try:
                os.unlink(p)
            except OSError:
                pass
        return r


def parse_null(text):
    out = {}
    m = re.search(r"residual[^-\d]*(-?\d+(?:\.\d+)?|-inf)\s*dB", text)
    if m:
        out["residual_db"] = None if m.group(1) == "-inf" else float(m.group(1))
    m = re.search(r"worst[^\n]*?(-?\d+(?:\.\d+)?)\s*dB", text)
    if m:
        out["worst_window_db"] = float(m.group(1))
    return out


def cmd_run(args):
    m = cl.load_manifest(args.manifest)
    if args.max_null:
        m["max_null_instruments"] = args.max_null
    runner = Runner(args, m)
    runner.preflight(selected(m, args.only))
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    results, failures = [], 0
    for e in selected(m, args.only):
        t0 = time.time()
        work = os.path.join(out, "work", e["id"])
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work)
        try:
            tree, sha = cl.fetch_entry(e, args.cache, pin_manifest=args.manifest if args.pin else None, deep=args.deep,
                                       overrides=cl.parse_local_overrides(args.local), repo=REPO)
            if args.pin and not cl.pin_of(e):
                e[cl.pin_key(e)] = sha
            res = runner.library(e, tree, work)
        except cl.CorpusError as exc:
            res = {"id": e["id"], "name": e["name"], "kind": e["kind"], "status": "fail", "failures": ["fetch/verify: %s" % exc], "instruments": []}
        res["seconds"] = round(time.time() - t0, 1)
        if not args.keep_work:
            shutil.rmtree(os.path.join(work, "root"), ignore_errors=True)
        results.append(res)
        failures += res["status"] != "pass"
        print("%s: %s (%.0f s)%s" % (e["id"], res["status"].upper(), res["seconds"], "".join("\n    " + f for f in res["failures"][:8])), flush=True)
    report = {"generated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "manifest": os.path.relpath(args.manifest, REPO),
              "libraries": results, "excluded": [{"id": e["id"], "name": e["name"], "reason": e["excluded"]["reason"]} for e in cl.excluded(m)],
              "failed": failures}
    with open(os.path.join(out, "report.json"), "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=1)
    with open(os.path.join(out, "report.md"), "w", encoding="utf-8") as fh:
        fh.write(render_markdown(report))
    print("report: %s" % os.path.join(out, "report.md"))
    print("%s: %d of %d libraries failed; %d excluded with a reason" % ("FAIL" if failures else "PASS", failures, len(results), len(report["excluded"])))
    return 1 if failures else 0


def render_markdown(rep):
    L = ["# Reference corpus report", "", "Generated %s from `%s`." % (rep["generated"], rep["manifest"]), "",
         "| Library | Format | Status | Imported | Null-tested renders | Worst residual (dB) | Tolerance (dB) |", "|---|---|---|---|---|---|---|"]
    for r in rep["libraries"]:
        suites = [s for i in r["instruments"] for s in i["suites"]]
        resid = [s["residual_db"] for s in suites if s.get("residual_db") is not None]
        L.append("| %s | %s | %s | %s/%s | %d (%d passed) | %s | %s |" % (r["id"], r.get("format", "sfz").upper(), r["status"].upper(), r.get("imported", 0), r.get("files_found", r.get("sfz_found", "?")), len(suites),
                 sum(s["status"] == "pass" for s in suites), "%.1f" % max(resid) if resid else "n/a", r.get("tolerance_db", "n/a")))
    L += ["", "## Per library", ""]
    for r in rep["libraries"]:
        L += ["### %s (%s)" % (r["name"], r.get("format", "sfz").upper()), "", "- source `%s`, version %s, sha256 `%s`" % (r.get("source_url"), r.get("version"), r.get("sha256")),
              "- licence: %s (%s)" % (r.get("license"), r.get("license_url"))]
        for note in r.get("notes", []):
            L.append("- note: %s" % note)
        for s in r.get("skipped_listed", []):
            L.append("- skipped (listed in the manifest): `%s`: %s" % (s["path"], s["reason"]))
        for f in r["failures"][:40]:
            L.append("- FAILURE: %s" % f)
        if len(r["failures"]) > 40:
            L.append("- ... and %d more failures (report.json)" % (len(r["failures"]) - 40))
        for x in r.get("null_excluded_listed", []):
            L.append("- imported but not null-tested (listed in the manifest): `%s`: %s" % (x["path"], x["reason"]))
        if r.get("null_excluded_random_layers"):
            L.append("- imported but not null-tested (layers chosen with lorand/hirand, and an opcode the sfizz random replay cannot align, see the README): %s" %
                     ", ".join("`%s` (%s)" % (x["path"], ", ".join(x["blockers"])) for x in r["null_excluded_random_layers"]))
        if r.get("unsupported_histogram"):
            L.append("- import rejections by cause (count of lines): " + ", ".join("`%s` x%d" % kv for kv in r["unsupported_histogram"].items()))
        for i in r["instruments"]:
            L.append("- `%s`%s (%s regions)%s" % (i["file"], (" preset %s `%s`" % (i["preset"], i.get("preset_name", ""))) if "preset" in i else "", i["regions"], (" [recorded tolerance %s dB: %s]" % (i["tolerance_db"], i["tolerance_reason"])) if "tolerance_db" in i else ""))
            for s in i["suites"]:
                L.append("    - %s: %s, residual %s dB, worst window %s dB%s%s" % (s["suite"], s["status"], s.get("residual_db"), s.get("worst_window_db"),
                         " [oracle silent for this probe]" if s.get("oracle_silent") else "",
                         (" [sfizz random replay%s]" % ("; not aligned: " + ", ".join(s["replay_blockers"]) if s.get("replay_blockers") else "")) if s.get("random_replay") else ""))
        L.append("")
    L += ["## Excluded entries", ""]
    for e in rep["excluded"]:
        L.append("- `%s`: %s" % (e["id"], e["reason"]))
    return "\n".join(L) + "\n"


def repo_files(root):
    """Files git would add (tracked + untracked, minus ignored); the whole tree when it is not a git work tree."""
    if os.path.isdir(os.path.join(root, ".git")):
        out = subprocess.run(["git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard"], capture_output=True, check=True).stdout
        return [os.path.join(root, f.decode("utf-8", "surrogateescape")) for f in out.split(b"\0") if f]
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        files += [os.path.join(dirpath, f) for f in filenames]
    return files


def cmd_guard(args):
    root = os.path.abspath(args.root)
    hits = [f for f in repo_files(root) if f.lower().endswith(cl.AUDIO_EXTS) and os.path.exists(f)]
    bad = [h for h in hits if not _is_tracked_fixture(root, h)]
    if bad:
        print("FAIL: audio files in the repository tree outside the small test-fixture directories (corpus audio must never be committed):")
        for b in bad[:20]:
            print("  " + os.path.relpath(b, root))
        return 1
    print("guard ok: %d small test-fixture audio files, no corpus audio" % len(hits))
    return 0


def _is_tracked_fixture(root, path):
    rel = os.path.relpath(path, root).replace(os.sep, "/")
    return rel.startswith(("tools/offline-render/tests/", "librarian/tests/", "formatlib/tests/", "engine/", "tests/")) and os.path.getsize(path) < 8 * 1024 * 1024


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default=MANIFEST)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("--require-pins", action="store_true")
    c.add_argument("--local", action="append", default=[], metavar="ID=PATH")
    i = sub.add_parser("ids", help="print the ids of the included entries (one per line)")
    i.add_argument("--no-local", action="store_true", help="leave out local_dir entries (they cannot run on a machine without the founder's files)")
    for name in ("fetch", "run"):
        s = sub.add_parser(name)
        s.add_argument("--cache", default=os.environ.get("VELORA_CORPUS_CACHE", DEFAULT_CACHE))
        s.add_argument("--only", nargs="*", default=[])
        s.add_argument("--pin", action="store_true", help="trust on first use: pin the hash of a first download into the manifest")
        s.add_argument("--local", action="append", default=[], metavar="ID=PATH", help="directory of a local_dir entry (else $VELORA_CORPUS_LOCAL_<ID>); repeatable")
        s.add_argument("--deep", action="store_true", help="re-hash every cached file instead of the path+size listing")
        if name == "run":
            s.add_argument("--build", default=os.path.join(REPO, "build"))
            s.add_argument("--out", default=os.path.join(REPO, "build", "corpus-report"))
            s.add_argument("--keep-work", action="store_true")
            s.add_argument("--max-null", type=int, default=None, help="null-test up to N instruments per library (default: manifest max_null_instruments)")
            s.add_argument("--timeout", type=int, default=3600, help="seconds per tool invocation")
    g = sub.add_parser("guard")
    g.add_argument("--root", default=REPO)
    args = ap.parse_args(argv)
    try:
        return {"check": cmd_check, "fetch": cmd_fetch, "ids": cmd_ids, "run": cmd_run, "guard": cmd_guard}[args.cmd](args)
    except cl.CorpusError as exc:
        print("FAIL: %s" % exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
