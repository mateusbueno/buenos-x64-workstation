#!/usr/bin/env python3
"""Power-cut rig orchestrator + verifier (Story 8.13; also usable for 3.18 / 8.11 cut loops). Stdlib only.

NOT hardware-tested. Only --self-test and --dry-run have been executed (simulated unit, no relay).

Subcommands
  run      orchestrate N cuts, append one JSON line per cycle to --out (JSONL), then print the summary
  summary  re-compute the summary from an existing JSONL
  --self-test                run built-in checks (no hardware)
  run --dry-run              stub relay + simulated unit, zero real waiting; output is NOT release evidence

Cycle: start_cmd -> random delay U(0, --window-s) -> relay OFF -> hold --off-s (5 s per Story 8.13) -> relay ON
       -> wait for unit (--check ping:HOST | serial:DEV:REGEX | cmd:SHELL) up to --boot-timeout-s
       -> optional --phase-cmd read BEFORE the cut (phase hit) -> --verify-cmd (exit 0 = integrity OK;
       last stdout line may be JSON, e.g. {"firmware":"new","slots_consistent":true,"libs_ok":true}).
Update campaign extras: --prev-fw/--new-fw: JSON "firmware" must be one of them and "slots_consistent" must not be false.
Requirement (epics.md:5237, 5240, 5245): >=100 cycles per campaign, ZERO failures; any single failure fails the gate.

Example relay drivers (documented only, not hardware-tested): see the comment block above class CommandRelay.
"""
import argparse, json, os, random, re, subprocess, sys, tempfile, time

REQUIRED_CUTS = 100   # epics.md:5237 (update), 5240 (import) "100 times"; thresholds.json cuts_required (20) is Story 11.9 only
DEFAULT_OFF_S = 5.0   # epics.md:5236 "cuts power for 5 s"

# ---------------------------------------------------------------- relay drivers
class RelayDriver:
    name = "base"
    def power_off(self): raise NotImplementedError
    def power_on(self): raise NotImplementedError
    def sleep(self, s): time.sleep(s)

class DryRunRelay(RelayDriver):
    """Stub: records calls, never sleeps, controls nothing."""
    name = "dry-run"
    def __init__(self): self.calls = []
    def power_off(self): self.calls.append("off")
    def power_on(self): self.calls.append("on")
    def sleep(self, s): pass

# Example drivers (NOT hardware-tested; adapt and bench-verify before use). Wire the relay so that
# "off" = DC input to the unit open (use NO/NC contacts so a rig crash leaves the unit powered).
#
# 1) USB serial relay (e.g. a 'ch340' style board speaking ASCII/hex), using pyserial (not stdlib):
#     import serial
#     class SerialRelay(RelayDriver):
#         name = "serial"
#         def __init__(s, dev="/dev/ttyUSB0"): s.p = serial.Serial(dev, 9600, timeout=1)
#         def power_off(s): s.p.write(bytes([0xA0, 0x01, 0x01, 0xA2]))   # frame is board specific
#         def power_on(s):  s.p.write(bytes([0xA0, 0x01, 0x00, 0xA1]))
#
# 2) Linux GPIO via gpiod CLI (libgpiod tools), no Python deps:   --relay cmd:"gpioset -t0 gpiochip0 17=1" ...
#     i.e. CommandRelay(off_cmd="gpioset gpiochip0 17=1", on_cmd="gpioset gpiochip0 17=0")
#
# 3) Network smart plug / programmable supply (SCPI over TCP):
#     import socket
#     class ScpiSupply(RelayDriver):
#         name = "scpi"
#         def __init__(s, host, port=5025): s.s = socket.create_connection((host, port))
#         def power_off(s): s.s.sendall(b"OUTP OFF\n")
#         def power_on(s):  s.s.sendall(b"OUTP ON\n")
class CommandRelay(RelayDriver):
    """Generic: run shell commands for off/on (works for gpioset, usbrelay, curl to a smart plug...)."""
    name = "cmd"
    def __init__(self, off_cmd, on_cmd): self.off_cmd, self.on_cmd = off_cmd, on_cmd
    def power_off(self): subprocess.run(self.off_cmd, shell=True, check=True)
    def power_on(self): subprocess.run(self.on_cmd, shell=True, check=True)

