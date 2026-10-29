// Story 9.9: the Part Editor view model and controller (no DOM): registry-driven sections, the Studio Knob rules, the command table, coalescing with a fake clock,
// optimistic values and the revert on a typed error, against tests/support/fake_unit.js.
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";

import { REGISTRY } from "../src/params_registry.js";
import {
  ASSIGN_SLOTS,
  EditSender,
  PartEditorController,
  REFLOW,
  applyDrag,
  applyKey,
  applyWheel,
  assignableParams,
  buildPartEditorModel,
  columnsFor,
  commandKind,
  envelopePolyline,
  formatValue,
  knobSpec,
  paramLabel,
  parseEntry,
  partParams,
  quantise,
  requests,
} from "../src/part_editor.js";
import { FakeUnit, CODE } from "./support/fake_unit.js";
import { CATALOGS } from "../src/catalogs.js";

const def = (name) => REGISTRY.params.find((d) => d.name === name);
const spec = (name) => knobSpec(def(name), REGISTRY);
const tr = (key) => CATALOGS.en[key] ?? key;

// ---- a fake clock -------------------------------------------------------------------------------------------------------------------------------------------
function makeClock() {
  let t = 1000;
  let id = 0;
  const timers = new Map();
  const clock = {
    now: () => t,
    setTimer: (f, ms) => { const k = ++id; timers.set(k, { at: t + ms, f }); return k; },
    clearTimer: (k) => timers.delete(k),
    async tick() { for (let i = 0; i < 12; i++) await new Promise((r) => setImmediate(r)); },
    async advance(ms) {
      const end = t + ms;
      for (;;) {
        let next = null;
        for (const [k, v] of timers) if (v.at <= end && (!next || v.at < next[1].at)) next = [k, v];
        if (!next) break;
        timers.delete(next[0]);
        t = Math.max(t, next[1].at);
        next[1].f();
        await clock.tick();
      }
      t = end;
      await clock.tick();
    },
    pending: () => timers.size,
  };
  return clock;
}

const rig2 = (o) => rig(o);
function rig({ engines, throttleMs = 30, unitOptions } = {}) {
  const unit = new FakeUnit({ engines, ...unitOptions });
  const clock = makeClock();
  const errors = [];
  let changes = 0;
  const ctl = new PartEditorController({
    request: (m) => unit.request(m), clock: clock.now, setTimer: clock.setTimer, clearTimer: clock.clearTimer, throttleMs,
    onChange: () => { changes++; }, onError: (e) => errors.push(e), structuralThrottleMs: 140, rereadMs: [150, 500],
  });
  const engineOf = { 1: "sample", 2: "organ", 3: "fm", 4: "va" };
  const open = async (part) => {
    ctl.select({ liveSetId: unit.liveSetId, part, engine: engineOf[unit.parts[part].engine_type_id] });
    ctl.onState(unit.state());
    await clock.tick();
    await clock.advance(100);
  };
  return { unit, clock, ctl, errors, open, get changes() { return changes; } };
}

const knobs = (m) => m.sections.flatMap((s) => s.groups.flatMap((g) => g.knobs));
const byId = (m, id) => knobs(m).find((k) => k.id === id);

// ---- registry-driven sections ---------------------------------------------------------------------------------------------------------------------------------
test("the four sections are built from the registry for the part's engine", () => {
  const m = buildPartEditorModel({ registry: REGISTRY, partIndex: 0, engine: "organ", editable: false });
  assert.deepEqual(m.sections.map((s) => s.id), ["sound", "amp_eq", "effects", "zone"]);
  const drawbars = m.sections[0].groups.find((g) => g.id === "organ.drawbars");
  assert.equal(drawbars.knobs.length, 9);
  assert.ok(m.sections[0].groups.some((g) => g.id === "organ.rotary"));
  assert.ok(!m.sections[0].groups.some((g) => g.id.startsWith("fm.") || g.id.startsWith("va.")));
  const fm = buildPartEditorModel({ registry: REGISTRY, partIndex: 1, engine: "fm", editable: false });
  assert.equal(fm.sections[0].groups.filter((g) => /^fm\.op[1-6]$/.test(g.id)).length, 6);
  const va = buildPartEditorModel({ registry: REGISTRY, partIndex: 3, engine: "va", editable: false });
  assert.deepEqual(va.sections[0].groups.map((g) => g.id).filter((g) => g.startsWith("va.")).sort(), ["va.envelopes", "va.filter", "va.lfo", "va.mixer", "va.noise", "va.osc1", "va.osc2", "va.sub", "va.voice"]);
  // a sample part and an unknown engine get the generic sections only
  for (const e of ["sample", "mystery"]) {
    const g = buildPartEditorModel({ registry: REGISTRY, partIndex: 4, engine: e, editable: false });
    assert.deepEqual(g.sections[0].groups.map((x) => x.id), ["sound.main"]);
    assert.ok(g.sections[1].groups.length > 0 && g.sections[3].groups.length > 0);
  }
});

test("every registry parameter the engine shows is drawn: the number of knobs follows the registry", () => {
  for (const engine of ["sample", "organ", "fm", "va"]) {
    const m = buildPartEditorModel({ registry: REGISTRY, partIndex: 0, engine, editable: false });
    const drawn = knobs(m).filter((k) => k.kind === "param").map((k) => k.paramId).sort((a, b) => a - b);
    const expected = partParams(REGISTRY, engine).map((d) => d.id).sort((a, b) => a - b);
    assert.deepEqual(drawn, expected, engine);
  }
});

test("a parameter added to a registry fixture appears without any UI code", () => {
  const fixture = JSON.parse(JSON.stringify(REGISTRY));
  fixture.params.push({ id: 4000, name: "brand_new_knob", class: "performance", type: "float", scope: "part", unit: "ratio", min: 0, max: 1, default: 0.5, enum: null, cc: -1, assignable: true, localeKey: "", section: "sound", engine: "any", group: "other" });
  fixture.params.push({ id: 4001, name: "fx_new_switch", class: "performance", type: "bool", scope: "part", unit: "flag", min: 0, max: 1, default: 0, enum: null, cc: -1, assignable: true, localeKey: "", section: "effects", engine: "any", group: "fx.custom" });
  fixture.params.push({ id: 4002, name: "organ_only_new", class: "performance", type: "int", scope: "part", unit: "step", min: 0, max: 8, default: 0, enum: null, cc: -1, assignable: false, localeKey: "", section: "sound", engine: "organ", group: "organ.drawbars" });
  const sample = buildPartEditorModel({ registry: fixture, partIndex: 0, engine: "sample", editable: true, live: true, state: { parts: [{ part: 0 }] } });
  const k = byId(sample, "brand_new_knob");
  assert.ok(k && k.type === "float" && k.value === 0.5 && !k.readOnly);
  assert.ok(sample.sections[0].groups.some((g) => g.id === "other"));
  assert.ok(sample.sections[2].groups.some((g) => g.id === "fx.custom"));
  assert.ok(!byId(sample, "organ_only_new"));
  const organ = buildPartEditorModel({ registry: fixture, partIndex: 0, engine: "organ", editable: false });
  assert.equal(organ.sections[0].groups.find((g) => g.id === "organ.drawbars").knobs.length, 10);
  assert.equal(paramLabel({ name: "brand_new_knob", localeKey: "" }, tr), "brand new knob"); // no catalog entry: the name, readable
});

test("every part-visible registry parameter has a label and every enum value a translation in en, es and pt-BR", () => {
  for (const lang of Object.keys(CATALOGS)) {
    const t = (k) => CATALOGS[lang][k] ?? k;
    for (const d of REGISTRY.params.filter((x) => x.section !== "master" && x.section !== "hidden")) {
      const label = paramLabel(d, t);
      assert.ok(label !== d.name.replace(/_/g, " ") || /^[a-z]+$/.test(d.name), `${lang}: no label for ${d.name}`);
    }
    for (const [en, vals] of Object.entries(REGISTRY.enums)) for (const v of vals) assert.ok(`studio.enum.${en}.${v}` in CATALOGS[lang], `${lang}: ${en}.${v}`);
    const groups = new Set(REGISTRY.params.map((d) => d.group));
    for (const g of groups) if (g !== "master" && g !== "timbre") assert.ok(`studio.group.${g}` in CATALOGS[lang], `${lang}: group ${g}`);
  }
});

