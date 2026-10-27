// Story 9.11: the DOM side of the "+ Add timbre" picker against a minimal DOM shim (no jsdom here): the dialog (role, labelled, modal), focus into the search field and back to the
// strip, keyboard (Up / Down / PageUp / PageDown / Enter / Esc / Tab trap), search with diacritics, IME composition, hostile names as text, the capped list, the empty / no-match /
// error / assigning states, mouse choice and the backdrop.
import test from "node:test";
import assert from "node:assert/strict";

import { TimbrePickerController } from "../src/timbre_picker.js";
import { TimbrePickerView } from "../src/timbre_picker_dom.js";
import { CATALOGS } from "../src/catalogs.js";
import { FakeUnit } from "./support/fake_unit.js";

class El {
  constructor(doc, tag) {
    this.ownerDocument = doc;
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.attrs = {};
    this.dataset = {};
    this.listeners = {};
    this.className = "";
    this.hidden = false;
    this.disabled = false;
    this.tabIndex = 0;
    this.value = "";
    this.checked = false;
    this.type = "";
    this.id = "";
    this.parent = null;
    this._text = "";
    this._rect = { top: 0, height: 20 };
  }
  get textContent() { return this._text + this.children.map((c) => c.textContent).join(""); }
  contains(n) { for (let x = n; x; x = x.parent) if (x === this) return true; return false; }
  // Like a browser: removing the focused element (or its ancestor) drops the focus to the body.
  set textContent(v) {
    const a = this.ownerDocument.activeElement;
    if (a && this.children.some((c) => c.contains(a))) this.ownerDocument.activeElement = null;
    this.children.forEach((c) => { c.parent = null; });
    this.children = [];
    this._text = String(v);
  }
  set innerHTML(v) { throw new Error("innerHTML must never be used for device-supplied text: " + v); }
  appendChild(c) { c.parent = this; this.children.push(c); return c; }
  append(...cs) { cs.forEach((c) => this.appendChild(c)); }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; }
  removeAttribute(k) { delete this.attrs[k]; }
  addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
  getBoundingClientRect() { return this._rect; }
  closest(sel) {
    const role = /\[role="(\w+)"\]/.exec(sel)[1];
    for (let n = this; n; n = n.parent) if (n.attrs.role === role) return n;
    return null;
  }
  focus() {
    this.ownerDocument.activeElement = this;
    this.fire("focusin", {});
  }
  fire(type, ev = {}) {
    const e = { preventDefault() { this.prevented = true; }, stopPropagation() { this.stopped = true; }, ...ev, target: this };
    for (let n = this; n; n = n.parent) (n.listeners[type] || []).forEach((f) => f(e));
    return e;
  }
  all(pred, out = []) { if (pred(this)) out.push(this); this.children.forEach((c) => c.all(pred, out)); return out; }
}
const makeDoc = () => { const d = { activeElement: null, createElement: (t) => new El(d, t) }; return d; };

const EN = CATALOGS.en;
const tr = (key, ...args) => {
  assert.ok(key in EN, `locale key missing: ${key}`);
  let i = 0;
  return EN[key].replace(/%[ds]/g, () => String(args[i++]));
};
const settle = async () => { for (let i = 0; i < 8; i++) await new Promise((r) => setImmediate(r)); };
const T = (library_id, instrument_key, name, category, library_name = "Lib") => ({ library_id, instrument_key, name, category, library_name });

async function setup({ list, assign, unit = new FakeUnit({ empties: [3] }) } = {}) {
  const doc = makeDoc();
  const host = doc.createElement("div");
  const strip = doc.createElement("div");
  strip.tabIndex = 0;
  const others = doc.createElement("button");
  const announced = [];
  const sent = [];
  const closes = [];
  const ctl = new TimbrePickerController({
    listTimbres: list || (() => unit.listTimbres()),
    assign: assign || (async (a) => { sent.push(a); return { ok: true }; }),
    categoryLabel: (it) => (it.categoryKey ? tr(it.categoryKey) : ""),
    onChange: () => view.render(),
    onClose: (reason) => { closes.push(reason); view.restoreFocus(); },
  });
  const view = new TimbrePickerView({ doc, host, tr, controller: ctl, announce: (t) => announced.push(t) });
  strip.focus();
  const open = async () => { view.open(strip); await ctl.open({ liveSetId: "ls-1", part: 3 }); await settle(); };
  const parts = () => {
    const all = (p) => host.all(p);
    return {
      dialog: all((n) => n.attrs.role === "dialog")[0],
      input: all((n) => n.tagName === "INPUT")[0],
      options: all((n) => n.attrs.role === "option"),
      groups: all((n) => n.attrs.role === "group"),
      status: all((n) => n.attrs.role === "status")[0],
      alert: all((n) => n.attrs.role === "alert")[0],
      buttons: all((n) => n.tagName === "BUTTON"),
      listbox: all((n) => n.attrs.role === "listbox")[0],
      backdrop: host.children[0],
    };
  };
  return { doc, host, strip, others, ctl, view, open, parts, sent, closes, announced, unit };
}

