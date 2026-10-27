// Story 9.9: the Part Editor DOM against a minimal DOM shim (no jsdom here): ARIA sliders, keyboard, pointer drag, wheel, value entry, read-only controls, in-place updates,
// textContent-only rendering of text from the unit, and that every locale key the modules use exists in en, es and pt-BR.
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";

import { PartEditorController } from "../src/part_editor.js";
import { PartEditorView } from "../src/part_editor_dom.js";
import { CATALOGS } from "../src/catalogs.js";
import { REGISTRY } from "../src/params_registry.js";
import { FakeUnit, CODE } from "./support/fake_unit.js";

class El {
  constructor(doc, tag) {
    this.ownerDocument = doc;
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.attrs = {};
    this.style = {};
    this.listeners = {};
    this.className = "";
    this.hidden = false;
    this.disabled = false;
    this.title = "";
    this.value = "";
    this.id = "";
    this.parent = null;
    this._text = "";
  }
  get textContent() { return this._text + this.children.map((c) => c.textContent).join(""); }
  contains(n) { for (; n; n = n.parent) if (n === this) return true; return false; }
  set textContent(v) { this.dropFocusIn(this.children); this.children = []; this._text = String(v); }
  dropFocusIn(list) { const a = this.ownerDocument.activeElement; if (a && list.some((c) => c.contains(a))) this.ownerDocument.activeElement = null; } // a removed element loses the focus (blur)
  setPointerCapture(id) { this.ownerDocument.captured = this; this.captureId = id; }
  set innerHTML(v) { throw new Error("innerHTML must never be used: " + v); }
  appendChild(c) { c.parent = this; this.children.push(c); return c; }
  remove() { if (this.parent) { this.dropFocusIn([this]); this.parent.children = this.parent.children.filter((c) => c !== this); } this.parent = null; }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; }
  removeAttribute(k) { delete this.attrs[k]; }
  addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
  focus() { this.ownerDocument.activeElement = this; }
  fire(type, ev = {}) {
    const e = { preventDefault() { this.prevented = true; }, ...ev, target: this };
    (this.listeners[type] || []).forEach((f) => f(e));
    return e;
  }
  all(pred, out = []) { if (pred(this)) out.push(this); this.children.forEach((c) => c.all(pred, out)); return out; }
}
const makeDoc = () => {
  const d = { activeElement: null, createElement: (t) => new El(d, t), createElementNS: (ns, t) => new El(d, t) };
  return d;
};

const tr = (key, ...a) => { let i = 0; return (CATALOGS.en[key] ?? key).replace(/%[ds]/g, () => String(a[i++])); };
// the view's `tr` takes only a key and the view fills the %s itself
const trKey = (key) => CATALOGS.en[key] ?? key;

function makeClock() {
  let t = 1000;
  let id = 0;
  const timers = new Map();
  const c = {
    now: () => t,
    setTimer: (f, ms) => { const k = ++id; timers.set(k, { at: t + ms, f }); return k; },
    clearTimer: (k) => timers.delete(k),
    tick: async () => { for (let i = 0; i < 12; i++) await new Promise((r) => setImmediate(r)); },
    advance: async (ms) => {
      const end = t + ms;
      for (;;) {
        let next = null;
        for (const [k, v] of timers) if (v.at <= end && (!next || v.at < next[1].at)) next = [k, v];
        if (!next) break;
        timers.delete(next[0]);
        t = Math.max(t, next[1].at);
        next[1].f();
        await c.tick();
      }
      t = end;
      await c.tick();
    },
  };
  return c;
}

async function rig(part = 3, engines) {
  const doc = makeDoc();
  const host = doc.createElement("div");
  const unit = new FakeUnit({ engines });
  const clock = makeClock();
  const announced = [];
  let view = null;
  const ctl = new PartEditorController({
    request: (m) => unit.request(m), clock: clock.now, setTimer: clock.setTimer, clearTimer: clock.clearTimer,
    onChange: () => view && view.render(ctl.model()),
    onError: (e) => view.showError(e),
  });
  view = new PartEditorView({ doc, host, tr: trKey, controller: ctl, announce: (m) => announced.push(m) });
  view.setHeader({ number: part + 1, name: "Glass Keys", category: "E.PIANO" });
  const engine = { 1: "sample", 2: "organ", 3: "fm", 4: "va" }[unit.parts[part].engine_type_id];
  ctl.select({ liveSetId: unit.liveSetId, part, engine });
  ctl.onState(unit.state());
  await clock.advance(100);
  const sliders = () => host.all((e) => e.attrs.role === "slider");
  const slider = (label) => sliders().find((s) => s.attrs["aria-label"] === label);
  return { doc, host, unit, clock, ctl, view, announced, sliders, slider };
}

