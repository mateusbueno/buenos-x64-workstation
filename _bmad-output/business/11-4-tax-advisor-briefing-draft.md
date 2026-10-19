# DRAFT FOR FOUNDER — does not satisfy Story 11.4 acceptance criteria

Preparation notes for briefing a Brazilian tax advisor (contador / tributarista with import and industrial experience) for the Ladiani Velora 73. Prepared 2026-10-19. The companion file `tools/business/margin_model.py` is **arithmetic, not tax advice**: it holds no tax rates, exchange rates or prices of its own, and every rate in it must come from the advisor. Story 11.4 is satisfied only by the advisor's written report and the founder's recorded decision on the price range.

Source for the story: `_bmad-output/planning-artifacts/epics.md:6231-6255` (Story 11.4; covers EXT1, NFR48; runs on Business, no device). Downstream: Story 11.5 (`epics.md:6257-6281`) and Story 11.18 (`epics.md:6562-6586`).

## 1. What the repo already states (all unverified forge estimates unless noted)

| # | Input | Statement | Cite |
|---|---|---|---|
| 1 | v1 price range | R$ 17,990–19,990; competes on value at the Electro 6D / YC73 price. Street prices Oct 2026: Electro 7 R$ 22,990, Electro 6D R$ 18,990, YC73 R$ 17,590 | `specs/spec-arm64-stage-workstation/go-to-market.md:19-20`; `SPEC.md:119` |
| 2 | Pro price | R$ 24,990+, adding sequencer, 16 parts, 32 GB | `go-to-market.md:21`; `hardware-platform.md:6,44` |
| 3 | BOM estimate | US$ 1,200–1,600 low-volume, "unverified" | `SPEC.md:149`; `specs/.../.memlog.md:58`; `epics.md:316` |
| 4 | Landed cost | About R$ 11,000, "unverified" (BOM ~US$ 1,400 ≈ R$ 7.7k plus import taxes on parts). This implies about 5.5 R$/US$, an unstated exchange-rate assumption | `go-to-market.md:22`; `SPEC.md:149`; `.memlog.md:66` |
| 5 | Sales tax load and dealer margin | "~25–35%" sales taxes and "~30%" dealer margin, no breakdown or source | `go-to-market.md:22`; `.memlog.md:66` |
| 6 | Floor claim | No retail price below R$ 15,000 viable at low volume | `go-to-market.md:22`; `forged-idea.md:34` |
| 7 | Pricing thesis | Local assembly avoids import duty on the finished instrument; only parts are imported; needs advisor check of II/IPI/ICMS/PIS-COFINS on SoM, keybed, NVMe, plus ex-tarifário | `.memlog.md:60`; `SPEC.md:153`; `epics.md:407` |
| 8 | Volume | About 100 units for v1 (direct); launch month 18 | `SPEC.md:141`; `go-to-market.md:26,40` |
| 9 | Channel | Hybrid: first ~100 units direct, dealers after scale; no retail for the first ~100 (NFR52) | `go-to-market.md:24-26`; `epics.md:421,6568` |
| 10 | Direct channel details | Storefront on ladiani.com.br; Pix, boleto, card in instalments "as decided"; NF-e per sale; 7-day withdrawal (CDC art. 49); ~20 kg shipped by insured carrier | `epics.md:6576,6579-6581` |
| 11 | Warranty | Legal 90 days plus a "chosen contractual warranty" (period not chosen); RMA incl. SoM-swap; spare-parts stock (keybed, panel boards, PSU, display) | `epics.md:6585` |
| 12 | Imported-part candidates | SoM (RK3588 industrial, 16 GB v1 / 32 GB Pro), keybed (Fatar 73SW from partner's ~100-unit stock), display (7 in MIPI DSI), NVMe up to 1 TB, audio codec | `hardware-platform.md:5-7,31,68`; `epics.md:6243,6245`; `architecture/.../HARDWARE-INTERFACES.md:16,77` |
| 13 | Made in Brazil | "made in Brazil" is a stated selling point | `go-to-market.md:20` |
| 14 | Compliance cost items | No radio, so no Anatel; off-the-shelf Inmetro PSU (requirement unconfirmed); IEC 62368-1 testing | `go-to-market.md:30-32`; `business/11-10-inmetro-psu-prep-draft.md` |
| 15 | Margin depends on part taxes | "falta validar com um contador" | `forged-idea.md:39` |
| 16 | Architecture adds BOM cost | Second MCU and supercap hold-up add BOM cost not in the US$ 1,200–1,600 estimate | `architecture/.../reviews/review-reconcile.md:171-173` |

Inconsistency to flag to the advisor: an early forge note chose a 76-key keybed (`.memlog.md:54`), the spec now says Fatar 73SW (`hardware-platform.md:31`, `SPEC.md:117`). Use the spec. Resupply terms beyond ~100 keybeds are open (`SPEC.md:154`, Story 11.6), so the keybed cost at volume is not known.

## 2. Questions mapped to the Acceptance Criteria

### AC1: verified per-unit landed cost (replaces the R$ 11,000 estimate)

Given: BOM estimate, imported vs local part list, planned tax regime, direct channel. Then: report states per unit II, IPI, ICMS (state of manufacture, interstate rules), PIS/COFINS, ex-tarifário reductions (SoM, keybed, display), freight, customs brokerage, assembly cost.

| Item the report must state | Repo has | Missing (founder must supply or advisor must decide) |
|---|---|---|
| BOM by line | Estimate range only (row 3); part families (row 12) | Itemized BOM with supplier, unit cost in invoice currency, MOQ price breaks at ~100 units; NCM code for each part and for the finished instrument; PSU, panel boards, faders, enclosure, connectors, second MCU, supercap |
| Imported vs local split | Candidates only (row 12) | Final split; which suppliers are Brazilian; whether the SoM/keybed come via the partner's stock or are imported by the company (importer of record) |
| II and ex-tarifário | Question only (rows 7) | Whether ex-tarifário is available for SoM, keybed, display; process, timing, who files; current II rate per NCM (advisor to state, do not assume) |
| IPI | Question only | Whether the company is an industrial establishment for IPI; IPI on the finished instrument; credit on imported-part IPI |
| ICMS | Question only | State of manufacture (not stated anywhere); import ICMS and any state benefit; interstate sale rules for direct-to-consumer sales (destination state, DIFAL, e-commerce rules); ICMS credit treatment |
| PIS/COFINS | Question only | Import PIS/COFINS on parts; credit vs cumulative regime consequence |
| Freight, brokerage | Nothing numeric; 20 kg parcel (row 10) | International freight and insurance, customs broker fee per shipment (and shipments needed for ~100 units), outbound freight and packaging per unit |
| Assembly cost | Nothing | Who assembles (founder, partner, contract manufacturer), labor, test, burn-in; whether assembly qualifies as "industrialização" |
| Tax regime | Nothing; "planned company tax regime" is not recorded anywhere | Legal entity (exists? CNPJ, CNAE, state), expected revenue (about 100 units ≈ R$ 1.8–2.0M at the stated prices, simple arithmetic), regime options and which fits (Simples, Lucro Presumido, Lucro Real) |

AC1 second clause: assumptions and sensitivity (exchange rate, ~100 units, tax regime). Repo has: ~100 units (row 8); implied FX (row 4, unstated). Missing: reference exchange rate and date, FX scenarios to test, volume scenarios (for example 100 / 300 / 1,000), regime alternatives. Ask the advisor to show the result per assumption, not just a total. The model supports this by re-running with changed inputs.

### AC2: gross margin at R$ 17,990 / 18,990 / 19,990

Required cases: direct sale paid in full (Pix/boleto); direct sale in 10 interest-free instalments (card fees included); future dealer sale (~30% dealer margin). Founder then records whether the range holds, with a minimum viable price if not.

| Item | Repo has | Missing |
|---|---|---|
| Price points | R$ 17,990, 18,990, 19,990 given by the story | none |
| Revenue taxes on sale | "~25–35%" unsourced (row 5) | Actual taxes on the sale by regime (ICMS, PIS/COFINS, IPI if any, or the Simples/presumido equivalent), as named rates the advisor states; whether ICMS-ST or DIFAL applies to a consumer in another state |
| Pix/boleto fees | Nothing | Fee per transaction at the chosen processor |
| Card, 10x interest-free | Instalment need is in the story only | MDR for 10x, cost of receivable anticipation (company receives money over 10 months otherwise), chargeback allowance, processor chosen |
| Dealer case | ~30% dealer margin (row 5) | Definition: 30% of list price or markup on dealer cost; whether the dealer pays the invoice at net price and the tax base follows; ICMS-ST implications for a manufacturer selling to retailers; dealer terms |
| Warranty reserve | 90 days legal plus unspecified contractual (row 11) | Contractual warranty period; expected failure rate, repair/swap cost, SoM-swap cost; how to book a reserve (accounting treatment) |
| Margin definition | "gross margin" only | Which costs sit above the line (warranty, payment fees, outbound freight, support) so that Story 11.5 compares like with like |
| Decision | Founder records range holds or minimum viable price | Target margin % that makes the unit viable (overheads, development amortization are out of scope of unit margin; founder to say) |

### AC3: Pro margin at R$ 24,990

Repo has: Pro price (row 2); 32 GB SoM and Pro additions named (sequencer, 16 parts, motorized faders, `hardware-platform.md:40,44`; `SPEC.md:126,136`). Missing: Pro BOM (32 GB SoM cost, motorized faders, anything else), Pro volume and timing, whether Pro parts have different NCM/ex-tarifário treatment.

## 3. Other questions to ask the advisor

1. Is a company structure already in place, and which state is best for manufacturing/assembly given ICMS?
2. Can the product qualify for incentives (for example the "Lei de Informática" or other manufacturing incentives)? Do not assume; ask whether it is relevant for a 100-unit run.
3. Importer of record: should the company import directly (needs Radar/habilitação at the right modality) or buy through a Brazilian importer/distributor, and how does that change the cost?
4. NF-e and tax treatment of pre-orders (cash received before delivery; when revenue is recognized).
5. Warranty and the instrument as consumer product (CDC): any tax effect of swaps and RMA, SoM-swap re-provisioning.
6. Does export later (stated: Brazil first, export later, `go-to-market.md:18`) change the structure choice now?
7. Library content and software: is the price partly software/licensing (services vs goods tax)? Velora Studio and libraries may be taxed differently (`go-to-market.md:6`).
8. Cost of the formal reports: fee, timeline, deliverable format so the report maps to AC1 to AC3.

## 4. Documents to bring

Itemized BOM draft; supplier quotes; part datasheets and NCM proposals; company registration or plan; Story 11.4 text; this briefing; the filled copy of `margin_inputs.template.json` (even partly filled) so the advisor can see what arithmetic will be run.

## 5. How to use the model (when numbers exist)

1. Copy `tools/business/margin_inputs.template.json`, fill each null. Rates are percent numbers. The template deliberately contains no real value.
2. `python3 tools/business/margin_model.py --inputs <copy>.json --target-margin-pct <N>` prints, per tier (v1, Pro), landed cost, then margin at each price for the three channel cases and the minimum list price for the target margin. It refuses to run and lists every missing field otherwise. `--self-test` checks the arithmetic with fake numbers.
3. The import-tax cascade (II on customs value; IPI on value plus II; PIS/COFINS-import on customs value; ICMS grossed up "por dentro"; credit percentages) is a placeholder shape. If the advisor's bases differ, edit `landed_cost()`.
4. The model does not decide the regime, apply any statute, or replace the advisor's report.

## 6. Open items to resolve before the meeting

- Choose the advisor (not yet named in the repo).
- Draft the itemized BOM (the single biggest missing input).
- Decide the company state and regime candidates.
- Decide the contractual warranty period under consideration (Story 11.18 depends on it).
- Settle the first-batch keybed source (Story 11.6) so the keybed line is real.