// ---- the knob rules --------------------------------------------------------------------------------------------------------------------------------------------
test("knob specs: step, large step and quantisation per type", () => {
  const drawbar = spec("drawbar_8");
  assert.deepEqual([drawbar.type, drawbar.min, drawbar.max, drawbar.step, drawbar.large], ["int", 0, 8, 1, 1]);
  const fmLevel = spec("fm_op1_level_1");
  assert.equal(fmLevel.large, 10);
  const rel = spec("part_release");
  assert.equal(rel.log, true); // 1..5000 ms: a logarithmic grid, so a 5 ms attack is reachable
  assert.equal(spec("va_filter_cutoff").log, true);
  assert.equal(spec("drawbar_8").log, false);
  assert.equal(spec("part_brightness").log, false);
  assert.equal(spec("key_click").step, 0.01);
  assert.equal(spec("part_eq_low").step, 0.5);
  const en = spec("va_filter_type");
  assert.deepEqual([en.min, en.max, en.type], [0, 3, "enum"]);
  assert.equal(spec("percussion_on").max, 1);
  // quantise: clamp + snap
  assert.equal(quantise(drawbar, 9.4), 8);
  assert.equal(quantise(drawbar, -3), 0);
  assert.equal(quantise(drawbar, 3.6), 4);
  assert.equal(quantise(spec("percussion_on"), 0.6), 1);
  assert.equal(quantise(spec("percussion_on"), 0.4), 0);
  assert.equal(quantise(en, 7), 3);
  assert.equal(quantise(spec("key_click"), 0.3333), 0.33);
  assert.equal(quantise(rel, 1), 1); // the range ends are reachable
  assert.ok(Math.abs(quantise(rel, 120) - 120) / 120 < 0.06); // snapped to the log grid, 3 significant digits
  assert.equal(quantise(rel, 9999), 5000);
  assert.equal(quantise(rel, 0), 1);
  assert.equal(quantise(spec("part_eq_low"), 14.9), 15);
  assert.equal(quantise(drawbar, NaN), 0);
});

test("keys: Up/Down 1 step, Shift 10, PageUp/PageDown large, Home/End min/max, Enter entry, Space toggles a switch", () => {
  const s = spec("fm_op1_level_1"); // int 0..99, large 10
  const k = (key, shiftKey = false) => applyKey(s, 50, { key, shiftKey });
  assert.equal(k("ArrowUp").value, 51);
  assert.equal(k("ArrowRight").value, 51);
  assert.equal(k("ArrowDown").value, 49);
  assert.equal(k("ArrowLeft").value, 49);
  assert.equal(k("ArrowUp", true).value, 60);
  assert.equal(k("ArrowDown", true).value, 40);
  assert.equal(k("PageUp").value, 60);
  assert.equal(k("PageDown").value, 40);
  assert.equal(k("Home").value, 0);
  assert.equal(k("End").value, 99);
  assert.deepEqual(k("Enter"), { handled: true, entry: true });
  assert.equal(k("x").handled, false);
  assert.equal(k("Tab").handled, false);
  assert.equal(applyKey(s, 95, { key: "ArrowUp", shiftKey: true }).value, 99); // clamped
  assert.equal(applyKey(s, 3, { key: "ArrowDown", shiftKey: true }).value, 0);
  const f = spec("part_release"); // logarithmic 1..5000 ms, 100 points
  const up = applyKey(f, 300, { key: "ArrowUp" }).value;
  assert.ok(up > 300 && up < 330, String(up));
  const up10 = applyKey(f, 300, { key: "ArrowUp", shiftKey: true }).value;
  assert.ok(up10 > 300 * 2 && up10 < 300 * 2.5, String(up10)); // 10 grid points = x2.34
  assert.equal(applyKey(f, 300, { key: "PageUp" }).value, up10);
  assert.equal(applyKey(f, 300, { key: "PageDown" }).value < 300, true);
  assert.equal(applyKey(f, 4900, { key: "End" }).value, 5000);
  assert.equal(applyKey(f, 1, { key: "ArrowDown" }).value, 1);
  // a short attack and a low cutoff are reachable by keys alone from the minimum
  let v = 1;
  for (let i = 0; i < 100 && v < 5; i++) v = applyKey(f, v, { key: "ArrowUp" }).value;
  assert.ok(v >= 5 && v < 5.6, `reached ${v}`);
  const cut = spec("va_filter_cutoff");
  v = 20;
  for (let i = 0; i < 100 && v < 100; i++) v = applyKey(cut, v, { key: "ArrowUp" }).value;
  assert.ok(v >= 100 && v < 112, `reached ${v}`);
  const sw = spec("percussion_on");
  assert.equal(applyKey(sw, 0, { key: " " }).value, 1);
  assert.equal(applyKey(sw, 1, { key: " " }).value, 0);
  assert.equal(applyKey(sw, 0, { key: "ArrowUp" }).value, 1);
  assert.equal(applyKey(sw, 1, { key: "ArrowDown" }).value, 0);
  assert.equal(applyKey(s, 5, { key: " " }).handled, false);
  const en = spec("va_filter_type"); // enum 0..3, large 1
  assert.equal(applyKey(en, 0, { key: "ArrowDown" }).value, 0);
  assert.equal(applyKey(en, 1, { key: "End" }).value, 3);
  assert.equal(applyKey(en, 1, { key: "PageUp" }).value, 2);
  // an unknown current value starts from the minimum
  assert.equal(applyKey(s, null, { key: "ArrowUp" }).value, 1);
});

test("wheel and drag map onto the same quantised values", () => {
  const s = spec("fm_op1_level_1");
  assert.equal(applyWheel(s, 50, { deltaY: -100 }), 51);
  assert.equal(applyWheel(s, 50, { deltaY: 100 }), 49);
  assert.equal(applyWheel(s, 50, { deltaY: -3 }), 51);
  assert.equal(applyWheel(s, 50, { deltaY: -100, shiftKey: true }), 60);
  assert.equal(applyWheel(s, 50, { deltaY: 0 }), 50);
  // drag: up (negative dy) raises; the full range takes 200 px, Shift makes it 5 times finer
  assert.equal(applyDrag(s, 0, 0, -200, false), 99);
  assert.equal(applyDrag(s, 0, 0, -100, false), 50);
  assert.equal(applyDrag(s, 50, 20, 0, false), 60);
  assert.equal(applyDrag(s, 50, 0, 100, false), 1);
  assert.equal(applyDrag(s, 50, 0, -100, true), 60);
  assert.equal(applyDrag(s, 90, 0, -1000, false), 99); // clamped
  const sw = spec("percussion_on");
  assert.equal(applyDrag(sw, 0, 0, -30, false), 1);
  assert.equal(applyDrag(sw, 1, 0, 30, false), 0);
  assert.equal(applyDrag(sw, 0, 0, -10, false), 0);
  const en = spec("va_filter_type");
  assert.equal(applyDrag(en, 0, 0, -50, false), 2);
  const small = spec("drawbar_8");
  assert.equal(applyDrag(small, 0, 0, -24, false), 1); // a small integer range steps by 24 px
});