test("four hairline-divided sections, no menus or tabs, every knob an ARIA slider with name, range and value text", async () => {
  const r = await rig(3); // organ
  const secs = r.host.all((e) => e.tagName === "SECTION");
  assert.equal(secs.length, 4);
  assert.deepEqual(secs.map((s) => s.children[0].textContent), ["Sound", "Amp envelope + EQ", "Effects inserts + sends", "Zone & keyboard · Arpeggiator · Assign"]);
  assert.equal(r.host.all((e) => ["tab", "tablist", "menu", "menubar", "menuitem", "listbox"].includes(e.attrs.role)).length, 0);
  const s = r.sliders();
  assert.ok(s.length > 30);
  for (const el of s) {
    assert.ok(el.attrs["aria-label"], "a name");
    assert.ok("aria-valuemin" in el.attrs && "aria-valuemax" in el.attrs, "a range");
    assert.equal(el.attrs.tabindex, "0");
    assert.ok(el.attrs["aria-valuetext"] !== undefined, "a value text");
  }
  const d8 = r.slider("Drawbar 8'");
  assert.deepEqual([d8.attrs["aria-valuemin"], d8.attrs["aria-valuemax"], d8.attrs["aria-valuenow"], d8.attrs["aria-valuetext"]], ["0", "8", "8", "8"]);
  const rel = r.slider("Release");
  assert.equal(rel.attrs["aria-valuetext"], "300 ms"); // the unit is in the value text
  const eq = r.slider("EQ low");
  assert.equal(eq.attrs["aria-valuetext"], "0.0 dB");
  const perc = r.slider("Percussion");
  assert.equal(perc.attrs["aria-valuetext"], "Off");
  const harm = r.slider("Harmonic");
  assert.equal(harm.attrs["aria-valuetext"], "3rd");
  // an envelope polyline is drawn in Amp envelope + EQ
  assert.equal(secs[1].all((e) => e.tagName === "POLYLINE").length, 1);
  // the header
  assert.match(r.host.textContent, /PART 4/);
  // the 8 ASSIGN slots and the "+ Add insert" button
  assert.equal(r.host.all((e) => e.tagName === "SELECT").length, 8);
  assert.ok(r.host.all((e) => e.tagName === "BUTTON" && e.textContent === "+ Add insert").length === 1);
  assert.match(r.host.textContent, /ASSIGN · KNOBS \(THIS LIVE SET\)/);
});

test("duplicate labels in a section take their group title into the accessible name", async () => {
  const r = await rig(1); // FM
  assert.ok(r.slider("OP 1 · Level 1"));
  assert.ok(r.slider("OP 6 · Level 1"));
  assert.ok(r.slider("OP 3 · Ratio fine"));
  assert.ok(r.slider("Algorithm"));
});

test("keys change the value through the controller and the unit plays it; aria-valuetext follows", async () => {
  const r = await rig(3);
  const d = r.slider("Drawbar 16'");
  d.focus();
  const key = async (k, shift = false) => { const e = d.fire("keydown", { key: k, shiftKey: shift }); await r.clock.advance(40); return e; };
  assert.equal((await key("ArrowUp")).prevented, true);
  assert.equal(r.unit.perf.get("100:3"), 1);
  assert.equal(d.attrs["aria-valuenow"], "1");
  await key("ArrowUp", true); // Shift: 10 steps, clamped to 8
  assert.equal(r.unit.perf.get("100:3"), 8);
  await key("Home");
  assert.equal(r.unit.perf.get("100:3"), 0);
  await key("End");
  assert.equal(r.unit.perf.get("100:3"), 8);
  await key("PageDown");
  assert.equal(r.unit.perf.get("100:3"), 7);
  await key("ArrowLeft");
  assert.equal(r.unit.perf.get("100:3"), 6);
  assert.equal(d.attrs["aria-valuetext"], "6");
  const other = await key("a"); // not a knob key: not handled, not prevented
  assert.equal(other.prevented, undefined);
  // every edit carried base_revision
  assert.ok(r.unit.log.filter((l) => l[0] === "SetPerformanceParam").every((l) => l[2] === r.unit.liveRevision));
  // a switch toggles with Space
  const p = r.slider("Percussion");
  p.fire("keydown", { key: " " });
  await r.clock.advance(40);
  assert.equal(r.unit.perf.get("110:3"), 1);
  assert.equal(p.attrs["aria-valuetext"], "On");
});

