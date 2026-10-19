# Power-fail protocol (Story 11.9 AC1; NFR18; Story 3.18 shares the setup)

Targets (powerfail/thresholds.json, sourced): hold-up >= 150 ms after input loss; POWER_FAIL asserted >= 100 ms before rails drop, in EVERY cut; 20 cuts at full load.

Setup
- Prototype carrier board with supercap hold-up circuit. Load: 3 A76 cores busy (e.g. `taskset -c 4-6 stress-ng --cpu 3`; core numbering is board-specific, confirm), NVMe writing (`fio --name=w --rw=write --bs=1m --direct=1 --filename=<file> --time_based --runtime=600`).
- Programmable supply or relay cutting the DC input at the board input connector; trigger output marks input loss (t0).
- Oscilloscope / logic analyzer, >= 4 channels on one time base: ch1 input rail at connector (input loss), ch2 POWER_FAIL GPIO, ch3 the first main rail that drops (e.g. 3V3/5V system rail; name it in results), ch4 trigger. Record probe points and sample rate (must resolve << 1 ms).
- Randomize cut phase/time (random delay 1-30 s between cuts, not periodic) and note SSD/OS state; remount and `fsck`/journal check after each cut if Story 3.18 firmware present.

Procedure per cut (n = 1..20)
1. Confirm full load stable 30 s. 2. Arm scope single-shot on falling input. 3. Cut. 4. Save capture `cuts/cut_NN.*`. 5. Read three timestamps: t_input_loss (input falls below the threshold you define, state it), t_power_fail (POWER_FAIL asserted), t_rail_drop (rail falls below its brown-out limit, state it). 6. Append row to cuts.csv.
cuts.csv columns: `cut,t_input_loss_ms,t_power_fail_ms,t_rail_drop_ms`.

Analysis: `python3 powerfail_analyze.py cuts.csv --expect-cuts 20` prints min/max of input->POWER_FAIL, POWER_FAIL->rail drop, hold-up, and PASS/FAIL. On FAIL the report must name the redesign (e.g. supercap value, detector threshold) per AC.

Report: table of 20 rows, min/max, scope screenshots, setup, board revision, verdict.
