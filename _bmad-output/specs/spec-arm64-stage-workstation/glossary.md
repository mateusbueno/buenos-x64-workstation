# Glossary

- **Head:** the opening portion of a sample, kept resident in RAM so a voice can start instantly while the rest streams from NVMe.
- **Streaming (virtual memory):** playing a sample from its RAM head and then from disk-backed buffers that refill ahead of the playhead.
- **Part:** one instrument slot in a performance (a layer or split zone). v1 has 8 and Pro has 16.
- **Voice floor:** the minimum polyphony an engine is guaranteed, even when the shared pool is saturated.
- **Clean-room:** an implementation written without copying third-party source code.
- **UAC2:** USB Audio Class 2, the class-compliant USB audio standard that needs no driver.
- **Milestone 1:** the investor demo of the sample engine on an RK3588 dev board, proving the targets in `performance-targets.md`.
- **73SW:** a 73-key semi-weighted keybed.
- **Multi-contact onset:** organ notes fade in as successive key contacts close, emulating the 9 contacts per key of a B-3.
- **Companion app:** the desktop software that manages, converts, and activates libraries over a mediated USB protocol.
- **Pro:** a later tier with a sequencer, 16 parts, and 32 GB of RAM, on the same SoC and software base.