test("pointer drag: up raises, Shift is finer, release ends it; wheel only changes the focused knob", async () => {
  const r = await rig(1); // FM part: fm_op1_level_1 0..99
  const k = r.slider("OP 1 · Level 1");
  const idf = 1024;
  const start = r.unit.perf.get(`${idf}:1`);
  assert.equal(start, 99);
  k.fire("pointerdown", { button: 0, clientX: 10, clientY: 300 });
  assert.equal(r.doc.activeElement, k);
  k.fire("pointermove", { clientX: 10, clientY: 400 }); // down 100 px: half the range down
  await r.clock.advance(40);
  assert.equal(r.unit.perf.get(`${idf}:1`), 50);
  const at = r.unit.perf.get(`${idf}:1`);
  k.fire("pointermove", { clientX: 10, clientY: 400, shiftKey: true }); // Shift pressed mid-drag without moving: no jump
  await r.clock.advance(40);
  assert.equal(r.unit.perf.get(`${idf}:1`), at);
  k.fire("pointermove", { clientX: 10, clientY: 350, shiftKey: true }); // then 50 px up at 1/5 of the sensitivity: +5 of 99
  await r.clock.advance(40);
  assert.equal(r.unit.perf.get(`${idf}:1`), at + 5);
  k.fire("pointermove", { clientX: 10, clientY: 350, shiftKey: false }); // Shift released: again no jump
  await r.clock.advance(40);
  assert.equal(r.unit.perf.get(`${idf}:1`), at + 5);
  k.fire("pointerup", {});
  assert.equal(k.attrs["data-dragging"], undefined);
  await r.clock.advance(300); // the read-backs of the drag settle
  const edits = () => r.unit.log.filter((l) => l[0] === "SetPerformanceParam").length;
  const before = edits();
  k.fire("pointermove", { clientX: 10, clientY: 0 }); // no drag in progress: nothing
  await r.clock.advance(40);
  assert.equal(edits(), before);
  // wheel: ignored while the knob has no focus, and the page keeps scrolling (not prevented)
  r.doc.activeElement = null;
  let e = k.fire("wheel", { deltaY: -100 });
  assert.equal(e.prevented, undefined);
  await r.clock.advance(40);
  assert.equal(edits(), before);
  const v = r.unit.perf.get(`${idf}:1`);
  k.focus();
  e = k.fire("wheel", { deltaY: -100 });
  assert.equal(e.prevented, true);
  await r.clock.advance(40);
  assert.equal(r.unit.perf.get(`${idf}:1`), v + 1);
  k.fire("wheel", { deltaY: 100, shiftKey: true });
  await r.clock.advance(40);
  assert.equal(r.unit.perf.get(`${idf}:1`), v + 1 - 10);
});

test("a rapid drag sends a few requests, never one per pointer event", async () => {
  const r = await rig(1);
  const k = r.slider("OP 2 · Level 1");
  k.fire("pointerdown", { button: 0, clientX: 0, clientY: 500 });
  r.unit.delay = 2;
  for (let i = 1; i <= 100; i++) {
    k.fire("pointermove", { clientX: 0, clientY: 500 + i }); // down: lowers from the default 99
    await r.clock.advance(1);
  }
  k.fire("pointerup", {});
  await r.clock.advance(300);
  const sent = r.unit.log.filter((l) => l[0] === "SetPerformanceParam");
  assert.ok(sent.length >= 2 && sent.length <= 8, `sent ${sent.length}`);
  assert.equal(r.ctl.sender.maxInFlight, 1);
});

