# Mac-first work plan — Ladiani Velora 73

Generated from the **Runs on** and **Blocked by** lines in `epics.md` (226 stories). Mac stories can be executed before the RK3588 dev board arrives as long as each story's own prerequisites (earlier Mac stories) are done; skipping a board-only story does not block the Mac stories listed here unless their ACs say so. Latency and xrun budgets are only accepted on the dev board.

## Runs entirely on the Mac (no hardware needed) (80)

| Story | Title | Runs on | Blocked by |
|---|---|---|---|
| 1.3 | Monorepo skeleton with host build and CI | Mac + CI |  |
| 1.8 | Engine core block loop and engine interface | Mac |  |
| 1.9 | Offline renderer and null-test harness | Mac |  |
| 1.10 | Engine plays live on the developer's Mac (macOS host adapters) | Mac |  |
| 1.13 | Import the demo SFZ library subset into bundles | Mac | Story 11.12 (target month 5) |
| 1.14 | 1 TB library resolved from the index without scanning | Mac |  |
| 1.15 | Sample engine plays RAM-resident instruments and passes the sfizz null test | Mac |  |
| 1.19 | Voice pool arbiter with the sample floor | Mac |  |
| 2.1 | Parameter registry and control-thread snapshots for engine settings | Mac |  |
| 2.3 | Organ percussion and key click | Mac |  |
| 2.4 | Vibrato/chorus scanner and tonewheel leakage | Mac |  |
| 2.5 | Multi-contact onset hook in the organ | Mac |  |
| 2.6 | Rotary speaker in the organ voice path | Mac |  |
| 2.7 | Fixed routing topology with insert chains, sends, master and output buses | Mac |  |
| 2.8 | EQ and compressor as inserts and in the master section | Mac |  |
| 2.9 | Chorus/phaser, EP amp/drive and rotary speaker inserts | Mac |  |
| 2.10 | Reverb send | Mac |  |
| 2.11 | Delay send with tempo sync | Mac |  |
| 2.13 | Organ objective A/B against Hammond M-solo renders | Mac (renders and analysis) + M-solo recording rig | Story 11.13 (target month 8) |
| 2.14 | Organ blind ABX listening test with professional keyboardists | Mac | Story 11.14 (target month 11) |
| 2.15 | Design story — Effects surface mockup | Mac (HTML mockup in `mockups/`) |  |
| 2.16 | Design story — organ section of the Part surface | Mac (HTML mockup) |  |
| 3.1 | Versioned MCU frame protocol library and control/LED ID maps | Mac (host tests, fuzzing); compiles for the MCU target |  |
| 4.2 | Versioned Live Set format with parts, scenes and ASSIGN map | Mac |  |
| 4.4 | Scene recall as a parameter-only snapshot swap | Mac |  |
| 4.6 | Setlists, setlist tags and Live Set stepping | Mac |  |
| 4.9 | Edits in memory and Write scopes | Mac |  |
| 4.10 | ASSIGN maps with a global default | Mac |  |
| 4.14 | Design story — bring existing mockups in line with the spines | Mac (HTML mockups) |  |
| 4.15 | Design story — Launcher Grid mockup | Mac (HTML mockup) |  |
| 4.16 | Design story — On-screen Keyboard mockup | Mac (HTML mockup) |  |
| 4.17 | Design story — Scenes surface mockup | Mac (HTML mockup) |  |
| 4.18 | Design story — Part surface mockup | Mac (HTML mockup) |  |
| 4.19 | Design story — Action Sheet and ASSIGN Overlay mockups | Mac (HTML mockups) |  |
| 4.27 | ASSIGN Overlay | Mac (simulator + virtual panel) |  |
| 4.28 | On-screen Keyboard | Mac (simulator) |  |
| 4.29 | Setlist surface | Mac (simulator + engine-host) |  |
| 4.30 | Setlist Browser with search, chips, tags and favorites | Mac (simulator + engine-host + librarian) |  |
| 4.31 | Scenes surface | Mac (simulator + engine-host) |  |
| 4.32 | Write Dialog, Write hold and the Unsaved Indicator | Mac (simulator + engine-host) |  |
| 4.33 | Part surface — timbre, zone and keyboard, level and output | Mac (simulator + engine-host) |  |
| 4.34 | Part surface — sound, envelope and EQ, effects | Mac (simulator + engine-host) |  |
| 4.35 | Settings surface with Knobs · Global Assign | Mac (simulator + engine-host) |  |
| 5.1 | Full SFZ v1 opcode subset in the importer | Mac |  |
| 5.2 | Sample engine plays keyswitches, CC crossfades and basic filters | Mac |  |
| 5.3 | SF2 import into the same instrument model | Mac |  |
| 5.4 | Reference corpus imports and plays with mapping intact | Mac + CI (nightly) | Story 11.12 (target month 5) |
| 5.5 | Category model, localized labels and auto-suggestion | Mac |  |
| 5.7 | Import preflight: batch scan, free space and duplicates | Mac |  |
| 5.12 | Import surface: rows, categories and apply to all | Mac (LVGL simulator against the host librarian) |  |
| 5.13 | Import surface: progress, background and failure states | Mac (LVGL simulator against the host librarian) |  |
| 5.16 | Library surface: search, categories, favorites and recent | Mac (LVGL simulator against the host librarian) |  |
| 5.17 | Choose a timbre for a part with audition | Mac (LVGL simulator + host engine) |  |
| 5.20 | Missing library state and recovery | Mac (LVGL simulator + host engine) |  |
| 6.1 | Signing and content-key hierarchy with dev roots | Mac |  |
| 6.2 | Protected bundle format in formatlib | Mac |  |
| 6.3 | Content-pack tool | Mac |  |
| 6.4 | Librarian verifies and installs protected bundles | Mac |  |
| 6.5 | Engine decrypts protected bundles on I/O edge threads | Mac (dev key-provider port) |  |
| 6.8 | Device registry and content-key vault | Vendor server (developed and tested on the Mac) |  |
| 6.9 | Factory library packaging | Mac | Story 11.17 (target month 12) |
| 7.1 | Single tempo and transport clock on the RT plane | Mac |  |
| 7.2 | Event-processor framework and arpeggiator note generation | Mac |  |
| 7.3 | Swing, latch and arpeggiator performance behaviour | Mac |  |
| 7.4 | Arpeggiator settings in Live Sets and scenes | Mac |  |
| 7.5 | Part surface Arpeggiator section and Live Set tempo | Mac (LVGL simulator + host engine) |  |
| 7.6 | External MIDI clock follower and clock-lost behaviour | Mac (engine core and offline renderer with synthetic clock streams) |  |
| 7.8 | Top Bar tempo, clock source and CLOCK LOST display | Mac (LVGL simulator + host engine with synthetic clock) |  |
| 9.4 | Design story — Velora Studio footer, dialog, banner and Backup & Restore | Mac (HTML mockups next to `mockups/key-screen-studio.html`) |  |
| 9.5 | Velora Studio app shell, offline | Mac (also builds on Windows and Linux in CI) |  |
| 9.8 | Live Set view: scenes and part strips | Mac |  |
| 9.13 | Setlists view in Velora Studio | Mac |  |
| 9.14 | Library management and heavy SFZ conversion on the computer | Mac (formatlib FFI builds on Windows and Linux in CI) |  |
| 9.22 | Velora Studio accessibility audit | Mac (VoiceOver), Windows (NVDA), Linux (Orca, best effort) |  |
| 10.7 | PT-BR and ES locale catalogs and language selection | Mac (SDL simulator and Velora Studio) |  |
| 10.8 | Pseudo-localization QA and truncation rules | Mac (SDL simulator and Velora Studio; CI screenshot job) |  |
| 12.1 | Clean-room 6-operator FM voice in the engine core | Mac |  |
| 12.2 | FM null test against an unshipped reference oracle | Mac + CI |  |
| 12.6 | Clean-room virtual-analog voice in the engine core | Mac |  |
| 12.8 | FM and VA timbre parameters in the versioned formats | Mac |  |

