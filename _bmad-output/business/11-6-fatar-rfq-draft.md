# DRAFT FOR FOUNDER — does not satisfy Story 11.6 acceptance criteria

Preparation draft for Story 11.6 (Fatar OEM resupply terms and 73SW contact and aftertouch confirmation). Prepared 2026-10-19. Nothing here is a supplier statement. All supplier facts, prices, MOQs, lead times and specifications are blank on purpose. Only the names and values found in the repo are used, each with a citation.

Path shorthand used below:
- EPICS = `_bmad-output/planning-artifacts/epics.md`
- HWP = `_bmad-output/specs/spec-arm64-stage-workstation/hardware-platform.md`
- SPEC = `_bmad-output/specs/spec-arm64-stage-workstation/SPEC.md`
- HWI = `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/HARDWARE-INTERFACES.md`

## 1. Acceptance Criteria (verbatim source: EPICS:6283-6303)

Covers EXT2, EXT10 (item 2), NFR42 (EPICS:6289). Runs on: business, plus a contact check on a sample keybed (EPICS:6291).

- AC1 Given the partner's stock of ~100 Fatar 73SW keybeds, when Fatar (or its authorised distributor) is contacted for OEM supply, then written terms are stored stating: minimum order quantity, unit price at that quantity, lead time, payment terms, model continuity / last-time-buy notice policy, and whether the waterfall E-to-E 73SW variant is available (EPICS:6295-6297).
- AC1b And if Fatar declines or the terms break the margin from Story 11.4, an alternative keybed (model and supplier) is named with the impact on FR73 (calibration table, no code change expected) (EPICS:6298).
- AC2 Given a sample 73SW from the stock, when the hardware partner inspects and measures it (datasheet plus a scope on the contact lines), then a short report states contact count per key (2 or 3), presence or absence of an aftertouch sensor strip, connector pinout and matrix layout, with photos (EPICS:6300-6302).
- AC2b And the result is sent to the software team as input to the keybed MCU firmware (AR40) and the keybed calibration table (FR72, FR73) (EPICS:6303).

## 2. What the repo already says (evidence)

| # | Fact | Source (file:line) |
|---|------|--------------------|
| 1 | Keybed is a Fatar 73SW (semi-weighted) from the partner's stock of ~100 units; assumed waterfall profile, E to E | HWP:31; SPEC:117; SPEC:147 |
| 2 | Resupply beyond the first ~100 units needs Fatar OEM terms (open question) | HWP:34; SPEC:154 |
| 3 | Contact count (2 or 3) and aftertouch presence are unknown; the scan MCU handles both | SPEC:148; SPEC:155; HWI:142 |
| 4 | A different Fatar model needs no code change; the keybed model is a calibration table | HWP:33; EPICS:157 (FR73) |
| 5 | Aftertouch is read and routed when the keybed has it; Settings row only on keybeds with aftertouch | EPICS:156 (FR72) |
| 6 | NFR42: Fatar 73SW from ~100-unit stock; scan >=10 kHz; scan plus transport inside the 5 ms budget | EPICS:304 |
| 7 | Scan MCU: >=10 kHz per contact, every contact reported separately (2 or 3 per key), aftertouch if present, tick >=1 MHz, contact change to frame on the wire <=200 us, link UART >=3 Mbaud or SPI slave plus data-ready IRQ; velocity NOT computed on MCU | HWI:35-44 (section 3 table); EPICS:385 (AR40) |
| 8 | MCU `HELLO` frame carries keybed contact count; `AFTERTOUCH` frame is a 16-bit value (channel or per key) | HWI:75, HWI:77 |
| 9 | SoC applies the velocity curve once; organ engine starts sound on the first contact; raw per-contact times are sent | ARCHITECTURE-SPINE.md:154 (under `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/`) |
| 10 | Partner deliverable #2: keybed contact count and aftertouch presence on the 73SW stock | HWI:142; EPICS:416 (EXT10) |
| 11 | EXT2 definition: Fatar OEM resupply terms (minimum order, lead time) beyond the initial ~100; confirm contact count and aftertouch | EPICS:408 |
| 12 | Margin reference for the "terms break the margin" test: Story 11.4, retail range R$ 17,990-19,990 | EPICS:6231 (heading); the margin threshold itself is NOT in this draft, take it from the 11.4 output |
| 13 | Downstream stories depending on the answer: keybed scan firmware (dependency on contact count/aftertouch) and aftertouch handling | EPICS:2031 (Covers line of the keybed scan story); EPICS:2155 (Covers line of the aftertouch story) |