test("Enter opens a value entry: a good value applies, junk and out-of-range are told in the live region and change nothing, Escape cancels", async () => {
  const r = await rig(1);
  const k = r.slider("OP 1 · Level 1");
  k.focus();
  k.fire("keydown", { key: "Enter" });
  const input = r.host.all((e) => e.tagName === "INPUT")[0];
  assert.ok(input);
  assert.match(input.attrs["aria-label"], /^Enter a value for OP 1 · Level 1$/);
  assert.equal(r.doc.activeElement, input);
  input.value = "250";
  input.fire("keydown", { key: "Enter" });
  assert.equal(input.attrs["aria-invalid"], "true");
  const err = r.host.all((e) => e.className === "pe-err" && !e.hidden)[0];
  assert.equal(err.textContent, "OP 1 · Level 1 must be between 0 and 99.");
  await Promise.resolve();
  const live = r.host.all((e) => e.attrs["aria-live"] === "polite")[0];
  assert.match(live.textContent, /must be between 0 and 99/);
  assert.equal(r.unit.perf.get("1024:1"), 99); // unchanged
  assert.equal(k.attrs["aria-valuenow"], "99");
  input.value = "abc";
  input.fire("keydown", { key: "Enter" });
  assert.equal(err.textContent, "That is not a valid value for OP 1 · Level 1.");
  assert.equal(r.unit.perf.get("1024:1"), 99);
  input.fire("keydown", { key: "Escape" });
  assert.equal(r.host.all((e) => e.tagName === "INPUT").length, 0);
  assert.equal(r.doc.activeElement, k);
  // again, a valid one
  k.fire("keydown", { key: "Enter" });
  const input2 = r.host.all((e) => e.tagName === "INPUT")[0];
  input2.value = "42";
  input2.fire("keydown", { key: "Enter" });
  await r.clock.advance(40);
  assert.equal(r.unit.perf.get("1024:1"), 42);
  assert.equal(k.attrs["aria-valuenow"], "42");
  assert.equal(r.host.all((e) => e.tagName === "INPUT").length, 0);
  assert.equal(r.doc.activeElement, k);
  assert.ok(r.host.all((e) => e.attrs["aria-live"] === "polite")[0].textContent.length > 0);
});

test("a read-only knob (no command yet) is aria-readonly with its reason and ignores keys, drag and wheel", async () => {
  const r = await rig(0);
  const arp = r.slider("Mode");
  assert.equal(arp.attrs["aria-readonly"], "true");
  assert.equal(arp.title, "The Velora cannot take this edit from Studio yet.");
  assert.equal(arp.attrs["aria-valuetext"], "—");
  assert.ok(!("aria-valuenow" in arp.attrs));
  const before = r.unit.log.length;
  arp.focus();
  const e = arp.fire("keydown", { key: "ArrowUp" });
  assert.equal(e.prevented, undefined);
  arp.fire("pointerdown", { button: 0, clientX: 0, clientY: 100 });
  arp.fire("pointermove", { clientX: 0, clientY: 0 });
  arp.fire("wheel", { deltaY: -100 });
  await r.clock.advance(200);
  assert.equal(r.unit.log.length, before);
  const low = r.slider("Low key");
  assert.equal(low.attrs["aria-readonly"], "true");
  assert.equal(low.attrs["aria-valuetext"], "C2");
  // a Live Set that does not sound: the banner says why and every knob is read-only
  r.unit.sounding = false;
  r.ctl.onState(r.unit.state());
  const banner = r.host.all((e) => e.className === "pe-banner")[0];
  assert.equal(banner.hidden, false);
  assert.match(banner.textContent, /not playing on the Velora/);
  assert.ok(r.sliders().every((s) => s.attrs["aria-readonly"] === "true"));
  assert.ok(r.host.all((e) => e.tagName === "SELECT").every((s) => s.disabled));
  assert.equal(r.host.all((e) => e.className === "btn pe-add")[0].disabled, true);
});

test("State pushes update the knobs in place: same elements, focus kept; a new structure rebuilds and keeps the focus on the same knob", async () => {
  const r = await rig(3);
  const d = r.slider("Drawbar 4'");
  d.focus();
  const first = r.sliders();
  r.unit.perf.set("103:3", 5);
  r.unit.bump();
  r.ctl.onState(r.unit.state());
  await r.clock.advance(300);
  assert.equal(r.slider("Drawbar 4'"), d); // the same element
  assert.equal(d.attrs["aria-valuenow"], "5");
  assert.equal(r.doc.activeElement, d);
  assert.equal(r.sliders().length, first.length);
  // adding an insert changes the structure: rebuilt, focus returns to the same knob
  r.ctl.addInsert();
  await r.clock.advance(200);
  r.ctl.onState(r.unit.state());
  await r.clock.advance(200);
  const d2 = r.slider("Drawbar 4'");
  assert.notEqual(d2, d);
  assert.equal(r.doc.activeElement, d2);
  assert.equal(r.host.all((e) => e.className === "pe-insert").length, 1);
});

