**DRAFT FOR FOUNDER — does not satisfy Story 11.5 acceptance criteria**

# Story 11.5 preparation: price and feature-priority decision record (template)

Prepared 2026-10-19. This is an empty template. It contains no survey result, tax result or price decision. Every `____` is for the founder to fill after Stories 11.3 and 11.4 are done. Nothing here may be filled from assumption; if a number has no source, write "unknown".

Sources in repo:
- Story 11.5, `_bmad-output/planning-artifacts/epics.md` (line 6257). Covers EXT9, EXT1, NFR48, NFR51.
- NFR48 (epics.md line 316): v1 retail R$ 17,990–19,990; BOM est. US$ 1,200–1,600; landed cost est. R$ 11,000 **unverified**; Pro R$ 24,990+.
- NFR51 (epics.md line 319): release is date-driven; v1 requires sample engine, organ and effects; FM and VA may follow by firmware (Epic 12).
- Inputs: survey report from Story 11.3 (in `_bmad-output/research/`), tax report from Story 11.4 (confidential items may live outside the repo with a pointer). `go-to-market.md` is the document to update if the price changes.

## 0. Record header

| Field | Value |
|---|---|
| Decision date | `____` |
| Decision owner | Founder `____` |
| Survey report (file, version, valid n) | `____` |
| Tax report (file or pointer, advisor, date) | `____` |
| Status of Story 11.3 / 11.4 at time of decision | `____` / `____` |

Gate: do not complete sections 1 to 3 until both inputs exist. If either is missing, this record stays a template.

## 1. Input summary (copy figures from the reports, with page/section reference)

### 1.1 Survey (Story 11.3)

| Item | Value | Source ref |
|---|---|---|
| Valid responses after cleaning | `____` | `____` |
| Cleaning rules file/section | `____` | `____` |
| Van Westendorp: lower bound of acceptable range | R$ `____` | `____` |
| Van Westendorp: upper bound of acceptable range | R$ `____` | `____` |
| Van Westendorp: optimal price point | R$ `____` | `____` |
| Crossing definitions used | `____` | `____` |
| Payment preference (Q26) | `____` | `____` |
| Affordable monthly instalment (Q27) | `____` | `____` |
| Caveats on sample (convenience sample, under-represented groups) | `____` | `____` |

### 1.2 Tax report (Story 11.4)

| Item | Value | Source ref |
|---|---|---|
| Verified landed cost per unit (replaces unverified R$ 11,000) | R$ `____` | `____` |
| Gross margin at R$ 17,990: direct, paid in full | `____` | `____` |
| Gross margin at R$ 17,990: direct, 10 interest-free instalments | `____` | `____` |
| Gross margin at R$ 17,990: dealer (~30% dealer margin) | `____` | `____` |
| Same three at R$ 18,990 | `____` | `____` |
| Same three at R$ 19,990 | `____` | `____` |
| Founder's record: does R$ 17,990–19,990 hold? | yes / no | `____` |
| Minimum viable price (if range does not hold) | R$ `____` | `____` |
| Pro margin at R$ 24,990 | `____` | `____` |
| Key assumptions (exchange rate, ~100 units, tax regime) and sensitivities | `____` | `____` |

## 2. Price decision

### 2.1 Comparison

| Quantity | Value (R$) |
|---|---|
| Current NFR48 range | 17,990–19,990 (from repo) |
| Survey acceptable range | `____` – `____` |
| Survey optimal price point | `____` |
| Minimum viable price (tax) | `____` |

Overlap analysis (does the survey acceptable range intersect the range that is profitable?): `____`

Decision rules to fill before looking at the numbers (so the rule is not fitted to the result). The founder proposes; each is a blank, not a recommendation:

| Rule | Founder's rule |
|---|---|
| R1. If the minimum viable price is above the survey's acceptable upper bound, then | `____` |
| R2. If the optimal price point lies inside R$ 17,990–19,990, then | `____` |
| R3. If the optimal price point is below the minimum viable price, then | `____` |
| R4. If the optimal price point is above R$ 19,990, then | `____` |
| R5. Minimum margin required (threshold and which scenario it applies to: full payment / 10x / dealer) | `____` |
| R6. How much weight the survey carries given its sample caveats | `____` |

### 2.2 Chosen launch price

| Field | Value |
|---|---|
| Chosen launch price or range | R$ `____` |
| Rule(s) applied | `____` |
| Instalment offer (e.g. number of instalments, interest-free or not; justified by Q26–27 and the card-fee margin in 11.4) | `____` |
| Dealer price/margin note | `____` |
| Pro tier price (from 11.4 Pro margin) | R$ `____` |
| Reasoning (3–6 sentences, citing 1.1 and 1.2) | `____` |

### 2.3 Change process check

- [ ] Chosen price lies inside R$ 17,990–19,990: no document change required; record "no change" here: `____`
- [ ] Chosen price lies outside the range: raise a change via bmad-correct-course (do not edit silently). Proposal reference: `____`. Documents to update through that process: `go-to-market.md` and NFR48 (and any other place quoting the range: `____`).

## 3. Feature priority

### 3.1 Feature table

One row per surveyed feature (the 12 items in Q17/Q18). Importance and top-3 come from the survey report; the delivering epic must be looked up in `epics.md` by the founder or the architect (do not guess). Epic list for reference: Epic 1 Deep library with stage latency; 2 Organ and effects; 3 Physical instrument; 4 Complete show: Live Sets, scenes and panel; 5 Library: import and find timbres; 6 Protected factory content; 7 Arpeggiator and clock; 8 Reliability: boot, updates and power loss; 9 Velora Studio and USB; 10 Settings, languages and themes; 11 Pre-launch; 12 FM and VA engines (post-launch).

