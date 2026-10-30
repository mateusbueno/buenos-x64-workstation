# Rejected Approaches

These were rejected in the forge session. Do not reopen them without new evidence.

| Approach | Reason |
|---|---|
| Port the Kronos OS to a 64-bit Linux kernel | OA.ko, the Kronos synth engine (~14 MB), is a closed i386 kernel module, and an x86_64 kernel cannot load it. The source tree shared by Korg holds only GPL parts (kernel 2.6.32.11, RTAI 3.8.1, toolchain, patches). |
| PAE / 0.5G-3.5G split on the 32-bit Kronos | OA.ko maps its heap into the kernel's virtual space, so the hard ceiling stays below 4 GB. marcan reached about 2277 MiB. |
| Kronos in a 32-bit VM on a 64-bit host | The guest is still 32-bit, so the cap is unchanged. |
| Run OA.ko as a 32-bit process on a 64-bit kernel | The ceiling is about 3.8 GB, short of the 8 GB goal. |
| Serve Kronos streaming from host RAM | The bottleneck is head RAM, not streaming speed. |
| Shrink Kronos preload heads | It would mean rewriting Korg's factory preload. |
| Emulate OA.ko on ARM64 | Realtime x86/SSE DSP cannot be emulated within audio latency. |
| "Kronos 4" pitch to Korg | Korg owns the OA.ko source, so the port is their in-house job. A proof of concept that bypasses their anti-tamper would be a liability. |
| Fully modeled acoustic piano | Rejected on 2026-10-30. It has no null-test oracle, so 88 notes of calibration depend on listening alone, a multi-year effort. Pedal-down cost is estimated at about 3 GFLOP/s, against about 0.6 for a resonance layer (both unmeasured). Samples plus modeled resonance keep the streaming differentiator. |
| Unbreakable encryption | The key lives on the device, and the Kronos crypto was broken publicly. The only realistic goal is deterrence. |