test("effects: the insert row has type, bypass and parameters; the buttons move and remove; ASSIGN selects assign", async () => {
  const r = await rig(0);
  const add = () => r.host.all((e) => e.className === "btn pe-add")[0];
  assert.equal(add().disabled, false);
  add().fire("click", {});
  await r.clock.advance(200);
  r.ctl.onState(r.unit.state());
  add().fire("click", {});
  await r.clock.advance(200);
  r.ctl.onState(r.unit.state());
  const rows = r.host.all((e) => e.className === "pe-insert");
  assert.equal(rows.length, 2);
  assert.ok(r.slider("Insert 2, Type"));
  assert.ok(r.slider("Insert 2, Bypass"));
  assert.ok(r.slider("Insert 2, Parameter 5"));
  const btn = (row, cls) => row.all((e) => e.className === `btn ${cls}`)[0];
  assert.equal(btn(rows[0], "pe-up").disabled, true);
  assert.equal(btn(rows[0], "pe-down").disabled, false);
  assert.equal(btn(rows[1], "pe-down").disabled, true);
  assert.equal(btn(rows[1], "pe-up").attrs["aria-label"], "Move insert 2 up");
  r.slider("Insert 1, Type").fire("keydown", { key: "ArrowUp" }); // rotary -> chorus / phaser
  await r.clock.advance(200);
  r.ctl.onState(r.unit.state());
  assert.deepEqual(r.unit.parts[0].inserts.map((s) => s.type), [2, 1]);
  btn(r.host.all((e) => e.className === "pe-insert")[1], "pe-up").fire("click", {});
  await r.clock.advance(200);
  r.ctl.onState(r.unit.state());
  assert.deepEqual(r.unit.parts[0].inserts.map((s) => s.type), [1, 2]);
  btn(r.host.all((e) => e.className === "pe-insert")[0], "pe-remove").fire("click", {});
  await r.clock.advance(200);
  r.ctl.onState(r.unit.state());
  assert.deepEqual(r.unit.parts[0].inserts.map((s) => s.type), [2]);
  // ASSIGN: choosing a parameter in K3 sends SetAssignSlot, choosing Empty clears
  const sel = r.host.all((e) => e.tagName === "SELECT")[2];
  assert.equal(sel.attrs["aria-label"], "Parameter on knob K3");
  sel.value = "400";
  sel.fire("change", {});
  await r.clock.advance(200);
  assert.deepEqual(r.unit.log.find((l) => l[0] === "SetAssignSlot")[1], { target: 1, slot: 3, param_id: 400 });
  r.ctl.onState(r.unit.state());
  assert.equal(r.host.all((e) => e.tagName === "SELECT")[2].value, "400");
  sel.value = "0";
  sel.fire("change", {});
  await r.clock.advance(200);
  assert.deepEqual(r.unit.log.find((l) => l[0] === "ClearAssignSlot")[1], { target: 1, slot: 3 });
});

test("a typed error reverts the knob and the view shows the unit's message as text next to it", async () => {
  const r = await rig(3);
  const d = r.slider("Drawbar 8'");
  r.unit.fail = (n) => (n === "SetPerformanceParam" ? CODE.BUSY : null);
  d.focus();
  d.fire("keydown", { key: "ArrowDown" });
  assert.equal(d.attrs["aria-valuenow"], "7"); // optimistic
  await r.clock.advance(40);
  assert.equal(r.slider("Drawbar 8'").attrs["aria-valuenow"], "8"); // reverted
  const err = r.host.all((e) => e.className === "pe-err" && !e.hidden);
  assert.equal(err.length, 1);
  assert.equal(err[0].textContent, "The Velora did not accept the change: injected");
  assert.equal(d.attrs["aria-invalid"], "true");
  // the next good edit clears it
  r.unit.fail = null;
  d.fire("keydown", { key: "ArrowDown" });
  await r.clock.advance(40);
  assert.equal(r.host.all((e) => e.className === "pe-err" && !e.hidden).length, 0);
  assert.equal(d.attrs["aria-invalid"], undefined);
});