## Starts on the Mac, finished/verified on the dev board or prototype (89)

| Story | Title | Runs on | Blocked by |
|---|---|---|---|
| 1.4 | Minimal Yocto dev image for the dev board | Linux build host or ARM64 Linux VM on the Mac; image runs on the dev board |  |
| 1.12 | formatlib lossless block-indexed bundle | Mac (bundle format); verified on the board later |  |
| 1.16 | Instrument heads decoded into locked RAM at load | Dev board (p99.99 measured on the target SSD); logic prototyped on Mac |  |
| 1.17 | Streaming from NVMe ahead of the playhead | Dev board (io_uring/O_DIRECT); a portable reader prototypes on Mac |  |
| 1.18 | Parallel voice render on the three isolated cores | Mac (correctness), dev board (timing budget) |  |
| 1.20 | Timestamped event path from a USB MIDI keyboard | Dev board (ALSA DLL); CoreMIDI path already on Mac from 1.10 |  |
| 1.21 | In-RAM diagnostics counters and latency histogram | Mac + dev board |  |
| 2.2 | Tonewheel generator and drawbar voices at full polyphony | Mac (DSP, null and unit tests); dev board (CPU cost) |  |
| 2.17 | Device UI shell on the LVGL simulator and the dev board | Mac (LVGL SDL simulator 1024×600); dev board (DRM/KMS on the Story 1.5 panel) |  |
| 2.18 | Design tokens, Stage and Studio themes and the contrast gate | Mac (generator, simulator and CI); dev board (visual check on the Story 1.5 panel) |  |
| 2.19 | Hint Bar, List Row, Segmented Control and the Mac virtual panel | Mac (LVGL SDL simulator 1024×600); dev board |  |
| 2.20 | Effects surface — part inserts | Mac (simulator + engine-host); dev board |  |
| 2.21 | Effects surface — sends, master and outputs | Mac (simulator + engine-host); dev board |  |
| 3.2 | Prototype board bring-up | Linux build host or ARM64 Linux VM (image); prototype board | Story 11.7 (target month 7) |
| 3.5 | Keybed reader on the SoC with drift-tracked device time and contact_k events | Mac (reader core against recorded frames and a simulated MCU); dev board / prototype board with the keybed MCU |  |
| 3.6 | Attack and release velocity with selectable curves | Mac (curve math, tests); prototype board (real keybed) |  |
| 3.7 | Aftertouch read and routed when the keybed has it | Mac (routing tests with simulated frames); prototype board if the keybed has aftertouch | Story 11.6 (target month 7) |
| 3.10 | Panel reader on the SoC and the Mac virtual panel | Mac (reader core + virtual panel from keyboard and a USB MIDI control surface); dev board / prototype board with the panel MCU |  |
| 3.11 | Pedals with half-pedal calibration and expression | Mac (calibration math, simulated input); prototype board (real pedals) |  |
| 3.12 | SoC-driven LED state and toggle state with LED per state | Mac (simulator + virtual panel with on-screen LED view); prototype board |  |
| 4.1 | Tier profile read at boot | Mac (profile logic, override flag); prototype board (strap / OTP) |  |
| 4.3 | Live Set load with 8 parts, zones, layers and splits | Mac (render tests); dev board |  |
| 4.5 | Seamless Live Set switching with background load | Mac (logic, renders); dev board (load times) |  |
| 4.7 | Next Live Set preload within the head budget | Dev board (head budget, timing); logic on Mac |  |
| 4.8 | Seamless switching render suite | Mac (offline renderer, CI); dev board (live run) |  |
| 4.11 | Live Set favorites and Recent in the catalog | Mac (host process); packaged as a systemd unit for the board |  |
| 4.12 | Fader bindings with pickup | Mac (virtual panel); prototype board |  |
| 4.13 | Encoder bindings for PART, ALL and ASSIGN | Mac (virtual panel); prototype board |  |
| 4.20 | Performance surface — Top Bar, Live Set Hero and Scene strip | Mac (simulator + engine-host); dev board |  |
| 4.21 | Performance surface — Part Strips, Volume Bars and Status Bar | Mac (simulator + engine-host); dev board |  |
| 4.22 | Navigation rules, MENU and the Launcher | Mac (simulator + virtual panel); prototype board |  |
| 4.23 | PARTS/SCENES toggle and the part/scene buttons | Mac (simulator + virtual panel); prototype board |  |
| 4.24 | Pickup Ghost and the Status Bar takeover | Mac (simulator + virtual panel faders); prototype board |  |
| 4.25 | DRAWBARS/MIXER toggle with the Organ Card and Drawbar Columns | Mac (simulator + virtual panel); prototype board |  |
| 4.26 | PART/ALL/ASSIGN toggle and encoder LED rings | Mac (simulator + virtual panel); prototype board |  |
| 4.36 | Footswitch function | Mac (virtual panel); prototype board |  |
| 4.37 | Accidental-press guards and Touch on Performance | Mac (simulator + virtual panel, touch emulation); prototype board (touch panel) |  |
| 5.6 | Librarian libraries catalog, import API and atomic publish | Mac (service runs as a host process); packaged as a systemd unit for the board |  |
| 5.8 | Import execution survives drive removal and power loss | Mac (disk-image detach); verified on the dev board with a physical drive |  |
| 5.14 | "USB drive found" notice | Mac (LVGL simulator with a simulated drive event); dev board (real hotplug) |  |
| 5.19 | Favorites by long press and from the Part surface | Mac (LVGL simulator); board timing check on the dev board or prototype |  |
| 5.21 | Storage & Library: free space, installed libraries and removal | Mac (LVGL simulator + host librarian and engine); dev board check |  |
| 6.11 | Read-only root filesystem and journaled data partition | Linux build host or ARM64 Linux VM on the Mac (image build); dev board (verification) |  |
| 6.12 | RAUC A/B slots with rollback on failed boot | Linux build host or ARM64 Linux VM (bundle build); dev board (install and rollback) |  |
| 6.13 | Dev and factory-test image variants with per-root keyrings | Linux build host or ARM64 Linux VM + CI; dev board and prototype (unfused) |  |
| 6.14 | Production image with secure boot and debug lockdown | Prototype (sacrificial units for fusing tests) |  |
| 6.15 | OTP anti-rollback counter for security releases | Fused dev board (sacrificial) or prototype |  |
| 7.7 | Clock source setting and DIN/USB clock inputs | Mac (sysd config and CoreMIDI USB clock); dev board or prototype (DIN clock) |  |
| 7.9 | Arpeggiator timing acceptance on the board | Mac + CI (offline tests); dev board or prototype (external-clock rig) |  |
| 8.1 | sysd as the sole installer, from a USB-A stick | Dev board (Mac for sysd unit tests with a mocked RAUC D-Bus) |  |
| 8.3 | Install flow on the device | Mac (LVGL SDL simulator with a mocked `sysd`); dev board (end-to-end) |  |
| 8.4 | MCU firmware manifest in the A/B image and boot-time version check | Prototype (keybed and panel MCUs with the Story 3.4 bootloader); Mac for the boot-ordering and failure-reporting logic against a simulated MCU |  |
| 8.5 | Format versions N/N−1 and migrations only after the slot is marked good | Mac + CI (readers, corpus); dev board (mark-good hook) |  |
| 8.7 | Boot Screen with drawbar animation and reduced motion | Mac (LVGL SDL simulator); dev board (DRM/KMS timing) |  |
| 8.8 | Journaled Boot-into pointer | Mac (control-thread logic); dev board (write monitoring) |  |
| 8.9 | Boot into setting and restoring the target | Mac (SDL simulator + host engine); dev board |  |
| 8.12 | Startup reconcile of interrupted imports: fault injection and boot bound | Mac (librarian on host with a temp `/data`); dev board |  |
| 9.1 | USB-C gadget composition with class-compliant MIDI | Dev board (USB-C OTG/peripheral port); Mac, Windows and Linux hosts for enumeration tests |  |
| 9.2 | USB audio interface that follows the codec clock | Dev board with the I2S codec path (prototype with PCM3168A for final acceptance); Mac, Windows, Linux hosts |  |
| 9.3 | sysd ws.v1 bridge on USB NCM with a single open port | Dev board; Mac (bridge logic tested against the host engine on localhost) |  |
| 9.6 | Pairing Velora Studio with the unit | Mac (Studio + device UI in the SDL simulator against host services); dev board (end-to-end over USB) |  |
| 9.7 | Connection states: version mismatch, disconnect and reconnect | Mac (against host engine with version overrides); dev board |  |
| 9.9 | Part Editor layout with Studio Knobs | Mac (against host engine); dev board (end-to-end) |  |
| 9.10 | Live edit state, stale edits and disconnect mid-edit | Mac (against host engine); dev board (end-to-end) |  |
| 9.11 | Add a timbre to an empty part from Velora Studio | Mac (against host engine and librarian); dev board |  |
| 9.12 | Write to Velora with the device's scopes | Mac (against host engine); dev board |  |
| 9.15 | Library transfer to the unit | Dev board (Mac for protocol tests against a host librarian) |  |
| 9.16 | Backup to a .velorabackup file | Mac (against host engine); dev board |  |
| 9.17 | Restore with Merge or Replace, and offline backup editing | Mac (against host engine); dev board |  |
| 9.18 | Export selected Live Sets and timbres (individually or as a bundle) | Mac (against host engine); dev board |  |
| 9.19 | Import partial exports with per-item merge decisions | Mac (against host engine); dev board |  |
| 9.20 | Send firmware from Velora Studio | Dev board (Mac for UI against a mocked sysd) |  |
| 9.21 | Device view and diagnostics export | Mac (against host engine); dev board |  |
| 10.1 | Settings surface with the seven canonical sections | Mac (LVGL SDL simulator with host `sysd` config service); dev board |  |
| 10.2 | Settings › Keyboard & Pedals and MIDI & Clock rows | Mac (LVGL SDL simulator with host `sysd` config, host engine and the virtual panel); prototype board (real keybed and pedals) |  |
| 10.3 | Theme selection on the device | Mac (LVGL SDL simulator, Velora Studio); prototype display for the final visual check |  |
| 10.4 | Screen and panel LED brightness | Mac (UI in the SDL simulator with a simulated backlight); prototype (backlight and LED PWM measured) |  |
| 10.5 | Brightness quick path: hold EXIT and turn VALUE | Mac (SDL simulator with keyboard-mapped EXIT/VALUE); prototype (panel) |  |
| 10.6 | Large text on Performance and Reduced motion | Mac (LVGL SDL simulator); prototype for the 1.5 m legibility check |  |
| 11.11 | SSD qualification for streaming latency and endurance | Dev board (measurements); approval is a business decision |  |
| 11.19 | Brand assets integration: final typeface, app icon and boot artwork | Mac (LVGL SDL simulator and Velora Studio builds) and dev board |  |
| 12.3 | Import 6-OP SysEx banks and single voices from files (USB-A and Velora Studio) | Mac (formatlib, librarian, offline renderer, LVGL simulator and Velora Studio); dev board for the USB-A path |  |
| 12.4 | Receive 6-OP SysEx over USB MIDI and DIN | Mac (engine host with a virtual MIDI port, librarian, LVGL simulator); dev board or prototype for USB MIDI and DIN |  |
| 12.5 | FM voices in the shared pool with a 128-voice floor | Mac (arbiter logic); prototype unit or dev board (voice budget) |  |
| 12.7 | VA voices in the shared pool with a 64-voice floor | Mac (arbiter logic); prototype unit or dev board (voice budget) |  |
| 12.9 | Init and factory FM/VA timbres in the Library | Mac (catalog and Library logic); verified on a unit |  |
| 12.11 | Part surface Sound section for FM timbres | Mac (LVGL UI on host); verified on a unit |  |
| 12.12 | Part surface Sound section for VA timbres | Mac (LVGL UI on host); verified on a unit |  |
| 12.13 | Velora Studio Part Editor Sound sections for FM and VA | Mac (Velora Studio) + a connected unit |  |