# ---------------------------------------------------------------- "unit is back" checks
def make_check(spec, dry_state=None):
    """Return fn(timeout_s)->bool. spec: ping:HOST | serial:DEV:REGEX | cmd:SHELL | dry"""
    kind, _, rest = spec.partition(":")
    if kind == "dry":
        return lambda t: True
    if kind == "ping":
        def f(t):
            end = time.time() + t
            while time.time() < end:
                if subprocess.run(["ping", "-c", "1", "-W", "1", rest], capture_output=True).returncode == 0:
                    return True
                time.sleep(1)
            return False
        return f
    if kind == "cmd":
        def f(t):
            end = time.time() + t
            while time.time() < end:
                if subprocess.run(rest, shell=True, capture_output=True).returncode == 0:
                    return True
                time.sleep(1)
            return False
        return f
    if kind == "serial":   # DEV:REGEX ; reads the tty as a plain file (set baud first: stty -F DEV 115200 raw)
        dev, _, rx = rest.partition(":")
        def f(t):
            import select
            fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK); buf = ""; end = time.time() + t
            try:
                while time.time() < end:
                    if select.select([fd], [], [], 0.5)[0]:
                        buf = (buf + os.read(fd, 4096).decode("utf-8", "replace"))[-65536:]
                        if re.search(rx, buf): return True
            finally:
                os.close(fd)
            return False
        return f
    raise SystemExit(f"bad --check '{spec}'")

# ---------------------------------------------------------------- campaign
def run_verify(cmd, args, dry_fail):
    """-> (ok, detail dict)"""
    if dry_fail is not None:
        return (not dry_fail), {"firmware": "new", "slots_consistent": not dry_fail, "simulated": True}
    p = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=args.verify_timeout_s)
    detail = {"exit": p.returncode}
    lines = p.stdout.strip().splitlines()
    if lines:
        try: detail.update(json.loads(lines[-1]))
        except ValueError: detail["stdout_tail"] = lines[-1][:200]
    ok = p.returncode == 0
    if args.prev_fw or args.new_fw:
        if detail.get("firmware") not in (args.prev_fw, args.new_fw): ok = False
        if detail.get("slots_consistent") is False: ok = False
    return ok, detail

def run_campaign(a, relay=None):
    rng = random.Random(a.seed)
    relay = relay or (DryRunRelay() if a.dry_run else CommandRelay(a.relay_off, a.relay_on))
    check = make_check("dry" if a.dry_run else a.check)
    open(a.out, "w").close()
    for n in range(1, a.cuts + 1):
        rec = {"cycle": n, "campaign": a.campaign, "dry_run": a.dry_run, "relay": relay.name}
        t0 = time.time()
        try:
            if a.start_cmd and not a.dry_run: subprocess.Popen(a.start_cmd, shell=True)
            delay = rng.uniform(0, a.window_s)
            rec["delay_s"] = round(delay, 3)
            relay.sleep(delay)
            rec["phase_hit"] = "simulated" if a.dry_run else (
                subprocess.run(a.phase_cmd, shell=True, capture_output=True, text=True).stdout.strip() if a.phase_cmd else "unknown")
            relay.power_off(); rec["t_off"] = time.time()
            relay.sleep(a.off_s); relay.power_on()
            back = check(a.boot_timeout_s)
            rec["unit_back"] = back
            if back:
                dry_fail = (n in a.dry_fail_at) if a.dry_run else None
                ok, detail = run_verify(a.verify_cmd, a, dry_fail)
                rec["detail"] = detail
            else:
                ok = False; rec["detail"] = {"error": "unit did not return"}
            rec["ok"] = ok
        except Exception as e:   # a rig error is a failed cycle, never silently skipped
            rec["ok"] = False; rec["detail"] = {"error": repr(e)}
        rec["elapsed_s"] = round(time.time() - t0, 2)
        with open(a.out, "a") as f: f.write(json.dumps(rec) + "\n")
    return summarize(a.out, a.required)

