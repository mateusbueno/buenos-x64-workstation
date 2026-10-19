**DRAFT BRIEFING FOR FOUNDER — does not satisfy Story 11.1 acceptance criteria**

# Trademark attorney briefing: Ladiani, Velora, Ladiani Velora, Velora Studio

Prepared 2026-10-19 from the repo and web research. This is a briefing to hand to a Brazilian trademark attorney. It contains no search results, no conflict findings and no clearance conclusion. The clearance is the attorney's job (Story 11.1). Where a fact is uncertain, it is marked **to confirm with attorney**.

Language note: the attorney may prefer Portuguese. Sections 1 to 3 can be translated as they stand.

## 1. Product and business summary (from the repo)

Sources in repo: `_bmad-output/specs/spec-arm64-stage-workstation/SPEC.md`, `go-to-market.md`, `planning-artifacts/epics.md` (Story 11.1, EXT8, NFR29, FR121).

- **Brand:** Ladiani (from "Ladislau + Oliani"). **Model:** Velora. v1 is "Ladiani Velora 73", the Pro tier is "Ladiani Velora 73 Pro", and the desktop companion app is "Velora Studio".
- **Product:** a 73-key ARM64 stage keyboard workstation. It has a streamed sample engine (library up to 1 TB), a tonewheel organ engine, effects, and class-compliant USB audio and MIDI. After launch, a 6-operator FM engine and a virtual-analog engine arrive by firmware update (CAP-5, CAP-6). Users can import their own SFZ/SF2 libraries, and factory libraries ship in a proprietary encrypted format.
- **Goods the marks will identify:** a musical instrument (hardware keyboard); the embedded software and firmware; a USB audio and MIDI interface function; downloadable content (factory and partner sound libraries); and a desktop application (Velora Studio, which backs up and restores the unit).
- **Market and timing:** Brazil first (v1 priced about R$ 17,990 to 19,990), export later. About 100 units are sold direct to musicians. v1 launch is about month 18 from work start. The brand has not been made public, and the keyboardist survey deliberately avoids the names.
- **Owner:** the filing party is to be "the founder's company" (Story 11.1). **Company legal name, CNPJ and company size (ME/EPP or other) are not in the repo. Founder to supply.** Company size may affect INPI fee discounts.
- **Preliminary screening (Oct 2026, by the founder, not a clearance):** the WIPO Global Brand Database showed no conflicting marks in classes 15 and 9. The founder also found `@ladiani` taken on Instagram and YouTube, while `@ladianimusic` and `@ladiani.instruments` were free. The domains ladiani.com, .com.br, .net, .io, .music and .audio were free. Treat all of this as unverified input for the attorney, not as a finding.
- **Planned design language:** "elegance, luxury, discretion". A figurative logo for Ladiani and a related one for Velora are still to be designed (Stories 11.16, 11.19). The final logo does not exist yet.
- **Brand architecture:** Ladiani is the house brand and Velora is the first model. Velora is meant to be the first product of a future portfolio, so the brand system must scale to further model names.

## 2. Marks and classes to clear

| # | Mark | Role | Class 15 (instruments) | Class 9 (software, audio interface, downloadable content) |
|---|---|---|---|---|
| 1 | Ladiani | House brand | clear? | clear? |
| 2 | Velora | Model name | clear? | clear? |
| 3 | Ladiani Velora | Full product name (v1 is "Ladiani Velora 73") | clear? | clear? |
| 4 | Velora Studio | Desktop app name | clear? | clear? |

Scope notes the attorney should confirm (including the exact goods wording for the INPI specification):

- Class 15 covers musical instruments (the keyboard itself). Class 9 covers apparatus and instruments and computer software, which is where the app, firmware, audio interface and downloadable libraries are likely to fall. **Which of these items goes in which class is to confirm with attorney.** Whether INPI treats an embedded-firmware keyboard as class 15 only, or also class 9, is to confirm with attorney.
- Whether class 41 (music production and entertainment services) or class 42 (software as a service) is worth considering for the future is a question for the attorney. It is not in the Story 11.1 scope.
- INPI applications are per class, so each mark and class pair is a separate filing (see sources [S1], [S3]).

## 3. Attorney deliverables checklist (from Story 11.1 acceptance criteria)

Source: `epics.md`, Story 11.1 (lines 6136 to 6166). Reference IDs: EXT8, NFR29, FR121.

**A. Formal clearance search and written opinion (AC block 1)**

