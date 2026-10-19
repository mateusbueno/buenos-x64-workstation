# organ-ab: Hammond M-solo CC/NRPN map (Story 11.13, EXT6, NFR20)

Scaffold only. **No CC/NRPN number is filled in.** Every value must be transcribed from the
M-solo manual, Drawbar Data List (pp. 64-65, image only). Do not guess or recall numbers.

## Files
- `mapping.template.json` - one entry per required control (9 upper drawbars, percussion on/volume/decay/harmonic,
  vibrato/chorus, key click, leakage, rotary slow/fast/stop), all message fields `null`/`"TODO"`, plus a `source` block.
- `validate_mapping.py` - stdlib checker (`--self-test` available).
- `send_and_record.py` - sends each control across its range, records audio, writes a verification log skeleton.
  `--dry-run` needs no extra packages.

## Workflow
1. `cp mapping.template.json mapping.json`
2. Fill `source` (manual title, edition, pages 64-65), `midi_channel`, and for each control:
   - `type`: `CC` (set `number`, leave `nrpn_*` null) or `NRPN` (set `nrpn_msb`/`nrpn_lsb`, leave `number` null)
   - `value_bits` (7 or 14), `value_min`, `value_max`
   - `position_mapping`: list of `{"position": label, "value": int}`. Drawbars use labels `"0"`..`"8"`;
     `percussion_on` uses `"off"`/`"on"` (see `positions_expected`). Other labels are free-form; use the manual's names.
   - The template has one control each for vibrato/chorus, key click, leakage, rotary slow/fast/stop. If the data list
     splits one of them into several messages, add extra controls (they are validated too).
   - 14-bit CC assumes the standard MSB/LSB pairing (number, number+32); NRPN sends CC99/98 select, CC6 (+CC38), then RPN null.
     If the manual says otherwise, extend `control_messages()` and note it in `source.notes`.
3. `python3 validate_mapping.py mapping.json` until it prints `VALID`.
4. Preview: `python3 send_and_record.py --mapping mapping.json --dry-run`
5. Verify with the M-solo connected (`pip install mido python-rtmidi sounddevice soundfile`):
   `python3 send_and_record.py --list-ports`, then
   `python3 send_and_record.py --port "<name>" --audio-device "<interface>" --out-dir runs/verify1`
   (add `--controls drawbar_8,key_click` or `--sweep-steps 8` as needed). Audio: 48 kHz, 24-bit WAV per message in `runs/.../audio/`.
6. Fill `verification_log.json` per entry (`expected_control_changed`, `observed_change`, `mismatch`, `correction`, `verdict`)
   after listening/inspecting spectra. Fix `mapping.json` for mismatches and re-run those controls.
7. Full registration by script:
   `python3 send_and_record.py --mapping mapping.json --port "<name>" --registration "88 8000 000" --percussion on --play-note 60 --audio-device "<interface>"`
   (digits = 16', 5-1/3', 8', 4', 2-2/3', 2', 1-3/5', 1-1/3', 1'). Add `--dry-run` to only print the messages.
8. Commit `mapping.json` and the stored verification log (`runs/` is git-ignored; copy the log you keep elsewhere, e.g. `verification/`).

## Acceptance criteria supported (Story 11.13)
- AC1: committed machine-readable map with CC/NRPN number, value range, value-to-position mapping for every required control,
  and cited source (manual edition, page) - steps 1-3; enforced by `validate_mapping.py`.
- AC2: script sends each mapped message across its range and records audio; each mapping confirmed by audible/spectral change,
  mismatches corrected, log stored - steps 4-6.
- AC3: a full registration such as `88 8000 000` with percussion on set by script and rendered without touching the instrument - step 7.
  Story 2.13 reuses this mapping and these helpers.

## Self-tests (no hardware)
`python3 validate_mapping.py --self-test`
