// Story 9.8: the DOM side of the Live Set view against a minimal DOM shim (no jsdom here): tile identity, focus, roving tabindex, keyboard, textContent-only rendering.
import test from "node:test";
import assert from "node:assert/strict";

import { buildGrids, setGridLabels, updatePartCell, updateSceneCell } from "../src/live_set_dom.js";
import { applyState, buildModel } from "../src/live_set_view.js";

class El {
  constructor(doc, tag) {
    this.ownerDocument = doc;
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.attrs = {};
    this.dataset = {};
    this.style = {};
    this.listeners = {};
    this.className = "";
    this.hidden = false;
    this.tabIndex = 0;
    this.title = "";
    this.parent = null;
    this._text = "";
  }
  get textContent() { return this._text + this.children.map((c) => c.textContent).join(""); }
  set textContent(v) { this.children = []; this._text = String(v); }
  set innerHTML(v) { throw new Error("innerHTML must never be used for device-supplied text: " + v); }
  appendChild(c) { c.parent = this; this.children.push(c); return c; }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; }
  removeAttribute(k) { delete this.attrs[k]; }
  addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
  closest(sel) {
    const role = /\[role="(\w+)"\]/.exec(sel)[1];
    for (let n = this; n; n = n.parent) if (n.attrs.role === role) return n;
    return null;
  }
  focus() {
    this.ownerDocument.activeElement = this;
    const ev = { target: this };
    for (let n = this; n; n = n.parent) (n.listeners.focusin || []).forEach((f) => f(ev));
  }
  fire(type, ev) {
    const e = { preventDefault() { this.prevented = true; }, ...ev, target: this };
    for (let n = this; n; n = n.parent) (n.listeners[type] || []).forEach((f) => f(e));
    return e;
  }
  all(pred, out = []) { if (pred(this)) out.push(this); this.children.forEach((c) => c.all(pred, out)); return out; }
}
const makeDoc = () => { const d = { activeElement: null, createElement: (t) => new El(d, t) }; return d; };

const tr = (key, ...args) => {
  const T = { "studio.live_set.part": "PART %d", "studio.live_set.vol": "VOL %d", "studio.live_set.off": "OFF", "studio.live_set.add_timbre": "+ Add timbre",
    "studio.live_set.missing_library": "Missing library", "studio.live_set.engine_unavailable": "Engine not available", "studio.live_set.scene_label": "%d %s",
    "studio.live_set.scene_add": "+ %d", "studio.live_set.scene_empty": "empty scene", "studio.live_set.scene_current": "current", "studio.live_set.part_aria": "Part %d, %s, %s",
    "studio.live_set.organ": "Organ", "studio.live_set.grid_scenes": "Scenes", "studio.live_set.grid_parts": "Parts", "category.piano.short": "Piano", "category.organ.short": "Organ",
    "studio.live_set.add_timbre_tooltip": "Choose a timbre", "studio.live_set.add_timbre_hint": "press Enter to add a timbre" };
  let i = 0;
  return (T[key] ?? key).replace(/%[ds]/g, () => String(args[i++]));
};

const ID = "11111111-1111-4111-8111-111111111111";
const view = (parts) => buildModel({ live_set_id: ID, name: "N", scenes: [{ index: 0, name: "<b>Intro</b>" }], parts, part_count: 8 });
const p = (i, o = {}) => ({ index: i, on: true, engine_type_id: 1, timbre_name: "T" + i, category: 1, volume: 0.5, low_key: 36, high_key: 96, ...o });

function setup() {
  const doc = makeDoc();
  const scenes = doc.createElement("div"); scenes.setAttribute("role", "grid");
  const parts = doc.createElement("div"); parts.setAttribute("role", "grid");
  const selected = [];
  const g = buildGrids(doc, scenes, parts, { onSelectPart: (i) => selected.push(i) });
  return { doc, scenes, parts, g, selected };
}