const key = (target, k, extra = {}) => target.fire("keydown", { key: k, ...extra });
const type = (input, text) => { input.value = text; input.fire("input", {}); };
const names = (p) => p.options.map((o) => o.children[0].textContent);

test("opens as a labelled modal dialog with the focus in the search field and the list grouped by category", async () => {
  const s = await setup();
  await s.open();
  const p = s.parts();
  assert.equal(p.dialog.attrs["aria-modal"], "true");
  assert.equal(p.dialog.attrs["aria-labelledby"], p.dialog.children[0].id);
  assert.equal(p.dialog.children[0].textContent, "Add a timbre to part 4");
  assert.equal(s.doc.activeElement, p.input);
  assert.equal(p.input.attrs.role, "combobox");
  assert.equal(p.input.attrs["aria-controls"], p.listbox.id);
  assert.equal(p.listbox.attrs["aria-label"], "Timbres installed on the Velora");
  assert.deepEqual(p.groups.map((g) => g.attrs["aria-label"]), ["Piano", "E.Piano", "Strings"]);
  assert.deepEqual(names(p), ["Concert Grand", "Soft Grand", "Rhôdes Mk I", "String Ensemble"]);
  assert.equal(p.options[0].attrs["aria-selected"], "true");
  assert.equal(p.input.attrs["aria-activedescendant"], p.options[0].id);
  assert.equal(p.status.textContent, "4 timbres");
  assert.equal(p.status.attrs["aria-live"], "polite");
  assert.deepEqual(s.unit.log, []);
});

test("keyboard: Up / Down / PageDown / PageUp move the active option (the focus stays in the field), Enter adds it, nothing is sent before", async () => {
  const s = await setup();
  await s.open();
  let p = s.parts();
  key(p.input, "ArrowDown");
  key(p.input, "ArrowDown");
  p = s.parts();
  assert.equal(p.options[2].attrs["aria-selected"], "true");
  assert.equal(p.input.attrs["aria-activedescendant"], p.options[2].id);
  assert.equal(s.doc.activeElement, p.input);
  key(p.input, "ArrowUp");
  key(p.input, "PageDown");
  p = s.parts();
  assert.equal(p.options[3].attrs["aria-selected"], "true");
  key(p.input, "PageUp");
  p = s.parts();
  assert.equal(p.options[0].attrs["aria-selected"], "true");
  assert.deepEqual(s.sent, []);
  key(p.input, "ArrowDown");
  const e = key(s.parts().input, "Enter");
  assert.ok(e.prevented);
  await settle();
  assert.deepEqual(s.sent, [{ part: 3, libraryId: "lib-b", instrumentKey: "grand", liveSetId: "ls-1" }]);   // "Soft Grand" (lib-b), not lib-a's "grand"
  assert.deepEqual(s.closes, ["assigned"]);
  assert.equal(s.parts().dialog, undefined);
  assert.equal(s.doc.activeElement, s.strip, "the focus returns to the strip");
});

test("Esc closes without any request and returns the focus to the strip; Esc during an IME composition only ends the composition", async () => {
  const s = await setup();
  await s.open();
  let p = s.parts();
  type(p.input, "gra");
  const e1 = key(p.input, "Escape", { isComposing: true });
  assert.ok(!e1.prevented);
  assert.equal(s.ctl.isOpen, true);
  const e2 = key(p.input, "Escape");
  assert.ok(e2.prevented);
  assert.deepEqual(s.closes, ["cancelled"]);
  assert.deepEqual(s.sent, []);
  assert.equal(s.parts().dialog, undefined);
  assert.equal(s.doc.activeElement, s.strip);
  // and the Cancel button and a click on the backdrop do the same
  await s.open();
  p = s.parts();
  p.buttons.find((b) => b.textContent === "Cancel").fire("click", {});
  assert.deepEqual(s.closes, ["cancelled", "cancelled"]);
  assert.equal(s.doc.activeElement, s.strip);
  await s.open();
  p = s.parts();
  p.backdrop.fire("click", {});
  assert.deepEqual(s.closes, ["cancelled", "cancelled", "cancelled"]);
  await s.open();
  p = s.parts();
  p.options[0].fire("click", {}); // a click inside the dialog is not a click on the backdrop
  await settle();
  assert.deepEqual(s.closes.at(-1), "assigned");
});