- [ ] Search run on **INPI**, **WIPO Global Brand Database**, and **common-law (uso de fato)** for all four marks.
- [ ] Search covers classes **15 and 9** for each mark.
- [ ] Written clearance opinion stored (filename and date recorded in the status table).
- [ ] For **each mark and class** (8 cells), a status of **clear / risky / blocked**.
- [ ] For every risky or blocked cell, the **conflicting marks are cited** (mark, owner, number, class, status, and the basis for the risk).
- [ ] If any cell is risky or blocked, the opinion names the **fallback**: different mark, figurative mark, or class narrowing.
- [ ] **Founder records a go/no-go decision** before any further branding spend (domains, handles, designer). This is the founder's decision, not the attorney's. Story 11.2 depends on it.

**B. INPI filings (AC block 2, only after a clear or accepted-risk opinion)**

- [ ] Word mark **"Ladiani"** filed in classes 15 and 9.
- [ ] Word mark **"Velora"** filed in classes 15 and 9.
- [ ] Applicant is the **founder's company**.
- [ ] INPI **process numbers** stored.
- [ ] **Filing receipts and GRU payment proof** stored.
- [ ] Written decision, with reasons, on whether to also file **"Velora Studio"** and the **figurative mark** (once a final logo exists).

**C. Export and deadlines (AC block 3)**

- [ ] Dated **Madrid Protocol plan**: target territories (at least those considered after Brazil), estimated fees, and the trigger for filing. The trigger is within the 6-month priority window from the INPI filing date, or a written decision to let it lapse.
- [ ] **INPI deadline calendar** (publication, opposition period, office actions, grant fee) with an owner and reminder dates.

**D. Compatibility wording (AC block 4, FR121 and NFR29)**

- [ ] Written opinion on the **allowed compatibility wording** for marketing and documentation (nominative use of third-party product names, with the required disclaimer).
- [ ] The opinion lists the **places where the wording may appear**.
- [ ] Confirmation that the device UI, Velora Studio and timbre names stay generic ("6-operator FM", "Import 6-OP SysEx") regardless of the opinion. This is a product rule, and the attorney need not change it.

## 4. Questions for the attorney

**Compatibility wording (FR121, NFR29)**

1. The 6-operator FM engine imports the classic 6-operator FM SysEx bank format. May marketing say "compatible with" or "imports banks for" a named third-party product? If so, what exact phrasing, and what disclaimer text in Portuguese and English?
2. Which surfaces may carry that wording: website, datasheet, manual, packaging, social media, store listings? Which must not (the repo rule is that the device UI, the Velora Studio UI and timbre names stay generic)?
3. Is a nominative-use statement in a separate "compatibility" page safer than in running marketing copy? Does the answer change for export markets?
4. Our policy is to avoid third-party trademarks entirely in marketing, UI and panel (for example "rotary speaker", never a brand name). Are there Brazilian-law reasons to relax or keep that policy?

**Search and risk**

5. Which databases and what search strategy will you use (identical, phonetic and figurative similarity, translations, prefixes and suffixes)? Does "Velora" need extra scrutiny because of how common similar words are? Please state the scope of the search in the opinion.
6. How do you treat the "Ladiani" and "Velora" elements inside the composite marks "Ladiani Velora" and "Velora Studio"? Is "Studio" likely to be treated as non-distinctive, requiring a disclaimer?
7. How will you check common-law use (uso de fato), including the prior-user precedence right in LPI art. 129 §1? What evidence do you collect, and how current is it? (See [S6].)
8. Do you also screen for company names and business names (nome empresarial, Junta Comercial) and for domains and social handles, in addition to trademarks?

**Strategy**

9. Should we file the figurative mark now, or wait for the final logo? Should "Ladiani Velora" or "Velora Studio" be filed, or do they rely on protection of the two word marks?
10. The specification of goods: pre-approved list versus free text, and how to draft it so that it covers a keyboard, firmware, a companion app and downloadable libraries without being overbroad.
11. Any known INPI practice affecting the timeline (for example the status of backlog, any fast-track available to us)? **To confirm with attorney.**
12. If a mark is risky, what are the realistic fallbacks: coexistence agreement, class narrowing, or an alternative mark? Which would you recommend per mark?

**Madrid and export**

13. Which territories should be assessed first, and what is the cost model of a Madrid filing based on a pending Brazilian application ("basic application" dependency risk)? Confirm the 6-month priority window and how to claim it.

**Domains and handles**

