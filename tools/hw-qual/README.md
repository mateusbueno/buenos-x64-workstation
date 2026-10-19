# hw-qual: hardware qualification measurement tooling

For Story 11.9 (power-fail, codec, thermal; partner prototype board) and Story 11.11 (SSD qualification; RK3588 dev board). Source ACs: `_bmad-output/planning-artifacts/epics.md` lines 6354-6377 and 6405-6429. Python 3 stdlib only. Nothing here has been run on hardware.

Rule: thresholds are copied only from the repo, each with a file:line `source` (`*/thresholds.json`). `null` = not defined in repo (TODO): tools report the measurement and mark the verdict TODO. Do not fill them in without a cited source.

## Procedure per acceptance criterion

| Story / AC | Where | Steps |
|---|---|---|
| 11.9 AC1: 20 cuts at full load, POWER_FAIL timing, hold-up >=150 ms, lead >=100 ms | `powerfail/PROTOCOL.md`, `powerfail/powerfail_analyze.py` | Capture on scope, fill `cuts.csv`, run analyzer, attach table + captures |
| 11.9 AC2: codec noise floor, THD+N, dynamic range (main, aux, headphone), analog in-to-out latency, with setup | not scripted | Audio analyzer (e.g. APx or calibrated ADC) per output; loopback for latency. No numeric targets in repo (partner reference figures are the comparison, epics.md:2345): record setup, level, bandwidth/weighting, sample rate, buffer config |
| 11.9 AC3: 24 h, 40 C, closed chassis, temps + CPU freq, no throttling | `thermal/PROTOCOL.md`, `thermal/thermal_logger.py` | Run logger 24 h at 10 s, analyze, compare cur vs max freq |
| 11.11 AC1: >=3 SSDs, 256 voices, io_uring+O_DIRECT, >=90% full, >=12 h, p99/p99.9/p99.99/max, closed-chassis temp, head size vs 4-6 GB | `ssd/run_ssd_qual.py plan`, `ssd/jobs/*.fio` | `plan --dry-run` to review, run for real as root, `summarize`; log drive temp with thermal logger; head size = p99.99 + margin (margin not in repo) x streaming rate, state vs budget |
| 11.11 AC2: rated TBW, power-loss behaviour, writes/yr heavy user | `ssd/ENDURANCE.md` | nvme/smartctl snapshots before/after, datasheet TBW cited |
| 11.11 AC3: approval record | results file | Model, firmware, supplier, second SSD, minimum spec for future SSDs (business decision) |

Quick checks (no hardware, no root):

    python3 ssd/run_ssd_qual.py --self-test
    python3 ssd/run_ssd_qual.py plan --target /mnt/ssd/f --size 900G --ssd-label A --dry-run
    python3 thermal/thermal_logger.py --self-test
    python3 powerfail/powerfail_analyze.py --self-test

## Results-recording convention

Create `results/<story>/<YYYYMMDD>_<board-id>_<item>/` (not committed unless the founder decides) containing:
- `meta.txt`: board revision and serial, dev vs prototype, SoM/BSP/kernel (`uname -a`), tool git revision, operator, ambient temperature, chassis open/closed.
- raw data untouched (fio JSON + logs, CSVs, scope captures, `nvme smart-log` outputs before/after).
- `summary.md`: table of measured values, the threshold used with its `source`, verdict PASS / FAIL / TODO (no threshold), deviations from the protocol.
- Never edit raw files; derived numbers go in summary.md with the command that produced them.
