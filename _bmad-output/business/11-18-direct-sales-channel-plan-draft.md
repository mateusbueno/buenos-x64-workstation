# DRAFT FOR FOUNDER — does not satisfy Story 11.18 acceptance criteria

Plan skeleton for the direct-sales channel for the first ~100 Ladiani Velora 73 units in Brazil. Prepared 2026-10-19. It maps to the four Given/When/Then blocks of Story 11.18 (`_bmad-output/planning-artifacts/epics.md`:6562-6595; covers EXT15, NFR47, NFR52). No vendors, fees, tax rules or legal conclusions are asserted: every external fact is "to confirm" with a named owner. Blank cells are for the founder.

Repo facts used (paths relative to repo root):
- Channel: hybrid, first ~100 units direct to musicians, retail only after scale (`_bmad-output/specs/spec-arm64-stage-workstation/go-to-market.md`, "Channel"); retail excluded for the first ~100 units (`epics.md`:320, NFR52).
- Price band v1 R$ 17,990-19,990 (go-to-market.md, "Market and price"); the actual price comes from Story 11.5 (`epics.md`:6257). Sales taxes ~25-35% quoted in go-to-market.md are marked unverified there.
- Domain: ladiani.com.br from Story 11.2 (draft `11-2-domains-handles-checklist-draft.md`).
- Survey waitlist consent from Story 11.3 (`epics.md`:6194); LGPD rules NFR47 (`epics.md`:312).
- SoM-swap re-provisioning AR33 (`epics.md`:372). Unit weight ~20 kg (`epics.md`:6577, Story 11.18 AC2).
- Brazilian tax/legal inputs also come from Story 11.4 (tax report) - not yet read here; to confirm.

## 0. Prerequisites and dependencies

| Item | Status | Owner | Note |
|---|---|---|---|
| Price decision (Story 11.5) | open | founder | price incl./excl. taxes: ____ |
| Domain ladiani.com.br registered (Story 11.2) | open | founder | see 11-2 draft |
| Company tax registration (CNPJ, state/municipal registrations, tax regime) | to confirm | accountant | which regime and which registrations enable selling goods + issuing NF-e: ____ |
| Tax report (Story 11.4) | to confirm | founder/accountant | ____ |
| Product available to ship (month 18 launch; first playable units month 12) | per go-to-market.md timeline | founder | ship date: ____ |

## 1. AC1 - Storefront, payment, invoicing, legal pages

### 1.1 Storefront
Requirement: storefront on ladiani.com.br taking orders and pre-orders (epics.md:6576).

| Option | Pros / cons (to research) | Cost (to confirm with provider) | Decision |
|---|---|---|---|
| Hosted e-commerce platform | ____ | ____ | |
| Self-built / open-source shop | ____ | ____ | |
| Order form + manual invoicing (only if volume allows ~100 units) | ____ | ____ | |

Checks for any option (all to confirm): supports pre-orders with deposit or full prepayment; supports Brazilian payment methods below; supports NF-e integration or export; hosts/publishes legal pages; LGPD-compliant data handling; data export for the order log (section 6).
Note: the Shopify connector available in this tooling is not a chosen or recommended platform; it is an example only. Platform fit, pricing and Brazilian payment/NF-e support: to confirm with the vendor's own documentation.

### 1.2 Payment options (as decided)
Epics names Pix, boleto, card in instalments "as decided" (epics.md:6576).

| Method | Include? | Provider / acquirer | Fee structure (to confirm with provider) | Settlement time (to confirm) | Refund mechanism (to confirm) |
|---|---|---|---|---|---|
| Pix | ____ | ____ | ____ | ____ | ____ |
| Boleto | ____ | ____ | ____ | ____ | ____ |
| Card, instalments (number of instalments, who bears interest) | ____ | ____ | ____ | ____ | ____ |
| Bank transfer / other | ____ | ____ | ____ | ____ | ____ |

Open questions: deposit vs full payment for pre-orders; behaviour when ship date slips; chargeback handling. Owner: founder, with payment provider.

### 1.3 Invoicing and NF-e (CONFIRM WITH ACCOUNTANT - nothing below is advice)
Requirement: NF-e issued for each sale under the company's tax registration (epics.md:6576).