14. Our domain and handle plan is in Story 11.2 (ladiani.com, .com.br, and the handles `@ladianimusic` and `@ladiani.instruments`; `@ladiani` is taken by others). Does the taken `@ladiani` handle affect risk? May we register domains before the clearance opinion, or must we wait for the go decision?

**Timeline and costs**

15. Fixed-fee or hourly quote for each stage: clearance opinion, filing (per class), compatibility opinion, Madrid plan, docketing. Your own fees are not in this document.
16. Expected turnaround for the opinion, and the earliest filing date after our go decision.
17. Total official INPI fees for 2 marks across 2 classes (4 filings), and whether our company size qualifies for a discount. See section 6 for the unverified figures found.
18. Who is the docketing owner for deadlines after filing, and how will you notify us?

## 5. Reference notes from web research

All URLs accessed 2026-10-19 via the web search tool. The direct fetch tool could not reach `gov.br` or `wipo.int` from this environment (DNS lookup failed), so no official page was read in full. Everything below comes from search-result summaries. **Everything here is to confirm with attorney, and the official INPI and WIPO pages must be checked before any payment.**

**INPI filing process (basics)**

- Reported flow: free prior search on INPI's tool; register in e-INPI (individual or company); issue a GRU in the INPI GRU portal, choosing the Marcas service; then complete the application in e-Marcas using the GRU number. One class per application, so a separate filing per class. If the fee changes between GRU issue and filing, the difference is payable. Sources: [S1] (summary of a third-party article, not INPI). [S3] states one class per filing, from a university guide, and the 1999 INPI act that adopted the Nice classification.
- Reported GRU options: free-text specification (code 394) and pre-approved specification (code 389). From [S1] only. **To confirm with attorney.**
- Madrid: since 2019 Brazil takes part in the Madrid Protocol, INPI acts as the office of origin, and the holder must already have a Brazilian registration or application. Source: [S4] (search summary, citing Resolution INPI/PR 247/2019). The 6-month priority window (Paris Convention) was **not** confirmed by a source in my research. **To confirm with attorney.**

**Nice classes 9 and 15 (scope notes)**

- Class 15 heading: musical instruments; music stands and stands for musical instruments; conductors' batons. Class 9 heading: a broad set of scientific, nautical, surveying, photographic, cinematographic, optical, weighing, measuring, signalling, checking, life-saving and teaching apparatus and instruments, along with computer software. Source: [S2] (search summary quoting WIPO). The WIPO Nice edition in force and the exact INPI-translated wording are **to confirm with attorney**.
- A national office's guidance puts sound-modifying goods (amplifiers) and music players in class 9, and musical instruments, parts and accessories in class 15. Downloadable music files are reported in class 9 by a third-party guide only. [S2] **To confirm with attorney**, especially for a keyboard with embedded audio-interface functions.

**Search entry points**

- WIPO Global Brand Database: reported entry `https://www3.wipo.int/branddb` (from 2020 WIPO training slides). Supports brand name, number, date, class and country searches, image search, and fuzzy and wildcard operators. It draws on many national sources but is not a real-time display of all marks. An identical-match search does not prove that a mark is free. [S5]
- INPI trademark search: the INPI trademark database is on the INPI portal (gov.br/inpi), in the trademarks area. I could not confirm the exact URL from a source. **To confirm with attorney.**

**Common-law / prior-use checks**

- LPI art. 129 §1: a good-faith user of an identical or similar mark for at least 6 months before the filing or priority date may hold a precedence right to registration. Reported courts require clear proof of use as a mark, and domain registration or social profiles alone were held not enough in one TRF4 decision. INPI's trademark manual reportedly restricts the doctrine to prior users who never sought registration, which is criticized. All from press and opinion articles, not primary law or case reports. [S6] **To confirm with attorney.**

## 6. Fees (not verified — to confirm with attorney)

I have not found an official INPI fee table. The only figures found are from a search summary of third-party blogs, which differ from each other and may be out of date. They are **not** to be used for budgeting without confirmation.

- One blog reports filing fees of R$ 142 (MEI/ME/EPP) and R$ 355 (others), and grant fees of R$ 298 (MEI/ME/EPP) and R$ 745 (others). Another reports different line items (for example R$ 355 for the filing, R$ 355 for the merit exam, R$ 163 per class for the grant publication, R$ 195 per class for the certificate). Source: [S1]. The two do not reconcile.
- Official fee table to read: INPI "Tabela de retribuição dos serviços do INPI" on gov.br/inpi (URL not verified).
- Madrid and WIPO fees: no figure found. Ask the attorney.
- Attorney fees: none known. No figure is invented here.

