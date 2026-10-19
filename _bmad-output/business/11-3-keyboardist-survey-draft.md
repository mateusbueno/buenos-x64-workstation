**DRAFT FOR FOUNDER — does not satisfy Story 11.3 acceptance criteria**

# Story 11.3 preparation: Brazil keyboardist survey (draft)

Prepared 2026-10-19. Nothing here is a result. No respondents exist, no data has been collected, and no market figure is asserted. Blanks (`____`) are for the founder to fill. Anything legal is marked **confirm with counsel**.

Sources in repo:
- Story 11.3, `_bmad-output/planning-artifacts/epics.md` (line 6194). Covers EXT9 and NFR47.
- Survey design: `_bmad-output/research/pesquisa-tecladistas-brasil.md` (the questions below follow it one to one, same numbering).
- NFR47 (epics.md line 312): personal data only with explicit LGPD consent, used only for the stated purpose.
- NFR48 and EXT1: price range R$ 17,990–19,990 (to be compared in Story 11.5).

## 0. AC key

The five Given/When/Then blocks of Story 11.3, referenced below:

- **AC1 Build:** form has 9 sections, ~30 questions, branching on Q12 and Q28, required Q6, 7, 13, 14, 15, 17, 18, 22–25, "exactly 3" on Q18, numeric validation on Q22–25, automatic e-mail collection off. The form never says "Ladiani" or "Velora" (only "um novo teclado de palco").
- **AC2 LGPD:** personal data only on Q28a/28b. Q29 consent is required when an e-mail is given; the consent text states the only purposes, the controller's identity and how to withdraw. Contact data lives in a separate, access-restricted sheet with a stored retention/deletion rule.
- **AC3 Pilot:** pilot with 10 keyboardists; median completion time recorded and ≤10 min (otherwise cut Section 1 questions); feedback and changes logged; pilot answers excluded from the final dataset.
- **AC4 Collection:** ≥400 valid responses after removing duplicates, test entries and inconsistent Van Westendorp answers (e.g. "too cheap" > "too expensive"), cleaning rules written down; distribution by region and usage context reported, with under-represented groups and extra outreach noted.
- **AC5 Report:** results report in `_bmad-output/research/` with the items listed in section 5 below; prize draw run and winner contacted only through the consented channel.

## 1. Questionnaire (pt-BR, with English gloss)

Wording is taken from the research design. Where the design leaves a choice open, it is flagged. "Req." = required in the form per AC1. The AC column shows which acceptance criterion each question serves, and the analysis column what it feeds.

### Seção 0 · Apresentação (texto, sem perguntas)

pt-BR: "Estamos pesquisando como os tecladistas brasileiros usam seus teclados, para criar um instrumento novo, feito para o palco. Leva cerca de 8 minutos e as respostas são anônimas. No final você pode deixar seu e-mail para concorrer a [prêmio] e entrar na lista de testes."
EN gloss: "We are researching how Brazilian keyboardists use their keyboards, to create a new instrument made for the stage. About 8 minutes; answers are anonymous. At the end you may leave your e-mail to enter the draw for [prize] and join the test list."
- Serves: AC1 (no brand names; "novo teclado de palco" only), AC2 (states purpose up front). Note: "[prêmio]" is a blank: `____`. The intro says answers are anonymous; this must stay true given Q28a (see section 3).

### Seção 1 · Perfil

| # | pt-BR | English gloss | Req. | Serves AC | Feeds |
|---|---|---|---|---|---|
| 1 | Faixa de idade (Até 17 · 18–24 · 25–34 · 35–44 · 45–54 · 55+) | Age band | no | AC4 (distribution), AC3 (cut candidate) | Segmenting |
| 2 | Estado onde você mora (27 UFs) | State of residence | no | AC4 (region distribution) | Region coverage |
| 3 | Onde você mais toca? (até 3: Igreja · Banda/shows · Casamentos e eventos · Bares/restaurantes · Estúdio/gravação · Dou aulas · Em casa · Outro) | Where do you play most? (up to 3) | no | AC4 (usage-context distribution) | Segmenting |
| 4 | Quantas apresentações por mês, em média? (0 · 1–2 · 3–5 · 6–10 · Mais de 10) | Gigs per month | no | AC3 (cut candidate) | Segmenting |
| 5 | Como você se define? (Iniciante · Amador · Semiprofissional · Profissional) | Self-described level | no | AC3 (cut candidate) | Segmenting |