test("buildGrids: 8 + 8 cells, one tab stop each, labels from the catalog", () => {
  const { scenes, parts, g } = setup();
  setGridLabels(scenes, parts, tr);
  assert.equal(g.sceneCells.length, 8);
  assert.equal(g.partCells.length, 8);
  assert.deepEqual(g.partCells.map((c) => c.tabIndex), [0, -1, -1, -1, -1, -1, -1, -1]);
  assert.equal(scenes.getAttribute("aria-label"), "Scenes");
  assert.equal(parts.getAttribute("aria-label"), "Parts");
  assert.ok(g.partCells.every((c) => c.getAttribute("role") === "gridcell"));
});

test("updatePartCell is in place: the cell node, its children and the focus survive; only text changes", () => {
  const { g, doc } = setup();
  const m = buildModel({ live_set_id: ID, name: "N", parts: [p(0), p(1)], part_count: 8 });
  m.parts.forEach((q, i) => updatePartCell(g.partCells[i], q, { tr, selected: i === 0 }));
  const cell0 = g.partCells[0];
  const kids = [...cell0.children];
  g.partCells[0].focus();
  const m2 = applyState(m, { live_set_loaded: true, live_set_id: ID, parts: [{ part: 0, view_fields: true, volume: 0.9, keyboard_on: true, low_key: 36, high_key: 96, part_state: 0 }] });
  updatePartCell(cell0, m2.parts[0], { tr, selected: true });
  assert.equal(g.partCells[0], cell0);
  assert.deepEqual(cell0.children, kids);                 // the same child nodes
  assert.equal(doc.activeElement, cell0);                 // focus kept
  assert.equal(cell0._r.left.textContent, "VOL 90");
  assert.equal(cell0._r.fill.style.width, "90%");
  assert.equal(cell0.getAttribute("aria-selected"), "true");
  const label = cell0.getAttribute("aria-label");
  assert.match(label, /Part 1, T0, PIANO/);                // name and category
  assert.match(label, /VOL 90/);
  assert.match(label, /C2–C7/);                            // volume and key range
});

test("hostile device text is written as text, never as markup", () => {
  const { g } = setup();
  const evil = '<img src=x onerror="alert(1)">';
  const m = view([p(0, { timbre_name: evil }), p(2, { engine_type_id: 2, timbre_name: "", organ_registration: "<script>" })]);
  updatePartCell(g.partCells[0], m.parts[0], { tr, selected: false });   // would throw if innerHTML were used
  updatePartCell(g.partCells[2], m.parts[2], { tr, selected: false });
  updateSceneCell(g.sceneCells[0], m.scenes[0], tr);
  assert.equal(g.partCells[0]._r.name.textContent, evil);
  assert.equal(g.partCells[0].all((n) => n.tagName === "IMG" || n.tagName === "SCRIPT").length, 0);
  assert.equal(g.partCells[2]._r.reg.textContent, "<script>");
  assert.equal(g.sceneCells[0].textContent, "1 <b>Intro</b>");
  assert.equal(g.sceneCells[0].children.length, 0);
});

test("statuses: empty is a dashed placeholder with a tooltip, missing and unavailable are warn text with a glyph and no timbre name, off shows the OFF tag", () => {
  const { g } = setup();
  const m = view([p(0, { missing_library: true, timbre_name: "Other" }), p(1, { empty: true, timbre_name: "" }), p(2, { on: false }), p(3)]);
  const un = applyState(m, { live_set_loaded: true, live_set_id: ID, parts: [{ part: 3, view_fields: true, volume: 0.5, keyboard_on: true, low_key: 0, high_key: 127, part_state: 3, engine_type_id: 4 }] });
  [0, 1, 2].forEach((i) => updatePartCell(g.partCells[i], m.parts[i], { tr, selected: false }));
  updatePartCell(g.partCells[3], un.parts[3], { tr, selected: false });
  assert.equal(g.partCells[0]._r.name.textContent, "⚠ Missing library");
  assert.ok(g.partCells[0].className.includes("missing"));
  assert.equal(g.partCells[0].getAttribute("aria-label"), "Part 1, Missing library");
  assert.equal(g.partCells[1]._r.name.textContent, "+ Add timbre");
  assert.equal(g.partCells[1]._r.name.title, "Choose a timbre");      // Story 9.11: an enabled affordance, no longer a disabled placeholder
  assert.equal(g.partCells[1]._r.name.getAttribute("aria-disabled"), null);
  assert.match(g.partCells[1].getAttribute("aria-label"), /press Enter to add a timbre$/);
  assert.doesNotMatch(g.partCells[0].getAttribute("aria-label"), /press Enter/);
  assert.ok(g.partCells[1].className.includes("empty"));
  assert.equal(g.partCells[2]._r.left.textContent, "OFF");
  assert.equal(g.partCells[3]._r.name.textContent, "⚠ Engine not available");
  assert.ok(g.partCells[3]._r.meter.hidden);
});