## Business or design work (no device) (21)

| Story | Title | Runs on | Blocked by |
|---|---|---|---|
| 5.10 | Design story — Notice Toast mockup | Design (HTML mockup in the UX folder) |  |
| 5.11 | Design story — Category Picker mockup | Design (HTML mockup in the UX folder) |  |
| 5.18 | Design story — Action Sheet library variants (missing library, remove library, install, pairing) | Design (HTML mockup in the UX folder) |  |
| 11.1 | Trademark clearance and INPI filing for Ladiani and Velora | Business (no device) |  |
| 11.2 | Register domains and social handles | Business (no device) |  |
| 11.3 | Run the Brazil keyboardist survey | Business (no device) |  |
| 11.4 | Tax advisor validation of margin at R$ 17,990–19,990 | Business (no device) |  |
| 11.5 | Price and feature-priority decision from survey and tax results | Business (no device) |  |
| 11.6 | Fatar OEM resupply terms and 73SW contact and aftertouch confirmation | Business (no device); contact check on a sample keybed |  |
| 11.7 | SoM longevity commitment and board-level partner deliverables | Business (no device) |  |
| 11.8 | Panel, display, LED and legend specifications agreed with partners | Business (no device); measurements on partner samples |  |
| 11.9 | Power-fail timing, codec and thermal measurements from the partner | Business (no device); measurements on the prototype board |  |
| 11.10 | Inmetro power supply requirement and IEC 62368-1 test plan | Business (no device) |  |
| 11.12 | License check of the vendored sfizz parser and dependencies | Business (no device) |  |
| 11.13 | Hammond M-solo drawbar CC/NRPN numbers for automated organ A/B | Business (no device); verification with the M-solo |  |
| 11.14 | Tonewheel B-3 and rotary cabinet reference recordings and ABX panel | Business (no device) |  |
| 11.15 | Final typeface licensed and delivered by a designer | Business (no device) |  |
| 11.16 | Final logo, symbol, app icon and avatar delivered by a designer | Business (no device) |  |
| 11.17 | Factory library content plan and source delivery | Business (no device) |  |
| 11.18 | Direct-sales channel for the first ~100 units | Business (no device) |  |
| 12.10 | Design story — FM and VA Sound sections and not-available state | Design (no device) |  |

