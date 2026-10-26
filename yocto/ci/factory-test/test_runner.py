#!/usr/bin/env python3
"""Story 6.13: tests of the factory test runner (meta-velora/recipes-core/velora-factory-test/files/velora-factory-test) with simulated hardware.
Usage: test_runner.py <path to the runner> . Runs in Docker (stdlib only)."""
import importlib.machinery
import importlib.util
import io
import json
import os
import re
import sys
import tempfile
import time
import unittest

RUNNER = sys.argv.pop(1)
loader = importlib.machinery.SourceFileLoader("vft", RUNNER)
spec = importlib.util.spec_from_loader("vft", loader)
vft = importlib.util.module_from_spec(spec)
loader.exec_module(vft)

NAMES = [t[0] for t in vft.TESTS]
GOOD = {
    "mcu_links": {"links": {"velora-mcu-keybed": True, "velora-mcu-panel": True}},
    "keybed_scan": {"keys_expected": 88, "keys_seen": 88, "stuck": []},
    "panel_status": {"controls_expected": 40, "controls_responding": 40, "leds_ok": 64, "leds_total": 64},
    "codec_loopback": {"snr_db": 98.0, "level_error_db": 0.1},
    "display_touch": {"display_connected": True, "touch_devices": 1},
    "nvme_speed": {"write_mb_s": 900.0, "read_mb_s": 2000.0, "corrupt_chunks": 0},
}
BAD = {
    "mcu_links": {"links": {"velora-mcu-keybed": True, "velora-mcu-panel": False}},
    "keybed_scan": {"keys_expected": 88, "keys_seen": 86, "stuck": [12]},
    "panel_status": {"controls_expected": 40, "controls_responding": 39, "leds_ok": 64, "leds_total": 64},
    "codec_loopback": {"snr_db": 20.0, "level_error_db": 0.1},
    "display_touch": {"display_connected": True, "touch_devices": 0},
    "nvme_speed": {"write_mb_s": 10.0, "read_mb_s": 2000.0, "corrupt_chunks": 0},
}


class Mock(vft.Hardware):
    """values: method -> dict | None | Exception instance (raised) | callable (called)."""

    def __init__(self, values=None, ids=None):
        self.values = dict(GOOD)
        self.values.update(values or {})
        self.ids = ids if ids is not None else {"serial": "SN-0001", "machine_id": "abc"}
        self.calls = []

    def _get(self, m):
        self.calls.append(m)
        v = self.values[m]
        if isinstance(v, BaseException):
            raise v
        if callable(v):
            return v()
        return v

    def unit_identifiers(self):
        if isinstance(self.ids, BaseException):
            raise self.ids
        return self.ids


for _m in ("mcu_links", "keybed_scan", "panel_status", "codec_loopback", "display_touch", "nvme_speed"):
    setattr(Mock, _m, (lambda m: lambda self: self._get(m))(_m))


def by_name(report):
    return {t["name"]: t for t in report["tests"]}


