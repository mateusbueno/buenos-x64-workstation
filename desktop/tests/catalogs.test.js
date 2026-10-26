// Story 9.8: the catalogs embedded in the app (src/catalogs.js) and locale/*.json must not drift: every embedded key is in the locale file with the same text (so the
// string lint covers it), and the three languages carry the same keys.
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";

import { CATALOGS } from "../src/catalogs.js";

const LOCALES = { en: "en.json", es: "es.json", "pt-BR": "pt-BR.json" };
const read = (lang) => JSON.parse(fs.readFileSync(new URL(`../../locale/${LOCALES[lang]}`, import.meta.url), "utf8"));

for (const lang of Object.keys(LOCALES)) {
  test(`catalog ${lang} matches locale/${LOCALES[lang]}`, () => {
    const json = read(lang);
    for (const [k, v] of Object.entries(CATALOGS[lang])) assert.equal(json[k], v, `${lang}: ${k}`);
  });
}

test("the three embedded catalogs have the same keys", () => {
  const en = Object.keys(CATALOGS.en).sort();
  assert.deepEqual(Object.keys(CATALOGS.es).sort(), en);
  assert.deepEqual(Object.keys(CATALOGS["pt-BR"]).sort(), en);
});

test("every studio.* key of the locale files is embedded (so the page can show it)", () => {
  for (const lang of Object.keys(LOCALES)) {
    for (const k of Object.keys(read(lang)).filter((x) => x.startsWith("studio."))) assert.ok(k in CATALOGS[lang], `${lang}: ${k} is not embedded`);
  }
});

// Story 9.13: every locale key the Setlists view can ask for (literals in its two modules, plus the keys built from a limit name) exists in all three catalogs.
test("every studio.* key used by the Setlists view exists in en, es and pt-BR", () => {
  const keys = new Set(["studio.setlists.err.limit_setlists", "studio.setlists.err.limit_tags", "studio.setlists.err.limit_live_sets_per_setlist", "studio.setlists.err.limit_tags_per_setlist"]);
  for (const f of ["setlists_view.js", "setlists_dom.js"]) {
    const src = fs.readFileSync(new URL(`../src/${f}`, import.meta.url), "utf8");
    for (const m of src.matchAll(/"(studio\.[a-z0-9_.]+)"/g)) if (!m[1].endsWith("_")) keys.add(m[1]);
  }
  assert.ok(keys.size > 60);
  for (const lang of Object.keys(CATALOGS)) for (const k of keys) assert.ok(k in CATALOGS[lang], `${lang}: ${k}`);
  // the index.html button and the app wiring use these too
  for (const k of ["studio.sidebar.manage_setlists", "studio.offline.hint"]) for (const lang of Object.keys(CATALOGS)) assert.ok(k in CATALOGS[lang], `${lang}: ${k}`);
});

// Printf-style parity: every translation takes the same %d / %s placeholders, in the same order, as the English text (the app fills them positionally).
test("placeholder sequences (%d / %s) are identical in en, es and pt-BR", () => {
  const seq = (t) => (t.match(/%[ds]/g) || []).join("");
  for (const k of Object.keys(CATALOGS.en)) {
    for (const lang of ["es", "pt-BR"]) assert.equal(seq(CATALOGS[lang][k]), seq(CATALOGS.en[k]), `${lang}: ${k}`);
  }
});
