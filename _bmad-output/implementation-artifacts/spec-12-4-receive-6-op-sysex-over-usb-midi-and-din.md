---
title: 'Story 12.4: Receive 6-OP SysEx over USB MIDI and DIN'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: 'e2ebc92'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/engine/core/include/velora/core/ump.hpp'
  - '{project-root}/engine/host/include/velora/host/midi_ingest.hpp'
  - '{project-root}/engine/host/src/midi_ingest.cpp'
  - '{project-root}/engine/host/include/velora/host/sysex_receiver.hpp'
  - '{project-root}/engine/host/src/sysex_receiver.cpp'
  - '{project-root}/formatlib/include/velora/format/sysex_6op.hpp'
  - '{project-root}/formatlib/src/sysex_6op.cpp'
  - '{project-root}/ui/include/velora/ui/components/notice_toast.hpp'
  - '{project-root}/ui/include/velora/ui/navigation_controller.hpp'
  - '{project-root}/ui/src/navigation_controller.cpp'
  - '{project-root}/engine/host/tests/sysex_receiver_test.cpp'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists with a hardware FM synth (such as a DX7) or a librarian app want to send a 6-OP SysEx dump to the Velora over USB MIDI or a DIN cable, so they can move sounds straight from gear or a computer without requiring a USB flash drive.

**Acceptance Criteria (from Epics - Story 12.4):**
1. **Real-Time Safety on MIDI Input Path (FR121, NFR16, NFR17):**
   - SysEx arriving on USB MIDI or MIDI DIN in is intercepted on the input thread without parsing or buffering beyond handing 64-bit UMP packets (MT 0x3) to a lock-free queue for the control thread.
   - The audio render thread never parses, allocates, or touches SysEx data.
   - A dump arriving during performance causes zero xruns and changes no sound.
2. **Reassembly & Story 12.3 Validation:**
   - The control thread reassembles incoming UMP SysEx packets into a byte stream off the audio threads.
   - Reassembled messages with the Yamaha 6-OP header are validated using the Story 12.3 parser (`format::parse_sysex_memory`).
   - A valid single voice (163 bytes) or 32-voice bulk dump (4104 bytes) raises a Notice Toast `"6-OP SysEx received: n voices · Import?"` that never blocks play.
   - Nothing is imported until the player confirms; confirming opens the same Import rows, Category Picker (default Synth Poly), and naming rules as Story 12.3.
   - A malformed or truncated dump is discarded with the copy `"SysEx received but damaged. Nothing was imported."` and increments a diagnostics counter (`sysex_damaged_count`).
   - SysEx that is not a 6-OP voice dump (non-Yamaha or non-6-OP format) is ignored silently without a notice.
3. **Performance & Multi-Packet Concurrency:**
   - A 32-voice dump sent at full DIN speed, and the same dump split across several USB MIDI packets while 256 voices sound, reassembles exactly, imports as 32 timbres equal to the file import of the same bank, and the engine counters show zero xruns.

</intent-contract>