test("value text carries the unit; aria-valuetext uses it", () => {
  assert.equal(formatValue(spec("part_release"), 300, tr), "300 ms");
  assert.equal(formatValue(spec("part_eq_low"), 3.5, tr), "+3.5 dB");
  assert.equal(formatValue(spec("part_eq_low"), -2, tr), "-2.0 dB");
  assert.equal(formatValue(spec("part_eq_low"), 0, tr), "0.0 dB");
  assert.equal(formatValue(spec("part_eq_mid_freq"), 1000, tr), "1000 Hz");
  assert.equal(formatValue(spec("key_click"), 0.5, tr), "50 %");
  assert.equal(formatValue(spec("va_filter_cutoff"), 2000, tr), "2000 Hz");
  assert.equal(formatValue(spec("va_lfo_rate"), 40, tr), "4.0 Hz");
  assert.equal(formatValue(spec("va_osc1_detune"), 5, tr), "+5 ct");
  assert.equal(formatValue(spec("va_osc1_octave"), -1, tr), "-1 oct");
  assert.equal(formatValue(spec("fm_transpose"), 7, tr), "+7 st");
  assert.equal(formatValue(spec("drawbar_8"), 8, tr), "8");
  assert.equal(formatValue(spec("percussion_on"), 1, tr), "On");
  assert.equal(formatValue(spec("percussion_on"), 0, tr), "Off");
  assert.equal(formatValue(spec("reverb_type"), 1, tr), "Hall");
  assert.equal(formatValue(spec("va_filter_type"), 1, tr), "LP 12");
  assert.equal(formatValue(spec("arp_mode"), 2, tr), "Up / Down");
  assert.equal(formatValue(spec("arp_mode"), null, tr), "—");
  assert.equal(formatValue(spec("arp_mode"), 99, tr), "—");
  assert.equal(formatValue({ type: "int", unit: "note", min: 0, max: 127 }, 36, tr), "C2");
  assert.equal(formatValue(spec("va_amp_attack"), 5, tr), "5.00 ms");
  assert.equal(formatValue(spec("arp_octaves"), 2, tr), "2 oct"); // an unsigned unit takes no "+"
  assert.equal(formatValue(spec("fm_op1_freq_fine"), 37, tr), "37"); // a 0..99 "ratio" is a plain number
  assert.equal(formatValue(knobSpec(def("master_comp_ratio"), REGISTRY), 4, tr), "4.0:1");
});

test("value entry: numbers, units, enum labels, switches; out of range and junk are refused", () => {
  const level = spec("fm_op1_level_1");
  assert.deepEqual(parseEntry(level, "42"), { ok: true, value: 42 });
  assert.deepEqual(parseEntry(level, " 42 "), { ok: true, value: 42 });
  assert.deepEqual(parseEntry(level, "100"), { ok: false, reason: "range" });
  assert.deepEqual(parseEntry(level, "-1"), { ok: false, reason: "range" });
  assert.deepEqual(parseEntry(level, "4.5"), { ok: false, reason: "invalid" });
  assert.deepEqual(parseEntry(level, "abc"), { ok: false, reason: "invalid" });
  assert.deepEqual(parseEntry(level, ""), { ok: false, reason: "invalid" });
  assert.deepEqual(parseEntry(level, "12 ms"), { ok: false, reason: "invalid" });
  assert.deepEqual(parseEntry(level, "1e3"), { ok: false, reason: "invalid" });
  const eq = spec("part_eq_low");
  assert.deepEqual(parseEntry(eq, "+3.5 dB"), { ok: true, value: 3.5 });
  assert.deepEqual(parseEntry(eq, "-3,5"), { ok: true, value: -3.5 });
  assert.deepEqual(parseEntry(eq, "−2"), { ok: true, value: -2 });
  assert.deepEqual(parseEntry(eq, "16"), { ok: false, reason: "range" });
  const rel = spec("part_release");
  assert.deepEqual(parseEntry(rel, "1.5 s"), { ok: true, value: 1500 });
  assert.deepEqual(parseEntry(rel, "250ms"), { ok: true, value: 250 });
  assert.deepEqual(parseEntry(rel, "6 s"), { ok: false, reason: "range" });
  const hz = spec("part_eq_mid_freq");
  assert.deepEqual(parseEntry(hz, "2k"), { ok: true, value: 2000 });
  assert.deepEqual(parseEntry(hz, "1.5 kHz"), { ok: true, value: 1500 });
  assert.deepEqual(parseEntry(hz, "100"), { ok: false, reason: "range" });
  const pct = spec("key_click");
  assert.deepEqual(parseEntry(pct, "50"), { ok: true, value: 0.5 });
  assert.deepEqual(parseEntry(pct, "50 %"), { ok: true, value: 0.5 });
  assert.deepEqual(parseEntry(pct, "25.5"), { ok: true, value: 0.255 }); // a typed value keeps one more decimal than the keys' step
  assert.deepEqual(parseEntry(pct, "101"), { ok: false, reason: "range" });
  const en = spec("va_filter_type");
  assert.deepEqual(parseEntry(en, "hp 12", tr), { ok: true, value: 3 });
  assert.deepEqual(parseEntry(en, "LP12", tr), { ok: true, value: 1 }); // the registry value name works too
  assert.deepEqual(parseEntry(en, "lp 99", tr), { ok: false, reason: "invalid" });
  assert.deepEqual(parseEntry(en, "bp12", tr), { ok: true, value: 2 });
  assert.deepEqual(parseEntry(en, "2", tr), { ok: true, value: 2 });
  assert.deepEqual(parseEntry(en, "9", tr), { ok: false, reason: "range" });
  const sw = spec("percussion_on");
  assert.deepEqual(parseEntry(sw, "on", tr), { ok: true, value: 1 });
  assert.deepEqual(parseEntry(sw, "Off", tr), { ok: true, value: 0 });
  assert.deepEqual(parseEntry(sw, "maybe", tr), { ok: false, reason: "invalid" });
  const bpm = knobSpec(def("tempo_bpm"), REGISTRY);
  assert.deepEqual(parseEntry(bpm, "120 bpm"), { ok: true, value: 120 });
  const lfo = spec("va_lfo_rate"); // deciHz shown as Hz
  assert.deepEqual(parseEntry(lfo, "4 Hz"), { ok: true, value: 40 });
});

test("reflow: the columns follow the TEXT size (rem), 4 -> 2 -> 1, at the smallest window and at wider ones", () => {
  // container widths of the 1280 px window (about 1024) and of a 1400 px window (about 1129); root 16 / 24 / 32 px = 100 / 150 / 200 % text
  for (const [w, expected] of [[1024, [4, 2, 1]], [1129, [4, 2, 1]], [1400, [5, 3, 2]]]) {
    assert.deepEqual([16, 24, 32].map((root) => columnsFor(w, root)), expected, `container ${w}px`);
  }
  assert.equal(columnsFor(500, 16), 1);
  assert.equal(columnsFor(0, 16), 1);
  assert.equal(columnsFor(1024, 0), 1);
  // monotonic: more text never gives more columns
  for (const w of [700, 900, 1024, 1129, 1300, 1600]) {
    const c = [16, 20, 24, 28, 32].map((root) => columnsFor(w, root));
    assert.deepEqual([...c].sort((x, y) => y - x), c, `container ${w}px`);
  }
  // the CSS says what columnsFor mirrors
  const css = fs.readFileSync(new URL("../src/style.css", import.meta.url), "utf8");
  assert.ok(css.includes(`grid-template-columns: repeat(auto-fit, minmax(min(100%, ${REFLOW.minColumnRem}rem), 1fr))`), "the rem-based auto-fit columns");
  assert.ok(css.includes(`@container (max-width: ${REFLOW.oneColumnEm}em)`), "the one-column query");
  assert.ok(!/\.pe-host \{[^}]*overflow-x: hidden/.test(css), "no overflow-x: hidden hiding an overflow");
});

test("the envelope polyline follows attack, decay, sustain and release", () => {
  const e1 = envelopePolyline({ attackMs: 2, decayMs: 1800, sustain: 0.5, releaseMs: 420 });
  assert.equal(e1.points.length, 5);
  assert.equal(e1.points[0].y, 64);
  assert.equal(e1.points[1].y, 8); // the peak
  assert.equal(e1.points[2].y, 36); // sustain 50 %
  assert.equal(e1.points[4].y, 64);
  assert.match(e1.pointsAttr, /^6,64 \d+,8 \d+,36 \d+,36 \d+,64$/);
  const xs = e1.points.map((p) => p.x);
  assert.deepEqual([...xs].sort((a, b) => a - b), xs);
  assert.ok(xs[4] <= 214);
  const slow = envelopePolyline({ attackMs: 4000, decayMs: 0, sustain: 1, releaseMs: 4000 });
  const fast = envelopePolyline({ attackMs: 1, decayMs: 0, sustain: 1, releaseMs: 1 });
  assert.equal(slow.points.length, 4); // no decay stage: peak is the sustain level
  assert.ok(slow.points[1].x > fast.points[1].x && slow.points[3].x > fast.points[3].x);
  assert.ok(slow.points[3].x <= 214);
  const v = buildPartEditorModel({ registry: REGISTRY, partIndex: 0, engine: "va", editable: false });
  assert.equal(v.sections[1].envelope, null); // unknown values (the Live Set does not sound): no polyline
});

