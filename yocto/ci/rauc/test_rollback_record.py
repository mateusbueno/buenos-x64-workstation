#!/usr/bin/env python3
"""Story 6.12: tests of velora-rollback-record. Usage: test_rollback_record.py <script> <status-dir>
<status-dir> holds real `rauc status --detailed --output-format=json-pretty` output made by test-bundles.sh:
rolledback.json (slot rootfs.1 = 1.1.0 newest, but rootfs.0 = 1.0.0 booted) and updated.json (rootfs.1 booted)."""
import copy
import importlib.machinery
import importlib.util
import io
import contextlib
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest

SCRIPT, STATUS = sys.argv[1], sys.argv[2]
sys.argv = sys.argv[:1]
loader = importlib.machinery.SourceFileLoader("rollback_record", SCRIPT)
spec = importlib.util.spec_from_loader("rollback_record", loader)
rr = importlib.util.module_from_spec(spec)
loader.exec_module(rr)


def load(name):
    with open(os.path.join(STATUS, name + ".json")) as f:
        return json.load(f)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.out = os.path.join(self.tmp.name, "rollback.json")

    def write_status(self, obj, name="status.json"):
        p = os.path.join(self.tmp.name, name)
        with open(p, "w") as f:
            json.dump(obj, f)
        return p

    def run_main(self, status_path, now="2026-10-25T12:00:00Z"):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            rc = rr.main(["--status-file", status_path, "--out", self.out, "--now", now])
        return rc, buf.getvalue()

    def record(self):
        with open(self.out) as f:
            return json.load(f)

    def leftovers(self):
        return [n for n in os.listdir(self.tmp.name) if n.startswith(".rollback.")]


class Detect(Base):
    def test_rollback_is_recorded_from_real_rauc_json(self):
        rc, _ = self.run_main(os.path.join(STATUS, "rolledback.json"))
        self.assertEqual(rc, 0)
        r = self.record()
        self.assertEqual((r["from_version"], r["to_version"]), ("1.1.0", "1.0.0"))
        self.assertEqual((r["failed_slot"], r["booted_slot"]), ("rootfs.1", "rootfs.0"))
        self.assertEqual(r["reason"], "boot-failed")  # RAUC reports boot_status bad for the failed slot
        self.assertEqual(r["timestamp"], "2026-10-25T12:00:00Z")
        self.assertEqual(r["schema"], 1)
        self.assertEqual(self.leftovers(), [])
        self.assertEqual(stat.S_IMODE(os.stat(self.out).st_mode), 0o644)

    def test_no_rollback_when_booted_slot_is_the_attempted_one(self):
        rc, out = self.run_main(os.path.join(STATUS, "updated.json"))
        self.assertEqual(rc, 0)
        self.assertFalse(os.path.exists(self.out))
        self.assertIn("no rollback", out)

    def test_reason_slot_mismatch_when_attempted_slot_not_bad(self):
        s = load("rolledback")
        for e in s["slots"]:
            for slot in e.values():
                slot["boot_status"] = "good"
        rc, _ = self.run_main(self.write_status(s))
        self.assertEqual(rc, 0)
        self.assertEqual(self.record()["reason"], "slot-mismatch")

    def test_idempotent_keeps_original_timestamp(self):
        p = os.path.join(STATUS, "rolledback.json")
        self.run_main(p, now="2026-10-25T12:00:00Z")
        rc, out = self.run_main(p, now="2026-10-26T08:00:00Z")
        self.assertEqual(rc, 0)
        self.assertIn("unchanged", out)
        self.assertEqual(self.record()["timestamp"], "2026-10-25T12:00:00Z")

    def test_new_rollback_replaces_the_old_record(self):
        self.run_main(os.path.join(STATUS, "rolledback.json"), now="2026-10-25T12:00:00Z")
        s = load("rolledback")
        for e in s["slots"]:
            for name, slot in e.items():
                if name == "rootfs.1":
                    slot["slot_status"]["bundle"]["version"] = "1.2.0"
                    slot["slot_status"]["installed"]["timestamp"] = "2026-11-01T10:00:00Z"
        rc, _ = self.run_main(self.write_status(s), now="2026-11-01T12:00:00Z")
        self.assertEqual(rc, 0)
        r = self.record()
        self.assertEqual((r["from_version"], r["to_version"], r["timestamp"]), ("1.2.0", "1.0.0", "2026-11-01T12:00:00Z"))

    def test_a_stale_record_is_not_removed_when_nothing_rolled_back(self):
        self.run_main(os.path.join(STATUS, "rolledback.json"))
        before = open(self.out).read()
        self.run_main(os.path.join(STATUS, "updated.json"), now="2026-12-01T00:00:00Z")
        self.assertEqual(open(self.out).read(), before)