Not found in the repo (leave blank, do not assume): Fatar contact details, distributor name, any price, MOQ, lead time, payment terms, last-time-buy policy, keybed connector pinout, matrix layout, any alternative keybed model or supplier.

## 3. Request-for-quotation / commitment letter template

> To: [Fatar OEM sales contact or authorised distributor — NAME / ADDRESS TO BE FILLED]
> From: [Founder name, company, address, tax ID]
> Date: [____]
> Subject: OEM supply request — Fatar 73SW keybed (semi-weighted, 73 keys, E to E, waterfall profile)
>
> Dear [____],
>
> We are developing a 73-key stage workstation instrument and intend to use the Fatar 73SW keybed. Our hardware partner currently holds approximately 100 units of this keybed. We are writing to establish OEM supply beyond that initial stock and ask for the written information below.
>
> Please reply in writing, on company letterhead or by signed email, for each item:
>
> 1. Is the 73SW waterfall E-to-E variant available for OEM supply today? Please give the exact ordering code. [____]
> 2. Minimum order quantity. [____]
> 3. Unit price at that minimum quantity, with currency, Incoterm and validity date of the quotation. [____]
> 4. Price breaks at other quantities, if any. [____]
> 5. Lead time from order to shipment, for the first order and for repeat orders. [____]
> 6. Payment terms. [____]
> 7. Model continuity policy: how long do you commit to keep this model in production? [____]
> 8. Last-time-buy notice policy: how much notice do you give before discontinuing, and in what form? [____]
> 9. Technical documentation: datasheet, connector pinout, matrix layout, number of contacts per key, and whether an aftertouch strip is fitted or available as an option. [____]
> 10. Are there other keybed models in your range with a compatible mechanical footprint that you would recommend as an alternative? [____]
> 11. Is your supply direct, or through an authorised distributor for Brazil? Please name it. [____]
>
> We would be grateful for your reply by [date]. This letter is a request for information and quotation and creates no purchase obligation.
>
> Sincerely, [name, title, contact]

## 4. Question list mapped one-to-one to the Acceptance Criteria

| AC | Question to ask | Answer (blank) | Letter item |
|----|-----------------|----------------|-------------|
| AC1 | What is the minimum order quantity? | | 2 |
| AC1 | What is the unit price at that quantity (currency, Incoterm, validity)? | | 3, 4 |
| AC1 | What is the lead time (first order and repeat)? | | 5 |
| AC1 | What are the payment terms? | | 6 |
| AC1 | What is the model-continuity commitment? | | 7 |
| AC1 | What is the last-time-buy notice policy? | | 8 |
| AC1 | Is the waterfall E-to-E 73SW variant available? Ordering code? | | 1 |
| AC1b | Do the terms break the Story 11.4 margin? (compare to the 11.4 output; yes/no and by how much) | | internal, not sent |
| AC1b | If Fatar declines or margin breaks: alternative keybed model and supplier | | 10 |
| AC1b | Impact on FR73 of that alternative (calibration table; confirm "no code change") | | internal, with software team |
| AC2 | Contact count per key (2 or 3)? | | 9 + section 6 check |
| AC2 | Aftertouch sensor strip present or absent? | | 9 + section 6 check |
| AC2 | Connector pinout and matrix layout? | | 9 + section 6 check |
| AC2 | Photos attached? | | section 6 check |
| AC2b | Result sent to the software team (AR40 firmware, FR72/FR73 calibration)? Date and recipient | | section 6 check |

## 5. Required written commitments (blank)