| Question for accountant | Answer | Source / date |
|---|---|---|
| Is NF-e required for each online sale of a physical instrument, and what is the company's emitter setup (certificate, series, software or emitter service)? | to confirm with accountant | |
| Which tax regime and which taxes/rates apply to the sale (ICMS, IPI, PIS/COFINS, state-to-state rules for out-of-state buyers, DIFAL)? Feeds price in Story 11.5. | to confirm with accountant | |
| Product classification (NCM code; the repo only notes NCM 9207.10.10 as a customs code for electric keyboard instruments in `11-10-inmetro-psu-prep-draft.md`, not verified for this product) | to confirm with accountant | |
| Pre-order: when is the invoice issued (at payment, at shipment)? How are deposits recorded? | to confirm with accountant | |
| Refund/withdrawal: NF-e cancellation vs return invoice rules and deadlines | to confirm with accountant | |
| Invoice must accompany goods in transit: format (DANFE) and carrier requirements | to confirm with accountant / carrier | |
| Invoice delivery to buyer (email) and record keeping period | to confirm with accountant | |
| Integration option (storefront-to-NF-e emitter vs manual issuing) | to confirm with accountant | |

### 1.4 Legal pages
Requirement: terms of sale, privacy policy (LGPD), right of withdrawal 7 days per CDC art. 49 (epics.md:6576).

| Document | Draft owner | Reviewed by | Status | Notes |
|---|---|---|---|---|
| Terms of sale | ____ | lawyer: to confirm | not started | must cover pre-order terms, delivery, warranty reference |
| Privacy policy (LGPD) and consent text | ____ | lawyer: to confirm | not started | also gates any data collection on the domain (Story 11.2 AC3, epics.md:6190-6192) |
| Right of withdrawal (7 days, CDC art. 49) page and process | ____ | lawyer: to confirm | not started | who pays return shipping for a ~20 kg unit: ____ (legal position to confirm) |
| Cookie notice (if analytics used) | ____ | lawyer: to confirm | not started | |

Source to cite once read: CDC (Lei 8.078/1990) art. 49 - official text at planalto.gov.br: to confirm (not fetched here).

### 1.5 End-to-end test order (gate before launch)
Checklist (epics.md:6577): place a test order; pay it with each enabled method; confirm NF-e is issued (homologation vs production environment: to confirm with accountant); refund it; confirm NF-e handling and payment reversal; record date, tester and result.

| Step | Pix | Boleto | Card | Date | Result |
|---|---|---|---|---|---|
| Order placed | | | | | |
| Paid | | | | | |
| Invoice issued | | | | | |
| Refunded | | | | | |

## 2. AC2 - Logistics for a ~20 kg instrument

| Item | Requirement | Option / choice | Evidence needed |
|---|---|---|---|
| Carrier | Contracted, insured, door-to-door (epics.md:6581) | carrier(s) compared: ____ ; selected: ____ | contract, insurance terms and declared-value rule: to confirm with carrier |
| Insurance | Insured delivery; claim process | insurer/carrier: ____ | policy: ____ |
| Packaging | Shipping box passes a drop test with a packed unit | test standard/height (to confirm with carrier or packaging supplier): ____ | drop-test report with date, photos, result |
| Pricing by region | Shipping cost and lead time per region published at checkout | rate table by region/CEP: ____ | published table; checkout shows it |
| Who pays shipping | included / charged / mixed | ____ | |
| Returns freight (withdrawal and RMA) | defined cost bearer | ____ | |
| Pickup option (local, founder) | optional | ____ | |
| Damaged-on-arrival procedure (photo evidence, refusal on delivery) | | ____ | |

## 3. AC3 - Warranty, RMA, spare parts, support

Context: new brand trust is the main barrier (survey Q21, epics.md:6583; the survey itself is Story 11.3).

### 3.1 Warranty terms (written)
| Element | Value | Source |
|---|---|---|
| Legal warranty | 90 days (as stated in the story) | epics.md:6585; legal basis CDC art. 26 - to confirm with lawyer (not fetched here) |
| Contractual warranty (added) | ____ months | founder decision |
| What is covered / excluded | ____ | lawyer to confirm |
| Who pays freight in warranty | ____ | |
| Warranty start date (invoice date vs delivery date) | ____ | lawyer to confirm |

### 3.2 RMA process
Required steps (epics.md:6585): contact, diagnosis, pickup, repair or swap, SoM-swap re-provisioning per AR33 (factory provisioning station links serial to SoM ID and re-provisions on SoM swap, epics.md:372).

