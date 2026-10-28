"""Offline tests for the import-failure reason table of the corpus report (synthetic librarian report lines, no real library).
python3 -m unittest discover -s tools/corpus/tests"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import corpus_lib as cl  # noqa: E402
import corpus  # noqa: E402

T = "/cache/lib/tree"


def line(status, reason, messages, **kw):
    o = {"type": "file", "file": T + "/A.sfz", "status": status, "reason": reason, "messages": messages, "offenders": [], "failing_path": ""}
    o.update(kw)
    return o


class ClassifyTests(unittest.TestCase):
    def one(self, msg):
        c, d, f, l = cl.classify_message(msg)
        return c, d, f, l

    def test_each_category(self):
        cases = [
            (T + "/A.sfz:3: unsupported opcode 'rt_decay'", ("unsupported_opcode", "rt_decay")),
            (T + "/A.sfz:3: unsupported header <effect>", ("unsupported_opcode", "header <effect>")),
            (T + "/A.sfz:4: unsupported value '1' for opcode 'ampeg_dynamic'", ("unsupported_value", "ampeg_dynamic")),
            (T + "/A.sfz:5: unsupported value 'x' for opcode 'on_locc64'", ("unsupported_value", "on_locc64")),
            (T + "/A.sfz:6: path '..\\Samples\\a.wav' leaves the SFZ directory (no '..' escapes or absolute paths outside it)", ("path_escapes_sfz_dir", "relative path with '..' (backslash separators)")),
            (T + "/A.sfz:6: path '/x/a.wav' leaves the SFZ directory (no '..' escapes or absolute paths outside it)", ("path_escapes_sfz_dir", "absolute path outside the SFZ directory")),
            (T + "/A.sfz:7: " + T + "/s/a.flac: unsupported audio: not a WAV file", ("sample_format_not_supported", ".flac")),
            (T + "/A.sfz:7: " + T + "/s/a.wav: unsupported audio format (need WAV: 16/24-bit PCM ...)", ("sample_format_not_supported", ".wav (format outside the subset)")),
            (T + "/A.sfz:8: sample file not found: " + T + "/s/a.wav", ("missing_sample", "sample= file does not exist")),
            (T + "/A.sfz:8: sample file not found: " + T + "/s/a.ogg", ("sample_format_not_supported", ".ogg (file not found; only .wav is read)")),
            (T + "/includes/b.sfz: no regions in SFZ file", ("include_fragment", "no <region>")),
            (T + "/A.sfz:9: " + T + "/s/a.wav: damaged WAV file (truncated)", ("damaged_parse", "damaged WAV file (truncated)")),
            (T + "/A.sfz:10: region without a sample opcode", ("damaged_parse", "region without sample=")),
            (T + "/A.sfz:11: invalid value 'zz' for opcode 'lokey'", ("damaged_parse", "invalid value for lokey")),
            ("something unforeseen", ("other", "something unforeseen")),
        ]
        for msg, want in cases:
            with self.subTest(msg=msg):
                self.assertEqual(self.one(msg)[:2], want)

    def test_location(self):
        self.assertEqual(self.one(T + "/inc/x.sfz:12: unsupported opcode 'foo'")[2:], (T + "/inc/x.sfz", 12))

    def test_fragment_without_line(self):
        # the importer reports "<file>: no regions in SFZ file" with no line number
        self.assertEqual(self.one(T + "/includes/b.sfz: no regions in SFZ file")[0], "include_fragment")


class SummaryTests(unittest.TestCase):
    def test_table_counts_and_examples(self):
        a = line("skipped", "unsupported_features", [T + "/A.sfz:3: unsupported opcode 'rt_decay'", T + "/inc/x.sfz:9: unsupported opcode 'rt_decay'",
                                                     T + "/A.sfz:4: unsupported value '1' for opcode 'ampeg_dynamic'"])
        b = line("skipped", "damaged", [T + "/B.sfz:2: path '..\\S\\a.wav' leaves the SFZ directory (x)", T + "/B.sfz:3: " + T + "/S/a.flac: unsupported audio: not a WAV file"], file=T + "/B.sfz")
        c = line("skipped", "damaged", [T + "/includes/c.sfz: no regions in SFZ file"], file=T + "/includes/c.sfz")
        s = cl.summarize_reasons([("A.sfz", cl.classify_file_report(a, T)), ("B.sfz", cl.classify_file_report(b, T)), ("includes/c.sfz", cl.classify_file_report(c, T))])
        self.assertEqual(s["files_failed"], 3)
        rows = {(r["category"], r["detail"]): r for r in s["rows"]}
        r = rows[("unsupported_opcode", "rt_decay")]
        self.assertEqual((r["count"], r["files"], r["example"]), (2, 1, "A.sfz:3"))
        self.assertEqual(rows[("unsupported_value", "ampeg_dynamic")]["example"], "A.sfz:4")
        self.assertEqual(rows[("sample_format_not_supported", ".flac")]["example"], "B.sfz:3")
        self.assertEqual(rows[("include_fragment", "no <region>")]["example"], "includes/c.sfz")
        order = [r["category"] for r in s["rows"]]
        self.assertEqual(order, sorted(order, key=cl.REASON_ORDER.index))

    def test_fallback_without_messages(self):
        o = line("failed", "damaged", [], detail="io error")
        e = cl.classify_file_report(o, T)
        self.assertEqual(e[0]["category"], "other")
        o = line("skipped", "unsupported_features", [], offenders=[{"opcode": "x", "file": T + "/A.sfz", "line": 2, "message": "unsupported opcode 'x'"}])
        self.assertEqual(cl.classify_file_report(o, T)[0]["detail"], "x")

    def test_markdown(self):
        o = line("skipped", "unsupported_features", [T + "/A.sfz:3: unsupported opcode 'rt_decay'"])
        ir = cl.summarize_reasons([("A.sfz", cl.classify_file_report(o, T))])
        md = "\n".join(corpus.render_reasons(ir))
        self.assertIn("| unsupported opcode | `rt_decay` | 1 | 1 | `A.sfz:3` |", md)
        self.assertEqual(corpus.render_reasons(None), [])
        self.assertEqual(corpus.render_reasons({"files_failed": 0, "rows": []}), [])


if __name__ == "__main__":
    unittest.main()