def validate_schema(tc, r):
    tc.assertEqual(r["schema"], "velora-factory-test-report/2")
    tc.assertEqual(sorted(r), ["boot_id", "schema", "summary", "tests", "timestamp", "unit", "variant", "version"])
    for k, typ in (("variant", str), ("version", str), ("timestamp", str), ("boot_id", str), ("unit", dict), ("tests", list), ("summary", dict)):
        tc.assertIsInstance(r[k], typ, k)
    tc.assertRegex(r["timestamp"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")
    tc.assertEqual([t["name"] for t in r["tests"]], NAMES)
    for t in r["tests"]:
        tc.assertEqual(sorted(t), ["detail", "duration_ms", "name", "status"])
        tc.assertIn(t["status"], ("pass", "fail", "not present"))
        tc.assertIsInstance(t["detail"], str)
        tc.assertTrue(t["detail"])
        tc.assertIsInstance(t["duration_ms"], int)
        tc.assertGreaterEqual(t["duration_ms"], 0)
    s = r["summary"]
    tc.assertEqual(sorted(s), ["fail", "not_present", "overall", "pass", "total"])
    tc.assertEqual(s["total"], len(r["tests"]))
    for key, st in (("pass", "pass"), ("fail", "fail"), ("not_present", "not present")):
        tc.assertEqual(s[key], sum(1 for t in r["tests"] if t["status"] == st), key)
    tc.assertEqual(s["pass"] + s["fail"] + s["not_present"], s["total"])
    tc.assertIn(s["overall"], ("pass", "fail", "incomplete"))
    json.loads(json.dumps(r))


class RunnerTests(unittest.TestCase):
    def run_hw(self, values=None, **kw):
        hw = Mock(values)
        r = vft.run_all(hw, timeout=kw.pop("timeout", 2.0), variant="factory-test", version="0.1.0", **kw)
        validate_schema(self, r)
        return r, hw

    def test_all_present_and_good(self):
        r, hw = self.run_hw()
        self.assertEqual([t["status"] for t in r["tests"]], ["pass"] * 6)
        self.assertEqual(r["summary"], {"pass": 6, "fail": 0, "not_present": 0, "total": 6, "overall": "pass"})
        self.assertEqual(r["unit"], {"serial": "SN-0001", "machine_id": "abc"})
        self.assertEqual((r["variant"], r["version"]), ("factory-test", "0.1.0"))

    def test_each_bad_value_fails_only_its_test(self):
        for method, bad in BAD.items():
            with self.subTest(method):
                r, _ = self.run_hw({method: bad})
                idx = [t[1] for t in vft.TESTS].index(method)
                for i, t in enumerate(r["tests"]):
                    self.assertEqual(t["status"], "fail" if i == idx else "pass", (method, t))
                self.assertEqual(r["summary"]["overall"], "fail")
                self.assertTrue(r["tests"][idx]["detail"])

    def test_specific_failure_details(self):
        r, _ = self.run_hw({"keybed_scan": BAD["keybed_scan"], "mcu_links": BAD["mcu_links"], "display_touch": BAD["display_touch"]})
        b = by_name(r)
        self.assertIn("86 of 88", b["Keybed scan"]["detail"])
        self.assertIn("stuck: 12", b["Keybed scan"]["detail"])
        self.assertIn("velora-mcu-panel", b["MCU links"]["detail"])
        self.assertIn("no touch", b["Display and touch"]["detail"])

    def test_probe_expectation_is_not_trusted(self):
        # the runner compares with its own constants: a probe that expects nothing (0) or the wrong count must not produce a pass
        cases = {
            "keybed_scan": [{"keys_expected": 0, "keys_seen": 0, "stuck": []}, {"keys_expected": 87, "keys_seen": 87, "stuck": []}],
            "panel_status": [{"controls_expected": 0, "controls_responding": 0, "leds_ok": 64, "leds_total": 64},
                             {"controls_expected": 40, "controls_responding": 40, "leds_ok": 0, "leds_total": 0},
                             {"controls_expected": 39, "controls_responding": 39, "leds_ok": 64, "leds_total": 64}],
        }
        for method, vals in cases.items():
            for v in vals:
                with self.subTest(method=method, v=v):
                    r, _ = self.run_hw({method: v})
                    idx = [t[1] for t in vft.TESTS].index(method)
                    self.assertEqual(r["tests"][idx]["status"], "fail", v)
                    self.assertIn("probe expects", r["tests"][idx]["detail"])

    def test_single_missing_or_empty_links_fail(self):
        for links, down in (({"velora-mcu-keybed": True}, ["velora-mcu-panel"]), ({"velora-mcu-panel": True}, ["velora-mcu-keybed"]),
                            ({"velora-mcu-keybed": True, "velora-mcu-panel": False}, ["velora-mcu-panel"]),
                            ({}, ["velora-mcu-keybed", "velora-mcu-panel"])):
            with self.subTest(links=links):
                r, _ = self.run_hw({"mcu_links": {"links": links}})
                c = by_name(r)["MCU links"]
                self.assertEqual(c["status"], "fail")
                for n in down:
                    self.assertIn(n, c["detail"])
                self.assertIn("presence only, no protocol", c["detail"])
        r, _ = self.run_hw()
        self.assertIn("presence only, no protocol", by_name(r)["MCU links"]["detail"])

    def test_nvme_corruption_in_probe_data_fails(self):
        r, _ = self.run_hw({"nvme_speed": {"write_mb_s": 900.0, "read_mb_s": 2000.0, "corrupt_chunks": 2}})
        c = by_name(r)["NVMe read/write speed"]
        self.assertEqual(c["status"], "fail")
        self.assertIn("data corruption", c["detail"])

    def test_missing_hardware_is_not_present_and_runner_continues(self):
        for method in [t[1] for t in vft.TESTS]:
            with self.subTest(method):
                r, hw = self.run_hw({method: None})
                idx = [t[1] for t in vft.TESTS].index(method)
                self.assertEqual(r["tests"][idx]["status"], "not present")
                self.assertEqual(sum(1 for t in r["tests"] if t["status"] == "pass"), 5)
                self.assertEqual(r["summary"]["overall"], "incomplete")
                self.assertEqual(len(hw.calls), 6)

    def test_everything_absent(self):
        r, _ = self.run_hw({m: None for m in GOOD})
        self.assertEqual(r["summary"], {"pass": 0, "fail": 0, "not_present": 6, "total": 6, "overall": "incomplete"})

    def test_fail_outranks_not_present(self):
        r, _ = self.run_hw({"codec_loopback": None, "keybed_scan": BAD["keybed_scan"]})
        self.assertEqual(r["summary"]["overall"], "fail")

    def test_probe_crash_is_a_failed_test_and_the_rest_run(self):
        r, hw = self.run_hw({"panel_status": RuntimeError("bus error")})
        b = by_name(r)
        self.assertEqual(b["Panel controls and LEDs"]["status"], "fail")
        self.assertIn("crash: RuntimeError: bus error", b["Panel controls and LEDs"]["detail"])
        self.assertEqual(sum(1 for t in r["tests"] if t["status"] == "pass"), 5)
        self.assertEqual(len(hw.calls), 6)

    def test_probe_raising_systemexit_and_keyboardinterrupt_is_contained(self):
        r, _ = self.run_hw({"mcu_links": SystemExit(3), "keybed_scan": KeyboardInterrupt()})
        b = by_name(r)
        self.assertEqual(b["MCU links"]["status"], "fail")
        self.assertEqual(b["Keybed scan"]["status"], "fail")
        self.assertEqual(sum(1 for t in r["tests"] if t["status"] == "pass"), 4)

    def test_probe_hang_times_out_and_the_rest_run(self):
        t0 = time.monotonic()
        r, hw = self.run_hw({"codec_loopback": lambda: time.sleep(30)}, timeout=0.3)
        self.assertLess(time.monotonic() - t0, 5.0)
        c = by_name(r)["Codec loopback"]
        self.assertEqual(c["status"], "fail")
        self.assertIn("timeout after 0.3s", c["detail"])
        self.assertGreaterEqual(c["duration_ms"], 250)
        self.assertEqual(sum(1 for t in r["tests"] if t["status"] == "pass"), 5)

    def test_malformed_probe_data_is_a_failed_test(self):
        r, _ = self.run_hw({"keybed_scan": {"keys_seen": 88}, "nvme_speed": "fast"})
        b = by_name(r)
        self.assertEqual(b["Keybed scan"]["status"], "fail")
        self.assertIn("bad probe data", b["Keybed scan"]["detail"])
        self.assertEqual(b["NVMe read/write speed"]["status"], "fail")

    def test_unit_identifiers_failure_does_not_break_the_report(self):
        hw = Mock()
        hw.ids = RuntimeError("no eeprom")
        r = vft.run_all(hw, timeout=1)
        validate_schema(self, r)
        self.assertEqual(r["unit"], {})
        self.assertEqual(r["variant"], "unknown")

    def test_base_hardware_class_runs_without_crashing(self):
        r = vft.run_all(vft.Hardware(), timeout=1)
        validate_schema(self, r)
        self.assertTrue(all(t["status"] == "fail" and "NotImplementedError" in t["detail"] for t in r["tests"]))

    def test_summary_text_lists_every_subsystem(self):
        r, _ = self.run_hw({"keybed_scan": BAD["keybed_scan"], "codec_loopback": None})
        text = vft.summary_text(r)
        for n in NAMES:
            self.assertIn(n, text)
        self.assertRegex(text, r"\[FAIL\] Keybed scan")
        self.assertRegex(text, r"\[N/P \] Codec loopback")
        self.assertRegex(text, r"\[PASS\] MCU links")
        self.assertIn("4 pass, 1 fail, 1 not present of 6: FAIL", text)
        self.assertIn("variant factory-test", text)
        self.assertIn("serial=SN-0001", text)
        self.assertTrue(text.endswith("\n"))

    def main_args(self, d, **kw):
        a = ["--report", os.path.join(d, "o", "r.json"), "--summary", os.path.join(d, "o", "s.txt"), "--fallback-dir", os.path.join(d, "fb"),
             "--os-release", os.path.join(d, "os-release"), "--boot-id-file", os.path.join(d, "boot_id"), "--timeout", "2"]
        return a

    def mk_env(self, d):
        with open(os.path.join(d, "os-release"), "w") as f:
            f.write('ID=velora\nVERSION_ID=0.1.0-dev\nVARIANT_ID=factory-test\n')
        with open(os.path.join(d, "boot_id"), "w") as f:
            f.write("11111111-2222-3333-4444-555555555555\n")

    def test_main_writes_report_and_summary_and_exit_codes(self):
        with tempfile.TemporaryDirectory() as d:
            self.mk_env(d)
            for values, rc in (({}, 0), ({"keybed_scan": BAD["keybed_scan"]}, 1), ({"nvme_speed": None}, 2)):
                out = io.StringIO()
                got = vft.main(self.main_args(d), hw=Mock(values), out=out)
                self.assertEqual(got, rc, values)
                with open(os.path.join(d, "o", "r.json")) as f:
                    r = json.load(f)
                validate_schema(self, r)
                self.assertEqual((r["variant"], r["version"], r["boot_id"]), ("factory-test", "0.1.0-dev", "11111111-2222-3333-4444-555555555555"))
                with open(os.path.join(d, "o", "s.txt")) as f:
                    self.assertEqual(f.read(), out.getvalue())
                self.assertEqual(sorted(os.listdir(os.path.join(d, "o"))), ["r.json", "s.txt"], "no temp files left behind")
                self.assertFalse(os.path.exists(os.path.join(d, "fb")), "no fallback copy when the primary location works")

    def test_files_are_world_readable_0644(self):
        with tempfile.TemporaryDirectory() as d:
            self.mk_env(d)
            vft.main(self.main_args(d), hw=Mock(), out=io.StringIO())
            for n in ("r.json", "s.txt"):
                self.assertEqual(os.stat(os.path.join(d, "o", n)).st_mode & 0o777, 0o644, n)

    def test_boot_id_unknown_when_unreadable(self):
        with tempfile.TemporaryDirectory() as d:
            self.mk_env(d)
            os.unlink(os.path.join(d, "boot_id"))
            vft.main(self.main_args(d), hw=Mock(), out=io.StringIO())
            with open(os.path.join(d, "o", "r.json")) as f:
                self.assertEqual(json.load(f)["boot_id"], "unknown")

    def test_stale_report_and_summary_are_removed_before_the_run(self):
        with tempfile.TemporaryDirectory() as d:
            self.mk_env(d)
            paths = [os.path.join(d, "o", "r.json"), os.path.join(d, "o", "s.txt"), os.path.join(d, "fb", "report.json"), os.path.join(d, "fb", "summary.txt")]
            for p in paths:
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "w") as f:
                    f.write("STALE")
            seen = {}

            def probe():
                seen["stale"] = [os.path.exists(p) for p in paths]
                return GOOD["mcu_links"]

            vft.main(self.main_args(d), hw=Mock({"mcu_links": probe}), out=io.StringIO())
            self.assertEqual(seen["stale"], [False] * 4, "stale files must be gone while the tests run")
            with open(paths[0]) as f:
                self.assertNotIn("STALE", f.read())
            # a run that cannot write anything (both locations unwritable) leaves no stale report behind either
            blocker = os.path.join(d, "blocker")
            open(blocker, "w").close()
            for p in paths:
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "w") as f:
                    f.write("STALE")
            rc = vft.main(["--report", paths[0], "--summary", paths[1], "--fallback-dir", paths[2].rsplit("/", 1)[0], "--os-release", os.path.join(d, "x"),
                           "--boot-id-file", os.path.join(d, "x")], hw=Mock({"mcu_links": lambda: (_ for _ in ()).throw(RuntimeError("x"))}), out=io.StringIO())
            self.assertIn(rc, (1, 3))
            for p in paths:
                if os.path.exists(p):
                    with open(p) as f:
                        self.assertNotIn("STALE", f.read(), p)

    def test_unwritable_primary_falls_back_and_summary_is_independent(self):
        with tempfile.TemporaryDirectory() as d:
            self.mk_env(d)
            blocker = os.path.join(d, "file")
            open(blocker, "w").close()
            # report location unwritable, summary fine: the summary is still written, the report goes to the fallback directory
            out = io.StringIO()
            args = self.main_args(d)
            args[args.index("--report") + 1] = os.path.join(blocker, "r.json")
            rc = vft.main(args, hw=Mock(), out=out)
            self.assertEqual(rc, 3)
            self.assertTrue(os.path.exists(os.path.join(d, "o", "s.txt")))
            self.assertFalse(os.path.exists(os.path.join(d, "o", "r.json")))
            with open(os.path.join(d, "fb", "report.json")) as f:
                validate_schema(self, json.load(f))
            self.assertFalse(os.path.exists(os.path.join(d, "fb", "summary.txt")))
            self.assertIn("cannot write the report", out.getvalue())
            self.assertIn("MCU links", out.getvalue())
            # summary location unwritable, report fine
            args = self.main_args(d)
            args[args.index("--summary") + 1] = os.path.join(blocker, "s.txt")
            rc = vft.main(args, hw=Mock(), out=io.StringIO())
            self.assertEqual(rc, 3)
            self.assertTrue(os.path.exists(os.path.join(d, "o", "r.json")))
            self.assertTrue(os.path.exists(os.path.join(d, "fb", "summary.txt")))

    def test_both_locations_unwritable_still_prints_summary(self):
        with tempfile.TemporaryDirectory() as d:
            self.mk_env(d)
            blocker = os.path.join(d, "file")
            open(blocker, "w").close()
            out = io.StringIO()
            rc = vft.main(["--report", os.path.join(blocker, "r.json"), "--summary", "", "--fallback-dir", os.path.join(blocker, "fb"),
                           "--os-release", os.path.join(d, "none")], hw=Mock(), out=out)
            self.assertEqual(rc, 3)
            self.assertIn("MCU links", out.getvalue())
            self.assertIn("cannot write the report", out.getvalue())
            self.assertIn("either", out.getvalue())