test("text that came from the unit is only ever textContent: hostile names and messages stay text", async () => {
  const r = await rig(3);
  const evil = '<img src=x onerror="boom()"><script>boom()</script>';
  r.view.setHeader({ number: 4, name: evil, category: evil });
  assert.ok(r.host.textContent.includes(evil));
  assert.equal(r.host.all((e) => e.tagName === "IMG" || e.tagName === "SCRIPT").length, 0);
  r.unit.fail = (n) => (n === "SetPerformanceParam" ? CODE.BUSY : null);
  const origErr = r.unit.err.bind(r.unit);
  r.unit.err = (code) => { const m = origErr(code); m.body.Error.message = evil; return m; };
  r.slider("Drawbar 8'").fire("keydown", { key: "ArrowDown" });
  await r.clock.advance(40);
  assert.ok(r.host.textContent.includes(evil));
  assert.equal(r.host.all((e) => e.tagName === "IMG" || e.tagName === "SCRIPT").length, 0);
  // and a chain error with no knob of its own goes to the top line
  r.unit.fail = (n) => (n === "SetInsertChain" ? CODE.FX_BUDGET : null);
  r.ctl.addInsert();
  await r.clock.advance(40);
  const msg = r.host.all((e) => e.className === "pe-msg")[0];
  assert.equal(msg.hidden, false);
  assert.ok(msg.textContent.includes(evil));
});

test("an engine this firmware lacks, and another engine's controls, are not drawn", async () => {
  const r = await rig(0); // a sample part
  const names = r.sliders().map((s) => s.attrs["aria-label"]);
  assert.ok(!names.includes("Drawbar 8'"));
  assert.ok(!names.some((n) => n.startsWith("OP 1")));
  assert.ok(names.includes("Part volume"));
  assert.ok(names.includes("Brightness"));
  assert.ok(names.includes("Reverb send"));
});