If the pilot median exceeds 10 minutes, AC3 says to cut from Section 1 (Q1–5). Decide which in advance: `____`. Note Q2 and Q3 are needed for the AC4 distribution report, so cut Q4/Q5 first (founder to confirm).

### Seção 2 · Seu teclado principal

| # | pt-BR | English gloss | Req. | Serves AC | Feeds |
|---|---|---|---|---|---|
| 6 | Marca do seu teclado principal (lista suspensa) | Brand of main keyboard | yes | AC1, AC5 (brand ranking) | Brand/model ranking |
| 7 | Modelo (resposta curta) | Model (short text) | yes | AC1, AC5 | Brand/model ranking |
| 8 | Tipo de teclado (Workstation · Stage piano · Arranjador · Sintetizador · Controlador MIDI + computador · Não sei) | Keyboard type | no | AC5 (context for Q6–8) | Ranking |
| 9 | Quantas teclas? (61 · 73/76 · 88 · Outro) | Key count | no | AC5 | Segment: 73-key relevance |
| 10 | Há quanto tempo você tem esse teclado? | Ownership duration | no | AC5 | Replacement cycle |
| 11 | Quanto pagou nele (aproximadamente)? (faixas, "Prefiro não dizer") | Approx. price paid | no | AC5 | Price context for Q22–25 |
| 12 | Você usa computador ou VST no palco? (Sim, sempre · Às vezes · Não) | Computer/VST on stage? | no | AC1 (branch) | Segmenting |
| 12a | (se Sim/Às vezes) Qual software? (MainStage · Ableton Live · Kontakt · Gig Performer · Outro) | Which software? | no | AC1 (branch) | Segmenting |

Brand list in Q6 is a dropdown of existing market brands, not ours. Whether to keep every brand in the design's list: `____` (founder to confirm; no market-share claim is being made).

### Seção 3 · Satisfação e dores

