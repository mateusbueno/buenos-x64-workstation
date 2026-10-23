# DRAFT FOR FOUNDER — does not satisfy Story 11.10 acceptance criteria

Preparation notes to help consult an Inmetro-accredited OCP / certification consultant for the Ladiani Velora 73 (73-key stage workstation, ARM64 SoM board, external or internal PSU, no radio in v1).
Prepared 2026-10-19. Context: `_bmad-output/planning-artifacts/epics.md`, Story 11.10; NFR41 (no radio, avoids Anatel), NFR45 (off-the-shelf Inmetro-compliant PSU, exact requirement to confirm), NFR46 (design/test to IEC 62368-1 for later CE/FCC), EXT3 (confirm Inmetro requirement, select PSU, plan IEC 62368-1 safety/EMC testing).

How this was researched: a handful of web searches on 2026-10-19. Search snippets were thin and one page fetch failed (latam.ul.com: DNS error). Nothing below was read from a primary source (Inmetro, Anatel, Diario Oficial). Treat EVERY item as a lead for the OCP, not a finding. No ordinance numbers, dates, certificate numbers, PSU models, prices or lab names are asserted beyond what a cited result states.

## 1. Likely applicable (UNVERIFIED — cite source URL and date for each claim)

Retrieval date for all rows: 2026-10-19 (via web search results; secondary sources unless noted).

| # | Candidate | What the source says (as reported by search result) | Source URL | Status |
|---|-----------|------------------------------------------------------|------------|--------|
| 1 | Inmetro Portaria 304/2023 (IT equipment conformity assessment) | Reported by UL as published 6 Nov 2023, in force 4 Dec 2023, adds IEC 62368-1 as an alternative to IEC 60950-1 for IT equipment. Scope of "IT equipment" vs a keyboard/PSU not stated in the result. | https://latam.ul.com/en/news/information-technology-equipment-have-conformity-assessment-requirements-updated-and (page itself could not be fetched; claim comes from search summary) | UNVERIFIED — to confirm with OCP whether it covers a standalone AC/DC PSU and/or a musical instrument |
| 2 | Portaria 148/2022 (household appliances and similar) | A consultancy says it mentions battery appliances recharged via charger base or power supply; says Inmetro FAQ (updated 30/03/2026) states no Registro de Objeto under it. Not shown to cover standalone AC/DC adapters. | https://abcpcertificacao.com.br/blog/certificacao-eletrodomesticos-inmetro-portaria-148/ ; FAQ example: https://www.gov.br/inmetro/pt-br/acesso-a-informacao/perguntas-frequentes/avaliacao-da-conformidade/aparelhos-eletrodomesticos-e-similares/carregadores-de-veiculos-eletricos-estao-no-escopo-da-portaria-inmetro-ndeg-148-de-2022-carregadores-de-veiculos-eletricos-devem-ser-certificados | UNVERIFIED — likely not the PSU rule; to confirm with OCP |
| 3 | Portaria 674/2024 (low-voltage electrical devices technical regulation, dated 13 Nov 2024 per Inmetro FAQ) | Inmetro FAQ applies it to a socket adapter (NCM 8536.61.00). Not shown to cover AC/DC PSUs. | https://www.gov.br/inmetro/pt-br/acesso-a-informacao/perguntas-frequentes/avaliacao-da-conformidade/dispositivos-eletricos-de-baixa-tensao/necessidade-de-certificacao-compulsoria-para-adaptador-de-soquete-eletrico-ncm-8536-61-00 | UNVERIFIED — check whether PSU falls in or out |
| 4 | Portaria 09/2022 (plug and socket adapters) | Covers plug/socket adapters for household use; reported not to cover an ordinary external power supply. Relevant only if the mains plug/connector arrangement counts. | https://www.grsorganizacoes.com.br/s-n/noticias-descricao.php?id_=OTQ2NzU0OTgyOTU%3D&o=fc9dc22ccbf2965c1a1d85a3cbd28b7350bdb810c1e057a784da89fe6bfb769aa23f5bc5b4ddf58276c13c4cb08b499dc9277c4de4570429963e77e6e9ebfc84 | UNVERIFIED — probably not applicable; to confirm with OCP |
| 5 | Portaria 44/2000 (flexible cables and cords) | Secondary summary says it created compulsory certification for flexible cables/cords incl. those for electrical/electronic appliances; may have been superseded. Relevant to the AC mains cord of the PSU or keyboard. | https://www.contabeis.com.br/legislacao/6891/portaria-inmetro-44-2000/ | UNVERIFIED — current ordinance to confirm with OCP |
| 6 | Anatel Resolucao 715/2019 (conformity assessment and homologation of telecom products, dated 23/10/2019) | Homologation is mandatory prerequisite for use/sale of products within its scope. Search results did NOT show an explicit rule on products without radio. | https://www.legisweb.com.br/legislacao/?id=383825 ; https://www.normasbrasil.com.br/norma/resolucao-715-2019_383825.html | UNVERIFIED — whether a no-radio, no-telecom-function instrument is out of scope: to confirm with OCP / Anatel |
| 7 | Anatel LRPT (product reference list), Ato 7280/2020 per an industry presentation | Defines product families under the declaration-of-conformity model; useful to check that a keyboard does not match any family (e.g., USB/Ethernet are not radio, but confirm). | https://abiis.org.br/wp-content/uploads/2021/11/Apresentacao-Davison.pdf | UNVERIFIED — to confirm with OCP |
| 8 | IEC 62368-1 | Described as successor to IEC 60065 (audio/video) and IEC 60950-1 (IT), hazard-based. | https://assured-systems.com/faq/what-is-iec-62368-1 ; https://www.intertek.com/form/appliances-overview-of-iec-62368-1-wp/ | General description only; Brazilian adoption (ABNT NBR IEC 62368-1) to confirm with OCP |
| 9 | Musical instrument / keyboard specifically | Searches found NO Inmetro ordinance naming keyboards/synthesizers. NCM 9207.10.10 (electric keyboard instruments) is a customs code only. | https://actana.com.br/tabela-ncm/92071010 | No evidence found either way. Whether any compulsory Inmetro scheme covers the finished instrument: to confirm with OCP |
| 10 | EMC: CISPR 32 / CISPR 35, IEC 61000-x series, Brazilian ABNT equivalents | Searches returned no source linking these to a Brazilian requirement for this product. | (none found) | Marked "to confirm with OCP" |