// Every locale key the two modules (and the model) use exists in en, es and pt-BR.
test("every studio.* key the Part Editor uses exists in all three catalogs", () => {
  const keys = new Set();
  for (const f of ["part_editor.js", "part_editor_dom.js"]) {
    const src = fs.readFileSync(new URL(`../src/${f}`, import.meta.url), "utf8");
    for (const m of src.matchAll(/"(studio\.[a-z0-9_.]+)"/g)) if (!m[1].endsWith(".")) keys.add(m[1]);
    for (const m of src.matchAll(/`(studio\.[a-z0-9_]+\.)\$\{/g)) void m; // dynamic keys are covered below
  }
  for (const k of ["studio.part_editor.zone_low_key", "studio.part_editor.zone_high_key", "studio.part_editor.zone_keyboard", "studio.part_editor.zone_octave", "studio.part_editor.zone_transpose"]) keys.add(k);
  assert.ok(keys.size > 30);
  for (const lang of Object.keys(CATALOGS)) for (const k of keys) assert.ok(k in CATALOGS[lang], `${lang}: ${k}`);
  // the dynamic ones: a title for every group, a label for every registry enum value
  for (const lang of Object.keys(CATALOGS)) {
    for (const d of REGISTRY.params) if (d.section !== "master" && d.section !== "hidden") assert.ok(`studio.group.${d.group}` in CATALOGS[lang], `${lang}: group ${d.group}`);
    for (const [en, vals] of Object.entries(REGISTRY.enums)) for (const v of vals) assert.ok(`studio.enum.${en}.${v}` in CATALOGS[lang]);
  }
  void tr;
});

test("B5: focus survives a rebuild on the buttons and selects (add insert, up, down, remove, ASSIGN) and a changed ASSIGN slot is not a rebuild", async () => {
  const r = await rig(0);
  const btn = (cls, n = 0) => r.host.all((e) => e.className === `btn ${cls}`)[n];
  const add = () => r.host.all((e) => e.className === "btn pe-add")[0];
  add().focus();
  add().fire("click", {});
  await r.clock.advance(300);
  r.ctl.onState(r.unit.state());
  assert.equal(r.doc.activeElement, add(), "the focus is back on + Add insert after the rebuild");
  add().fire("click", {});
  await r.clock.advance(300);
  r.ctl.onState(r.unit.state());
  btn("pe-down", 0).focus();
  btn("pe-down", 0).fire("click", {});
  await r.clock.advance(300);
  r.ctl.onState(r.unit.state());
  assert.deepEqual(r.unit.parts[0].inserts.length, 2);
  btn("pe-remove", 0).focus();
  btn("pe-remove", 0).fire("click", {});
  await r.clock.advance(300);
  r.ctl.onState(r.unit.state());
  assert.equal(r.unit.parts[0].inserts.length, 1);
  assert.equal(r.doc.activeElement.attrs["data-fid"], "ins:0:remove");
  btn("pe-remove", 0).focus();
  btn("pe-remove", 0).fire("click", {}); // the last one goes: its buttons are gone, the focus goes to + Add insert
  await r.clock.advance(300);
  r.ctl.onState(r.unit.state());
  assert.equal(r.unit.parts[0].inserts.length, 0);
  assert.equal(r.doc.activeElement, add());
  // ASSIGN: choosing in a select is not a structural change: the same select, still focused
  const sel = r.host.all((e) => e.tagName === "SELECT")[1];
  sel.focus();
  sel.value = "400";
  sel.fire("change", {});
  await r.clock.advance(300);
  r.ctl.onState(r.unit.state());
  assert.equal(r.host.all((e) => e.tagName === "SELECT")[1], sel);
  assert.equal(r.doc.activeElement, sel);
  assert.equal(sel.value, "400");
});

test("B5: no rebuild under an insert type drag; the drawn structure catches up when the drag ends", async () => {
  const r = await rig(0);
  r.ctl.addInsert();
  await r.clock.advance(300);
  r.ctl.onState(r.unit.state());
  const type = r.slider("Insert 1, Type");
  assert.ok(r.slider("Insert 1, Parameter 1"));
  type.fire("pointerdown", { button: 0, clientX: 0, clientY: 300, pointerId: 7 });
  assert.equal(r.doc.captured, type);
  type.fire("pointermove", { clientX: 0, clientY: 100 }); // far up: ... 5 (compressor)
  type.fire("pointermove", { clientX: 0, clientY: 600 }); // far down: 0 (none): the parameters go away
  await r.clock.advance(300);
  r.ctl.onState(r.unit.state());
  assert.equal(r.slider("Insert 1, Type"), type, "the dragged slider is still the same element");
  assert.equal(r.unit.parts[0].inserts[0].type, 0);
  type.fire("pointermove", { clientX: 0, clientY: 590 }); // still dragging it
  type.fire("pointerup", {});
  await r.clock.advance(300);
  assert.equal(r.slider("Insert 1, Parameter 1"), undefined, "after the drag the structure follows");
  assert.ok(r.slider("Insert 1, Type"));
});

test("D2: Ctrl / Cmd / Alt with a key is not ours; D6: the wheel accumulates; D3/D4: aria", async () => {
  const r = await rig(3);
  const d = r.slider("Drawbar 16'");
  d.focus();
  for (const mod of ["ctrlKey", "metaKey", "altKey"]) {
    const e = d.fire("keydown", { key: "ArrowUp", [mod]: true });
    assert.equal(e.prevented, undefined, mod);
  }
  await r.clock.advance(100);
  assert.equal(r.unit.log.filter((l) => l[0] === "SetPerformanceParam").length, 0);
  // a trackpad: 30 small deltas of -4 are 1.2 notches: one step, not 30
  for (let i = 0; i < 30; i++) d.fire("wheel", { deltaY: -4 });
  await r.clock.advance(300);
  assert.equal(r.unit.perf.get("100:3"), 1);
  // a notch in the other direction first resets the accumulator
  d.fire("wheel", { deltaY: -50 });
  d.fire("wheel", { deltaY: 100 });
  await r.clock.advance(300);
  assert.equal(r.unit.perf.get("100:3"), 0);
  // the value text is not a live region; the label is hidden from AT (the slider's aria-label names it); errors are announced once, by the editor's status region
  const val = r.host.all((e) => e.className === "pe-value")[0];
  assert.equal(val.tagName, "SPAN");
  assert.equal(val.attrs["aria-hidden"], "true");
  assert.ok(r.host.all((e) => e.className === "pe-knob-label").every((l) => l.attrs["aria-hidden"] === "true"));
  assert.equal(r.host.all((e) => e.attrs.role === "alert").length, 0);
  assert.equal(r.host.all((e) => e.attrs["aria-live"]).length, 1);
  // pending is not colour alone
  d.fire("keydown", { key: "ArrowUp" });
  assert.equal(d.attrs["aria-busy"], "true");
  await r.clock.advance(5);
  await r.clock.advance(300);
  assert.equal(d.attrs["aria-busy"], undefined);
});

test("every slider of every engine has a distinct accessible name", async () => {
  for (const part of [0, 1, 2, 3]) {
    const r = await rig(part);
    const names = r.sliders().map((s) => s.attrs["aria-label"]);
    const dup = names.filter((n, i) => names.indexOf(n) !== i);
    assert.deepEqual(dup, [], `part ${part}: ${dup.join(", ")}`);
  }
});

test("inert knobs carry the hint; the Amp envelope + EQ section says so; showError before the first render is safe", async () => {
  const r = await rig(1); // FM
  const op = r.slider("Attack"); // part_attack on an FM part: no engine plays it (the FM contour is the operator envelopes)
  assert.equal(op.attrs["data-inert"], "true");
  assert.equal(r.slider("OP 1 · Level 1").attrs["data-inert"], undefined); // the FM patch parameters sound
  assert.match(op.title, /No audible effect on this engine yet/);
  assert.match(op.attrs["aria-description"], /No audible effect/);
  assert.ok(r.host.all((e) => e.className === "pe-note").length >= 1);
  const vol = r.slider("Part volume");
  assert.equal(vol.attrs["data-inert"], undefined);
  assert.equal(r.slider("Reverb send").attrs["data-inert"], undefined);
  const organ = await rig(3);
  assert.equal(organ.slider("Drawbar 8'").attrs["data-inert"], undefined);
  // a view that has not rendered yet
  const doc = makeDoc();
  const v = new PartEditorView({ doc, host: doc.createElement("div"), tr: trKey, controller: {}, announce: () => {} });
  v.showError({ key: "chain", message: "x", code: 1 });
  v.showError({ key: "p:100", notice: "unconfirmed" });
  assert.equal(v.msgText.length > 0, true);
});

test("notices: a value the unit did not apply is told as text", async () => {
  const r = await rig(3);
  r.view.showError({ key: "p:102", code: 0, message: "", notice: "not_applied" });
  const err = r.host.all((e) => e.className === "pe-err" && !e.hidden)[0];
  assert.equal(err.textContent, "The Velora did not apply the change (it holds the earlier value).");
});

// ---- Story 9.10: the stale banner and Reload --------------------------------------------------------------------------------------------------------------------
test("a stale edit shows 'Changed on the Velora since you opened it · Reload', the knobs go read-only, and Reload applies the current snapshot", async () => {
  const r = await rig(3);
  const stale = () => r.host.all((e) => e.className === "pe-stale")[0];
  assert.equal(stale().hidden, true, "no banner while the view is current");
  const d = r.slider("Drawbar 16'");
  r.unit.perf.set("100:3", 2);
  r.unit.deviceChange(); // the player changed something on the unit after Studio loaded the Live Set
  assert.equal(stale().hidden, true, "not before an edit is refused");
  d.focus();
  d.fire("keydown", { key: "ArrowUp" });
  await r.clock.advance(300);
  assert.equal(r.unit.staleRejected, 1);
  assert.equal(stale().hidden, false);
  assert.equal(stale().textContent, "Changed on the Velora since you opened it · Reload");
  assert.equal(r.host.all((e) => e.className === "sr-only")[0].textContent, "Changed on the Velora since you opened it", "announced once, through the editor's polite region");
  const btn = r.host.all((e) => e.tagName === "BUTTON" && e.className.includes("pe-stale-reload"))[0];
  assert.equal(btn.textContent, "Reload");
  assert.equal(btn.type, "button");
  assert.equal(r.slider("Drawbar 16'").attrs["aria-readonly"], "true");
  assert.equal(r.slider("Drawbar 16'").attrs["aria-valuenow"], "2", "the knob shows what the unit holds");
  assert.equal(r.host.all((e) => e.className === "pe-banner")[0].hidden, true, "the read-only banner does not repeat the sentence");
  d.fire("keydown", { key: "ArrowUp" });
  await r.clock.advance(300);
  assert.equal(r.unit.log.filter((l) => l[0] === "SetPerformanceParam").length, 1, "nothing is sent while stale");
  btn.fire("click");
  await r.clock.advance(300);
  assert.equal(stale().hidden, true);
  assert.equal(r.slider("Drawbar 16'").attrs["aria-readonly"], undefined);
  assert.ok(r.unit.log.some((l) => l[0] === "GetState"));
  d.fire("keydown", { key: "ArrowUp" });
  await r.clock.advance(300);
  assert.equal(r.unit.perf.get("100:3"), 3);
  assert.equal(r.unit.staleRejected, 1);
  assert.equal(r.unit.log.filter((l) => l[0] === "SetPerformanceParam").at(-1)[2], r.unit.liveRevision);
});
