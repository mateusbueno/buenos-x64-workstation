# Thermal protocol (Story 11.9 AC3; NFR11; also Story 3.20 on the prototype)

Targets (sourced): closed production chassis, 40 C ambient (epics.md:6375), 24 h full load (epics.md:6376), no thermal throttling, CPU frequency logged, SoC and NVMe temps over time (epics.md:6377). Story 3.20 (epics.md:2510): log SoC, NVMe, SoM temps and CPU freqs every 10 s. Throttle point / temperature limits: not defined in repo (TODO; take from SoM and SSD datasheets and cite).

Setup
- Board in the closed chassis, in a climate chamber or controlled room at 40 C; measure ambient with an independent probe next to the chassis (log it by hand or a second sensor; thermal_logger covers only on-board sensors).
- Full load: the worst-case soak of Story 2.12 (8 parts, full FX, >=256 voices streaming) through the codec outputs; for dev/early use `stress-ng --cpu 0 --vm 2` plus fio reads/writes on the NVMe. State which was used.
- Let the chamber stabilise 1 h at 40 C before starting the 24 h clock.

Run
    python3 thermal_logger.py --out run_<date>.csv --interval 10 --duration-h 24
Then `python3 thermal_logger.py --analyze run_<date>.csv`.

Throttling check: for every CPU, compare scaling_cur_freq with cpuinfo_max_freq / the governor's expected frequency under load; also check `dmesg | grep -i thermal` and `/sys/class/thermal/cooling_device*/cur_state` before/after. Any sustained drop below the expected frequency at high temperature = throttling -> report names the thermal change (heatsink, airflow, power cap).
Report: temperature-vs-time plot (SoC, NVMe), frequency-vs-time, max temps, throttle verdict, setup. Compare with the partner's result.
