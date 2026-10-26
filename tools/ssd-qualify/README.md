# velora-ssd-qualify (Story 1.16)

Measures the read latency a sample stream sees on the SSD and recommends the instrument head length (`engine/host/include/velora/host/head_sizing.hpp`).

    velora-ssd-qualify --dir <library dir> [--seconds N] [--rate-mib-s N] [--block-kib N]
                       [--threads N] [--sample-rate HZ] [--max-pitch-ratio R] [--margin-ms N] [--seed N] [--out report.json]

It reads random, aligned blocks (default 128 KiB, 4 KiB aligned buffer, offset and length) from the regular files under `--dir` with the page cache bypassed
(`O_DIRECT` on Linux, `F_NOCACHE` on macOS), paced at `--rate-mib-s` (default 64; `0` = as fast as possible) for `--seconds` (default 2, enough for a test), and
prints a JSON report: reads, errors, achieved rate, latency in ns (`min`, `median`, `p99`, `p99_99`, `max`, `mean`), `p99_99_meaningful` (false below 100000 reads:
the percentile is then the max) and `head.recommended_ms` / `head.recommended_frames` = p99.99 + margin at `--sample-rate` and `--max-pitch-ratio`. Exit codes: 0 ok,
2 usage / bad configuration, 3 measurement error (missing or unreadable directory, no file of at least one block, open or read failure). A file system that refuses
the no-cache flag (tmpfs) is an error unless `--allow-cached` is given (development; the report then says `"no_cache": "none"`).

## Board procedure (dev board work: NOT run in CI, NOT run in Story 1.16)

1. Fill the library SSD to at least 90 % with real library content (the worst case for the drive's garbage collection), and put the library directory in `--dir`.
2. Streaming rate: 256 voices at the highest sample rate and bit depth of the libraries, each streaming from its file: about `256 * rate * channels * bytes` per second (for example
   256 voices of 48 kHz stereo 24-bit PCM is about 70 MiB/s; use the figure of the heaviest library) as `--rate-mib-s`.
3. Run for 1 h on the closed chassis (`tools/hw-qual` thermal logging beside it): `velora-ssd-qualify --dir /data/library --seconds 3600 --rate-mib-s <rate> --max-pitch-ratio <highest
   of the libraries> --margin-ms 1 --out ssd-qualify-<drive>.json`.
4. Record the JSON with the drive model and firmware. Put `latency_ns.p99_99` into the `HeadSizing::p9999_ns` that engine-host passes to the loader (and the margin you chose into
   `margin_ns`). Repeat on every candidate SSD.

A short run (seconds) is a functional check of the tool and of the page-cache bypass, not a qualification figure.

## What the figure is, and is not

- **Queue depth.** Each reader thread issues synchronous 128 KiB reads (default), so the device queue depth is `--threads` (default 1, alias `--queue-depth`); the rate is shared between the threads. A default run is queue depth 1, NOT 256 concurrent voices: for the board run use `--threads` near the voice count the SSD must sustain and `--block-kib` of the real stream chunk.
- **Coordinated omission.** Latency is measured from the issue of each read. A read that is slow delays nothing else (the next is issued as soon as it returns), so the wait a real stream would have queued behind a stall is not in the percentiles; `late_issues` counts the reads issued more than one interval late, and a non-zero count means the device did not keep the target rate.
- **File set.** At most 256 files, a seeded random subset when there are more; symlinks and files this user cannot read are skipped.
- **Units.** `latency_ns.p99_99` is nanoseconds; `engine-host --head-p9999-us` takes microseconds (divide by 1000). `head.recommended_ms` is p99.99 + margin only: the head length also adds the start offset of the regions and the interpolation guard (`recommended_frames` already includes the guard).
- **macOS.** `F_NOCACHE` stops new reads from being cached but does NOT evict pages that are already cached: on macOS a repeated run over the same files can read from RAM. Qualify on Linux (`O_DIRECT`).