Not found in research (do not assume): which ordinance, if any, makes certification compulsory for an external or internal AC/DC power supply; whether an Inmetro registration number is required for the PSU; whether the finished keyboard needs Inmetro certification; any ordinance on EMC for audio equipment. All to confirm with OCP.

## 2. Question list for OCP / consultant (one-to-one with Story 11.10 Acceptance Criteria)

### AC1 — Which ordinance applies; keyboard certification; Anatel
1. Given an off-the-shelf external AC/DC supply (option A) or an internal PSU module (option B) feeding a 73-key instrument, which Inmetro ordinance (number and date) applies to the PSU? Please state it in writing.
2. Is the PSU covered under compulsory certification at all? If it is exempt, state the basis and whether a voluntary scheme or declaration is advisable.
3. Does the finished keyboard (Velora 73) itself need Inmetro certification or registration? Under which ordinance, number and date? Does the answer change between external and internal PSU, or if the PSU is shipped separately?
4. If we import a PSU or the whole product, what documents does customs (import licensing) check against the Inmetro rules?
5. Does Portaria 304/2023 (IT equipment) or any audio/video scheme apply to a musical instrument with USB/Ethernet/audio I/O? Which scope clause?
6. Written confirmation that no Anatel homologation is needed because v1 has no Wi-Fi/Bluetooth/other radio (NFR41), including: are there any radio-like parts (e.g., SoM with radio module disabled, wireless chip on board even if unused, IR/NFC, USB dongle ports) that would still trigger homologation? Who issues that confirmation (OCP vs Anatel)?
7. Any labeling, manual (Portuguese), or marking obligations (Inmetro seal, importer data, energy-efficiency/no-load requirements) for PSU and product?

### AC2 — PSU selection and stored documents
8. Which certificates must the chosen PSU carry to be accepted: Inmetro registration/certificate number, IEC 62368-1 report/certificate (CB scheme acceptable?), and which scope/validity checks (expiry, factory, model variants)?
9. Is a PSU certified to IEC 60950-1 or 60065 still acceptable, or must it be 62368-1? Transition dates?
10. For an internal PSU, who holds certification responsibility: the PSU maker, us, or both? Can the module's certificate be reused in our product report?
11. What do we need to keep on file and for how long (registration number, test reports, change notices)? What happens when the PSU supplier changes the model?
12. Is a second (alternative) PSU needing a separate registration, and does swapping it require re-evaluation of the product?

### AC3 — Test plan, samples, lab, cost, schedule
13. Which IEC 62368-1 edition and Brazilian equivalent should the product be tested to, and which tests (safety, thermal, abnormal operation, mains isolation) are needed for external vs internal PSU?
14. Which EMC emission and immunity standards are required or advisable (CISPR 32 / CISPR 35 / IEC 61000-x, or Brazilian equivalents) for Brazil, and for a later CE/FCC path (NFR46)?
15. How many samples, and of which build state (production-intent board, chassis, PSU, cables), does the lab need? Can pre-compliance scans on the prototype (month 12) be reused?
16. Which Inmetro-accredited labs perform these tests? What are indicative cost and lead time? (Do not fill from memory; request written quotes.)
17. Recommended schedule so results arrive before v1 launch (month 18), including re-test buffer after a failure and ordinance-registration processing time.
18. Does any design change after testing (board rev, PSU swap, chassis) force a re-test?

## 3. PSU selection checklist

Fill from board measurements (Story 11.x power data) and supplier datasheets. Do not pick a model from memory.

