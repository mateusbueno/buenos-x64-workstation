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
