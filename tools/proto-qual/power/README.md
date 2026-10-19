# Power test packs: Stories 3.18, 8.11, 8.13, 4.38

Status: prepared, NOT run on hardware. Only `cut_rig.py --self-test` and `--dry-run` were executed (simulated unit).
Story text: `_bmad-output/planning-artifacts/epics.md` (3.18 at :2445, 4.38 at :3509, 8.11 at :5167, 8.13 at :5222).
Reused unchanged: `tools/hw-qual/powerfail/PROTOCOL.md`, `powerfail_analyze.py`, `thresholds.json` (setup, scope channels, `cuts.csv` columns `cut,t_input_loss_ms,t_power_fail_ms,t_rail_drop_ms`).
Thresholds come only from the repo (file:line); anything else is TODO. Do not copy numbers into new files; read `thresholds.json`.

## Shared equipment
Prototype carrier + supercap, programmable supply or relay at the DC input, scope >= 4 ch (input rail, POWER_FAIL, first dropping rail, trigger), audio capture of Main/outputs, dev image reporting slot/version/library state over the dev network (8.13). Procedure and scope setup: `PROTOCOL.md`.

## Thresholds in use
| Item | Value | Source |
|---|---|---|
| Hold-up after input loss | >= 150 ms | thresholds.json holdup_ms_min; epics.md:2467 (3.18), :5186 (8.11), :262 (NFR18) |
| POWER_FAIL before rail drop | >= 100 ms | thresholds.json power_fail_lead_ms_min; same lines |
| Output fade | <= 20 ms | epics.md:2461 (3.18); thresholds.json output_fade_ms_max |
| Cuts: 3.18 / 8.11 | 50 / 50 | epics.md:2466 / :5185 area (8.11 AC2, "50 times") |
| Brown-outs (8.11) | 20, "up to and beyond the hold-up window" | epics.md:5193 |
| Cuts: 8.13 | 100 update + 100 import; power off 5 s; any failure fails the gate | epics.md:5236-5245 |
| Input loss -> POWER_FAIL max | TODO: not defined in repo (thresholds.json loss_to_power_fail_ms_max null) | |
| "Audible pop" threshold | TODO: story says "defined threshold" (epics.md:5186), no value in repo | |
| Brown-out length set/steps | TODO: only "up to and beyond the window" | |

## AC mapping

### 3.18 Power-fail fade and write freeze (Prototype board; blocked by 11.9)
| AC | Procedure | Equipment | Results |
|---|---|---|---|
| AC1 POWER_FAIL -> RT fade <= 20 ms then HW mute; sysd freezes journaled partition, flushes NVMe; one shared freeze signal | Assert POWER_FAIL (supply cut or injected edge); scope ch: POWER_FAIL, audio out, mute line; read freeze event from dev log. Code-review item: no writer has own POWER_FAIL handling | scope, audio out probe, dev log | `3.18/ac1_fade.csv` (fade ms per cut) + screenshot; review note |
| AC2 50 cuts at random times, audio playing + test writer writing | `PROTOCOL.md` procedure with n=50 (not 20); `python3 ../../hw-qual/powerfail/powerfail_analyze.py cuts.csv --expect-cuts 50`; after each reboot run `--verify-cmd` (journal mount clean, via `cut_rig.py run --campaign other --cuts 50 --required 50`); no output transient on scope; compare with partner figures | programmable supply/relay, scope, test writer | `3.18/cuts.csv`, `3.18/cuts.jsonl`, scope captures `cuts/cut_NN.*`, analyzer output, partner comparison table (TODO: partner figures source) |
| AC3 POWER_FAIL then power returns inside window, rails never dropped -> clean reboot, event logged | Pulse supply off for a time < hold-up; confirm reboot, log line present | supply, serial/log | `3.18/ac3_interim_reboot.md` |

