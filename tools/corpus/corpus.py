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
import time

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


def tool_paths(build_dir):
    b = os.path.abspath(build_dir)
    return {
        "librarian": os.path.join(b, "librarian", "librarian"),
        "render": os.path.join(b, "tools", "offline-render", "velora-offline-render"),
        "nulltest": os.path.join(b, "tools", "offline-render", "velora-null-test"),
        "oracle_sh": os.path.join(REPO, "tools", "oracle", "render-oracle.sh"),
        "sfizz": os.environ.get("VELORA_SFIZZ_RENDER") or os.path.join(REPO, "tools", "oracle", ".work", "build", "library", "bin", "sfizz_render"),
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
    for e in cl.included(cl.load_manifest(args.manifest)):
        if not (args.no_local and cl.is_local(e)):
            print(e["id"])
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

    def preflight(self):
        missing = [k for k in ("librarian", "render", "nulltest", "oracle_sh", "sfizz") if not os.path.isfile(self.tools[k])]
        if missing:
            raise cl.CorpusError("missing tool(s): %s. Build with cmake (target librarian, velora-offline-render, velora-null-test) and tools/oracle/build-sfizz-render.sh" % ", ".join("%s=%s" % (k, self.tools[k]) for k in missing))

    def library(self, entry, tree, work):
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
        res["sfz_found"] = len(sfz)
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
    runner.preflight()
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
         "| Library | Status | SFZ imported | Null-tested renders | Worst residual (dB) | Tolerance (dB) |", "|---|---|---|---|---|---|"]
    for r in rep["libraries"]:
        suites = [s for i in r["instruments"] for s in i["suites"]]
        resid = [s["residual_db"] for s in suites if s.get("residual_db") is not None]
        L.append("| %s | %s | %s/%s | %d (%d passed) | %s | %s |" % (r["id"], r["status"].upper(), r.get("imported", 0), r.get("sfz_found", "?"), len(suites),
                 sum(s["status"] == "pass" for s in suites), "%.1f" % max(resid) if resid else "n/a", r.get("tolerance_db", "n/a")))
    L += ["", "## Per library", ""]
    for r in rep["libraries"]:
        L += ["### %s" % r["name"], "", "- source `%s`, version %s, sha256 `%s`" % (r.get("source_url"), r.get("version"), r.get("sha256")),
              "- licence: %s (%s)" % (r.get("license"), r.get("license_url"))]
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
            L.append("- `%s` (%s regions)%s" % (i["file"], i["regions"], (" [recorded tolerance %s dB: %s]" % (i["tolerance_db"], i["tolerance_reason"])) if "tolerance_db" in i else ""))
            for s in i["suites"]:
                L.append("    - %s: %s, residual %s dB, worst window %s dB%s" % (s["suite"], s["status"], s.get("residual_db"), s.get("worst_window_db"),
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
