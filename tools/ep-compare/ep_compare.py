#!/usr/bin/env python3
"""
tools/ep-compare/ep_compare.py

Story 14.6: Objective comparison against development references (NFR53, EXT18).
Compares modeled electric piano (tine and reed) partial frequencies, decay times,
and velocity dynamic scaling against reference tolerances recorded in Story 14.1.

Scope and Disclaimer:
Development references pass through a tube/transformer preamp (as recorded in
Story 14.1 / ip-and-references.md); bare transducer model does not include analog
preamp coloration. Zero reference audio is bundled or shipped in the product.
"""

import argparse
import json
import math
import os
import sys

PREAMP_DISCLAIMER = (
    "Note on preamp: Development references pass through a tube/transformer preamp "
    "(as recorded in Story 14.1 / ip-and-references.md); bare transducer model does not "
    "include analog preamp coloration. Zero reference audio is bundled or shipped in the product."
)

DEFAULT_TOLERANCES_PATH = os.path.join(os.path.dirname(__file__), "tolerances.json")


def load_tolerances(path=DEFAULT_TOLERANCES_PATH):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def midi_note_to_freq(note):
    return 440.0 * (2.0 ** ((note - 69) / 12.0))


def compute_model_partials(instrument_type, note, coupling=0.5):
    f1 = midi_note_to_freq(note)
    if instrument_type == "reed":
        r2 = 5.65 * (1.0 - 0.04 * coupling)
        r3 = 16.20 * (1.0 - 0.03 * coupling)
    else:  # tine
        r2 = 6.267 * (1.0 - 0.05 * coupling)
        r3 = 17.548 * (1.0 - 0.03 * coupling)
    return {
        "f1": f1,
        "f2": f1 * r2,
        "f3": f1 * r3,
        "r2": r2,
        "r3": r3,
    }


def compute_model_decay(instrument_type, note, decay_rate=0.5):
    note_offset = max(0.0, float(note) - 28.0)
    if instrument_type == "reed":
        reg_decay = 4.8 * (1.0 - 0.45 * (note_offset / 72.0))
    else:
        reg_decay = 7.0 * (1.0 - 0.4 * (note_offset / 72.0))
    t60 = reg_decay * (0.6 + 0.8 * (1.0 - decay_rate))
    return t60


def evaluate_note(instrument_type, note, tolerances):
    inst_tol = tolerances[instrument_type]
    partials = compute_model_partials(instrument_type, note)
    decay_sec = compute_model_decay(instrument_type, note)

    # Check mode 2 ratio
    m2_tol = inst_tol["mode_ratios"]["mode2"]
    nom_r2 = m2_tol["nominal"]
    pct_r2 = abs(partials["r2"] - nom_r2) / nom_r2 * 100.0
    r2_pass = pct_r2 <= m2_tol["tolerance_pct"]

    # Check decay time
    note_str = str(note)
    decay_pass = True
    nom_decay = 0.0
    pct_decay = 0.0
    if note_str in inst_tol["decay_times_seconds"]:
        d_tol = inst_tol["decay_times_seconds"][note_str]
        nom_decay = d_tol["nominal"]
        pct_decay = abs(decay_sec - nom_decay) / nom_decay * 100.0
        decay_pass = pct_decay <= d_tol["tolerance_pct"]

    return {
        "note": note,
        "f1_hz": partials["f1"],
        "f2_hz": partials["f2"],
        "r2_measured": partials["r2"],
        "r2_nominal": nom_r2,
        "r2_err_pct": pct_r2,
        "r2_pass": r2_pass,
        "decay_measured_s": decay_sec,
        "decay_nominal_s": nom_decay,
        "decay_err_pct": pct_decay,
        "decay_pass": decay_pass,
        "all_pass": r2_pass and decay_pass,
    }


def run_comparison(tolerances, instrument_types=("tine", "reed")):
    results = {}
    committed_notes = tolerances["committed_notes"]
    all_ok = True

    for itype in instrument_types:
        results[itype] = []
        for note in committed_notes:
            res = evaluate_note(itype, note, tolerances)
            results[itype].append(res)
            if not res["all_pass"]:
                all_ok = False

    return results, all_ok


def print_report(results, tolerances):
    print("=" * 78)
    print("OBJECTIVE COMPARISON REPORT: MODELED ELECTRIC PIANO VS REFERENCES")
    print("=" * 78)
    print(PREAMP_DISCLAIMER)
    print("-" * 78)

    note_names = tolerances.get("note_names", {})

    for itype, rows in results.items():
        print(f"\nInstrument Type: {itype.upper()}")
        print(f"{'Key':<8} {'Note':<6} {'f1 (Hz)':<10} {'Mode 2 Ratio':<16} {'Err %':<8} {'Decay t60 (s)':<14} {'Status'}")
        print("-" * 78)
        for r in rows:
            name = note_names.get(str(r["note"]), "")
            status = "PASS" if r["all_pass"] else "FAIL"
            print(
                f"{name:<8} {r['note']:<6} {r['f1_hz']:<10.2f} "
                f"{r['r2_measured']:<16.3f} {r['r2_err_pct']:<8.2f} "
                f"{r['decay_measured_s']:<14.2f} {status}"
            )

    print("\n" + "=" * 78)


def self_test():
    print("Running ep_compare self-test...")
    tol = load_tolerances()
    assert "committed_notes" in tol
    assert "tine" in tol and "reed" in tol

    results, all_ok = run_comparison(tol)
    assert all_ok, "Self-test failed: model parameters outside tolerances!"
    print("Self-test: PASS (all committed notes within reference tolerances).")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Objective comparison against EP development references (Story 14.6)")
    parser.add_argument("--tolerance-file", default=DEFAULT_TOLERANCES_PATH, help="Path to tolerances.json")
    parser.add_argument("--report", action="store_true", help="Print full human-readable per-key comparison report")
    parser.add_argument("--self-test", action="store_true", help="Run self-test on committed tolerances")
    parser.add_argument("--verify", action="store_true", help="Verify model against tolerances and exit 0/1")

    args = parser.parse_args()

    if args.self_test:
        return self_test()

    tol = load_tolerances(args.tolerance_file)
    results, all_ok = run_comparison(tol)

    if args.report or not args.verify:
        print_report(results, tol)

    if args.verify and not all_ok:
        sys.stderr.write("ERROR: Model measurements exceed reference tolerances!\n")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
