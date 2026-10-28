"""Offline tests for the SF2 kind of the reference corpus (Story 13.11): manifest rules, SoundFont handling, the FluidSynth-grid MIDI suite, and the runner's
success, failure and report behaviour with fake tools and a tiny synthetic SoundFont generated in code.   python3 -m unittest discover -s tools/corpus/tests
"""
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import corpus_lib as cl  # noqa: E402
import corpus  # noqa: E402
import midi_suite  # noqa: E402
import sf2_fixture  # noqa: E402


def sf2_entry(**kw):
    e = {"id": "sf2-lib", "name": "SF2 lib", "kind": "extra", "format": "sf2", "source_url": "https://codeload.github.com/a/b/tar.gz/c", "version": "1", "commit": "c",
         "license": "CC0", "license_url": "https://example.org/l", "tolerance_db": -26}
    e.update(kw)
    return e


def parse_smf(data):
    """[(absolute tick, bytes)] of the single track of a format 0 SMF, and the PPQ."""
    assert data[:4] == b"MThd"
    ppq = struct.unpack(">H", data[12:14])[0]
    pos, end, tick, out = 22, 22 + struct.unpack(">I", data[18:22])[0], 0, []
    status = 0
    while pos < end:
        delta = 0
        while True:
            b = data[pos]
            pos += 1
            delta = (delta << 7) | (b & 0x7F)
            if not b & 0x80:
                break
        tick += delta
        if data[pos] == 0xFF:
            ln = data[pos + 2]
            out.append((tick, data[pos:pos + 3 + ln]))
            pos += 3 + ln
            continue
        status = data[pos]
        n = 1 if status & 0xF0 == 0xC0 else 2
        out.append((tick, data[pos:pos + 1 + n]))
        pos += 1 + n
    return ppq, out


class Sf2ManifestTests(unittest.TestCase):
    def test_sf2_entry_rules(self):
        cl.validate_entry(sf2_entry())
        cl.validate_entry(sf2_entry(null_presets=["x/a.sf2#0:0", "b.SF2#128:3"], null_files=["x/a.sf2", "b.SF2"]))
        for bad in ({"format": "sf3"}, {"format": "SF2"}, {"null_presets": []}, {"null_presets": ["a.sf2"]}, {"null_presets": ["a.sfz#0:0"]},
                    {"null_presets": ["../a.sf2#0:0"]}, {"null_presets": ["a.sf2#0:200"]}, {"null_presets": ["a.sf2#99999:1"]}, {"null_presets": "a.sf2#0:0"},
                    {"null_files": ["a.sfz"]}, {"skip_files": [{"path": "a.sfz", "reason": "r" * 20}]},
                    {"tolerance_overrides": [{"path": "a.sfz", "tolerance_db": -20, "reason": "r" * 50}]}):
            with self.assertRaises(cl.CorpusError, msg=str(bad)):
                cl.validate_entry(sf2_entry(**bad))

    def test_malformed_lists_are_corpus_errors_not_python_errors(self):
        for key in ("skip_files", "null_files", "null_exclude", "tolerance_overrides"):
            for bad in (5, "a.sf2", {"path": "a.sf2"}, [5], [None], [{}], [{"reason": "r" * 50}], [{"path": 7, "reason": "r" * 50, "tolerance_db": -20}]):
                for fmt in ("sf2", "sfz"):
                    e = sf2_entry(**{key: bad})
                    if fmt == "sfz":
                        del e["format"]
                    with self.assertRaises(cl.CorpusError, msg="%s %r %s" % (key, bad, fmt)):
                        cl.validate_entry(e)

    def test_duplicate_null_presets_are_refused(self):
        with self.assertRaises(cl.CorpusError):
            cl.validate_entry(sf2_entry(null_presets=["a.sf2#0:1", "a.sf2#0:1"]))

    def test_sf2_needs_a_recorded_tolerance(self):
        e = sf2_entry()
        del e["tolerance_db"]
        with self.assertRaises(cl.CorpusError):
            cl.validate_entry(e)

    def test_null_presets_belong_to_sf2_and_sfz_stays_the_default(self):
        sfz = dict(sf2_entry(), null_presets=["a.sf2#0:0"])
        del sfz["format"]
        with self.assertRaises(cl.CorpusError):
            cl.validate_entry(sfz)
        del sfz["null_presets"]
        cl.validate_entry(sfz)  # no format = sfz, no mandatory tolerance
        del sfz["tolerance_db"]
        cl.validate_entry(sfz)

    def test_same_pinning_and_source_rules_as_sfz(self):
        for bad in ({"source_url": "https://evil.example.org/x.tar.gz"}, {"source_url": "http://github.com/x"}, {"sha256": "xyz"}, {"expected_tree_sha256": "0" * 64}):
            with self.assertRaises(cl.CorpusError, msg=str(bad)):
                cl.validate_entry(sf2_entry(**bad))
        local = {k: v for k, v in sf2_entry().items() if k not in ("source_url", "commit")}
        local.update(source_kind="local_dir", local_hint="Pack folder", expected_tree_sha256="0" * 64)
        cl.validate_entry(local)
        with self.assertRaises(cl.CorpusError):
            cl.validate_entry(dict(local, local_hint="/Users/someone/Packs"))

    def test_sf2_counts_as_corpus_audio_for_the_guard_and_the_local_copy(self):
        self.assertIn(".sf2", cl.AUDIO_EXTS)
        self.assertIn(".sf2", cl.LOCAL_EXTS)
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "x.sf2"), "wb") as fh:
                fh.write(b"abc")
            self.assertEqual(corpus.main(["guard", "--root", d]), 1)