| Step | Who | Target time | Tool / record |
|---|---|---|---|
| Contact (email/WhatsApp) -> RMA number | ____ | ____ | ____ |
| Remote diagnosis (Epic 9 plans a Velora Studio diagnostics export per `_bmad-output/implementation-artifacts/epic-1-context.md`:73; availability to confirm) | ____ | ____ | ____ |
| Pickup / shipping label | ____ | ____ | ____ |
| Repair or swap decision | ____ | ____ | ____ |
| SoM swap + re-provisioning | ____ (needs the factory provisioning station; location to confirm) | ____ | ____ |
| Return shipment + closing | ____ | ____ | ____ |

### 3.3 Spare-parts stock list (~100 units)
Categories named in the story: keybed, panel boards, PSU, display (epics.md:6585). Quantities are the founder's/partner's.

| Part | Part no. | Qty for ~100 units | Supplier | Lead time | Source of quantity |
|---|---|---|---|---|---|
| Keybed (Fatar 73SW per go-to-market spec; resupply terms beyond ~100 units are an open question, `hardware-platform.md`:34) | ____ | ____ | ____ | ____ | to confirm |
| Panel boards (panel MCU, encoders, LEDs) | ____ | ____ | ____ | ____ | |
| PSU (Story 11.10 selects it; alternative PSU also stored) | ____ | ____ | ____ | ____ | |
| Display + touch (Story 11.8 part numbers) | ____ | ____ | ____ | ____ | |
| SoM | ____ | ____ | ____ | ____ | |
| Other (faders, encoders, cables) | ____ | ____ | ____ | ____ | |

### 3.4 Support contact
Requirement: e-mail and WhatsApp Business live with a stated response time (epics.md:6586).

| Channel | Address / number | Owner | Stated response time | Hours | Live? |
|---|---|---|---|---|---|
| E-mail (company domain, depends on Story 11.2) | ____ | ____ | ____ | ____ | |
| WhatsApp Business (account type, number): terms and business policy to confirm at WhatsApp's own documentation | ____ | ____ | ____ | ____ | |
| Phone (optional) | ____ | | | | |

## 4. AC4 - Pre-order notifications and LGPD
Requirement: only contacts who consented to launch communication are notified, with an unsubscribe link in every message (epics.md:6588-6590); same LGPD rules as Story 11.3 (epics.md:312, NFR47).

| Item | Answer |
|---|---|
| Waitlist source file and number of consenting contacts | ____ (from Story 11.3; no data in repo) |
| Consent wording covers "launch communication"? | to confirm against the consent text used in Story 11.3 |
| Mail-sending tool and unsubscribe mechanism | ____ |
| Suppression list kept | ____ |
| Who is the LGPD controller/contact (encarregado) | to confirm with lawyer |
| Pre-order opening date and message draft | ____ |

## 5. Order flow (one-page sketch to complete)
1. Visitor on ladiani.com.br -> product page (price, ship estimate, warranty summary) -> checkout.
2. Pre-order or in-stock order -> payment (section 1.2) -> confirmation e-mail with order number.
3. Invoice (NF-e) issued per accountant rule (section 1.3).
4. Pre-order: status updates; ship-date commitments: ____.
5. Pack, drop-tested box, carrier pickup, tracking e-mail (section 2).
6. Delivery -> 7-day withdrawal window starts (date rule to confirm with lawyer) -> warranty starts (section 3.1).
7. Support/RMA (section 3).

## 6. First ~100 unit tracking
Sheet/database columns (founder to choose tool; the tool itself is not decided):

| Column | Notes |
|---|---|
| Unit # (1..~100) | |
| Serial / SoM ID | link per AR33 (epics.md:372) |
| Order #, date, channel (pre-order / in stock) | |
| Buyer (name, city/state) | personal data: LGPD, minimal access |
| Price paid, payment method, fees | |
| NF-e number and date | |
| Shipped date, carrier, tracking, delivered date | |
| Withdrawal/refund? | |
| Warranty end date | |
| Support tickets / RMA numbers | |
| Firmware version at shipment | |
| Buyer feedback / source (survey Q21 relation) | |

Weekly summary: units sold/100, pre-orders open, shipped, delivered, returns, open RMAs, cash received vs fees. Target dates: ____.

## 7. Open decisions and owners

| Decision | Owner | Due |
|---|---|---|
| Platform | founder | |
| Payment methods | founder + provider | |
| NF-e setup | accountant | |
| Carrier + packaging test | founder | |
| Contractual warranty length | founder + lawyer | |
| Legal pages review | lawyer | |
| Support hours/response time | founder | |