test("search: typing filters (diacritics and case ignored), the first match is active, no match says so, clearing brings everything back", async () => {
  const s = await setup();
  await s.open();
  let p = s.parts();
  type(p.input, "RHODES");
  p = s.parts();
  assert.deepEqual(names(p), ["Rhôdes Mk I"]);
  assert.equal(p.options[0].attrs["aria-selected"], "true");
  assert.equal(p.status.textContent, "1 of 4 timbres");
  type(p.input, "zzz");
  p = s.parts();
  assert.equal(p.options.length, 0);
  assert.equal(p.status.textContent, "No timbre matches “zzz”.");
  assert.equal(p.listbox.hidden, true);
  assert.equal(p.input.attrs["aria-expanded"], "false");
  assert.equal(p.input.attrs["aria-activedescendant"], undefined);
  key(p.input, "Enter");
  await settle();
  assert.deepEqual(s.sent, [], "Enter with nothing to choose sends nothing");
  type(p.input, "");
  assert.equal(s.parts().options.length, 4);
  assert.equal(s.doc.activeElement, s.parts().input, "the field keeps the focus while the list is rebuilt");
});

test("IME: the list waits for the committed text; Enter that confirms a composition does not add a timbre", async () => {
  const s = await setup();
  await s.open();
  const p = s.parts();
  p.input.fire("compositionstart", {});
  type(p.input, "r");           // an intermediate composition string
  type(p.input, "rh");
  assert.equal(s.parts().options.length, 4, "nothing is filtered while the IME composes");
  key(p.input, "Enter", { isComposing: true });
  key(p.input, "Enter", { keyCode: 229 });
  key(p.input, "ArrowDown", { isComposing: true });
  await settle();
  assert.deepEqual(s.sent, []);
  assert.equal(s.parts().options[0].attrs["aria-selected"], "true");
  p.input.value = "rhôdes";
  p.input.fire("compositionend", {});
  assert.deepEqual(names(s.parts()), ["Rhôdes Mk I"]);
  key(p.input, "Enter");
  await settle();
  assert.equal(s.sent.length, 1);
  assert.equal(s.sent[0].instrumentKey, "rhodes");
});

test("Tab and Shift+Tab stay inside the dialog (field, Try again when shown, Cancel)", async () => {
  const s = await setup();
  await s.open();
  const p = s.parts();
  const cancel = p.buttons.find((b) => b.textContent === "Cancel");
  key(p.input, "Tab");
  assert.equal(s.doc.activeElement, cancel);
  key(cancel, "Tab");
  assert.equal(s.doc.activeElement, p.input);
  key(p.input, "Tab", { shiftKey: true });
  assert.equal(s.doc.activeElement, cancel);
  key(cancel, "Tab", { shiftKey: true });
  assert.equal(s.doc.activeElement, p.input);
});

test("hostile names, libraries and queries are text, never markup", async () => {
  const evil = '<img src=x onerror="alert(1)">';
  const s = await setup({ list: async () => ({ timbres: [T("a", "k", evil, 1, "<script>"), T("a", "k2", "Normal", 1, "Lib")] }) });
  await s.open();
  let p = s.parts();
  assert.ok(names(p).includes(evil));
  assert.equal(p.options.find((o) => o.children[0].textContent === evil).children[1].textContent, "<script>");
  assert.equal(s.host.all((n) => n.tagName === "IMG" || n.tagName === "SCRIPT").length, 0);
  type(p.input, evil);
  p = s.parts();
  assert.equal(p.status.textContent.includes(evil) || p.options.length === 1, true);
  type(p.input, "<b>nothing</b>");
  assert.equal(s.parts().status.textContent, "No timbre matches “<b>nothing</b>”.");
  assert.equal(s.host.all((n) => n.tagName === "B").length, 0);
});