// ---- the command table -------------------------------------------------------------------------------------------------------------------------------------------
test("every registry parameter maps onto a command or is deferred; the table names the bridge-allowed commands", () => {
  const kinds = new Map();
  for (const d of REGISTRY.params) kinds.set(d.name, commandKind(d));
  assert.equal(kinds.get("drawbar_8"), "perf");
  assert.equal(kinds.get("fm_op3_detune"), "perf");
  assert.equal(kinds.get("send_reverb_level"), "sends");
  assert.equal(kinds.get("send_delay_level"), "sends");
  assert.equal(kinds.get("part_output"), "output");
  assert.equal(kinds.get("insert_param_3"), "insert");
  assert.equal(kinds.get("reverb_time"), "master");
  assert.equal(kinds.get("tempo_bpm"), "perf");
  for (const n of ["arp_on", "arp_mode", "arp_rate", "arp_octaves", "arp_gate", "arp_swing", "arp_latch"]) assert.equal(kinds.get(n), "arp", n);
  assert.equal(kinds.get("timbre_ref"), null, "timbre_ref");
  // the bridge's message table (sysd/src/bridge_server.cpp) routes every command the editor sends to the engine for a paired client
  const src = fs.readFileSync(new URL("../../sysd/src/bridge_server.cpp", import.meta.url), "utf8");
  const allowed = new Set([...src.matchAll(/\{B::k\w+, "(\w+)", "[\w.]+", R::kEngine, false\}/g)].map((m) => m[1]));
  const snake = (n) => n.replace(/([a-z0-9])([A-Z])/g, "$1_$2").toLowerCase();
  const sample = [
    requests.perf(100, 0, 1), requests.sends(0, 0.1, 0.2), requests.output(0, 1), requests.master(530, 1), requests.chain(0, []), requests.volume(0, 0.5),
    requests.assign(1, 100), requests.assign(1, 0), requests.zone(0, { low_key: 36, high_key: 84 }), requests.arp(0, { arp_on: true, arp_rate: 2 }), requests.getPerformanceValues(),
  ];
  for (const r of sample) assert.ok(allowed.has(snake(Object.keys(r.body)[0])), Object.keys(r.body)[0]);
  assert.ok(allowed.size > 30);
});

test("request bodies are the JSON the Rust client maps onto ClientMessage", () => {
  assert.deepEqual(requests.perf(102, 3, 5), { body: { SetPerformanceParam: { param_id: 102, part: 3, value: 5 } } });
  assert.deepEqual(requests.sends(2, 0.5, 0.25), { body: { SetSendLevels: { part: 2, reverb: 0.5, delay: 0.25 } } });
  assert.deepEqual(requests.output(1, 1), { body: { SetPartOutput: { part: 1, output: 1 } } });
  assert.deepEqual(requests.chain(0, [{ type: 2, bypass: true, params: [0.1, 0.2, 0.3, 0.4, 0.5] }]), { body: { SetInsertChain: { part: 0, slots: [{ type: 2, bypass: true, params: [0.1, 0.2, 0.3, 0.4, 0.5] }] } } });
  assert.deepEqual(requests.volume(0, 0.4), { body: { SetPartVolume: { part: 0, volume: 0.4 } } });
  assert.deepEqual(requests.assign(2, 100), { body: { SetAssignSlot: { target: 1, slot: 2, param_id: 100 } } });
  assert.deepEqual(requests.assign(2, 0), { body: { ClearAssignSlot: { target: 1, slot: 2 } } });
  assert.deepEqual(requests.zone(0, { low_key: 36, high_key: 84 }), { body: { SetPartZone: { part: 0, low_key: 36, high_key: 84 } } });
  assert.deepEqual(requests.arp(0, { arp_on: true, arp_rate: 2 }), { body: { SetPartArp: { part: 0, arp_on: true, arp_rate: 2 } } });
});

// ---- the sender -------------------------------------------------------------------------------------------------------------------------------------------------
test("the sender: one in flight per key, latest value wins, throttled, never a queue", async () => {
  const clock = makeClock();
  const sent = [];
  let inflight = 0;
  let maxInflight = 0;
  const sender = new EditSender({
    request: async (m) => { inflight++; maxInflight = Math.max(maxInflight, inflight); sent.push(m.v); await new Promise((r) => setImmediate(r)); inflight--; return { body: { Ack: {} } }; },
    clock: clock.now, setTimer: clock.setTimer, clearTimer: clock.clearTimer, throttleMs: 30, baseRevision: () => 7,
  });
  // 100 pointer events, 1 ms apart: only about one send per 30 ms of the (fake) clock, and the last value goes out
  for (let i = 1; i <= 100; i++) {
    sender.submit("k", () => ({ v: i }), {});
    await clock.advance(1);
  }
  await clock.advance(200);
  assert.ok(sent.length <= 5 && sent.length >= 3, `sent ${sent.length}`);
  assert.equal(sent[sent.length - 1], 100);
  assert.equal(maxInflight, 1);
  assert.deepEqual(sent, [...sent].sort((a, b) => a - b));
  // two keys do not block each other
  const before = sent.length;
  sender.submit("a", () => ({ v: "a" }), {});
  sender.submit("b", () => ({ v: "b" }), {});
  await clock.advance(1);
  assert.deepEqual(sent.slice(before).sort(), ["a", "b"]);
  assert.equal(sender.busy, false);
});

test("the sender attaches base_revision (only when known) and reports transport failures", async () => {
  const clock = makeClock();
  const seen = [];
  let rev = 0;
  const done = [];
  const sender = new EditSender({
    request: async (m) => { seen.push(m.base_revision); if (m.boom) throw new Error("timed out"); return { body: { Ack: {} } }; },
    clock: clock.now, setTimer: clock.setTimer, clearTimer: clock.clearTimer, throttleMs: 0, baseRevision: () => rev,
    done: (k, reply, err) => done.push([k, !!reply, err && err.message]),
  });
  sender.submit("x", () => ({}), {});
  await clock.advance(1);
  rev = 42;
  sender.submit("x", () => ({}), {});
  await clock.advance(1);
  sender.submit("y", () => ({ boom: true }), {});
  await clock.advance(1);
  sender.submit("z", () => null, {}); // a builder may skip
  await clock.advance(1);
  assert.deepEqual(seen, [undefined, 42, 42]);
  assert.deepEqual(done, [["x", true, null], ["x", true, null], ["y", false, "timed out"]]);
});

// ---- the controller against the fake unit -------------------------------------------------------------------------------------------------------------
test("a performance edit is sent with base_revision, shown at once and confirmed by the live value", async () => {
  const r = rig();
  await r.open(2); // an FM? no: part 2 is VA? engines = [1,3,4,2,...]: part 2 is VA
  assert.equal(r.ctl.engine, "va");
  const m0 = r.ctl.model();
  assert.equal(m0.editable, true);
  const cutoff = byId(m0, "va_filter_cutoff");
  assert.equal(cutoff.value, 2000);
  assert.equal(cutoff.readOnly, false);
  const shown = r.ctl.edit(cutoff.key, 4000);
  assert.ok(Math.abs(shown - 4000) < 100); // snapped to the logarithmic grid
  assert.equal(byId(r.ctl.model(), "va_filter_cutoff").value, shown); // optimistic, at once
  assert.equal(byId(r.ctl.model(), "va_filter_cutoff").pending, true);
  await r.clock.advance(5);
  const sent = r.unit.log.filter((l) => l[0] === "SetPerformanceParam");
  assert.deepEqual(sent.map((l) => l[1]), [{ param_id: def("va_filter_cutoff").id, part: 2, value: shown }]);
  assert.equal(sent[0][2], r.unit.liveRevision); // base_revision = the Live Set revision the view was loaded at (Story 9.10)
  await r.clock.advance(300);
  const after = byId(r.ctl.model(), "va_filter_cutoff");
  assert.equal(after.value, shown);
  assert.equal(after.pending, false); // confirmed by GetPerformanceValues
  assert.equal(r.unit.perf.get(`${def("va_filter_cutoff").id}:2`), shown);
  assert.equal(r.errors.length, 0);
});