class StepClock:
    """Deterministic clock: every call advances by step seconds."""

    def __init__(self, step):
        self.t, self.step = 0.0, step

    def __call__(self):
        self.t += self.step
        return self.t


class SysfsHardwareTests(unittest.TestCase):
    """The real probe layer over a fake root: absent hardware reports None, present hardware reports measurements, nothing crashes the runner."""

    def mk(self, root, rel, text=""):
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write(text)

    def hw(self, d, step=0.1, cls=None):
        return (cls or vft.SysfsHardware)(root=d, data_dir=os.path.join(d, "data"), clock=StepClock(step))

    def test_empty_system_everything_not_present(self):
        with tempfile.TemporaryDirectory() as d:
            r = vft.run_all(self.hw(d), timeout=2)
            validate_schema(self, r)
            self.assertEqual([t["status"] for t in r["tests"]], ["not present"] * 6)
            self.assertEqual(r["summary"]["overall"], "incomplete")

    def test_present_subsystems(self):
        with tempfile.TemporaryDirectory() as d:
            self.mk(d, "dev/velora-mcu-keybed")
            self.mk(d, "dev/velora-mcu-panel")
            self.mk(d, "sys/class/drm/card0/uevent")
            self.mk(d, "sys/class/drm/card0-HDMI-A-1/status", "connected\n")
            self.mk(d, "sys/class/input/input3/name", "ft5x06 Touchscreen\n")
            self.mk(d, "sys/class/nvme/nvme0/model", "x")
            self.mk(d, "proc/device-tree/serial-number", "RK-1234\x00")
            self.mk(d, "etc/machine-id", "deadbeef\n")
            r = vft.run_all(self.hw(d), timeout=20)
            validate_schema(self, r)
            b = by_name(r)
            self.assertEqual(b["MCU links"]["status"], "pass")
            self.assertEqual(b["Display and touch"]["status"], "pass")
            self.assertIn("presence only", b["Display and touch"]["detail"])
            self.assertEqual(b["NVMe read/write speed"]["status"], "pass")
            self.assertIn("0 corrupt chunk(s)", b["NVMe read/write speed"]["detail"])
            self.assertEqual(os.listdir(os.path.join(d, "data")), [], "speed-test file removed")
            for n in ("Keybed scan", "Panel controls and LEDs"):
                self.assertEqual(b[n]["status"], "fail")
                self.assertIn("NotImplementedError", b[n]["detail"])
            self.assertEqual(b["Codec loopback"]["status"], "not present")
            self.assertEqual(r["unit"], {"serial": "RK-1234", "machine_id": "deadbeef"})

    def test_single_missing_mcu_node_is_a_down_link(self):
        with tempfile.TemporaryDirectory() as d:
            self.mk(d, "dev/velora-mcu-keybed")
            r = vft.run_all(self.hw(d), timeout=2)
            c = by_name(r)["MCU links"]
            self.assertEqual(c["status"], "fail")
            self.assertIn("velora-mcu-panel", c["detail"])

    def test_display_cases(self):
        with tempfile.TemporaryDirectory() as d:  # DRM card, connected, no touch
            self.mk(d, "sys/class/drm/card0/uevent")
            self.mk(d, "sys/class/drm/card0-DSI-1/status", "connected\n")
            c = by_name(vft.run_all(self.hw(d), timeout=2))["Display and touch"]
            self.assertEqual(c["status"], "fail")
            self.assertIn("no touch", c["detail"])
        with tempfile.TemporaryDirectory() as d:  # DRM card, nothing connected: the unit should show something and does not
            self.mk(d, "sys/class/drm/card0/uevent")
            self.mk(d, "sys/class/drm/card0-DSI-1/status", "disconnected\n")
            c = by_name(vft.run_all(self.hw(d), timeout=2))["Display and touch"]
            self.assertEqual(c["status"], "fail")
            self.assertIn("no display connected", c["detail"])
        with tempfile.TemporaryDirectory() as d:  # a connector directory without any card is not a display subsystem
            self.mk(d, "sys/class/drm/card0-DSI-1/status", "connected\n")
            self.assertEqual(by_name(vft.run_all(self.hw(d), timeout=2))["Display and touch"]["status"], "not present")

    def nvme_root(self, d):
        self.mk(d, "sys/class/nvme/nvme0/model", "x")

    def test_nvme_deterministic_speed_pass_and_fail(self):
        with tempfile.TemporaryDirectory() as d:
            self.nvme_root(d)
            fast = vft.SysfsHardware(root=d, data_dir=os.path.join(d, "data"), clock=StepClock(0.1)).nvme_speed()
            self.assertEqual(fast, {"write_mb_s": round(64 * 1.048576 / 0.1, 1), "read_mb_s": round(64 * 1.048576 / 0.1, 1), "corrupt_chunks": 0})
            self.assertEqual(vft._eval_nvme(fast)[0], "pass")
            slow = vft.SysfsHardware(root=d, data_dir=os.path.join(d, "data"), clock=StepClock(1.0)).nvme_speed()
            self.assertEqual(slow["write_mb_s"], round(64 * 1.048576, 1))
            self.assertEqual(vft._eval_nvme(slow)[0], "fail")

    def test_nvme_corruption_is_detected_per_chunk(self):
        class Corrupting(vft.SysfsHardware):
            def _read_exact(self, fd, n):
                data = super()._read_exact(fd, n)
                self.calls = getattr(self, "calls", 0) + 1
                if self.calls in (5, 40):  # two chunks come back wrong
                    return bytes([data[0] ^ 1]) + data[1:]
                return data

        class Swapping(vft.SysfsHardware):
            def _read_exact(self, fd, n):  # stale/shifted data: every chunk is the first chunk's pattern
                super()._read_exact(fd, n)
                return self._chunk(0)

        with tempfile.TemporaryDirectory() as d:
            self.nvme_root(d)
            res = Corrupting(root=d, data_dir=os.path.join(d, "data"), clock=StepClock(0.1)).nvme_speed()
            self.assertEqual(res["corrupt_chunks"], 2)
            status, detail = vft._eval_nvme(res)
            self.assertEqual(status, "fail")
            self.assertIn("data corruption", detail)
            res = Swapping(root=d, data_dir=os.path.join(d, "data"), clock=StepClock(0.1)).nvme_speed()
            self.assertEqual(res["corrupt_chunks"], 63)
            r = vft.run_all(Corrupting(root=d, data_dir=os.path.join(d, "data"), clock=StepClock(0.1)), timeout=20)
            self.assertEqual(by_name(r)["NVMe read/write speed"]["status"], "fail")
            self.assertEqual(os.listdir(os.path.join(d, "data")), [], "test file removed even on corruption")


if __name__ == "__main__":
    unittest.main(verbosity=1)