test("a huge catalog draws a capped list and says so; narrowing the search shows the rest", async () => {
  const many = Array.from({ length: 5000 }, (_, i) => T("lib", "k" + i, "Timbre " + String(i).padStart(4, "0"), 1));
  const s = await setup({ list: async () => ({ timbres: many, truncated: true }) });
  await s.open();
  let p = s.parts();
  assert.equal(p.options.length, 200);
  assert.match(p.status.textContent, /^5000 timbres · Showing the first 200: type more to narrow the list\. · The Velora has more timbres than Studio lists\.$/);
  type(p.input, "4999");
  p = s.parts();
  assert.deepEqual(names(p), ["Timbre 4999"]);
  assert.equal(p.status.textContent, "1 of 5000 timbres · The Velora has more timbres than Studio lists.");
});

test("empty catalog, loading and failed load each have their own message; Try again reads the list again", async () => {
  const empty = await setup({ list: async () => ({ timbres: [] }) });
  await empty.open();
  assert.equal(empty.parts().status.textContent, "No timbres are installed on this Velora yet. Import a library on the Velora first.");
  assert.equal(empty.parts().options.length, 0);
  assert.equal(empty.doc.activeElement, empty.parts().input);
  key(empty.parts().input, "Escape");
  assert.equal(empty.doc.activeElement, empty.strip);

  let release;
  let fail = true;
  const s = await setup({ list: () => (fail ? new Promise((_, rej) => { release = rej; }) : Promise.resolve({ timbres: [T("a", "k", "Grand", 1)] })) });
  s.view.open(s.strip);
  const p0 = s.ctl.open({ liveSetId: "ls-1", part: 3 });
  assert.equal(s.parts().status.textContent, "Reading the timbres installed on your Velora…");
  assert.equal(s.parts().dialog.attrs["aria-busy"], "true");
  release(new Error("not connected"));
  await p0;
  await settle();
  let p = s.parts();
  assert.equal(p.alert.textContent, "Could not read the timbres from the Velora: not connected");
  const retry = p.buttons.find((b) => b.textContent === "Try again");
  assert.equal(retry.hidden, false);
  fail = false;
  retry.fire("click", {});
  await settle();
  p = s.parts();
  assert.equal(p.alert.textContent, "");
  assert.equal(retry.hidden, true);
  assert.deepEqual(names(p), ["Grand"]);
});

test("a refused assignment keeps the dialog open with the engine's message; while one is out Esc, the backdrop and Cancel do nothing", async () => {
  let release;
  let first = true;
  const s = await setup({ assign: (a) => { s2.push(a); return new Promise((r) => { release = r; first = first && false; }); } });
  const s2 = [];
  await s.open();
  let p = s.parts();
  key(p.input, "Enter");
  p = s.parts();
  assert.equal(p.status.textContent, "Adding the timbre…");
  assert.equal(p.input.readOnly, true);
  assert.equal(p.buttons.find((b) => b.textContent === "Cancel").disabled, true);
  key(p.input, "Escape");
  p.backdrop.fire("click", {});
  key(p.input, "Enter");                          // a second Enter while the first is out
  assert.equal(s.ctl.isOpen, true);
  assert.equal(s2.length, 1);
  release({ ok: false, error: { code: 13, message: "BUSY (reason=reload_in_progress)" } });
  await settle();
  p = s.parts();
  assert.equal(p.alert.textContent, "The Velora did not add the timbre: BUSY (reason=reload_in_progress)");
  assert.equal(p.input.readOnly, false);
  assert.equal(p.options.length, 4);
  assert.equal(s.doc.activeElement, p.input);
  assert.deepEqual(s.closes, []);
});

test("the link dropping (abort) removes the dialog and gives the focus back; a late answer changes nothing", async () => {
  let release;
  const s = await setup({ assign: () => new Promise((r) => { release = r; }) });
  await s.open();
  key(s.parts().input, "Enter");
  s.ctl.abort("disconnected");
  assert.equal(s.parts().dialog, undefined);
  assert.deepEqual(s.closes, ["aborted"]);
  assert.equal(s.doc.activeElement, s.strip);
  release({ ok: true });
  await settle();
  assert.deepEqual(s.closes, ["aborted"]);
});

test("re-opening builds a fresh dialog (ids stay unique) and the strip regains the focus every time", async () => {
  const s = await setup();
  await s.open();
  const first = s.parts().options[0].id;
  key(s.parts().input, "Escape");
  await s.open();
  const second = s.parts().options[0].id;
  assert.notEqual(first, second);
  assert.equal(s.host.children.length, 1, "one dialog at a time");
  key(s.parts().input, "Escape");
  assert.equal(s.doc.activeElement, s.strip);
});