test("a drag floods nothing: at most one request in flight per knob, throttled, last value wins", async () => {
  const r = rig();
  await r.open(3); // organ
  const k = byId(r.ctl.model(), "drawbar_16");
  r.unit.delay = 3;
  for (let i = 0; i <= 8; i++) {
    r.ctl.edit(k.key, i);
    await r.clock.advance(2);
  }
  for (let i = 8; i >= 0; i--) {
    r.ctl.edit(k.key, i);
    await r.clock.advance(2);
  }
  await r.clock.advance(300);
  const sent = r.unit.log.filter((l) => l[0] === "SetPerformanceParam").map((l) => l[1].value);
  assert.ok(sent.length < 18 && sent.length >= 2, `sent ${sent.length}`);
  assert.equal(sent[sent.length - 1], 0);
  assert.equal(r.ctl.sender.maxInFlight, 1);
  assert.equal(r.unit.perf.get(`${def("drawbar_16").id}:3`), 0);
  assert.equal(byId(r.ctl.model(), "drawbar_16").value, 0);
});

test("a typed error puts the knob back to the confirmed value and reports it; a later good edit works", async () => {
  const r = rig();
  await r.open(3);
  const k = byId(r.ctl.model(), "drawbar_8");
  assert.equal(k.value, 8);
  r.unit.fail = (name) => (name === "SetPerformanceParam" ? CODE.BUSY : null);
  r.ctl.edit(k.key, 3);
  assert.equal(byId(r.ctl.model(), "drawbar_8").value, 3);
  await r.clock.advance(5);
  assert.equal(byId(r.ctl.model(), "drawbar_8").value, 8); // reverted
  assert.equal(byId(r.ctl.model(), "drawbar_8").pending, false);
  assert.equal(r.errors.length, 1);
  assert.deepEqual([r.errors[0].key, r.errors[0].code, r.errors[0].message], [k.key, CODE.BUSY, "injected"]);
  r.unit.fail = null;
  r.ctl.edit(k.key, 5);
  await r.clock.advance(300);
  assert.equal(byId(r.ctl.model(), "drawbar_8").value, 5);
  // a transport failure (the request rejects) reverts the same way
  r.unit.request = async () => { throw new Error("The Velora did not answer"); };
  r.ctl.edit(k.key, 1);
  await r.clock.advance(5);
  assert.equal(byId(r.ctl.model(), "drawbar_8").value, 5);
  assert.equal(r.errors[1].message, "The Velora did not answer");
});

test("when a newer value is in flight, an old rejection does not revert it", async () => {
  const r = rig();
  await r.open(3);
  const k = byId(r.ctl.model(), "drawbar_4");
  let calls = 0;
  const real = r.unit.request.bind(r.unit);
  r.unit.request = async (m) => {
    calls++;
    if (calls === 1) { for (let i = 0; i < 6; i++) await new Promise((res) => setImmediate(res)); return r.unit.err(CODE.BUSY, "first fails"); }
    return real(m);
  };
  r.ctl.edit(k.key, 2);
  await r.clock.advance(1);
  r.ctl.edit(k.key, 6);
  await r.clock.advance(300);
  assert.equal(byId(r.ctl.model(), "drawbar_4").value, 6);
  assert.equal(r.unit.perf.get(`${def("drawbar_4").id}:3`), 6);
});

test("sends: the LIVE value is shown; an edit sends SetPerformanceParam (sounds at once) AND SetSendLevels (persists)", async () => {
  const r = await rig2();
  await r.open(0);
  const rev = byId(r.ctl.model(), "send_reverb_level");
  assert.equal(rev.command, "sends");
  assert.equal(rev.value, 0.25);
  r.ctl.edit(rev.key, 0.6);
  await r.clock.advance(5);
  await r.clock.advance(300);
  const perf = r.unit.log.find((x) => x[0] === "SetPerformanceParam");
  assert.deepEqual(perf[1], { param_id: 400, part: 0, value: 0.6 });
  const l = r.unit.log.find((x) => x[0] === "SetSendLevels");
  assert.deepEqual(l[1], { part: 0, reverb: 0.6, delay: 0.1 });
  assert.ok(r.unit.audibleLog.some((a) => a[0] === 400 && a[2] === 0.6)); // the mixer consumes the live value
  r.ctl.onState(r.unit.state());
  await r.clock.advance(300);
  const k = byId(r.ctl.model(), "send_reverb_level");
  assert.equal(k.value, 0.6);
  assert.equal(k.pending, false);
  // a CC / encoder / scene move of the LIVE value is what the knob shows, not the snapshot's initial level
  r.unit.perf.set("400:0", 0.9);
  r.unit.bump();
  r.ctl.onState(r.unit.state());
  await r.clock.advance(300);
  assert.equal(byId(r.ctl.model(), "send_reverb_level").value, 0.9);
  assert.equal(r.unit.parts[0].send_reverb, 0.6); // the initial level is unchanged
});

test("B2: reverb then delay edited together: both are answered, neither stays pending", async () => {
  const r = await rig2();
  await r.open(0);
  r.ctl.edit("p:400", 0.5);
  r.ctl.edit("p:401", 0.4);
  await r.clock.advance(5);
  await r.clock.advance(500);
  assert.equal(byId(r.ctl.model(), "send_reverb_level").pending, false);
  assert.equal(byId(r.ctl.model(), "send_delay_level").pending, false);
  const last = r.unit.log.filter((x) => x[0] === "SetSendLevels").at(-1);
  assert.deepEqual(last[1], { part: 0, reverb: 0.5, delay: 0.4 });
  assert.equal(r.errors.length, 0);
});

test("part volume and output use SetPartVolume and SetPartOutput", async () => {
  const r = rig();
  await r.open(0);
  const vol = byId(r.ctl.model(), "part_volume");
  assert.equal(vol.value, 0.7);
  r.ctl.edit(vol.key, 0.42);
  const out = byId(r.ctl.model(), "part_output");
  assert.equal(out.value, 0);
  r.ctl.edit(out.key, 1);
  await r.clock.advance(200);
  assert.deepEqual(r.unit.log.find((l) => l[0] === "SetPartVolume")[1], { part: 0, volume: 0.42 });
  assert.deepEqual(r.unit.log.find((l) => l[0] === "SetPartOutput")[1], { part: 0, output: 1 });
  r.ctl.onState(r.unit.state());
  assert.equal(byId(r.ctl.model(), "part_volume").value, 0.42);
  assert.equal(byId(r.ctl.model(), "part_output").value, 1);
});

