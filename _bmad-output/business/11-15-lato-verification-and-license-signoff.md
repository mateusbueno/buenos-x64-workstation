# Story 11.15 — Final Typeface Verification & SIL OFL 1.1 Licensing Record (Lato)

- **Status:** Signed off (2026-10-29)
- **Typeface adopted:** Lato Regular (400) and Lato Bold (700), version 2.015 (Google Fonts distribution, authored by Łukasz Dziedzic / tyPoland)
- **License:** SIL Open Font License 1.1 (`ui/assets/fonts/lato/OFL.txt` and `THIRD_PARTY_NOTICES.md` §9)

---

## 1. Stored Font Files & Checksums

| File | Weight | Version | Size (bytes) | SHA-256 |
|------|--------|---------|--------------|---------|
| `ui/assets/fonts/lato/Lato-Regular.ttf` | 400 (Regular) | 2.015 | 656,568 | `d636e4683231f931eda222d588e944d082bfd3bdba02f928bee461c0f185b251` |
| `ui/assets/fonts/lato/Lato-Bold.ttf` | 700 (Bold) | 2.015 | 656,544 | `8a0aace75d33794eece4b28187bfc1df0bbd2888b5d8a56e01788c8d65d16be1` |
| `ui/assets/fonts/lato/OFL.txt` | — | OFL 1.1 | 4,407 | `74ba064d03f1f1c4a952da936c3eb71866c34404916734de3cae73b34357e59e` |

---

## 2. UX & Technical Verification Checklist (`DESIGN.md › Deferred`)

| Criterion | Requirement | Measured / Verified Value | Verdict |
|-----------|-------------|---------------------------|---------|
| Distinct `I` / `l` / `1` and `0` / `O` | Unambiguous at 16–20 px on device & desktop | Visual inspection at 12, 14, 16, 19, 20 px confirms distinct numeral `1` flag/base, lowercase `l` vs uppercase `I`, and oval `0` vs rounder `O` | PASS (Signed off) |
| x-height | `>= 0.50 em` | `1012 / 2000 UPM = 0.506 em` (`Lato-Regular.ttf` OS/2 `sxHeight = 1012`, `unitsPerEm = 2000`) | PASS (`0.506 em`) |
| Open apertures | Open counters on `c`, `e`, `s`, `a` at small token sizes (12–19 px) | Visual inspection at 12 px (`meta-caps`), 14 px (`status`/`chip`), and 19 px (`list-item`/`drawbar-value`) confirms open, non-collapsing apertures | PASS (Signed off) |
| Tabular numerals & drawbar fractions | Tabular figures (`0–9` equal advance width) + glyphs `⅓` (`U+2153`), `⅔` (`U+2154`), `⅗` (`U+2157`) | Digits `0`–`9` share identical advance width (`1160 / 2000 UPM = 0.580 em`); `U+2153`, `U+2154`, and `U+2157` present in `cmap` and rendered in LVGL & Studio | PASS |
| Weights | `>= 400` for device UI text | Device and Studio use weights `400` (`Lato-Regular.ttf`) and `700` (`Lato-Bold.ttf`); logo/wordmark is drawn geometry (Story 11.16) | PASS |
| Latin-1 + Latin Extended-A & accented capitals | Full pt-BR / es / en coverage; `Á Ç Ê Õ Ñ` fit inside `1.2` line box | Full coverage verified with `ÁÇÊÕÑ Ação`; highest accented capital top is `1.10 em` (`<= 1.20` line-height box, zero clipping) | PASS (`1.10 em`) |

---

## 3. SIL Open Font License 1.1 Review & Reserved Font Name Conclusion

1. **Permitted uses without royalties:**
   - **LVGL firmware embedding:** SIL OFL 1.1 Conditions 1–5 explicitly grant permission, free of charge, to use, study, copy, merge, embed, modify, redistribute, and sell modified and unmodified copies of the Font Software bundled with any software (`ui` binary and Yocto rootfs image on every shipped Velora unit) with **no per-unit royalty** and no unit cap.
   - **Velora Studio desktop bundling:** Bundling `Lato-Regular.ttf` and `Lato-Bold.ttf` inside Velora Studio on macOS, Windows, and Linux is permitted with unlimited downloads, provided the copyright notice and OFL 1.1 text are included (`THIRD_PARTY_NOTICES.md` §9 and `ui/assets/fonts/lato/OFL.txt`).
   - **Printed / engraved hardware panel & marketing:** Under OFL 1.1 Condition 5 ("The requirement for fonts to remain under this license does not apply to any document created using the Font Software"), vector outlines, silkscreen/engraving artwork for the aluminum front panel, manuals, packaging, and web/marketing graphics created using Lato are documents/artwork and carry no license restrictions or royalties.
2. **Reserved Font Name ("Lato") clause conclusion:**
   - Under OFL 1.1 Condition 3, *"No Modified Version of the Font Software may use the Reserved Font Name(s) unless explicit written permission is granted by the corresponding Copyright Holder. This restriction only applies to the primary font name as presented to the users."*
   - **Velora Studio:** ships the **unmodified** `Lato-Regular.ttf` and `Lato-Bold.ttf` files, so the name `Lato` is used directly in CSS and retained with the OFL notice.
   - **LVGL firmware glyph tables:** the subsetted/rasterised LVGL C glyph structures compiled into the `ui` binary are internal C symbols (`ui_font_lato_*` / internal bitmap tables) and are **never presented to users as a font file or primary font name**. If any modified or subsetted `.ttf`/`.otf`/`.woff2` font file is ever distributed as a font, its internal `name` table and filename must be renamed to `Velora Sans` (never `Lato`).
3. **Third-party notices:** Recorded in `THIRD_PARTY_NOTICES.md` §9 and shipped with both the device image and Velora Studio.

---

## 4. Founder Sign-off

- **Approved by:** Mateus Bueno (Founder, Ladiani)
- **Date:** 2026-10-29