def summarize(path, required=REQUIRED_CUTS, quiet=False):
    rows = [json.loads(l) for l in open(path) if l.strip()]
    done = len(rows); fails = [r["cycle"] for r in rows if not r.get("ok")]
    dry = any(r.get("dry_run") for r in rows)
    ok = done >= required and not fails
    verdict = ("DRY-RUN (not evidence) " if dry else "") + ("PASS" if ok else "FAIL")
    s = {"cuts_done": done, "required": required, "failures": len(fails), "failed_cycles": fails, "verdict": verdict,
         "delay_s_min": min((r.get("delay_s", 0) for r in rows), default=None),
         "delay_s_max": max((r.get("delay_s", 0) for r in rows), default=None),
         "phases": sorted({str(r.get("phase_hit")) for r in rows})}
    if not quiet: print(json.dumps(s, indent=2))
    return s

def self_test():
    d = tempfile.mkdtemp(); out = os.path.join(d, "c.jsonl")
    def mk(**kw):
        a = argparse.Namespace(campaign="update", dry_run=True, cuts=100, required=100, out=out, seed=1, window_s=60,
            off_s=5, boot_timeout_s=1, verify_timeout_s=1, start_cmd=None, phase_cmd=None, verify_cmd=None, check="dry",
            relay_off=None, relay_on=None, prev_fw=None, new_fw=None, dry_fail_at=set())
        a.__dict__.update(kw); return a
    s = run_campaign(mk(), DryRunRelay()); assert s["verdict"].endswith("PASS") and s["cuts_done"] == 100, s
    assert s["verdict"].startswith("DRY-RUN")
    s = run_campaign(mk(dry_fail_at={37}), DryRunRelay()); assert s["failures"] == 1 and s["verdict"].endswith("FAIL"), s
    s = run_campaign(mk(cuts=99), DryRunRelay()); assert s["verdict"].endswith("FAIL"), s   # 99 < 100
    a = mk(dry_run=False, cuts=2, off_s=0, relay_off="true", relay_on="true", check="cmd:true",
           verify_cmd="echo '{\"firmware\":\"new\",\"slots_consistent\":true}'", new_fw="new", prev_fw="old", window_s=0.01)
    s = run_campaign(a); assert s["cuts_done"] == 2 and s["failures"] == 0 and s["verdict"] == "FAIL"  # 2<100
    a.verify_cmd = "echo '{\"firmware\":\"???\"}'"; s = run_campaign(a); assert s["failures"] == 2
    print("self-test OK"); return 0

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    r = sub.add_parser("run")
    r.add_argument("--campaign", choices=["update", "import", "other"], default="other")
    r.add_argument("--cuts", type=int, default=REQUIRED_CUTS)
    r.add_argument("--required", type=int, default=REQUIRED_CUTS)
    r.add_argument("--out", default="cuts.jsonl")
    r.add_argument("--seed", type=int, default=None, help="record it; unseeded runs are not reproducible")
    r.add_argument("--window-s", type=float, default=0, help="install/import window length; delay ~ U(0, window)")
    r.add_argument("--off-s", type=float, default=DEFAULT_OFF_S)
    r.add_argument("--boot-timeout-s", type=float, default=300)
    r.add_argument("--verify-timeout-s", type=float, default=600)
    r.add_argument("--start-cmd"); r.add_argument("--phase-cmd"); r.add_argument("--verify-cmd")
    r.add_argument("--check", default="ping:192.168.7.2", help="ping:HOST | serial:DEV:REGEX | cmd:SHELL")
    r.add_argument("--relay-off"); r.add_argument("--relay-on")
    r.add_argument("--prev-fw"); r.add_argument("--new-fw")
    r.add_argument("--dry-run", action="store_true")
    r.add_argument("--dry-fail-at", type=lambda s: {int(x) for x in s.split(",") if x}, default=set())
    s = sub.add_parser("summary"); s.add_argument("jsonl"); s.add_argument("--required", type=int, default=REQUIRED_CUTS)
    a = ap.parse_args(argv)
    if a.self_test: return self_test()
    if a.cmd == "summary": return 0 if summarize(a.jsonl, a.required)["verdict"] == "PASS" else 1
    if a.cmd == "run":
        if not a.dry_run and not (a.relay_off and a.relay_on and a.verify_cmd and a.window_s > 0):
            ap.error("real run needs --relay-off, --relay-on, --verify-cmd and --window-s > 0 (or use --dry-run)")
        if a.dry_run and not a.window_s: a.window_s = 60
        return 0 if run_campaign(a)["verdict"].endswith("PASS") else 1
    ap.print_help(); return 2

if __name__ == "__main__":
    sys.exit(main())