test("the insert chain: add, edit, bypass, reorder and remove always send the full chain; a typed error reverts", async () => {
  const r = rig();
  await r.open(0);
  assert.equal(r.ctl.model().sections[2].inserts.length, 0);
  assert.equal(r.ctl.addInsert(), true);
  await r.clock.advance(200);
  r.ctl.onState(r.unit.state());
  assert.equal(r.unit.parts[0].inserts.length, 1);
  assert.equal(r.unit.parts[0].inserts[0].type, 1);
  assert.deepEqual(r.unit.parts[0].inserts[0].params, [0.5, 0.5, 0.5, 0.5, 0]);
  r.ctl.addInsert();
  await r.clock.advance(200);
  r.ctl.onState(r.unit.state());
  let ins = r.ctl.model().sections[2].inserts;
  assert.equal(ins.length, 2);
  // change slot 2's type, a parameter and bypass
  r.ctl.edit(ins[1].type.key, 4);
  await r.clock.advance(200);
  r.ctl.onState(r.unit.state());
  ins = r.ctl.model().sections[2].inserts;
  r.ctl.edit(ins[1].params[0].key, 0.8);
  r.ctl.edit(ins[1].bypass.key, 1);
  await r.clock.advance(200);
  r.ctl.onState(r.unit.state());
  assert.deepEqual(r.unit.parts[0].inserts[1], { type: 4, bypass: true, params: [0.8, 0.5, 0.5, 0.5, 0] });
  // reorder
  assert.equal(r.ctl.moveInsert(1, -1), true);
  await r.clock.advance(200);
  r.ctl.onState(r.unit.state());
  assert.deepEqual(r.unit.parts[0].inserts.map((s) => s.type), [4, 1]);
  assert.equal(r.ctl.moveInsert(0, -1), false); // already first
  assert.equal(r.ctl.moveInsert(1, 1), false); // already last
  // every chain request carried every slot
  const chains = r.unit.log.filter((l) => l[0] === "SetInsertChain").map((l) => l[1].slots.length);
  assert.deepEqual(chains.slice(0, 2), [1, 2]);
  assert.ok(chains.length >= 5 && chains.every((n) => n >= 1 && n <= 2), String(chains)); // never a partial chain
  // remove
  r.ctl.removeInsert(0);
  await r.clock.advance(200);
  r.ctl.onState(r.unit.state());
  assert.deepEqual(r.unit.parts[0].inserts.map((s) => s.type), [1]);
  // a rejected chain (FX budget) reverts the display to the confirmed chain
  r.unit.fail = (n) => (n === "SetInsertChain" ? CODE.FX_BUDGET : null);
  r.ctl.addInsert();
  assert.equal(r.ctl.model().sections[2].inserts.length, 2); // optimistic
  await r.clock.advance(200);
  assert.equal(r.ctl.model().sections[2].inserts.length, 1);
  assert.equal(r.errors.at(-1).key, "chain");
  assert.equal(r.errors.at(-1).code, CODE.FX_BUDGET);
  // the chain is capped at the registry's slot count
  r.unit.fail = null;
  for (let i = 0; i < 6; i++) { r.ctl.addInsert(); await r.clock.advance(200); r.ctl.onState(r.unit.state()); }
  assert.equal(r.ctl.model().sections[2].inserts.length, REGISTRY.insertSlots);
  assert.equal(r.ctl.model().sections[2].canAdd, false);
  assert.equal(r.ctl.addInsert(), false);
  // a type of "none" shows no parameters
  const slot = r.ctl.model().sections[2].inserts[0];
  r.ctl.edit(slot.type.key, 0);
  assert.equal(r.ctl.model().sections[2].inserts[0].params.length, 0);
});

test("ASSIGN: K1..K8 from the assignable parameters, SetAssignSlot / ClearAssignSlot, revert on error", async () => {
  const r = rig();
  await r.open(3);
  const a = r.ctl.model().sections[3].assign;
  assert.equal(a.slots.length, ASSIGN_SLOTS);
  assert.ok(a.options.length > 10);
  assert.ok(a.options.every((o) => def(o.name).assignable));
  assert.ok(!a.options.some((o) => o.name.startsWith("fm_") || o.name.startsWith("va_"))); // an organ part offers organ and general parameters
  assert.ok(!a.options.some((o) => o.name === "line_in_level")); // assignable: false in the registry
  assert.ok(a.options.some((o) => o.name === "tempo_bpm"));
  assert.equal(r.ctl.assign(1, def("drawbar_16").id), true);
  assert.equal(r.ctl.model().sections[3].assign.slots[0].paramId, def("drawbar_16").id); // optimistic
  await r.clock.advance(200);
  r.ctl.onState(r.unit.state());
  assert.deepEqual(r.unit.log.find((l) => l[0] === "SetAssignSlot")[1], { target: 1, slot: 1, param_id: 100 });
  assert.equal(r.ctl.model().sections[3].assign.slots[0].paramId, 100);
  assert.equal(r.ctl.model().sections[3].assign.slots[0].pending, false);
  assert.equal(r.ctl.assign(1, 0), true);
  await r.clock.advance(200);
  assert.deepEqual(r.unit.log.find((l) => l[0] === "ClearAssignSlot")[1], { target: 1, slot: 1 });
  r.ctl.onState(r.unit.state());
  assert.equal(r.ctl.model().sections[3].assign.slots[0].paramId, 0);
  // refused locally: a slot out of 1..8, a parameter that is not assignable here
  assert.equal(r.ctl.assign(0, 100), false);
  assert.equal(r.ctl.assign(9, 100), false);
  assert.equal(r.ctl.assign(1, def("fm_algorithm").id), false);
  assert.equal(r.ctl.assign(1, def("line_in_level").id), false);
  // a typed error reverts
  r.unit.fail = (n) => (n === "SetAssignSlot" ? 14 : null);
  r.ctl.assign(2, def("drawbar_8").id);
  await r.clock.advance(200);
  assert.equal(r.ctl.model().sections[3].assign.slots[1].paramId, 0);
  assert.equal(r.errors.at(-1).key, "assign:2");
});

test("read-only: a Live Set that does not sound, an unavailable engine, and the classes with no command (deferred with a reason)", async () => {
  const r = rig();
  await r.open(0);
  const m = r.ctl.model();
  // Arp and zone controls are now editable when sounding
  assert.equal(byId(m, "arp_mode").readOnly, false);
  assert.equal(byId(m, "low_key").readOnly, false);
  assert.equal(byId(m, "low_key").value, 36);
  assert.equal(byId(m, "high_key").value, 96);
  assert.equal(byId(m, "keyboard").value, 1);
  assert.equal(byId(m, "octave").value, 0);
  assert.equal(byId(m, "transpose").value, 0);

  // A parameter with no command is deferred
  const deferredReg = {
    ...REGISTRY,
    params: [
      ...REGISTRY.params,
      { id: 9999, name: "deferred_param", class: "structural", type: "float", scope: "part", unit: "", min: 0, max: 10, default: 5, enum: null, cc: -1, assignable: false, localeKey: "", section: "sound", engine: "any", group: "sound.main", hideEngines: [] },
    ],
  };
  const mDef = buildPartEditorModel({ registry: deferredReg, partIndex: 0, engine: "sample", state: r.unit.state(), editable: true, live: true });
  const defKnob = byId(mDef, "deferred_param");
  assert.equal(defKnob.readOnly, true);
  assert.equal(defKnob.reason, "studio.part_editor.ro_no_command");
  assert.ok(mDef.deferred.some((d) => d.id === "deferred_param"));

  // not sounding
  r.unit.sounding = false;
  r.ctl.onState(r.unit.state());
  const m2 = r.ctl.model();
  assert.equal(m2.editable, false);
  assert.equal(m2.reason, "studio.part_editor.ro_not_playing");
  assert.ok(knobs(m2).every((k) => k.readOnly));
  assert.ok(knobs(m2).filter((k) => k.kind === "param").every((k) => k.value === null));
  assert.equal(r.ctl.edit("p:100", 3), null);
  assert.equal(r.ctl.addInsert(), false);
  assert.equal(r.ctl.assign(1, 100), false);
  // another Live Set on screen than the one that sounds
  r.unit.sounding = true;
  r.ctl.select({ liveSetId: "other", part: 0, engine: "sample" });
  r.ctl.onState(r.unit.state());
  assert.equal(r.ctl.model().editable, false);
  // an engine this firmware does not have
  r.ctl.select({ liveSetId: r.unit.liveSetId, part: 0, engine: "fm", available: false });
  assert.equal(r.ctl.model().reason, "studio.part_editor.ro_engine_unavailable");
  assert.equal(r.ctl.edit("p:1000", 3), null);
});

test("a scene change or a new revision reads the live values again", async () => {
  const r = rig();
  await r.open(3);
  assert.equal(byId(r.ctl.model(), "drawbar_16").value, 0);
  r.unit.perf.set(`${def("drawbar_16").id}:3`, 7); // a scene recall (or an encoder) moved it on the unit
  r.unit.scene = 1;
  r.unit.bump();
  r.ctl.onState(r.unit.state());
  await r.clock.advance(200);
  assert.equal(byId(r.ctl.model(), "drawbar_16").value, 7);
  // reset forgets everything
  r.ctl.reset();
  assert.equal(r.ctl.model().editable, false);
  assert.equal(r.ctl.edit("p:100", 1), null);
});

test("selecting another part or Live Set drops the pending overlay", async () => {
  const r = rig();
  await r.open(3);
  r.unit.delay = 5;
  r.ctl.edit("p:100", 4);
  assert.equal(byId(r.ctl.model(), "drawbar_16").value, 4);
  r.ctl.select({ liveSetId: r.unit.liveSetId, part: 2, engine: "va" });
  assert.ok(!byId(r.ctl.model(), "drawbar_16"));
  await r.clock.advance(300);
});