class BadInput(Base):
    def test_no_booted_slot(self):
        s = load("rolledback")
        for e in s["slots"]:
            for slot in e.values():
                slot["state"] = "inactive"
        rc, out = self.run_main(self.write_status(s))
        self.assertEqual(rc, 2)
        self.assertFalse(os.path.exists(self.out))

    def test_not_json(self):
        p = os.path.join(self.tmp.name, "bad.json")
        open(p, "w").write("not json")
        rc, _ = self.run_main(p)
        self.assertEqual(rc, 2)
        self.assertFalse(os.path.exists(self.out))

    def test_wic_flashed_slot_a_rolled_back_records_unknown_to_version(self):
        # real rauc JSON: slot A (booted) has no slot_status at all, slot B was installed with 1.1.0 and failed
        s = load("flashed-rolledback")
        booted = [slot for e in s["slots"] for slot in e.values() if slot["state"] == "booted"][0]
        self.assertNotIn("version", (booted.get("slot_status") or {}).get("bundle") or {})  # real rauc: slot_status holds only a null compatible
        rc, _ = self.run_main(os.path.join(STATUS, "flashed-rolledback.json"))
        self.assertEqual(rc, 0)
        r = self.record()
        self.assertEqual((r["from_version"], r["to_version"]), ("1.1.0", None))
        self.assertEqual((r["failed_slot"], r["booted_slot"], r["reason"]), ("rootfs.1", "rootfs.0", "boot-failed"))

    def test_install_less_boot_is_not_an_error(self):
        # real rauc JSON of a never-updated board: no slot has a record
        s = load("flashed-fresh")
        self.assertTrue(all("installed" not in (slot.get("slot_status") or {}) for e in s["slots"] for slot in e.values()))
        rc, out = self.run_main(os.path.join(STATUS, "flashed-fresh.json"))
        self.assertEqual(rc, 0)
        self.assertFalse(os.path.exists(self.out))


class AtomicWrite(Base):
    def seed_previous(self):
        prev = {"schema": 1, "from_version": "0.9.0", "to_version": "0.8.0", "failed_slot": "rootfs.1", "booted_slot": "rootfs.0",
                "failed_installed_at": "2026-01-01T00:00:00Z", "reason": "boot-failed", "timestamp": "2026-01-01T00:00:00Z"}
        with open(self.out, "w") as f:
            json.dump(prev, f)
        return open(self.out).read()

    def test_fsync_failure_leaves_the_previous_file_and_no_temp_file(self):
        before = self.seed_previous()
        real = os.fsync
        def boom(fd):
            raise OSError(5, "injected fsync failure")
        os.fsync = boom
        try:
            rc, out = self.run_main(os.path.join(STATUS, "rolledback.json"))
        finally:
            os.fsync = real
        self.assertEqual(rc, 1)
        self.assertIn("cannot write", out)
        self.assertEqual(open(self.out).read(), before)
        self.assertEqual(self.leftovers(), [])

    def test_rename_failure_leaves_the_previous_file_and_no_temp_file(self):
        before = self.seed_previous()
        real = os.replace
        def boom(a, b):
            raise OSError(28, "injected rename failure")
        os.replace = boom
        try:
            rc, _ = self.run_main(os.path.join(STATUS, "rolledback.json"))
        finally:
            os.replace = real
        self.assertEqual(rc, 1)
        self.assertEqual(open(self.out).read(), before)
        self.assertEqual(self.leftovers(), [])

    @unittest.skipIf(os.geteuid() == 0, "root ignores directory permissions")
    def test_unwritable_directory_leaves_the_previous_file(self):
        before = self.seed_previous()
        os.chmod(self.tmp.name, 0o555)
        try:
            rc, _ = self.run_main(os.path.join(STATUS, "rolledback.json"))
        finally:
            os.chmod(self.tmp.name, 0o755)
        self.assertEqual(rc, 1)
        self.assertEqual(open(self.out).read(), before)

    def test_temp_file_is_written_in_the_target_directory_then_renamed(self):
        seen = {}
        real = os.replace
        def spy(a, b):
            seen["src"], seen["dst"] = a, b
            seen["src_content"] = open(a).read()
            real(a, b)
        os.replace = spy
        try:
            self.run_main(os.path.join(STATUS, "rolledback.json"))
        finally:
            os.replace = real
        self.assertEqual(os.path.dirname(seen["src"]), os.path.dirname(self.out))
        self.assertEqual(seen["dst"], self.out)
        self.assertEqual(json.loads(seen["src_content"]), self.record())  # complete before the rename

    def test_cli_reads_stdin(self):
        with open(os.path.join(STATUS, "rolledback.json")) as f:
            p = subprocess.run([sys.executable, "-I", SCRIPT, "--status-file", "-", "--out", self.out, "--now", "2026-10-25T12:00:00Z"],
                               stdin=f, capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.record()["to_version"], "1.0.0")


if __name__ == "__main__":
    r = unittest.main(exit=False, verbosity=1, argv=["x"])
    sys.exit(0 if r.result.wasSuccessful() else 1)
