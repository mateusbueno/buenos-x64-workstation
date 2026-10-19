#!/usr/bin/env python3
"""Send every mapped Hammond M-solo control across its range and record audio.

Modes:
  --dry-run            print the MIDI messages only (stdlib only, no hardware)
  (default)            send via 'mido', record via 'sounddevice'+'soundfile',
                       write verification_log.json skeleton for the founder
  --registration "88 8000 000" [--percussion on|off]
                       set a full registration (+ percussion) from the mapping;
                       combine with --dry-run, or with --play-note to render it
  --list-ports         list MIDI output ports (needs mido)

The mapping must pass validate_mapping.py first; this script re-validates.
Optional packages are imported lazily: pip install mido python-rtmidi sounddevice soundfile
"""
import argparse
import datetime
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import validate_mapping as vm  # noqa: E402

DRAWBAR_ORDER = ["drawbar_16", "drawbar_5_1_3", "drawbar_8", "drawbar_4", "drawbar_2_2_3",
                 "drawbar_2", "drawbar_1_3_5", "drawbar_1_1_3", "drawbar_1"]


# ---------- message building (pure, no dependencies) ----------

def control_messages(ctrl, value, channel):
    """Raw MIDI byte lists for setting `ctrl` to `value` on `channel` (1..16)."""
    m = ctrl["message"]
    ch = channel - 1
    cc = lambda n, v: [0xB0 | ch, n, v]  # noqa: E731
    if m["type"] == "CC":
        return [cc(m["number"], value & 0x7F if m["value_bits"] == 7 else value >> 7)] + (
            [cc(m["number"] + 32, value & 0x7F)] if m["value_bits"] == 14 else [])
    out = [cc(99, m["nrpn_msb"]), cc(98, m["nrpn_lsb"])]  # standard NRPN select
    if m["value_bits"] == 14:
        out += [cc(6, value >> 7), cc(38, value & 0x7F)]
    else:
        out.append(cc(6, value))
    out += [cc(101, 127), cc(100, 127)]  # deselect (RPN null)
    return out


def sweep_values(ctrl, extra_steps=0):
    """(label, value) steps: mapped positions, plus min/max, plus N even steps."""
    m = ctrl["message"]
    steps = {}
    for e in m["position_mapping"]:
        steps.setdefault(e["value"], str(e["position"]))
    for v, lab in ((m["value_min"], "range_min"), (m["value_max"], "range_max")):
        steps.setdefault(v, lab)
    if extra_steps > 0:
        lo, hi = m["value_min"], m["value_max"]
        for i in range(extra_steps + 1):
            v = lo + round((hi - lo) * i / extra_steps)
            steps.setdefault(v, f"sweep_{v}")
    return sorted(((lab, v) for v, lab in steps.items()), key=lambda t: t[1])


def parse_registration(text):
    """'88 8000 000' -> [8,8,8,0,0,0,0,0,0] (16', 5-1/3', 8', 4', 2-2/3', 2', 1-3/5', 1-1/3', 1')."""
    digits = [c for c in text if not c.isspace()]
    if len(digits) != 9 or not all(c in "012345678" for c in digits):
        raise ValueError(f"registration must be 9 digits 0-8 (got {text!r})")
    return [int(c) for c in digits]


def position_value(ctrl, label):
    for e in ctrl["message"]["position_mapping"]:
        if str(e["position"]) == str(label):
            return e["value"]
    raise KeyError(f"{ctrl['id']}: no position {label!r} in mapping")


def registration_messages(mapping, reg_text, percussion=None):
    """Ordered [(description, [raw messages])] setting drawbars (+ percussion on/off)."""
    by_id = {c["id"]: c for c in mapping["controls"]}
    ch = mapping["midi_channel"]
    plan = []
    for cid, d in zip(DRAWBAR_ORDER, parse_registration(reg_text)):
        c = by_id[cid]
        plan.append((f"{cid}={d}", control_messages(c, position_value(c, str(d)), ch)))
    if percussion is not None:
        c = by_id["percussion_on"]
        lab = "on" if percussion else "off"
        plan.append((f"percussion_on={lab}", control_messages(c, position_value(c, lab), ch)))
    return plan


def hexs(msgs):
    return " | ".join(" ".join(f"{b:02X}" for b in m) for m in msgs)


# ---------- lazy optional deps ----------

def need(modname, pipname):
    try:
        return __import__(modname)
    except ImportError:
        sys.exit(f"error: this mode needs the optional package '{pipname}' "
                 f"(pip install {pipname}). Use --dry-run to print messages without it.")


def open_port(name):
    mido = need("mido", "mido python-rtmidi")
    names = mido.get_output_names()
    if name not in names:
        sys.exit(f"error: MIDI port {name!r} not found. Available: {names}")
    return mido, mido.open_output(name)


def send(mido, port, msgs):
    for raw in msgs:
        port.send(mido.Message.from_bytes(raw))


# ---------- main flows ----------

def load_valid(path, template):
    mapping = vm.load_json(path)
    errs = vm.validate(mapping, vm.load_json(template))
    if errs:
        print(f"mapping invalid ({len(errs)} problems); run validate_mapping.py", file=sys.stderr)
        for e in errs[:10]:
            print("  -", e, file=sys.stderr)
        sys.exit(1)
    return mapping