## Needs the dev board, prototype or other hardware (36)

| Story | Title | Runs on | Blocked by |
|---|---|---|---|
| 1.1 | PREEMPT_RT latency spike on mainline RK3588 under load | Dev board |  |
| 1.2 | Audio output bring-up on the dev board | Dev board |  |
| 1.5 | MIPI DSI 7" panel spike on mainline RK3588 | Dev board |  |
| 1.6 | Secure-boot chain selection and signed image on an unfused board | Dev board |  |
| 1.7 | Fused dev board boots signed and derives a per-device key | Dev board |  |
| 1.11 | Engine runs live on the dev board | Dev board |  |
| 1.22 | 24-hour soak gate on the dev board | Dev board |  |
| 1.23 | Milestone 1 investor demo proving NFR1–NFR4 | Dev board |  |
| 2.12 | Full effects load within the latency and dropout budget | Dev board |  |
| 3.3 | Keybed MCU firmware with raw per-contact timestamps | MCU dev kit of the candidate family wired to a 73SW keybed harness; then the prototype board | Story 11.6 (target month 7), Story 11.7 (target month 7) |
| 3.4 | MCU dual-bank bootloader and reflash from the SoC | MCU dev kit + dev board (link over UART); then the prototype board | Story 11.7 (target month 7) |
| 3.8 | Panel MCU firmware — inputs | MCU dev kit wired to a panel harness (encoders, faders, buttons, pedals); then the prototype board | Story 11.7 (target month 7), Story 11.8 (target month 9) |
| 3.9 | Panel MCU firmware — SoC-driven LEDs | MCU dev kit with LED harness; then the prototype board | Story 11.7 (target month 7), Story 11.8 (target month 9) |
| 3.13 | PCM3168A codec as clock master with the ALSA machine driver | Prototype board | Story 11.9 (target month 10) |
| 3.14 | Balanced outputs and headphone output | Prototype board |  |
| 3.15 | Stereo line input mixed into the master | Prototype board |  |
| 3.16 | Hardware mute for pop-free power-on and power-off | Prototype board |  |
| 3.17 | MIDI DIN in and out | Dev board with a DIN breakout (opto-isolated) on a UART; then the prototype board |  |
| 3.18 | Power-fail fade and write freeze within the hold-up window | Prototype board | Story 11.9 (target month 10) |
| 3.19 | Key-to-sound latency on the prototype | Prototype board |  |
| 3.20 | 24-hour thermal run in the closed chassis | Prototype board in the closed chassis, climate chamber or controlled room at 40 °C | Story 11.9 (target month 10) |
| 4.38 | First playable unit — full show run-through | Prototype board |  |
| 5.9 | Sandboxed librarian and read-only USB-A drives on the board | Dev board |  |
| 5.15 | 300 GB SSD library imports and plays end to end | Dev board or prototype |  |
| 6.6 | OP-TEE content-key trusted application on the board | Dev board (fused, from Story 1.7) |  |
| 6.7 | A copied or tampered library does not play elsewhere | Dev board (two fused boards) |  |
| 6.10 | Production signing root ceremony in an offline HSM | Offline signing laptop + HSM |  |
| 6.16 | Factory provisioning station | Factory station (Linux PC with the partner's programming fixture) + prototype units | Story 11.7 (target month 7) |
| 6.17 | SoM swap re-provisioning | Factory station (repair mode) + prototype |  |
| 8.2 | Production release signing and per-root update bundles | Linux build host or ARM64 Linux VM + CI; offline HSM host; fused production unit |  |
| 8.6 | A firmware update adds a new engine on a v1 unit | Dev board |  |
| 8.10 | Boot to playable in ≤10 s with a 1 TB library | Dev board (acceptance); prototype later re-runs it |  |
| 8.11 | Power-fail acceptance: brown-out recovery and scope verification | Prototype (POWER_FAIL GPIO and supercap hold-up) with a scope and a programmable supply |  |
| 8.13 | Power-cut rig: 100 cuts during update and 100 during import | Prototype or dev board with a controllable power relay and serial/network observation (dev image) |  |
| 8.14 | Kernel LTS migration plan | Documentation; later execution on the dev board and prototype |  |
| 12.14 | FM and VA firmware update reaches shipped v1 units | Production v1 unit (release acceptance) |  |