### 8.11 Power-fail acceptance (release image, scope + programmable supply)
| AC | Procedure | Equipment | Results |
|---|---|---|---|
| AC1 each writer (import, Write, Boot-into pointer, update staging) abandons in atomic-staging state on freeze; no own handler | Fault-injection test per writer (freeze signal during op; check no rename after POWER_FAIL; boot sees old OR new). Code-review checklist item | dev image hooks (TODO: injection mechanism owned by dev), reviewer | `8.11/ac1_writers.md`: 4 rows (import, Write, Boot-into, update) x injected/result/reviewed |
| AC2 50 random cuts, dense Live Set + import + Write in progress | As 3.18 AC2 but release image and `--expect-cuts 50`; silence+mute before rail drop every cut; NVMe flush within hold-up; pop recorded vs defined threshold (TODO value); partition mounts clean, each item old or new; margins compared with 3.18 bring-up | scope, audio capture | `8.11/cuts.csv`, analyzer output, `8.11/margins_vs_3.18.md` |
| AC3 short brown-out, rails never dropped: no reboot, writers resume, mute releases, fade-in, event logged | Drop input for t < hold-up; check uptime unchanged/boot count same, log event, audio fade-in on capture | supply, log, audio capture | `8.11/ac3.md` |
| AC4 20 brown-outs, lengths up to and beyond window | Sweep lengths (TODO: list; include values around 150 ms from thresholds.json); within window: no reboot, no pop; beyond: same clean power-down as AC2 | supply, scope | `8.11/brownouts.csv` columns `n,length_ms,rebooted,pop,clean_powerdown,verdict` |

### 8.13 Power-cut rig (relay + test host; dev image)
| AC | Procedure | Equipment | Results |
|---|---|---|---|
| AC1 update campaign: random delay uniform over whole install window, cut 5 s, restore, record booted firmware; 100 cycles | `python3 cut_rig.py run --campaign update --cuts 100 --seed N --window-s <install window> --start-cmd '<start USB update, Story 8.1>' --relay-off ... --relay-on ... --check ping:<ip> --verify-cmd '<prints JSON {"firmware","slots_consistent"}>' --prev-fw X --new-fw Y --phase-cmd '<reads phase>' --out update.jsonl` | relay on DC input, host, USB stick with update | `update.jsonl`, summary. PASS = 100 cycles, 0 failures (firmware previous or new, slots consistent) |
| AC2 import campaign: multi-GB SFZ batch from USB SSD, random cut, 100 times | same with `--campaign import`; verify-cmd must check no partial library in catalog and `/data/lib`, render a scripted note per existing library and check non-silent, log reconcile counts (TODO: dev-image commands for these) | + USB SSD with batch | `import.jsonl`, summary |
| AC3 report: each cycle (delay, phase hit, outcome), pass/fail; any failure fails gate | `python3 cut_rig.py summary update.jsonl` (exit 0 only on PASS); JSONL fields `cycle, delay_s, phase_hit, unit_back, ok, detail` | | report assembled from both JSONL files |

### 4.38 First playable unit full show run-through
`show_runthrough_checklist.md` (derived from the story and Flow 4 / Flow 1 in EXPERIENCE.md). Results: filled checklist + issues + milestone verdict (epics.md:3531).

## cut_rig.py notes
- Drivers: `DryRunRelay` (stub), `CommandRelay` (shell off/on commands; covers gpioset, usbrelay, smart plugs). Serial-relay, GPIO and SCPI examples are comments in the source, not hardware-tested.
- Unit-back checks: `ping:HOST`, `serial:DEV:REGEX` (tty must be configured first), `cmd:SHELL`.
- Dry-run output is labeled `DRY-RUN (not evidence)`; verdict compares to 100 cuts (override `--required`, e.g. 50 for 3.18/8.11 cut loops).
- Rig errors count as failed cycles. Safety: wire relay so a rig crash leaves the unit powered; keep PSU off-time at 5 s as in 8.13.
- `python3 cut_rig.py --self-test` ; `python3 cut_rig.py run --dry-run --out /tmp/x.jsonl`