| Item | Requirement / how to compute | Value (fill in) | Source / status |
|------|------------------------------|-----------------|-----------------|
| Board peak load (W) | Measured worst-case: SoM + NVMe + codec + USB peripherals + display/LEDs + audio output stage | ___ | to measure |
| Hold-up / charge load (W) | Extra power for hold-up capacitor/battery charging per the board design (Story spec: "board's load plus hold-up charge") | ___ | to confirm from hardware design |
| Total load (W) | Peak + hold-up charge | ___ | |
| Rated output headroom | PSU continuous rating >= total load x margin (suggest 20-30%, confirm with OCP/engineering) | ___ W rated | to confirm |
| Output voltage and tolerance | Match board input range incl. ripple and transient (connector, polarity) | ___ | |
| Input range | Brazilian mains 127/220 V, 60 Hz; PSU full-range 100-240 V preferred | ___ | to confirm with OCP |
| Plug / inlet | Brazilian NBR 14136 plug or compliant cord; cord certification per OCP | ___ | to confirm with OCP |
| Efficiency / no-load | Whatever Brazilian rule applies | ___ | to confirm with OCP |
| Noise | Ripple/noise low enough for audio path (measured on bench) | ___ | to measure |
| Hold-up / brown-out behaviour | Output dip tolerance vs board hold-up time | ___ | to measure |
| Thermal | Ambient 40 C operation derating | ___ | datasheet |
| Required certificates | Inmetro registration/certificate (if required, number ____); IEC 62368-1 certificate or CB report (number ____); EMC report; manufacturer declaration | ___ | Obtain copies and verify authenticity/validity with issuing body |
| Internal vs external | Compare: external = simpler product safety scope, shared cord/size; internal = our report covers more, thermal and creepage inside chassis | External, locking 5.5 × 2.5 mm DC jack (founder, 2026-10-23) | Confirm with the OCP that this choice holds; voltage and current to define |
| Chosen PSU | Model: ____ ; ratings: ____ ; certificates: ____ | ___ | to be stored (AC2) |
| One alternative PSU | Model: ____ ; ratings: ____ ; certificates: ____ ; from a different manufacturer ideally | ___ | to be stored (AC2) |
| Supply risk | Lead time, MOQ, last-time-buy, who notifies of changes | ___ | |

## 4. Skeleton IEC 62368-1 / EMC test plan

Standards and editions are placeholders: the OCP/lab decides. No lab, cost or date has been chosen.

| # | Test area | Standard / edition (to confirm with OCP) | Sample state | Qty | Lab | Cost | Start | Report due | Notes |
|---|-----------|-------------------------------------------|--------------|-----|-----|------|-------|-----------|-------|
| 1 | Product safety (electrical, thermal, fire, abnormal ops) | IEC 62368-1 / ABNT NBR IEC 62368-1 (confirm) | Production-intent | ___ | ___ | ___ | ___ | ___ | PSU certificate reuse? |
| 2 | Marking, instructions, labels (PT-BR) | per ordinance (confirm) | Production | ___ | ___ | ___ | ___ | ___ | |
| 3 | EMC emissions (conducted and radiated) | CISPR 32 or Brazilian equivalent (confirm) | Production-intent | ___ | ___ | ___ | ___ | ___ | pre-compliance scan month 12 |
| 4 | EMC immunity (ESD, EFT, surge, RF, etc.) | CISPR 35 / IEC 61000-4-x (confirm) | Production-intent | ___ | ___ | ___ | ___ | ___ | audio dropouts criterion |
| 5 | Mains harmonics / flicker (if applicable) | IEC 61000-3-2/-3 (confirm) | Production-intent | ___ | ___ | ___ | ___ | ___ | |
| 6 | PSU evidence review (certificates, CB, Inmetro) | per OCP | PSU sample | ___ | ___ | ___ | ___ | ___ | |
| 7 | Anatel scope confirmation (no radio) | Resolucao 715/2019 (confirm) | n/a | n/a | OCP | ___ | ___ | written | NFR41 |
| 8 | Later CE/FCC readiness | NFR46 | Production | ___ | ___ | ___ | ___ | ___ | optional |

### Milestones

| Milestone | Target | Gate | Status |
|-----------|--------|------|--------|
| OCP written answer (ordinance number and date, keyboard scope, Anatel) | ___ | AC1 | open |
| PSU chosen + alternative, certificates on file | ___ | AC2 | open |
| Lab selected, quote, schedule agreed | ___ | AC3 | open |
| Month 12: pre-compliance EMC scans on prototype, before first playable units | Month 12 | AC3 | open |
| Formal safety + EMC testing complete, reports in hand | before Month 18 (date ___) | AC3 | open |
| Month 18: v1 launch with Inmetro/PSU paperwork complete | Month 18 | AC3 | open |

## 5. Next steps for founder
1. Send Section 2 to 2-3 OCPs/consultants and ask for written answers; keep their replies as the Story 11.10 evidence.
2. Do not file this draft as the story deliverable: AC1-AC3 need the OCP's written ordinance reference, the stored PSU documents, and the lab plan.
3. Re-verify every Section 1 row against the primary text (Inmetro/Anatel/DOU) once the OCP names the ordinance.