| # | Commitment | Source document (letter / quote / email) | Date | Signed by | Stored at | Done |
|---|------------|-------------------------------------------|------|-----------|-----------|------|
| 1 | Minimum order quantity | | | | | |
| 2 | Unit price at that quantity (with currency, Incoterm, validity) | | | | | |
| 3 | Lead time | | | | | |
| 4 | Payment terms | | | | | |
| 5 | Model continuity / last-time-buy notice policy | | | | | |
| 6 | Availability of the waterfall E-to-E 73SW variant | | | | | |
| 7 | Alternative keybed (model and supplier), only if Fatar declines or margin breaks | | | | | |
| 8 | FR73 impact statement for the alternative | | | | | |
| 9 | Sample-keybed measurement report (see section 6) | | | | | |
| 10 | Report delivered to software team (AR40, FR72, FR73) | | | | | |

## 6. Contact check on a sample 73SW (checklist for the hardware partner)

Method per AC2: datasheet review plus a scope on the contact lines (EPICS:6301). Do not assume any value; record what is measured.

Sample identification
- [ ] Sample taken from the partner's stock (not a new purchase). Unit serial / label / batch marking: [____]
- [ ] Label reads as 73SW? Marking text: [____]
- [ ] Waterfall profile and E-to-E range confirmed on the sample: [yes / no / ____]
- [ ] Datasheet obtained (source and revision): [____]
- [ ] Photos taken: top, underside, PCB both sides, connector(s), any strip, any label (file names: [____])

Contact count per key
- [ ] Contacts per key: [2 / 3 / ____]
- [ ] Same on all 73 keys? Exceptions: [____]
- [ ] Contact-closure order and which line is first / second / third: [____]
- [ ] Scope capture of a slow press of one key showing each contact transition (file: [____])
- [ ] Measured time between contact 1 and contact 2 for a slow press / a fast press (instrument, settings noted): [____ / ____]
- [ ] Same for contact 3 if present: [____]
- [ ] Contact bounce observed (duration, scope capture): [____]
- [ ] Release behaviour: order and timing of contact opening: [____]
- [ ] Scan voltage / pull-up used during the test: [____]

Aftertouch
- [ ] Aftertouch sensor strip physically present: [present / absent]
- [ ] If present: sensor type, connector, output (resistive / analogue range, measured at rest and fully pressed): [____]
- [ ] Per key or one strip for the whole keybed: [____]
- [ ] If absent: confirmed by inspection of underside and connector pin count: [____]

Connector and matrix
- [ ] Connector count, type, pin count, pitch: [____]
- [ ] Pinout table (pin -> function), attached as file: [____]
- [ ] Matrix layout (rows x columns; which keys map to which row/column per contact): [____]
- [ ] Diodes on the PCB or not: [____]
- [ ] Ghosting / scan implications noted for the scan MCU (needs the MCU to read each contact separately, HWI:39): [____]
- [ ] Number of scan lines the keybed requires from the MCU (GPIO budget): [____]

Output of the check
- [ ] Short report written, with photos, signed and dated by the partner: [file / date / name]
- [ ] Report sent to the software team as input to AR40 (keybed MCU firmware) and FR72 / FR73 (calibration table): [date / recipient]
- [ ] `HELLO` contact-count value to be reported by firmware agreed with software team (HWI:75): [____]
- [ ] Open issues: [____]

## 7. Tracking table

| # | Action | Owner | Due | Status | Date sent | Date answered | Notes |
|---|--------|-------|-----|--------|-----------|---------------|-------|
| 1 | Identify Fatar OEM contact or authorised distributor | Founder | | | | | |
| 2 | Send RFQ letter (section 3) | Founder | | | | | |
| 3 | Chase reply | Founder | | | | | |
| 4 | Store written terms (AC1) | Founder | | | | | |
| 5 | Compare terms with Story 11.4 margin | Founder | | | | | |
| 6 | If needed: name alternative keybed and FR73 impact | Founder + software team | | | | | |
| 7 | Ask hardware partner to run section 6 check | Founder | | | | | |
| 8 | Receive measurement report with photos | Hardware partner | | | | | |
| 9 | Forward report to software team (AC2b) | Founder | | | | | |
| 10 | Confirm Story 11.6 criteria met against stored documents | Founder | | | | | |