test("keyboard: arrows move the roving tabindex and focus with wrap, Enter / Space select a part, Enter on a scene does nothing", () => {
  const { scenes, parts, g, selected, doc } = setup();
  g.partCells[0].focus();
  const press = (host, cell, key) => cell.fire("keydown", { key });
  press(parts, g.partCells[0], "ArrowLeft");
  assert.equal(doc.activeElement, g.partCells[7]);
  assert.deepEqual(g.partCells.map((c) => c.tabIndex), [-1, -1, -1, -1, -1, -1, -1, 0]);
  press(parts, g.partCells[7], "ArrowRight");
  assert.equal(doc.activeElement, g.partCells[0]);
  press(parts, g.partCells[0], "End");
  assert.equal(doc.activeElement, g.partCells[7]);
  const e = press(parts, g.partCells[7], "Enter");
  assert.deepEqual(selected, [7]);
  assert.ok(e.prevented);
  press(parts, g.partCells[7], " ");
  assert.deepEqual(selected, [7, 7]);
  press(parts, g.partCells[7], "ArrowUp");
  assert.equal(doc.activeElement, g.partCells[7]);        // one row: Up / Down stay
  g.sceneCells[2].focus();
  press(scenes, g.sceneCells[2], "Enter");
  assert.deepEqual(selected, [7, 7]);                     // scenes never act
  press(scenes, g.sceneCells[2], "ArrowRight");
  assert.equal(doc.activeElement, g.sceneCells[3]);
  g.partCells[4].fire("click", {});
  assert.deepEqual(selected.at(-1), 4);
});

// Story 9.11: an EMPTY strip is also the "+ Add timbre" button (click, Enter, Space); a strip with a timbre, a missing library or an unavailable engine only selects.
test("an empty strip opens the picker on click, Enter and Space; other strips only select", () => {
  const doc = makeDoc();
  const scenes = doc.createElement("div"); scenes.setAttribute("role", "grid");
  const parts = doc.createElement("div"); parts.setAttribute("role", "grid");
  const selected = [];
  const added = [];
  const g = buildGrids(doc, scenes, parts, { onSelectPart: (i) => selected.push(i), onAddTimbre: (i) => added.push(i) });
  const m = view([p(0), p(1, { empty: true, timbre_name: "" }), p(2, { missing_library: true }), p(3, { on: false })]);
  m.parts.forEach((q, i) => updatePartCell(g.partCells[i], q, { tr, selected: false }));
  g.partCells[1].fire("click", {});
  assert.deepEqual(added, [1]);
  g.partCells[0].fire("click", {});
  g.partCells[2].fire("click", {});
  g.partCells[3].fire("click", {});
  assert.deepEqual(added, [1], "a filled, missing or off strip never opens the picker");
  g.partCells[1].focus();
  const e = g.partCells[1].fire("keydown", { key: "Enter" });
  assert.ok(e.prevented);
  g.partCells[1].fire("keydown", { key: " " });
  assert.deepEqual(added, [1, 1, 1]);
  g.partCells[0].focus();
  g.partCells[0].fire("keydown", { key: "Enter" });
  assert.deepEqual(added, [1, 1, 1]);
  assert.deepEqual(selected.slice(0, 3), [1, 0, 2]);                   // every activation selects the part first
  // once the part has a timbre the same strip no longer offers it
  const filled = view([p(0), p(1, { timbre_name: "Concert Grand" })]);
  updatePartCell(g.partCells[1], filled.parts[1], { tr, selected: false });
  g.partCells[1].fire("click", {});
  assert.deepEqual(added, [1, 1, 1]);
});
