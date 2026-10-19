# Endurance and SMART logging (Story 11.11, epics.md:6422-6424)

AC: report each SSD's rated TBW, power-loss behaviour (flush on POWER_FAIL), and expected writes/year of a heavy user.
No numeric limits for these exist in the repo (see thresholds.json, null entries): record, do not judge.

Commands (need root, nvme-cli and smartmontools; `run_ssd_qual.py smart --dry-run` prints them):

    nvme id-ctrl /dev/nvme0            # mn (model), fr (firmware), vwc (volatile write cache -> flush needed on POWER_FAIL), oncs
    nvme smart-log /dev/nvme0          # data_units_written, percentage_used, available_spare, temperature, media_errors, unsafe_shutdowns
    smartctl -x /dev/nvme0             # same + temperature history, error log
    nvme smart-log /dev/nvme0 -o json > smart_<label>_<start|end>.json

Procedure
1. Record model, firmware, supplier, rated TBW (from datasheet; cite the document and page) in results.
2. SMART snapshot before fill, after fill, after the 12 h read run. Writes during test = delta of data_units_written * 512000 bytes.
3. Heavy-user writes/year: estimate from the library import plan (not defined in repo; TODO, business input), compare with TBW.
4. Power-loss behaviour: vwc bit from id-ctrl; unsafe_shutdowns delta after the powerfail/ cuts; confirm clean mount per powerfail/PROTOCOL.md. Datasheet statement on PLP (power-loss protection) cited, not assumed.
5. Drive temperature during the read run: log with thermal/thermal_logger.py (nvme hwmon) under the closed-chassis condition.