## 7. Sources

Access date for all: 2026-10-19. All were read only as search-result summaries.

- [S1] Neon, "Como registrar uma marca? Passo a passo e custos", https://neon.com.br/aprenda/empreender/como-registrar-marca/ (third-party; also in the same results: https://www.companyhero.com/blog/como-registrar-uma-marca-no-inpi, https://genyo.com.br/como-registrar-uma-marca-no-inpi/)
- [S2] WIPO Nice revision files and national guidance (results from a search on class 9 and 15 headings), e.g. https://www.wipo.int/classifications/data/nice/ITSupport_and_download_area/20210101/revision_files/ncl-20210101-E-revision_file.pdf and https://www.tipo.gov.tw/wSite/public/Attachment/005/f1745312977891.pdf; third-party downloadable-goods guide https://gleanmark.com/nice-class/15
- [S3] UFV, "Lista de classes de produtos e serviços", https://nit.ufv.br/wp-content/uploads/2023/03/Lista-de-classes-de-produtos-e-servicos.pdf; WIPOLex entry on the INPI Nice adoption act, https://www.wipo.int/wipolex/fr/legislation/details/9730
- [S4] Madrid Protocol in Brazil: https://periodicos.usp.br/rfdusp/article/download/218270/199510/659926 ; INPI panel https://www.gov.br/inpi/pt-br/assuntos/marcas/arquivos/PainelMadri14nov.pdf
- [S5] WIPO Global Brand Database: training slides https://www.wipo.org/edocs/mdocs/globalinfra/en/wipo_webinar_gbd_2020_12/wipo_webinar_gbd_2020_12_presentation.pdf ; flyer https://Wipo.Int/export/sites/www/global_ip/en/pdf/global_databases_flyer.pdf ; 2024 webinar https://Wipo.Int/edocs/mdocs/madrid/en/wipo_webinar_madrid_2024_12/wipo_webinar_madrid_2024_12_presentation.pdf
- [S6] Prior-use precedence in Brazil: https://www.migalhas.com.br/depeso/299257/direito-de-precedencia-em-xeque ; https://www.migalhas.com.br/depeso/355188/direito-do-usuario-anterior-de-boa-fe-inpi-muda-entendimento ; https://jus.com.br/artigos/42800/novo-entendimento-do-inpi-confere-maior-seguranca-juridica-nas-disputas-entre-depositantes-de-marcas-e-usuarios-anteriores-de-boa-fe

## 8. Founder status table (blank)

Fill in as the attorney delivers. Status values for the clearance cells are the attorney's: clear / risky / blocked. Leave blank until the opinion exists.

**Clearance (per mark and class)**

| Mark | Class | Attorney status | Conflicting marks cited (number / owner) | Fallback named | Founder decision (go / no-go / accepted risk) | Date |
|---|---|---|---|---|---|---|
| Ladiani | 15 | | | | | |
| Ladiani | 9 | | | | | |
| Velora | 15 | | | | | |
| Velora | 9 | | | | | |
| Ladiani Velora | 15 | | | | | |
| Ladiani Velora | 9 | | | | | |
| Velora Studio | 15 | | | | | |
| Velora Studio | 9 | | | | | |

**Deliverables**

| Item | Owner | Due | Done (date) | Stored at (path) | Notes |
|---|---|---|---|---|---|
| Attorney engaged, quote accepted | | | | | |
| Written clearance opinion | | | | | |
| Founder go/no-go recorded | | | | | |
| Filing "Ladiani" class 15: process no. | | | | | |
| Filing "Ladiani" class 9: process no. | | | | | |
| Filing "Velora" class 15: process no. | | | | | |
| Filing "Velora" class 9: process no. | | | | | |
| GRU payment proofs and filing receipts | | | | | |
| Decision on "Velora Studio" and figurative mark filings (with reason) | | | | | |
| Madrid Protocol plan (territories, fees, trigger, date) | | | | | |
| INPI deadline calendar (owner, reminders) | | | | | |
| Compatibility wording opinion (FR121) and allowed places | | | | | |
| Confirmation: UI, Velora Studio and timbre names stay generic (NFR29) | | | | | |

**Costs and time**

| Item | Quoted / paid (R$) | Source (quote, receipt) | Date |
|---|---|---|---|
| Attorney: clearance opinion | | | |
| Attorney: filings | | | |
| INPI official fees | | | |
| Madrid / WIPO fees | | | |
