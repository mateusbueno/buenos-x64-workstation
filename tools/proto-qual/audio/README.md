# proto-qual/audio: test packs for Stories 3.13, 3.14, 3.15, 3.16, 3.19

Prototype board only. Source ACs: `_bmad-output/planning-artifacts/epics.md` 3.13 (2321-2345), 3.14 (2347-2369), 3.15 (2371-2389), 3.16 (2391-2411), 3.19 (2473-2492). Style follows `tools/hw-qual/` (not modified). Python 3 stdlib only. Nothing here has been run on hardware; self-tests use synthetic data.

Rule: `thresholds.json` holds only repo values, each with a file:line `source`. `null` = not defined in repo: tools print TODO, never PASS/FAIL. Repo defines no numeric target for THD+N, noise floor, dynamic range, crosstalk, balance, pop margin or round-trip latency (grep of epics.md, performance-targets.md, spec folder); 3.13 AC3 compares with partner reference figures (epics.md:2345, EXT10 :416), which are not in the repo.

## Tools

    python3 latency_analyze.py --self-test | latency.csv [--dry-run] [--json]
    python3 wav_analyze.py --self-test | {level,noise,thdn,xtalk,pop} FILE [--dry-run]
    python3 alsa_checklist.py --self-test | --dry-run | --playback F --capture F

`latency.csv` columns (us): `note,key_ts_us,onset_ts_us[,scan_transport_us,time_sync_us,sched_us,render_us,codec_us]`. Breakdown columns come from engine/MCU timestamps (Story 1.20 TIME_SYNC, 3.x MCU timing).

## AC to procedure map

| AC | Procedure | Instruments | Tool / verdict |
|---|---|---|---|
| 3.13 AC1: PCM3168A BCLK/LRCK master on 24.576 MHz, SoC slave, no SoC audio PLL, 6ch playback + 2ch capture, 48 kHz, 64-frame periods | Boot; capture `aplay --dump-hw-params` / `/proc/asound/cardN/pcm0p/sub0/hw_params` (playback and capture) while streaming; DT review; `clk_summary` | Scope on BCLK/LRCK/MCLK at codec | `alsa_checklist.py` (channels/rate/period PASS/FAIL); clock master and PLL items MANUAL with evidence listed |
| 3.13 AC2: engine on codec 1 h, zero xruns, DLL locks | 1 h run; export xrun counter (FR120) and DLL state | none | `soak_hours`, `soak_xruns_max` from thresholds; MANUAL record |
| 3.13 AC3: DAC->ADC loopback, round-trip latency + noise floor vs partner figures | Loopback on factory test points; play impulse/tone, record capture; silence capture for noise | Loopback cable/test points; optional scope | `wav_analyze.py noise`, `level`; latency by impulse offset on scope or `latency_analyze.py`; verdict TODO until partner figures are entered |
| 3.14 AC1: Main->DAC1-2, Aux->DAC3-4, headphone (Main mix)->DAC5-6 | Tone on one bus at a time; measure each of 6 outputs | Audio analyzer or ADC, multichannel capture | `wav_analyze.py level` per channel; PASS if only mapped channels carry signal (judge from the table, mapping thresholds in json) |
| 3.14 AC2: part on Aux only heard on Aux, change click-free | Assign part to Aux in Effects > Outputs during sustained tone; capture | same | `wav_analyze.py level` on Main vs Aux; `pop` for clicks (TODO until margin defined) |
| 3.14 AC3: level, balance, Main/Aux crosstalk, noise; headphone amp 32 and 250 ohm unclipped at reference level | Per output: 1 kHz tone at reference level and silence; drive Main only, read Aux (and reverse); headphone with 32 and 250 ohm loads | Analyzer, load resistors/real headphones, scope for clipping | `level`, `xtalk --drive N --victim M`, `noise`, `thdn`; all verdicts TODO (no thresholds; reference level not in repo); clipping = peak at full scale or flat tops on scope |
| 3.15 AC1: ADC1-2 captured in same period as playback, feeds line-in port (2.7), master only, level per Effects > Master | Inject tone at line in; verify at Main out; check ALSA capture params | Signal generator | `alsa_checklist.py --capture`; `wav_analyze.py level` |
| 3.15 AC2: round-trip latency + noise recorded; line in never reaches parts/inserts/sends | Impulse at line in vs Main out; silence on line-in; with part/insert/send monitoring, confirm no signal | Signal generator, scope, capture | `latency_analyze.py` (rows = impulses), `wav_analyze.py noise`/`level`; latency verdict TODO beyond the recorded number (only the 5 ms key latency is defined) |
| 3.16 AC1: mute default, released only after codec open + 1 silent period; no transient at power-on over 20 cycles | 20 cold power-ons; capture Main out from AC power apply (trigger on supply rail); also scope the mute GPIO | Scope (DC-coupled, Main out + mute GPIO + 5 V rail), or ADC with long record | `wav_analyze.py pop` per capture; verdict TODO until `pop_margin_db` is set (source needed); record cycle count vs `power_cycles_required` (20) |
| 3.16 AC2: soft power-off: fade, assert mute, then stop audio; no transient, 20 cycles | 20 button shutdowns, capture as above; check order on scope (fade -> mute -> clocks stop) | same | `pop`; ordering MANUAL from scope |
| 3.16 AC3: crash/restart asserts mute before codec stops clocking | `kill -SEGV`/`kill -9` engine and `systemctl restart engine` while playing; scope mute GPIO vs BCLK | Scope with BCLK, mute GPIO, Main out | MANUAL (mute edge must precede BCLK stop); `pop` on Main capture |
| 3.19 AC1: 1,000 notes, 256 other voices, contact-to-analog <=5 ms every note; min/median/p99/max + breakdown; scan+transport <1 ms | Rig senses contact closing (or solenoid + timing ref), captures Main out; run 256-voice background; log breakdown stages | Latency rig (contact sense + ADC or scope, common clock), real keybed | `latency_analyze.py`: every-note max <=5 ms and count >=1000 PASS/FAIL, scan+transport <1 ms PASS/FAIL (needs column), p99 reported not gated (TODO); 256 voices attested in meta.txt |
| 3.19 AC2: organ onset from first contact measured separately | Same rig, organ part, first contact closing | same | Run `latency_analyze.py` separately with `--label organ`; no threshold (TODO) |

## Results convention (same as hw-qual)

`results/<story>/<YYYYMMDD>_<board-id>_<item>/` (not committed unless the founder decides):
- `meta.txt`: board revision/serial, kernel (`uname -a`), engine version, tool git revision, operator, instrument models and calibration, sample rate, bandwidth/weighting, reference level, load (ohm), number of voices, cycle counts.
- raw data untouched: WAVs, scope captures, CSVs, ALSA dumps, clk_summary.
- `summary.md`: table of values, threshold with its `source`, verdict PASS / FAIL / TODO, deviations, and the command that produced each number.
- Never edit raw files.