test("the sections stay hairline-divided with no menus or tabs: four sections only, in order", () => {
  const m = buildPartEditorModel({ registry: REGISTRY, partIndex: 0, engine: "fm", editable: false });
  assert.deepEqual(m.sections.map((s) => s.titleKey), ["studio.part_editor.sound", "studio.part_editor.amp_eq", "studio.part_editor.effects", "studio.part_editor.zone_arp_assign"]);
  assert.equal(assignableParams(REGISTRY, "fm").some((d) => d.name === "fm_algorithm"), true);
});

test("a State that arrives before its Ack still confirms the structural edit at once (no wait for the settle timer)", async () => {
  const r = rig();
  r.unit.onState((st) => r.ctl.onState(st)); // the unit pushes the State while it answers the edit, like a subscribed bridge
  await r.open(0);
  const vol = byId(r.ctl.model(), "part_volume");
  r.ctl.edit(vol.key, 0.3);
  await r.clock.advance(5);
  assert.equal(byId(r.ctl.model(), "part_volume").pending, false);
  assert.equal(byId(r.ctl.model(), "part_volume").value, 0.3);
  r.ctl.addInsert();
  await r.clock.advance(5);
  assert.equal(r.ctl.model().sections[2].inserts[0].type.pending, false);
  r.ctl.assign(1, 100);
  await r.clock.advance(5);
  assert.equal(r.ctl.model().sections[3].assign.slots[0].pending, false);
});

test("B1: switching parts cancels what is still queued: nothing lands on another part", async () => {
  const r = rig();
  await r.open(2); // VA
  const k = byId(r.ctl.model(), "va_filter_cutoff");
  r.unit.delay = 3;
  r.ctl.edit(k.key, 3000); // goes out at once
  r.ctl.edit(k.key, 500); // queued behind the 30 ms throttle
  r.ctl.select({ liveSetId: r.unit.liveSetId, part: 1, engine: "fm" });
  r.ctl.onState(r.unit.state());
  await r.clock.advance(400);
  const sent = r.unit.log.filter((l) => l[0] === "SetPerformanceParam");
  assert.equal(sent.length, 1);
  assert.equal(sent[0][1].part, 2);
  assert.ok(sent[0][1].value > 2000);
  assert.ok(!r.unit.log.some((l) => l[0] === "SetPerformanceParam" && l[1].part === 1));
  // and a builder that is run late refuses by itself (the guard): another part, another Live Set
  const build = r.ctl._guard(() => ({ body: {} }));
  assert.ok(build());
  r.ctl.select({ liveSetId: r.unit.liveSetId, part: 3, engine: "organ" });
  assert.equal(build(), null);
  const build2 = r.ctl._guard(() => ({ body: {} }));
  r.ctl.select({ liveSetId: "other", part: 3, engine: "organ" });
  assert.equal(build2(), null);
});

test("B3: a rejected performance edit reads the unit again, so the knob shows what it really holds", async () => {
  const r = rig();
  await r.open(3);
  const k = byId(r.ctl.model(), "drawbar_8");
  r.unit.perf.set("102:3", 2); // moved on the unit (a CC) since the last read
  r.unit.fail = (n) => (n === "SetPerformanceParam" ? CODE.BUSY : null);
  const gets = r.unit.log.filter((l) => l[0] === "GetPerformanceValues").length;
  r.ctl.edit(k.key, 5);
  await r.clock.advance(5);
  await r.clock.advance(200);
  assert.ok(r.unit.log.filter((l) => l[0] === "GetPerformanceValues").length > gets);
  assert.equal(byId(r.ctl.model(), "drawbar_8").value, 2);
  assert.equal(r.errors.length, 1);
});

test("B4: Ack means queued: a read-back that differs is read again before it is believed, and the player is told when it still differs", async () => {
  // no render thread (--no-audio): the unit never applies the edit
  const r = rig({ unitOptions: { noRender: true } });
  await r.open(3);
  r.ctl.edit("p:102", 3);
  await r.clock.advance(5);
  assert.equal(byId(r.ctl.model(), "drawbar_8").value, 3); // optimistic while it is being checked
  await r.clock.advance(1000);
  const reads = r.unit.log.filter((l) => l[0] === "GetPerformanceValues").length;
  assert.ok(reads >= 3, `reads ${reads}`); // the first read-back and the two re-reads
  assert.equal(byId(r.ctl.model(), "drawbar_8").value, 8); // what the unit holds
  assert.equal(r.errors.length, 1);
  assert.equal(r.errors[0].notice, "not_applied");
  assert.equal(r.errors[0].key, "p:102");
  // the RT plane applies it late: the second read-back matches and nothing is reported
  const r2 = rig({ unitOptions: { noRender: true } });
  await r2.open(3);
  r2.ctl.edit("p:102", 3);
  await r2.clock.advance(5);
  await r2.clock.advance(100); // the first read-back differs
  r2.unit.noRender = false;
  r2.unit.perf.set("102:3", 3); // applied by RT meanwhile
  await r2.clock.advance(1000);
  assert.equal(byId(r2.ctl.model(), "drawbar_8").value, 3);
  assert.equal(byId(r2.ctl.model(), "drawbar_8").pending, false);
  assert.equal(r2.errors.length, 0);
});

test("float32 read-back noise is rounded: 0.6 comes back as 0.6", async () => {
  const r = rig();
  await r.open(3);
  r.unit.perf.set("116:3", Math.fround(0.6)); // key_click
  r.ctl.refreshPerf();
  await r.clock.advance(200);
  assert.equal(byId(r.ctl.model(), "key_click").value, 0.6);
  assert.equal(byId(r.ctl.model(), "key_click").valueNow, 0.6);
});

test("A6: a structural drag is throttled (the generation ledger and the State pushes), the last value still goes out", async () => {
  const r = rig();
  await r.open(0);
  for (let i = 1; i <= 60; i++) {
    r.ctl.edit("vol", i / 100);
    await r.clock.advance(2);
  }
  await r.clock.advance(600);
  const vols = r.unit.log.filter((l) => l[0] === "SetPartVolume").map((l) => l[1].volume);
  assert.ok(vols.length >= 2 && vols.length <= 6, `sent ${vols.length}`); // 120 ms of drag: about one per 140 ms
  assert.equal(vols.at(-1), 0.6);
  // a performance drag keeps the finer 30 ms
  const before = r.unit.log.filter((l) => l[0] === "SetPerformanceParam").length;
  for (let i = 1; i <= 9; i++) {
    r.ctl.edit("p:403", i * 100);
    await r.clock.advance(2);
  }
  await r.clock.advance(300);
  assert.ok(r.unit.log.filter((l) => l[0] === "SetPerformanceParam").length - before >= 2);
});

test("B6: a failing result callback never stops a key's queue", async () => {
  const clock = makeClock();
  const sent = [];
  const sender = new EditSender({ request: async (m) => { sent.push(m.v); return { body: { Ack: {} } }; }, clock: clock.now, setTimer: clock.setTimer, clearTimer: clock.clearTimer, throttleMs: 0, done: () => { throw new Error("boom"); } });
  sender.submit("k", () => ({ v: 1 }), {});
  await clock.advance(1);
  sender.submit("k", () => ({ v: 2 }), {});
  await clock.advance(1);
  assert.deepEqual(sent, [1, 2]);
});

test("B6: the 0.8 s structural settle says so when it gives back a value the unit does not hold", async () => {
  const r = rig();
  await r.open(0);
  r.ctl.edit("vol", 0.3);
  await r.clock.advance(5);
  await r.clock.advance(1000); // no State push confirms it (a unit whose State never changed): the State's value is shown again
  assert.equal(byId(r.ctl.model(), "part_volume").value, 0.7);
  assert.equal(r.errors.at(-1).notice, "unconfirmed");
});