def play_and_record(a, mido, port, sd, sf, name, msgs, note_on, note_off):
    """Send msgs, play note, record; return wav filename."""
    frames = int(a.seconds * a.samplerate)
    send(mido, port, msgs)
    time.sleep(a.settle)
    rec = sd.rec(frames, samplerate=a.samplerate, channels=a.channels, device=a.audio_device)
    port.send(mido.Message.from_bytes(note_on))
    time.sleep(a.hold)
    port.send(mido.Message.from_bytes(note_off))
    sd.wait()
    path = os.path.join(a.out_dir, "audio", name + ".wav")
    sf.write(path, rec, a.samplerate, subtype="PCM_24")
    return os.path.relpath(path, a.out_dir)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mapping", default=os.path.join(HERE, "mapping.json"))
    ap.add_argument("--template", default=vm.DEFAULT_TEMPLATE)
    ap.add_argument("--port", help="MIDI output port name (exact)")
    ap.add_argument("--list-ports", action="store_true")
    ap.add_argument("--dry-run", action="store_true", help="print messages only; no extra packages")
    ap.add_argument("--controls", help="comma-separated control ids to run (default all)")
    ap.add_argument("--sweep-steps", type=int, default=0, help="extra evenly spaced values per control")
    ap.add_argument("--registration", help="e.g. '88 8000 000'")
    ap.add_argument("--percussion", choices=["on", "off"], help="with --registration")
    ap.add_argument("--play-note", type=int, default=None, metavar="NOTE",
                    help="MIDI note to hold while recording (required for recording a registration)")
    ap.add_argument("--note", type=int, default=60, help="test note for control sweeps (default 60)")
    ap.add_argument("--velocity", type=int, default=100)
    ap.add_argument("--seconds", type=float, default=4.0, help="recording length")
    ap.add_argument("--hold", type=float, default=3.0, help="note hold time")
    ap.add_argument("--settle", type=float, default=0.3, help="wait after control message")
    ap.add_argument("--samplerate", type=int, default=48000)
    ap.add_argument("--channels", type=int, default=2)
    ap.add_argument("--audio-device", default=None)
    ap.add_argument("--out-dir", default=os.path.join(HERE, "runs", datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
    a = ap.parse_args(argv)

    if a.list_ports:
        mido = need("mido", "mido python-rtmidi")
        print("\n".join(mido.get_output_names()) or "(no ports)")
        return 0

    mapping = load_valid(a.mapping, a.template)
    ch = mapping["midi_channel"]
    ids = set(a.controls.split(",")) if a.controls else None
    ctrls = [c for c in mapping["controls"] if ids is None or c["id"] in ids]
    if ids and len(ctrls) != len(ids):
        sys.exit(f"error: unknown control id(s): {sorted(ids - {c['id'] for c in ctrls})}")

    note_on = lambda n: [0x90 | (ch - 1), n, a.velocity]  # noqa: E731
    note_off = lambda n: [0x80 | (ch - 1), n, 0]  # noqa: E731

    # ---- registration mode ----
    if a.registration:
        plan = registration_messages(mapping, a.registration,
                                     None if a.percussion is None else a.percussion == "on")
        print(f"# registration {a.registration!r} percussion={a.percussion} channel={ch}")
        for desc, msgs in plan:
            print(f"{desc:24s} {hexs(msgs)}")
        if a.dry_run:
            return 0
        if not a.port:
            sys.exit("error: --port required (or use --dry-run)")
        mido, port = open_port(a.port)
        for _, msgs in plan:
            send(mido, port, msgs)
            time.sleep(0.02)
        if a.play_note is not None:
            sd, sf = need("sounddevice", "sounddevice"), need("soundfile", "soundfile")
            os.makedirs(os.path.join(a.out_dir, "audio"), exist_ok=True)
            tag = "reg_" + a.registration.replace(" ", "") + (f"_perc{a.percussion}" if a.percussion else "")
            wav = play_and_record(a, mido, port, sd, sf, tag, [], note_on(a.play_note), note_off(a.play_note))
            print("recorded", wav)
        return 0

    # ---- sweep mode ----
    sd = sf = mido = port = None
    if not a.dry_run:
        if not a.port:
            sys.exit("error: --port required (or use --dry-run)")
        mido, port = open_port(a.port)
        sd, sf = need("sounddevice", "sounddevice"), need("soundfile", "soundfile")
        os.makedirs(os.path.join(a.out_dir, "audio"), exist_ok=True)
    log = {"schema_version": 1, "started": datetime.datetime.now().isoformat(timespec="seconds"),
           "mapping_file": os.path.abspath(a.mapping), "mapping_source": mapping["source"],
           "port": a.port, "samplerate": a.samplerate, "test_note": a.note,
           "operator": "TODO", "m_solo_firmware": "TODO", "interface": "TODO", "entries": []}
    for c in ctrls:
        for label, value in sweep_values(c, a.sweep_steps):
            msgs = control_messages(c, value, ch)
            if a.dry_run:
                print(f"{c['id']:22s} {label:>10s} value={value:<6d} {hexs(msgs)}")
                continue
            name = f"{c['id']}__{label.replace('/', '-')}_{value}"
            wav = play_and_record(a, mido, port, sd, sf, name, msgs, note_on(a.note), note_off(a.note))
            print("recorded", wav)
            log["entries"].append({
                "control": c["id"], "position": label, "value": value,
                "midi_hex": hexs(msgs), "audio": wav,
                "expected_control_changed": None,  # true/false after listening / spectrum
                "observed_change": "TODO",         # e.g. 'harmonic at 8 ft rises'
                "mismatch": None,                  # true if a different control reacted
                "correction": "TODO", "verdict": None})  # 'confirmed'|'corrected'|'failed'
    if not a.dry_run:
        path = os.path.join(a.out_dir, "verification_log.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(log, fh, indent=2)
            fh.write("\n")
        print("wrote", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
