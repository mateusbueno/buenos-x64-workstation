# Performance Targets (v1)

| Metric | Target | Note |
|---|---|---|
| Sample polyphony | ≥256 stereo voices streaming at once | This is the floor, not the ceiling. Market references: Montage M is about 400 voices summed across engines; Dexibell claims "unlimited" but caps it at 4 timbres. |
| Latency key → output | ≤5 ms | Audio buffer of 64 frames @ 48 kHz. Keybed scan plus transport takes <1 ms of this |
| Indexed library on NVMe | up to 1 TB | Far larger than RAM |
| RAM | 16 GB (v1), 32 GB (Pro) | ~4–6 GB reserved for sample heads |
| Sample format on disk | up to 32-bit float / 96 kHz | Lossless, ~50% ratio, see `library-format.md` |
| Robustness | zero dropouts in a 24 h stress run, SSD near full | |
| Boot | ≤10 s from power-on to the playable Boot into target | Its heads load first, then favorites; the rest indexes in the background |
| Power-loss | 100 random power cuts during update or import, no brick, no corrupt library | See CAP-17 |

## Sizing estimates (forge, unmeasured)

- **CPU:** 256 stereo voices with quality interpolation, filter, and envelopes come to about 1.5 GFLOP/s, plus lossless decode and effects. 4× Cortex-A76 is expected to have headroom.
- **NVMe bandwidth:** about 74 MB/s at 24-bit/48 kHz, rising to about 400 MB/s worst case at 32f/96k with upward transposition.
- **Design driver:** tail latency matters more than bandwidth (for example, p99.99 reads during SSD garbage collection). Heads must cover the worst-case read, not the average.