Recommendation values: keep / raise priority / lower priority.

| # | Surveyed feature (Q17/Q18 wording) | Importance (% Muito + Indispensável) | Top-3 share (Q18) | Delivering epic(s) (to confirm in epics.md) | Recommendation | Reason |
|---|---|---|---|---|---|---|
| 1 | Centenas de GB de sons | `____` | `____` | `____` | `____` | `____` |
| 2 | Trocar de som/cena/música sem cortar as notas | `____` | `____` | `____` | `____` | `____` |
| 3 | Setlist com busca, cenas por música | `____` | `____` | `____` | `____` | `____` |
| 4 | Importar sons próprios (SFZ/SF2) | `____` | `____` | `____` | `____` | `____` |
| 5 | Órgão com drawbars físicos e rotary | `____` | `____` | `____` | `____` | `____` |
| 6 | Interface de áudio USB embutida | `____` | `____` | `____` | `____` | `____` |
| 7 | Tela touch de 7" legível no palco | `____` | `____` | `____` | `____` | `____` |
| 8 | 73 teclas semipesadas waterfall | `____` | `____` | `____` | `____` | `____` |
| 9 | App no computador com todos os parâmetros | `____` | `____` | `____` | `____` | `____` |
| 10 | Novos motores e sons por atualização gratuita | `____` | `____` | `____` | `____` | `____` |
| 11 | Fabricado no Brasil, assistência técnica local | `____` | `____` | `____` | `____` | `____` |
| 12 | Liga em menos de 10 segundos | `____` | `____` | `____` | `____` | `____` |

Priority decision rules (founder to fill before reading results):

| Rule | Founder's rule |
|---|---|
| F1. Threshold for "raise priority" (importance and/or top-3 share) | `____` |
| F2. Threshold for "lower priority" | `____` |
| F3. Treatment of features that are hardware or supplier-bound (cannot be reprioritised by software) | `____` |
| F4. Use of unprompted wishes (Q16/Q19) and pains (Q14–15) as tie-breakers | `____` |

### 3.2 Scope-change proposals (NFR51 guard)

NFR51 stands unless the founder decides otherwise: v1 requires sample engine, organ and effects; FM and VA may follow by firmware. Any recommendation that changes release scope (for example raising "new engines by free update" ahead of launch, which belongs to Epic 12) is recorded here as a **proposed change**, not applied.

| Proposed change | Triggering evidence | Affects NFR51? | Founder decision (accept / reject / defer) | Change process reference |
|---|---|---|---|---|
| `____` | `____` | `____` | `____` | `____` |

- [ ] No scope change proposed; NFR51 kept as is.
- [ ] Scope change(s) proposed and routed through bmad-correct-course (reference `____`).

## 4. Switching barriers (Q21)

Top 3 barriers from the survey report (examples the story names: new brand, resale value, local service; use the real top 3 from the data, whatever they are):

| Rank | Barrier | Share of respondents | Named answer in the launch plan (e.g. warranty, local service network, demo units, trial events; only as the founder decides) | Owner | Due | Where recorded |
|---|---|---|---|---|---|---|
| 1 | `____` | `____` | `____` | `____` | `____` | `____` |
| 2 | `____` | `____` | `____` | `____` | `____` | `____` |
| 3 | `____` | `____` | `____` | `____` | `____` | `____` |

Note: warranty terms, service network and demo/trial events carry commitments and costs that are not in this draft. Costs and legal terms: `____` (to confirm).

## 5. Downstream updates

Documents that may need to follow this decision (tick when done or write "no change"):

| Document | Change needed | Done |
|---|---|---|
| `go-to-market.md` | `____` | ☐ |
| NFR48 in `epics.md` and the PRD/spec lines that quote the range | `____` | ☐ |
| Epic priorities / sprint order | `____` | ☐ |
| Marketing copy and web store price | `____` | ☐ |
| Story 11.18 direct-sales channel (price, instalments) | `____` | ☐ |

## 6. Acceptance checklist for Story 11.5

Tick only when the evidence exists in the finished document.

**AC1 Price**
- [ ] Survey report (11.3) and tax report (11.4) both exist and are cited
- [ ] Van Westendorp acceptable range and optimal price compared with R$ 17,990–19,990 and with the minimum viable price
- [ ] Chosen launch price (or range), the instalment offer and the reasoning stated
- [ ] If the price is outside the current range: `go-to-market.md` and NFR48 updated through bmad-correct-course, not silently

**AC2 Feature priority**
- [ ] Each surveyed feature listed with importance and top-3 share
- [ ] Each feature mapped to the delivering epic
- [ ] Each feature has a keep / raise / lower recommendation
- [ ] Any recommendation changing release scope raised as a proposed change, NFR51 kept unless the founder decides otherwise

**AC3 Switching barriers**
- [ ] Top 3 barriers (Q21) each have a named answer in the launch plan
- [ ] Each answer has an owner

**Covers check:** EXT9 (feed results into pricing and feature priority) ☐ · EXT1 (decision) ☐ · NFR48 ☐ · NFR51 ☐

## 7. Sign-off

| Field | Value |
|---|---|
| Founder decision recorded | `____` (date) |
| Open items carried forward | `____` |
| Next story unblocked | `____` |