| # | pt-BR | English gloss | Req. | Serves AC | Feeds |
|---|---|---|---|---|---|
| 13 | Numa escala de 0 a 10, o quanto você recomendaria seu teclado atual para outro tecladista? | 0–10 likelihood to recommend current keyboard | yes | AC1, AC5 (NPS per brand) | NPS per brand |
| 14 | Com que frequência estes problemas acontecem? Grade (Nunca · Às vezes · Frequentemente · Sempre) para 11 itens: som corta ao trocar de timbre; tela difícil de ler no palco; falta memória para sons; sons de piano/órgão não satisfazem; edição difícil ou demorada; menus confusos; aperto botões sem querer; demora para ligar; pesado para transportar; preciso de interface de áudio externa; assistência técnica/peças no Brasil difíceis | Frequency grid of 11 pain points | yes | AC1, AC5 (top pains) | Pains; maps to features in 11.5 |
| 15 | Qual é a sua MAIOR frustração com o teclado que você usa hoje? (parágrafo) | Biggest frustration (open text) | yes | AC1, AC5 (musicians' words) | Pains in own words |

### Seção 4 · O que falta (antes de mostrar os nossos recursos)

| # | pt-BR | English gloss | Req. | Serves AC | Feeds |
|---|---|---|---|---|---|
| 16 | Que recurso você gostaria que o seu teclado tivesse e ele não tem? (parágrafo) | What feature do you wish your keyboard had? (open text) | no | AC5 (missing features) | Unprompted wishes |

Must stay before Section 5 so answers are not primed by our feature list (design note).

### Seção 5 · Recursos de um novo teclado

| # | pt-BR | English gloss | Req. | Serves AC | Feeds |
|---|---|---|---|---|---|
| 17 | Quão importante seria cada recurso? Grade (Nada · Pouco · Moderado · Muito · Indispensável) para 12 itens: centenas de GB de sons; trocar de som/cena/música sem cortar notas; setlist com busca e cenas por música; importar sons próprios (SFZ/SF2); órgão com drawbars físicos e rotary; interface de áudio USB embutida; tela touch de 7"; 73 teclas semipesadas waterfall; app de computador com todos os parâmetros; novos motores por atualização gratuita; fabricado no Brasil com assistência local; liga em menos de 10 segundos | Importance grid for 12 features | yes | AC1, AC5 (feature importance) | Feature importance; Story 11.5 AC2 |
| 18 | Destes, escolha os 3 mais importantes para você ("Selecione exatamente 3") | Pick the 3 most important | yes | AC1 (exactly-3 validation), AC5 (top-3) | Top-3 share; Story 11.5 AC2 |
| 19 | Algum outro recurso faria você trocar de teclado? (curta, opcional) | Any other feature that would make you switch? | no | AC5 (missing features) | Unprompted wishes |

The 12 list items must be the same text in Q17 and Q18. Feature wording must not contain "Ladiani" or "Velora" (AC1).

### Seção 6 · Disposição para trocar

| # | pt-BR | English gloss | Req. | Serves AC | Feeds |
|---|---|---|---|---|---|
| 20 | Se existisse um teclado com os recursos que você marcou, qual a chance de trocar nos próximos 2 anos? (1 Nenhuma … 5 Com certeza) | Likelihood to switch within 2 years (1–5) | no | AC5 (switching intent) | Switching intent |
| 21 | O que mais te impediria de trocar? (até 3: Preço · Ser uma marca nova · Valor de revenda · Assistência técnica · Os sons de fábrica · Já me acostumei · Peso/tamanho · Outro) | Biggest barriers to switching (up to 3) | no | AC5 (barriers) | Barriers; Story 11.5 AC3 |

### Seção 7 · Preço

Section text pt-BR: "Imagine um teclado de palco de 73 teclas com os recursos que você marcou como importantes." EN: "Imagine a 73-key stage keyboard with the features you marked as important."

| # | pt-BR | English gloss | Req. | Serves AC | Feeds |
|---|---|---|---|---|---|
| 22 | A partir de que preço você acharia esse teclado tão BARATO que duvidaria da qualidade? (número, R$) | Too cheap (quality doubt) | yes, numeric | AC1, AC4 (consistency check), AC5 (VW) | Van Westendorp |
| 23 | A que preço você o consideraria uma BOA COMPRA? (número, R$) | Bargain | yes, numeric | AC1, AC4, AC5 | Van Westendorp |
| 24 | A que preço ele começaria a ficar CARO, mas você ainda pensaria em comprar? (número, R$) | Getting expensive | yes, numeric | AC1, AC4, AC5 | Van Westendorp |
| 25 | A partir de que preço ele seria CARO DEMAIS e você nem consideraria? (número, R$) | Too expensive | yes, numeric | AC1, AC4, AC5 | Van Westendorp |
| 26 | Como você pagaria? (À vista · Parcelado até 10x · 12x ou mais · Consórcio/financiamento) | Payment method | no | AC5 (payment preference) | Instalment offer in 11.5 |
| 27 | Qual parcela mensal caberia no seu orçamento? (faixas) | Affordable monthly instalment | no | AC5 | Instalment offer in 11.5 |

Open point for the founder: the Q27 bands in the design top out at "Mais de R$ 2.000". Whether the bands are enough to evaluate 10 instalments of the price range: `____` (arithmetic on the founder's own range, not a survey claim; decide before pilot). No price is shown to respondents in the form; the form must not anchor with R$ 17,990–19,990.

### Seção 8 · Encerramento

| # | pt-BR | English gloss | Req. | Serves AC | Feeds |
|---|---|---|---|---|---|
| 28 | Você gostaria de testar o protótipo ou entrar na lista de espera? (Sim · Não) | Join prototype tests / waitlist? | no | AC1 (branch), AC2 | Contact list |
| 28a | (se Sim) Seu e-mail (validação de e-mail) | E-mail | no | AC2 (only personal-data field) | Separate contact sheet |
| 28b | (se Sim) WhatsApp (opcional) | WhatsApp (optional) | no | AC2 (only personal-data field) | Separate contact sheet |
| 29 | Autorizo o uso do meu e-mail somente para contato sobre esta pesquisa e o lançamento (LGPD) (caixa de seleção) | LGPD consent checkbox | required if e-mail given | AC2 | Consent record |
| 30 | Mais algum comentário? (parágrafo, opcional) | Any other comments? | no | AC5 (verbatims) | Verbatims |

Open points: the design's Q29 text mentions only e-mail, while Q28b collects WhatsApp. NFR47 and AC2 require explicit consent for all personal data collected. Decide whether the consent covers WhatsApp or Q28b is dropped: `____` **(confirm with counsel)**. Q30 is free text and respondents may type personal data; decide handling in the cleaning rules (section 4).

## 2. Build checklist for the Google Form (AC1)

- [ ] 9 sections (0–8), ~30 questions
- [ ] Branch on Q12 (to 12a) and Q28 (to 28a/28b)
- [ ] Required: Q6, 7, 13, 14, 15, 17, 18, 22–25
- [ ] Q18 "Selecione exatamente 3"
- [ ] Q22–25 numeric validation
- [ ] Settings: automatic e-mail collection OFF
- [ ] Text search for "Ladiani" and "Velora": zero hits
- [ ] Q29 required whenever Q28a is filled (Google Forms cannot do a conditional-required natively; implementation approach, e.g. putting Q29 in a section reached only after Sim and making it required there: `____`)

## 3. Consent and LGPD notes (all items: confirm with counsel)

I am not a lawyer; every item below is a question for counsel, not a conclusion.

- Lawful basis: AC2 and NFR47 require explicit consent. Confirm with counsel that consent under the LGPD (Lei 13.709/2018) is the right basis and that a checkbox is sufficient. Source to consult: https://www.planalto.gov.br/ccivil_03/_ato2015-2018/2018/lei/l13709.htm (to confirm current text).
- Controller identity in the consent text: legal name `____`, CNPJ/CPF `____`, contact channel for data subjects `____`. Confirm whether a data-protection officer contact is required: to confirm with counsel.
- Stated purposes: only "contact about this survey and the launch" (AC2). The draw winner is contacted through the consented channel only (AC5). Confirm whether a separate consent is needed for marketing of the launch: confirm with counsel.
- Withdrawal: describe how (e.g. reply to an address `____`) and what happens to the data on withdrawal.
- Retention/deletion rule (AC2 requires one stored): proposed shape, retention period `____`, deletion trigger `____`, who executes `____`. Period to be confirmed with counsel.
- Separation: contact data in a separate, access-restricted sheet; the anonymous answers sheet contains no e-mail, WhatsApp or link key back to the contact sheet. If a join key is needed for the prize draw, confirm whether that breaks the "anonymous" claim in Section 0: confirm with counsel.
- Minors: Q1 includes "Até 17". Confirm with counsel whether collecting contact data from under-18 respondents needs guardian consent or must be blocked on Q28a: confirm with counsel.
- Prize draw: confirm legal requirements for a draw/promotion in Brazil (authorization or notification rules may apply depending on the format): to confirm with counsel; do not assume it is exempt. Prize `____`, rules text `____`.
- Google Forms hosting: confirm implications of storing respondent data on Google servers (international transfer) and whether the notice must mention it: confirm with counsel.
- Per AC2 the confidential contact sheet should be stored outside the repository (Epic 11 intro: confidential items stay outside the repo; repo holds only a pointer and status). Location: `____`.

## 4. Sample plan and recruitment (options, blanks)

Target: ≥400 valid responses after cleaning (AC4). Number of raw responses needed to reach 400 valid: `____` (depends on the drop rate observed in the pilot and early data; do not assume).

Recruitment channels from the design and AC (each one: tick if used, record owner and date; "result" stays empty until real data exists):

| Channel (from design/AC) | Use? | Owner | Group/contact list | Date posted | Responses attributed |
|---|---|---|---|---|---|
| WhatsApp groups of keyboardists | ☐ | `____` | `____` | `____` | `____` |
| Facebook groups | ☐ | `____` | `____` | `____` | `____` |
| Church musicians | ☐ | `____` | `____` | `____` | `____` |
| Event / wedding musicians | ☐ | `____` | `____` | `____` | `____` |
| Teachers and music schools | ☐ | `____` | `____` | `____` | `____` |
| Instrument shops | ☐ | `____` | `____` | `____` | `____` |
| Instagram / YouTube keyboard influencers | ☐ | `____` | `____` | `____` | `____` |
| Other: `____` | ☐ | | | | |

Attribution of responses to channel needs a mechanism that does not collect personal data (e.g. one form link variant per channel, or a hidden pre-filled field). Method: `____`. Founder to decide; it must not break AC1 or AC2.

Pilot (AC3): 10 keyboardists, selected from `____`, run date `____`, timing method `____`. Pilot responses are marked and excluded from the final dataset (separate pilot form copy or a date cut-off: `____`). Log of pilot feedback and form changes: `____` (location).

Quotas / balance (AC4): the design says to watch region and usage context (church, band, events) and to push more outreach where a group is under-represented. Quota or alert thresholds: `____` (founder to choose; none are asserted here). The comparison population for judging "under-represented" needs an external source: `____` to confirm (no figure is quoted in this draft).

Margin-of-error claim: the design states ±5% at 95% confidence for 400 responses. That is a property of a simple random sample; this survey is a convenience sample via groups, so the claim should be treated as not applicable without caveat. Wording for the report: `____` (to confirm with a statistician if needed).

Prize draw: prize `____`, deadline `____`, draw method `____`, witness/record `____`, winner contacted only via the Q28a/28b channel with a valid Q29 consent (AC5).

## 5. Cleaning rules and analysis plan

### 5.1 Cleaning rules (AC4: must be written down; fill before closing collection)

Candidate rules to adopt or reject, each with decision `____`:

1. Exclude pilot responses (AC3).
2. Exclude test entries by the team (identify by `____`).
3. Duplicates: how detected without personal data in the anonymous sheet (e.g. identical full answer vectors and timestamps within `____`); contact-sheet duplicates by e-mail. Rule: `____`.
4. Van Westendorp consistency: AC4 names "too cheap" > "too expensive" (Q22 > Q25) as an example. The full expected ordering is Q22 ≤ Q23 ≤ Q24 ≤ Q25. Whether to drop a response on any ordering violation or only on the example: `____`.
5. Implausible values in Q22–25 (zero, negative, or extreme outliers): threshold `____`.
6. Straight-lining in Q14 and Q17 grids / empty required text: rule `____`.
7. Q18: exactly-3 is enforced by the form; verify count in the export.
8. Personal data typed into free text (Q7, Q15, Q16, Q19, Q30): redact before reporting verbatims. Rule `____`.
9. Record counts at each step: raw `____` → after each rule `____` → valid `____`.

### 5.2 Report content (AC5), with the analysis method per item

| AC5 item | Questions | Method (to be run on real data only) |
|---|---|---|
| Brand/model ranking | Q6–8 | Frequency counts by brand and normalised model text; base size n stated |
| NPS per brand | Q13 by Q6 | Standard NPS: % promoters (9–10) minus % detractors (0–6); minimum base size per brand to report: `____` |
| Top pains and missing features | Q14–16 | Q14 frequency table; Q15/Q16 coded into themes with quotes in respondents' words; coder and codebook `____` |
| Feature importance and top-3 ranking | Q17–18 | Importance: share Muito+Indispensável per feature; top-3 share from Q18; both side by side |
| Switching intent and barriers | Q20–21 | Q20 distribution; Q21 barrier frequency; optional cross by Q6 |
| Van Westendorp | Q22–25 | Cumulative curves; acceptable range and optimal point from the curve crossings, using the standard definitions; the report must state which crossing definitions it uses: `____` |
| Payment/instalment preference | Q26–27 | Frequency tables; cross with Q11 optional |
| Distribution report | Q2, Q3 (+ Q1, Q5) | Counts by region and usage context; under-represented groups and extra outreach done (AC4) |

Reference for the Van Westendorp method: a source URL is needed before the report cites it: `____` to confirm.

Cuts to consider, decided in advance to avoid fishing: by usage context (Q3), by pro level (Q5), by brand (Q6), by region (Q2). Minimum n to show a cut: `____`.

### 5.3 How results feed Story 11.5

| Survey output | Story 11.5 use | AC of 11.5 |
|---|---|---|
| Van Westendorp acceptable range and optimal price (Q22–25) | Compared with R$ 17,990–19,990 and the Story 11.4 minimum viable price | 11.5 AC1 |
| Q26–27 | Chooses the instalment offer (10 interest-free vs other) | 11.5 AC1 |
| Q17 importance and Q18 top-3 share, per feature | Feature table: importance, top-3 share, delivering epic, keep/raise/lower | 11.5 AC2 |
| Q16/Q19 unprompted wishes; Q14–15 pains | Evidence for raising/lowering priority; candidates for proposed scope changes | 11.5 AC2 |
| Q21 top-3 barriers | Each barrier needs a named answer and owner in the launch plan | 11.5 AC3 |
| Q20 switching intent | Context for the price decision; no volume forecast is implied | 11.5 AC1 |

Hand-off artifact: the AC5 results report in `_bmad-output/research/` (file name `____`).

## 6. Timeline and ownership blanks

| Step | Owner | Start | Done |
|---|---|---|---|
| Counsel review of consent text and draw | `____` | `____` | `____` |
| Build form (AC1) | `____` | `____` | `____` |
| Pilot of 10 (AC3) | `____` | `____` | `____` |
| Collection window (AC4) | `____` | `____` | `____` |
| Cleaning and distribution check (AC4) | `____` | `____` | `____` |
| Results report (AC5) | `____` | `____` | `____` |
| Prize draw and winner contact (AC5) | `____` | `____` | `____` |

Epic 11 deadlines are against the GTM timeline (Milestone 1 month 6, first units month 12, v1 launch month 18). Whether Story 11.3 or 11.5 has an explicit gate date: `____` (not found in the Epic 11 intro blocked-by list; to confirm).

## 7. Open issues found while preparing

1. Q29 text covers e-mail only but Q28b collects WhatsApp (see section 1, Seção 8).
2. "Anonymous" claim in Section 0 versus contact collection and prize draw (section 3).
3. ±5%/95% claim in the design assumes random sampling (section 4).
4. Conditional-required Q29 is not a native Google Forms feature (section 2).
5. Minors in Q1 band "Até 17" (section 3).

## Founder decision on consent (2026-10-19)

Contact data (e-mail, WhatsApp) and the prize draw move to a **separate form** linked from the end of the questionnaire; the questionnaire itself collects no personal data and stays anonymous. A random code shown after submission may be re-entered in the contact form only if the founder wants to correlate (otherwise no link key). The contact form carries its own consent text covering e-mail AND WhatsApp and every purpose (pre-sale notice, prize draw). This resolves open points 1, 2 and 4 of section 7; legal text and draw rules still need counsel (LGPD, promotions).