test("A1: only the knobs no engine plays are marked inert (organ / FM amp attack + release, VA unison spread); everything else is audible", () => {
  const inert = (engine) => knobs(buildPartEditorModel({ registry: REGISTRY, partIndex: 0, engine, editable: true, live: true, state: { parts: [{ part: 0 }] } })).filter((k) => k.inert).map((k) => k.id);
  assert.deepEqual(inert("fm").sort(), ["part_attack", "part_release"]);
  assert.deepEqual(inert("organ").sort(), ["part_attack", "part_release"]);
  assert.deepEqual(inert("va"), ["va_unison_spread"]);
  assert.deepEqual(inert("sample"), []);
  const fm = inert("fm");
  assert.ok(!fm.includes("fm_algorithm") && !fm.includes("part_eq_low") && !fm.includes("part_brightness"));
  assert.ok(!fm.includes("send_reverb_level") && !fm.includes("part_output") && !fm.includes("part_volume") && !fm.includes("arp_mode"));
  const organ = inert("organ");
  assert.deepEqual(organ.filter((n) => n.startsWith("drawbar_") || n.startsWith("rotary_") || n === "key_click"), []);
  const m = buildPartEditorModel({ registry: REGISTRY, partIndex: 0, engine: "fm", editable: true, live: true, state: { parts: [{ part: 0 }] } });
  assert.equal(m.sections[1].inert, true); // the Amp envelope + EQ section carries the note while part_attack / part_release are in it
  const ms = buildPartEditorModel({ registry: REGISTRY, partIndex: 0, engine: "sample", editable: true, live: true, state: { parts: [{ part: 0 }] } });
  assert.equal(ms.sections[1].inert, false);
  // the table agrees with the fake unit's engines (engine type ids: 1 sample, 2 organ, 3 FM, 4 VA): every inert knob is unconsumed, every other registry knob of the engine is consumed
  const unit = new FakeUnit();
  for (const [engine, idx] of [["fm", 1], ["va", 2], ["sample", 0], ["organ", 3]]) {
    const m2 = buildPartEditorModel({ registry: REGISTRY, partIndex: idx, engine, editable: true, live: true, state: { parts: [{ part: idx }] } });
    for (const k of knobs(m2)) {
      const d = REGISTRY.params.find((p) => p.id === k.numericId || p.name === k.id);
      if (!d || d.class !== "performance" || d.scope !== "part" || k.command !== "perf") continue;
      assert.equal(unit.consumes(idx, d.id), !k.inert, `${engine} ${d.name}`);
    }
  }
  assert.equal(unit.consumes(3, 102), true);
  assert.equal(unit.consumes(0, 400), true);
});

test("the zone controls use the engine's ranges (transpose -12..12, octave -4..4)", () => {
  const m = buildPartEditorModel({ registry: REGISTRY, partIndex: 0, engine: "sample", editable: false });
  const k = (id) => byId(m, id);
  assert.deepEqual([k("transpose").min, k("transpose").max, k("octave").min, k("octave").max], [-12, 12, -4, 4]);
});

test("VA: the generic amp attack / release are not shown, the VA amp envelope sits in Amp envelope + EQ", () => {
  const m = buildPartEditorModel({ registry: REGISTRY, partIndex: 0, engine: "va", editable: false });
  const amp = m.sections[1].groups.find((g) => g.id === "amp.env").knobs.map((k) => k.id);
  assert.deepEqual(amp, ["va_amp_attack", "va_amp_decay", "va_amp_sustain", "va_amp_release"]);
  assert.ok(!byId(m, "part_attack") && !byId(m, "part_release"));
  assert.ok(byId(buildPartEditorModel({ registry: REGISTRY, partIndex: 0, engine: "fm", editable: false }), "part_attack"));
});

test("Story 9.23: editing zone knobs sends SetPartZone with base_revision and updates model", async () => {
  const r = rig();
  await r.open(0);
  const baseRev = r.unit.liveRevision;

  // Edit low_key
  assert.equal(r.ctl.edit("zone:low_key", 40), 40);
  assert.equal(byId(r.ctl.model(), "low_key").pending, true);
  await r.clock.advance(160);
  const zLog1 = r.unit.log.filter((l) => l[0] === "SetPartZone");
  assert.equal(zLog1.length, 1);
  assert.deepEqual(zLog1[0], ["SetPartZone", { part: 0, low_key: 40 }, baseRev]);
  assert.equal(r.unit.parts[0].low_key, 40);
  assert.equal(byId(r.ctl.model(), "low_key").value, 40);

  // Edit octave
  assert.equal(r.ctl.edit("zone:octave", -2), -2);
  await r.clock.advance(160);
  const zLog2 = r.unit.log.filter((l) => l[0] === "SetPartZone");
  assert.equal(zLog2.length, 2);
  assert.deepEqual(zLog2[1], ["SetPartZone", { part: 0, octave: -2 }, baseRev]);
  assert.equal(r.unit.parts[0].octave, -2);
  assert.equal(byId(r.ctl.model(), "octave").value, -2);

  // Edit transpose
  assert.equal(r.ctl.edit("zone:transpose", 7), 7);
  await r.clock.advance(160);
  const zLog3 = r.unit.log.filter((l) => l[0] === "SetPartZone");
  assert.equal(zLog3.length, 3);
  assert.deepEqual(zLog3[2], ["SetPartZone", { part: 0, transpose: 7 }, baseRev]);
  assert.equal(r.unit.parts[0].transpose, 7);

  // Edit keyboard
  assert.equal(r.ctl.edit("zone:keyboard", 0), 0);
  await r.clock.advance(160);
  const zLog4 = r.unit.log.filter((l) => l[0] === "SetPartZone");
  assert.equal(zLog4.length, 4);
  assert.deepEqual(zLog4[3], ["SetPartZone", { part: 0, keyboard_on: false }, baseRev]);
  assert.equal(r.unit.parts[0].keyboard_on, false);
});

test("Story 9.23: editing arpeggiator knobs sends SetPartArp with base_revision and updates model", async () => {
  const r = rig();
  await r.open(0);
  const baseRev = r.unit.liveRevision;

  const arpOn = byId(r.ctl.model(), "arp_on");
  assert.equal(r.ctl.edit(arpOn.key, 1), 1);
  assert.equal(byId(r.ctl.model(), "arp_on").pending, true);
  await r.clock.advance(160);
  const aLog1 = r.unit.log.filter((l) => l[0] === "SetPartArp");
  assert.equal(aLog1.length, 1);
  assert.deepEqual(aLog1[0], ["SetPartArp", { part: 0, arp_on: true }, baseRev]);
  assert.equal(r.unit.parts[0].arp_on, true);
  assert.equal(byId(r.ctl.model(), "arp_on").value, 1);

  // Edit arp_rate (enum index 2: sixteenth)
  const arpRate = byId(r.ctl.model(), "arp_rate");
  assert.equal(r.ctl.edit(arpRate.key, 2), 2);
  await r.clock.advance(160);
  const aLog2 = r.unit.log.filter((l) => l[0] === "SetPartArp");
  assert.equal(aLog2.length, 2);
  assert.deepEqual(aLog2[1], ["SetPartArp", { part: 0, arp_rate: 2 }, baseRev]);
  assert.equal(r.unit.parts[0].arp_rate, 2);
  assert.equal(byId(r.ctl.model(), "arp_rate").value, 2);

  // Edit arp_gate (float 76%)
  const arpGate = byId(r.ctl.model(), "arp_gate");
  assert.equal(r.ctl.edit(arpGate.key, 76), 76);
  await r.clock.advance(160);
  const aLog3 = r.unit.log.filter((l) => l[0] === "SetPartArp");
  assert.equal(aLog3.length, 3);
  assert.deepEqual(aLog3[2], ["SetPartArp", { part: 0, arp_gate: 76 }, baseRev]);
  assert.equal(r.unit.parts[0].arp_gate, 76);
});

test("Story 9.23: stale revision on SetPartZone / SetPartArp triggers stale state and reverts", async () => {
  const r = rig();
  await r.open(0);
  // Unit changes underneath (moves revision)
  r.unit.deviceChange();

  assert.equal(r.ctl.edit("zone:low_key", 50), 50);
  await r.clock.advance(160);
  assert.ok(r.ctl.stale);
  assert.equal(byId(r.ctl.model(), "low_key").value, 36); // reverted to confirmed
  assert.equal(byId(r.ctl.model(), "low_key").readOnly, true);
});