class Sf2FilesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))

    def write(self, name, data):
        p = os.path.join(self.tmp, name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as fh:
            fh.write(data)
        return p

    def test_presets_are_read_from_the_header_and_sorted(self):
        p = self.write("a.sf2", sf2_fixture.make_sf2(((128, 0, "Kit"), (0, 5, "Five"), (0, 1, "One"), (0, 1, "One"))))
        self.assertEqual(corpus.sf2_presets(p), [(0, 1, "One"), (0, 1, "One"), (0, 5, "Five"), (128, 0, "Kit")])  # duplicates are kept: the runner fails them

    def test_not_a_soundfont_is_a_corpus_error(self):
        for data in (b"", b"RIFF", b"RIFF\x04\0\0\0WAVE", b"RIFF\x04\0\0\0sfbk", b"x" * 100):
            with self.assertRaises(cl.CorpusError, msg=repr(data[:12])):
                corpus.sf2_presets(self.write("b.sf2", data))
        good = sf2_fixture.make_sf2()
        with self.assertRaises(cl.CorpusError):  # truncated inside the pdta list
            corpus.sf2_presets(self.write("c.sf2", good[:len(good) - 300]))

    def test_oracle_wav_is_trimmed_after_the_last_sound(self):
        p = os.path.join(self.tmp, "o.wav")
        with wave.open(p, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(48000)
            w.writeframes(b"\0\0\0\0" * 1000 + b"\x01\0\0\0" + b"\0\0\0\0" * 20000)
        secs = corpus.trim_oracle_wav(p)
        self.assertAlmostEqual(secs, (1001 + 4800) / 48000.0)
        with wave.open(p, "rb") as w:
            self.assertEqual(w.getnframes(), 1001 + 4800)
        with wave.open(p, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(48000)
            w.writeframes(b"\0\0\0\0" * 100)
        self.assertIsNone(corpus.trim_oracle_wav(p))  # silence is reported, not trimmed away
        with wave.open(p, "wb") as w:  # not the oracle's format
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(44100)
            w.writeframes(b"\1\0" * 100)
        with self.assertRaises(cl.CorpusError):
            corpus.trim_oracle_wav(p)


class FluidGridSuiteTests(unittest.TestCase):
    def test_every_event_is_on_the_four_millisecond_grid_and_the_engine_is_one_block_later(self):
        for name in midi_suite.SUITES:
            ppq, ora = parse_smf(midi_suite.build_fluid(name, 128, 7))
            ppq2, eng = parse_smf(midi_suite.build_fluid(name, 128, 7, engine=True))
            self.assertEqual((ppq, ppq2), (375, 375))
            self.assertEqual(len(ora), len(eng))
            channel = [(t, b) for t, b in ora if b[0] != 0xFF]
            self.assertTrue(all(t % 3 == 0 for t, _ in channel), name)
            for (to, bo), (te, be) in zip(channel, [(t, b) for t, b in eng if b[0] != 0xFF]):
                self.assertEqual(bo, be)
                self.assertEqual(te, to + (1 if to > 0 else 0), name)
            setup = [b for t, b in channel if t == 0]
            self.assertEqual([bytes(b) for b in setup[:4]], [b"\xb0\x00\x01", b"\xb0\x20\x00", b"\xc0\x07", b"\xb0\x07\x7f"])  # bank 128 = MSB 1, LSB 0; program 7; volume
            self.assertTrue(any(b[0] == 0x90 for _t, b in channel), name)
            self.assertEqual(sum(1 for _t, b in channel if b[0] & 0xF0 == 0x90), sum(1 for _t, b in channel if b[0] & 0xF0 == 0x80))
        a, b = midi_suite.build_fluid("range", 0, 0), midi_suite.build_fluid("range", 0, 0)
        self.assertEqual(a, b)

    def test_last_event_time_matches_the_midi(self):
        for eng in (False, True):
            _ppq, ev = parse_smf(midi_suite.build_fluid("releases", 0, 0, engine=eng))
            last = max(t for t, b in ev if b[0] != 0xFF)
            self.assertAlmostEqual(midi_suite.last_event_seconds_fluid("releases", 0, 0, engine=eng), last * 64 / 48000.0)

    def test_the_sfz_suite_is_unchanged(self):
        self.assertEqual(midi_suite.build("range")[:4], b"MThd")
        self.assertEqual(struct.unpack(">H", midi_suite.build("range")[12:14])[0], midi_suite.PPQ)


class FakeTools:
    """Stands in for Runner.sh: librarian import, the FluidSynth wrapper, the engine render and the null test, all offline."""

    def __init__(self, import_status=None, residual=-40.0, oracle_rc=0, silent_oracle=False, null_rc=None, render_rc=0, import_rc=0, empty_report=False,
                 sound_seconds=0.1, silent_suites=()):
        self.calls = []
        self.import_rc, self.empty_report, self.sound_seconds, self.silent_suites = import_rc, empty_report, sound_seconds, set(silent_suites)
        self.oracle_wavs = []
        self.status = import_status or {}
        self.residual, self.oracle_rc, self.silent, self.null_rc, self.render_rc = residual, oracle_rc, silent_oracle, null_rc, render_rc

    def __call__(self, argv, log, env=None, cwd=None):
        self.calls.append((list(argv), env))
        tool = os.path.basename(argv[0])
        if tool == "librarian":
            files = [a for a in argv[2:argv.index("--root")]]
            lines = []
            for f in files:
                st = self.status.get(os.path.basename(f), "imported")
                try:
                    n = len(corpus.sf2_presets(f))
                except cl.CorpusError:
                    n = 0
                o = {"type": "file", "file": f, "status": st, "reason": "none" if st == "imported" else "unsupported_features", "messages": ["%s: sample rate 35135 Hz" % f] if st != "imported" else [],
                     "library_id": "lib-" + os.path.basename(f), "bundle_id": "b1", "samples": 2, "regions": 3 * n, "format": "sf2", "presets": n, "warning_count": 0}
                lines.append(json.dumps(o))
            with open(argv[argv.index("--report") + 1], "w") as fh:
                fh.write("" if self.empty_report else "\n".join(lines) + "\n")
            return self.import_rc, "", 0.0
        if tool == "render-fluidsynth.sh":
            if self.oracle_rc:
                return self.oracle_rc, "fluidsynth failed", 0.0
            with wave.open(argv[3], "wb") as w:
                w.setnchannels(2)
                w.setsampwidth(2)
                w.setframerate(48000)
                silent = self.silent or any(os.path.basename(argv[2]).startswith(x + "_") for x in self.silent_suites)
                w.writeframes(b"\0\0\0\0" * 48000 if silent else b"\x10\0\x10\0" * int(self.sound_seconds * 48000) + b"\0\0\0\0" * 96000)
            return 0, "", 0.0
        if tool == "velora-offline-render":
            if self.render_rc:
                return self.render_rc, "render failed", 0.0
            with open(argv[argv.index("--out") + 1], "wb") as fh:
                fh.write(b"RIFF")
            return 0, "rendered 1 notes", 0.0
        if tool == "velora-null-test":
            rc = self.null_rc if self.null_rc is not None else (0 if self.residual <= float(argv[argv.index("--tolerance-db") + 1]) else 1)
            return rc, "  result: %s residual %.1f dB offset 0\n  worst window -20.0 dB\n" % ("PASS" if rc == 0 else "FAIL", self.residual), 0.0
        raise AssertionError(argv)


class Sf2RunnerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))
        self.tree = os.path.join(self.tmp, "tree")
        self.work = os.path.join(self.tmp, "work")
        os.makedirs(self.tree)
        os.makedirs(self.work)
        for name, presets in (("a.sf2", ((0, 0, "Sine"),)), ("sub/b.sf2", ((0, 0, "B zero"), (0, 1, "B one"), (128, 0, "Kit")))):
            p = os.path.join(self.tree, *name.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as fh:
                fh.write(sf2_fixture.make_sf2(presets))
        self.entry = json.loads(json.dumps(sf2_entry(source_kind="local_dir", local_hint="pack", expected_tree_sha256="0" * 64)))
        for k in ("source_url", "commit"):
            self.entry.pop(k)

    def runner(self, tools, n=3):
        r = corpus.Runner.__new__(corpus.Runner)
        r.m = {"max_null_instruments": n, "default_tolerance_db": -60}
        r.tools = {"librarian": "/x/librarian", "render": "/x/velora-offline-render", "nulltest": "/x/velora-null-test", "oracle_sh": "/x/render-oracle.sh",
                   "sfizz": "/x/sfizz_render", "fluid_sh": "/x/render-fluidsynth.sh", "fluidsynth": "/x/fluidsynth"}
        r.sh = tools
        return r

    def test_all_probes_pass_and_the_tools_get_the_sf2_command_lines(self):
        tools = FakeTools()
        res = self.runner(tools).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "pass", res["failures"])
        self.assertEqual((res["format"], res["imported"], res["files_found"], res["presets_total"]), ("sf2", 2, 2, 4))
        self.assertEqual(len(res["instruments"]), 3)  # up to max_null_instruments presets spread over the sorted list
        self.assertEqual([(i["file"], i["preset"]) for i in res["instruments"]], [("a.sf2", "0:0"), ("sub/b.sf2", "0:1"), ("sub/b.sf2", "128:0")])
        self.assertTrue(all(len(i["suites"]) == 6 and all(s["status"] == "pass" for s in i["suites"]) for i in res["instruments"]))
        imp = [c for c, _e in tools.calls if c[0].endswith("librarian")]
        self.assertEqual(len(imp), 1)
        self.assertTrue(imp[0][2].endswith("a.sf2") and imp[0][3].endswith("b.sf2"))
        oracle = [(c, e) for c, e in tools.calls if c[0].endswith("render-fluidsynth.sh")]
        self.assertEqual(len(oracle), 18)
        self.assertEqual(oracle[0][1], {"VELORA_FLUIDSYNTH": "/x/fluidsynth"})
        self.assertTrue(all(c[2].endswith("_oracle.mid") for c, _ in oracle))
        render = [c for c, _e in tools.calls if c[0].endswith("velora-offline-render")][0]
        for opt, val in (("--engine", "sample"), ("--instrument", "0:0"), ("--gain-db", "0")):
            self.assertEqual(render[render.index(opt) + 1], val)
        self.assertTrue(render[render.index("--midi") + 1].endswith("_engine.mid"))
        self.assertNotIn("--oracle-block-frames", render)
        self.assertNotIn("--sfizz-random-replay", render)
        null = [c for c, _e in tools.calls if c[0].endswith("velora-null-test")][0]
        self.assertEqual(null[null.index("--max-offset-frames") + 1], "1")
        self.assertEqual(null[null.index("--tolerance-db") + 1], "-26")
        self.assertIn("--check-excess", null)
        self.assertNotIn("--candidate-quantize", null)
        self.assertNotIn("--reference-decode", null)

    def test_the_engine_tail_is_the_trimmed_oracle_length_minus_the_last_engine_event(self):
        tools = FakeTools(sound_seconds=60.0)  # sound for 60 s, then silence: the trimmed oracle is 60 s + 100 ms
        res = self.runner(tools, n=1).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "pass", res["failures"])
        renders = [c for c, _e in tools.calls if c[0].endswith("velora-offline-render")]
        self.assertEqual(len(renders), 6)
        for c in renders:
            suite = os.path.basename(c[c.index("--midi") + 1]).split("_0_0_")[0]
            want = 60.1 - midi_suite.last_event_seconds_fluid(suite, 0, 0, engine=True)
            self.assertAlmostEqual(float(c[c.index("--tail-seconds") + 1]), want, places=2, msg=suite)

    def test_a_sound_that_ends_before_the_last_event_pads_the_reference_instead_of_flooring_the_tail(self):
        tools = FakeTools(sound_seconds=0.05)
        res = self.runner(tools, n=1).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "pass", res["failures"])
        for s in res["instruments"][0]["suites"]:
            self.assertEqual(s["tail_seconds"], 0.01)

    def test_renders_are_deleted_on_every_exit_path(self):
        for tools in (FakeTools(), FakeTools(residual=-1.0), FakeTools(render_rc=1), FakeTools(null_rc=2), FakeTools(silent_oracle=True)):
            shutil.rmtree(self.work, ignore_errors=True)
            os.makedirs(self.work)
            self.runner(tools, n=1).library(self.entry, self.tree, self.work)
            left = [f for f in os.listdir(self.work) if f.endswith((".wav", ".trim"))]
            self.assertEqual(left, [])

    def test_null_presets_name_the_exact_presets_and_a_stale_one_fails(self):
        self.entry["null_presets"] = ["sub/b.sf2#128:0"]
        res = self.runner(FakeTools()).library(self.entry, self.tree, self.work)
        self.assertEqual([(i["file"], i["preset"], i["preset_name"]) for i in res["instruments"]], [("sub/b.sf2", "128:0", "Kit")])
        self.entry["null_presets"] = ["sub/b.sf2#5:5"]
        shutil.rmtree(self.work)
        os.makedirs(self.work)
        res = self.runner(FakeTools()).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "fail")
        self.assertTrue(any("sub/b.sf2#5:5" in f for f in res["failures"]))

    def test_an_sf2_that_does_not_import_fails_the_entry_with_the_reason(self):
        res = self.runner(FakeTools(import_status={"a.sf2": "skipped"})).library(self.entry, self.tree, self.work)
        self.assertEqual((res["status"], res["imported"], res["import_failed"]), ("fail", 1, 1))
        self.assertTrue(any("import a.sf2: skipped" in f and "sample rate 35135 Hz" in f for f in res["failures"]), res["failures"])
        self.assertNotIn(self.tree, " ".join(res["failures"]))
        self.assertTrue(res["instruments"])  # the other file is still tested

    def test_a_listed_skip_passes_and_a_stale_one_fails(self):
        self.entry["skip_files"] = [{"path": "a.sf2", "reason": "sample rate 35135 Hz is not supported by the importer"}]
        res = self.runner(FakeTools(import_status={"a.sf2": "skipped"})).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "pass", res["failures"])
        self.assertEqual([s["path"] for s in res["skipped_listed"]], ["a.sf2"])
        self.entry["skip_files"] = [{"path": "gone.sf2", "reason": "this file does not exist any more"}]
        shutil.rmtree(self.work)
        os.makedirs(self.work)
        res = self.runner(FakeTools()).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "fail")
        self.assertTrue(any("gone.sf2" in f and "stale" in f for f in res["failures"]))

    def test_a_residual_above_the_recorded_tolerance_fails_naming_file_preset_and_probe(self):
        res = self.runner(FakeTools(residual=-20.0), n=1).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "fail")
        self.assertEqual(len(res["failures"]), 6)
        self.assertIn("a.sf2 0:0 range: residual -20.0 dB vs tolerance -26 dB", res["failures"])
        self.assertEqual(res["instruments"][0]["suites"][0]["residual_db"], -20.0)

    def test_a_tolerance_override_needs_its_reason_and_applies_to_one_file(self):
        self.entry["tolerance_overrides"] = [{"path": "a.sf2", "tolerance_db": -10, "reason": "r" * 50}]
        res = self.runner(FakeTools(residual=-15.0), n=3).library(self.entry, self.tree, self.work)
        a = [i for i in res["instruments"] if i["file"] == "a.sf2"][0]
        self.assertEqual((a["tolerance_db"], a["tolerance_reason"]), (-10, "r" * 50))
        self.assertTrue(all(s["status"] == "pass" for s in a["suites"]))
        self.assertEqual(res["status"], "fail")  # the others still use -26
        self.assertFalse(any("a.sf2" in f for f in res["failures"]))

    def test_oracle_engine_and_null_test_failures_fail_the_entry(self):
        for tools, text in ((FakeTools(oracle_rc=5), "oracle render failed"), (FakeTools(oracle_rc=77), "oracle render failed"), (FakeTools(render_rc=1), "engine render failed"),
                            (FakeTools(null_rc=2), "residual")):
            shutil.rmtree(self.work, ignore_errors=True)
            os.makedirs(self.work)
            res = self.runner(tools, n=1).library(self.entry, self.tree, self.work)
            self.assertEqual(res["status"], "fail", text)
            self.assertTrue(any(text in f for f in res["failures"]), (text, res["failures"][:2]))

    def test_a_probe_the_oracle_renders_as_silence_is_reported_and_judged_by_the_null_test(self):
        res = self.runner(FakeTools(silent_suites=("range",)), n=1).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "pass", res["failures"])
        self.assertTrue(res["instruments"][0]["suites"][0].get("oracle_silent"))
        shutil.rmtree(self.work)
        os.makedirs(self.work)
        shutil.rmtree(self.work)
        os.makedirs(self.work)
        res = self.runner(FakeTools(silent_suites=("range",), null_rc=1), n=1).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "fail")
        self.assertTrue(any("oracle silent" in f for f in res["failures"]))

    def test_a_silent_reference_is_cut_to_the_engine_length_and_every_probe_silent_fails_the_preset(self):
        seen = []
        tools = FakeTools(silent_oracle=True)
        inner = tools.__call__

        def spy(argv, log, env=None, cwd=None):
            if argv[0].endswith("velora-null-test"):
                with wave.open(argv[argv.index("--reference") + 1], "rb") as w:
                    seen.append((w.getnframes() / 48000.0, os.path.basename(argv[argv.index("--reference") + 1])))
            return inner(argv, log, env, cwd)

        res = self.runner(spy, n=1).library(self.entry, self.tree, self.work)
        self.assertEqual(len(seen), 6)
        for secs, name in seen:
            suite = [x for x in midi_suite.SUITES if ("__" + x + ".") in name][0]
            self.assertAlmostEqual(secs, midi_suite.last_event_seconds_fluid(suite, 0, 0, engine=True) + 0.1, places=3)  # not 2 s longer
        self.assertEqual(res["status"], "fail")
        self.assertTrue(any("silence for every probe" in f for f in res["failures"]), res["failures"])
        shutil.rmtree(self.work)
        os.makedirs(self.work)
        res = self.runner(FakeTools(silent_suites=("range", "velocity")), n=1).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "pass", res["failures"])  # some probes silent, others not: reported per probe
        self.assertEqual([bool(x.get("oracle_silent")) for x in res["instruments"][0]["suites"]], [True, True, False, False, False, False])

    def test_zero_presets_every_file_skipped_and_empty_import_fail_explicitly(self):
        # (a) an SF2 without presets: nothing is null-tested
        with open(os.path.join(self.tree, "a.sf2"), "wb") as fh:
            fh.write(sf2_fixture.make_sf2(()))
        os.unlink(os.path.join(self.tree, "sub", "b.sf2"))
        res = self.runner(FakeTools()).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "fail")
        self.assertTrue(any("no preset was null-tested" in f for f in res["failures"]), res["failures"])
        # (a2) a file that is no SoundFont at all
        with open(os.path.join(self.tree, "a.sf2"), "wb") as fh:
            fh.write(b"not a soundfont")
        shutil.rmtree(self.work)
        os.makedirs(self.work)
        res = self.runner(FakeTools()).library(dict(self.entry), self.tree, self.work)
        self.assertEqual(res["status"], "fail")
        self.assertTrue(any("cannot list the presets" in f for f in res["failures"]))
        self.assertTrue(any("no preset was null-tested" in f for f in res["failures"]))

    def test_all_files_skipped_never_calls_the_librarian(self):
        self.entry["skip_files"] = [{"path": "a.sf2", "reason": "sample rate is not supported"}, {"path": "sub/b.sf2", "reason": "sample rate is not supported"}]
        tools = FakeTools()
        res = self.runner(tools).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "fail")
        self.assertTrue(any("listed in skip_files" in f for f in res["failures"]))
        self.assertEqual(tools.calls, [])

    def test_import_that_writes_no_report_or_times_out_fails(self):
        res = self.runner(FakeTools(empty_report=True)).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "fail")
        self.assertTrue(any("wrote no report line" in f for f in res["failures"]))
        self.assertEqual(res["imported"], 0)
        shutil.rmtree(self.work)
        os.makedirs(self.work)
        res = self.runner(FakeTools(empty_report=True, import_rc=-999)).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "fail")
        self.assertIn("import timed out", res["failures"])

    def test_duplicate_preset_keys_and_out_of_range_banks_fail(self):
        with open(os.path.join(self.tree, "a.sf2"), "wb") as fh:
            fh.write(sf2_fixture.make_sf2(((0, 1, "One"), (0, 1, "Uno"), (20000, 3, "Far"), (0, 200, "High"), (0, 2, "Ok"))))
        res = self.runner(FakeTools()).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "fail")
        self.assertTrue(any("preset key 0:1 appears more than once" in f for f in res["failures"]), res["failures"])
        self.assertTrue(any("bank 20000" in f for f in res["failures"]) and any("program 200" in f for f in res["failures"]))
        self.assertNotIn("20000:3", [i["preset"] for i in res["instruments"]])

    def test_pick_presets_null_exclude_fallback_and_null_files(self):
        why = "r" * 50
        ok = [("a.sf2", {"presets": 1}), ("sub/b.sf2", {"presets": 3})]
        r = self.runner(None, n=10)
        res = {"failures": [], "notes": []}
        picked = r.pick_presets(dict(self.entry, null_exclude=[{"path": "sub/b.sf2", "reason": why}]), ok, res, self.tree)
        self.assertEqual([p[0] for p in picked], ["a.sf2"])
        self.assertEqual(res["null_excluded_listed"], [{"path": "sub/b.sf2", "reason": why}])
        res = {"failures": [], "notes": []}  # everything excluded: the listed ones stay in and are not reported as left out
        picked = r.pick_presets(dict(self.entry, null_exclude=[{"path": "a.sf2", "reason": why}, {"path": "sub/b.sf2", "reason": why}]), ok, res, self.tree)
        self.assertEqual(len(picked), 4)
        self.assertNotIn("null_excluded_listed", res)
        res = {"failures": [], "notes": []}  # null_files naming a file that did not import fails, the rest is still picked
        picked = r.pick_presets(dict(self.entry, null_files=["a.sf2", "missing.sf2"]), ok, res, self.tree)
        self.assertEqual([p[0] for p in picked], ["a.sf2"])
        self.assertTrue(any("null_files lists 'missing.sf2'" in f for f in res["failures"]))

    def test_no_sf2_file_in_the_library_fails(self):
        os.unlink(os.path.join(self.tree, "a.sf2"))
        shutil.rmtree(os.path.join(self.tree, "sub"))
        res = self.runner(FakeTools()).library(self.entry, self.tree, self.work)
        self.assertEqual(res["status"], "fail")
        self.assertIn("no .sf2 file found in the library", res["failures"])

    def test_preflight_demands_only_the_oracle_of_the_selected_formats(self):
        r = self.runner(None)
        for k, v in list(r.tools.items()):
            p = os.path.join(self.tmp, k)
            open(p, "w").close()
            r.tools[k] = p
        sfz = {"id": "s", "format": "sfz"}
        sf2 = {"id": "f", "format": "sf2"}
        plain = {"id": "p"}
        os.unlink(r.tools["sfizz"])
        r.preflight([sf2])  # an SF2-only run does not need sfizz
        with self.assertRaises(cl.CorpusError) as cm:
            r.preflight([sfz, sf2])
        self.assertIn("sfizz", str(cm.exception))
        with self.assertRaises(cl.CorpusError):
            r.preflight([plain])
        os.unlink(r.tools["fluidsynth"])
        with self.assertRaises(cl.CorpusError) as cm:
            r.preflight([sf2])
        self.assertIn("fluidsynth", str(cm.exception))
        with self.assertRaises(cl.CorpusError):
            r.preflight()  # nothing selected = everything required

    def test_report_lists_sf2_and_sfz_together_and_a_failure_is_visible(self):
        ok = self.runner(FakeTools(), n=1).library(self.entry, self.tree, self.work)
        shutil.rmtree(self.work)
        os.makedirs(self.work)
        bad = self.runner(FakeTools(residual=-5.0), n=1).library(dict(self.entry, id="sf2-bad", name="Bad SF2"), self.tree, self.work)
        sfz = {"id": "an-sfz", "name": "An SFZ", "kind": "required", "format": "sfz", "status": "pass", "failures": [], "imported": 4, "sfz_found": 4, "files_found": 4,
               "tolerance_db": -60, "version": "1", "license": "CC0", "license_url": "u", "source_url": "s", "sha256": "h",
               "instruments": [{"file": "x.sfz", "regions": 2, "suites": [{"suite": "range", "status": "pass", "residual_db": -70.0, "worst_window_db": -60.0}]}]}
        legacy = dict(sfz, id="old", name="Old", sfz_found=2)  # a result written before this story has no format and no files_found
        del legacy["format"], legacy["files_found"]
        rep = {"generated": "t", "manifest": "m", "libraries": [sfz, ok, bad, legacy], "excluded": [], "failed": 1}
        md = corpus.render_markdown(rep)
        self.assertIn("| Library | Format | Status |", md)
        self.assertIn("| an-sfz | SFZ | PASS | 4/4 |", md)
        self.assertIn("| sf2-lib | SF2 | PASS | 2/2 |", md)
        self.assertIn("| sf2-bad | SF2 | FAIL | 2/2 |", md)
        self.assertIn("| old | SFZ | PASS | 4/2 |", md)
        self.assertIn("preset 0:0 `Sine`", md)
        self.assertIn("FAILURE: a.sf2 0:0 range: residual -5.0 dB vs tolerance -26 dB", md)
        json.dumps(rep)  # report.json stays serialisable

    def test_run_exit_code_is_one_when_any_sf2_entry_fails_and_zero_otherwise(self):
        manifest = {"schema": 1, "max_null_instruments": 1, "libraries": [
            dict(self.entry, id="sf2-lib", local_hint="pack"), {"id": "ex", "name": "Excluded", "kind": "extra", "excluded": {"reason": "no longer distributed"}}]}
        mpath = os.path.join(self.tmp, "m.json")
        out = os.path.join(self.tmp, "out")
        with open(mpath, "w") as fh:
            json.dump(manifest, fh)
        # the sfizz tools are NOT created: an SF2-only run must not need them. `missing` names a tool that is removed (exit 1 before any import).
        for residual, missing, code in ((-40.0, None, 0), (-1.0, None, 1), (-40.0, "fluidsynth", 1), (-40.0, "librarian", 1)):
            fake = FakeTools(residual=residual)
            orig_runner, orig_tp, orig_fetch = corpus.Runner, corpus.tool_paths, cl.fetch_entry
            tools_dir = os.path.join(self.tmp, "bin")
            shutil.rmtree(tools_dir, ignore_errors=True)
            os.makedirs(tools_dir)
            names = {"librarian": "librarian", "render": "velora-offline-render", "nulltest": "velora-null-test", "oracle_sh": "render-oracle.sh", "sfizz": "sfizz_render",
                     "fluid_sh": "render-fluidsynth.sh", "fluidsynth": "fluidsynth"}
            for k, n in names.items():
                if k not in ("sfizz", "oracle_sh", missing):
                    open(os.path.join(tools_dir, n), "a").close()

            def fake_paths(build_dir, d=tools_dir, names=names):
                return {k: os.path.join(d, v) for k, v in names.items()}

            class R(orig_runner):
                def sh(self, argv, log, env=None, cwd=None):
                    return fake(argv, log, env, cwd)

            corpus.Runner, corpus.tool_paths = R, fake_paths
            cl.fetch_entry = lambda e, *a, **k: (self.tree, "0" * 64)
            shutil.rmtree(out, ignore_errors=True)
            try:
                rc = corpus.main(["--manifest", mpath, "run", "--out", out, "--cache", os.path.join(self.tmp, "cache")])
            finally:
                corpus.Runner, corpus.tool_paths, cl.fetch_entry = orig_runner, orig_tp, orig_fetch
            self.assertEqual(rc, code, (residual, missing))
            if missing:
                self.assertEqual(fake.calls, [], "a missing tool must stop the run before any import")
                self.assertFalse(os.path.exists(os.path.join(out, "report.json")))
                continue
            with open(os.path.join(out, "report.json")) as fh:
                rep = json.load(fh)
            self.assertEqual(rep["failed"], code)
            self.assertEqual(rep["libraries"][0]["format"], "sf2")
            self.assertTrue(os.path.isfile(os.path.join(out, "report.md")))

    def test_ids_no_local_leaves_out_sf2_entries_with_a_notice(self):
        import contextlib
        import io
        mpath = os.path.join(self.tmp, "ids.json")
        sfz = {k: v for k, v in sf2_entry(id="an-sfz").items() if k not in ("format", "tolerance_db")}
        with open(mpath, "w") as fh:
            json.dump({"schema": 1, "libraries": [sfz, sf2_entry()]}, fh)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(corpus.main(["--manifest", mpath, "ids", "--no-local"]), 0)
        self.assertEqual(out.getvalue().split(), ["an-sfz"])
        self.assertIn("sf2-lib", err.getvalue())
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            corpus.main(["--manifest", mpath, "ids"])
        self.assertEqual(out.getvalue().split(), ["an-sfz", "sf2-lib"])


if __name__ == "__main__":
    unittest.main()
