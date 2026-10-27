// Story 9.12: the DOM side of Write to Velora against a minimal DOM shim (no jsdom here): the dialog (role, labelled, modal, focus in and back), the three scopes as a radio group with
// the disabled Scene and its reason, Save as new's name field and its errors, keyboard (arrows, Home / End, Space / Enter, Tab trap, Esc, Enter in the field), IME composition, the
// writing state (Primary Buttons disabled, Esc / backdrop ignored, focus kept), double click, hostile text, and the Studio Banner (text, Check now, Reload, Dismiss).
import test from "node:test";
import assert from "node:assert/strict";

import { CATALOGS } from "../src/catalogs.js";
import { SCOPE } from "../src/write_to_velora.js";
import { WriteBannerView, WriteDialogView, announceText } from "../src/write_to_velora_dom.js";
import { rig, settle } from "./support/write_rig.js";

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
    this.type = "";
    this.id = "";
    this.parent = null;
    this._text = "";
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
  // like a browser: a disabled element takes no focus
  focus() { if (this.disabled) return; this.ownerDocument.activeElement = this; }
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

async function setup(opts = {}) {
  const r = await rig(opts.rig);
  if (opts.dirty !== false) await r.dirtyIt();
  const doc = makeDoc();
  const host = doc.createElement("div");
  const writeButton = doc.createElement("button");
  const main = doc.createElement("div");
  main.tabIndex = 0;
  const bannerEl = doc.createElement("div");
  const view = new WriteDialogView({ doc, host, tr, controller: r.wc, restore: () => (writeButton.disabled ? main : writeButton) });
  const banner = new WriteBannerView({ doc, el: bannerEl, tr, controller: r.wc, onReload: () => { banner.reloaded = (banner.reloaded || 0) + 1; } });
  // the page's wiring: the controller drives the view
  r.wc.onChange = () => { view.render(); banner.render(); writeButton.disabled = !r.wc.availability("ls-1").enabled; };
  r.wc.onClose = () => { view.close(); };
  writeButton.disabled = false;
  writeButton.focus();
  const open = (o = {}) => { const ok = r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect", ...o }); return ok; };
  const parts = () => {
    const all = (p) => host.all(p);
    return {
      dialog: all((n) => n.attrs.role === "dialog")[0],
      radios: all((n) => n.attrs.role === "radio"),
      group: all((n) => n.attrs.role === "radiogroup")[0],
      input: all((n) => n.tagName === "INPUT")[0],
      alerts: all((n) => n.attrs.role === "alert"),
      status: all((n) => n.attrs.role === "status")[0],
      buttons: all((n) => n.tagName === "BUTTON"),
      changes: all((n) => n.tagName === "LI"),
      backdrop: host.children[0],
      title: all((n) => n.tagName === "H2")[0],
    };
  };
  return { ...r, doc, host, writeButton, main, bannerEl, banner, view, open, parts };
}

const key = (target, k, extra = {}) => target.fire("keydown", { key: k, ...extra });
const cancelBtn = (p) => p.buttons.find((b) => b.textContent === "Cancel");
const confirmBtn = (p) => p.buttons.find((b) => b.textContent === "Write to Velora");

test("opens as a labelled modal dialog with the focus on the chosen scope; what changed and the three scopes are listed", async () => {
  const s = await setup();
  assert.ok(s.open());
  const p = s.parts();
  assert.equal(p.dialog.attrs["aria-modal"], "true");
  assert.equal(p.dialog.attrs["aria-labelledby"], p.title.id);
  assert.equal(p.title.textContent, "Write to Velora");
  assert.equal(p.dialog.attrs["aria-describedby"], p.dialog.children[1].id);
  assert.equal(p.dialog.children[1].textContent, "Perfect: 1 unsaved change");
  assert.deepEqual(p.changes.map((c) => c.textContent), ["Part 1: volume"]);
  assert.equal(p.group.attrs["role"], "radiogroup");
  assert.deepEqual(p.radios.map((r) => r.children[0].textContent), ["Scene", "Whole Live Set", "Save as new…"]);
  assert.deepEqual(p.radios.map((r) => r.attrs["aria-checked"]), ["false", "true", "false"]);
  assert.equal(s.doc.activeElement, p.radios[1]);
  assert.equal(p.radios[1].tabIndex, 0);
  assert.equal(p.radios[0].tabIndex, -1);
  assert.ok(p.input.parent.hidden, "the name field belongs to Save as new");
  assert.equal(confirmBtn(p).disabled, false);
  assert.deepEqual(s.unit.writeLog, []);
});

test("Scene is disabled with its reason, announced as the description of the radio; keys skip it", async () => {
  const s = await setup({ rig: { unitOptions: { empties: [5] } } });
  await s.studioEdit("SetPartTimbre", { part: 5, library_id: "lib-a", instrument_key: "grand" });
  await settle();
  s.open();
  let p = s.parts();
  const scene = p.radios[0];
  assert.equal(scene.attrs["aria-disabled"], "true");
  const reason = p.dialog.all((n) => n.id === scene.attrs["aria-describedby"])[0];
  assert.equal(reason.textContent, "Timbre changes are saved with the Live Set");
  assert.equal(reason.hidden, false);
  // a click on it changes nothing; Up from Whole wraps past it to Save as new
  scene.fire("click");
  assert.equal(s.parts().radios[1].attrs["aria-checked"], "true");
  key(p.radios[1], "ArrowUp");
  p = s.parts();
  assert.deepEqual(p.radios.map((r) => r.attrs["aria-checked"]), ["false", "false", "true"]);
  assert.equal(s.doc.activeElement, p.radios[2]);
  key(p.radios[2], "ArrowDown"); // wraps to the first ENABLED radio: Whole
  assert.deepEqual(s.parts().radios.map((r) => r.attrs["aria-checked"]), ["false", "true", "false"]);
  key(s.parts().radios[1], "Home");
  assert.equal(s.parts().radios[1].attrs["aria-checked"], "true");
  key(s.parts().radios[1], "End");
  assert.equal(s.parts().radios[2].attrs["aria-checked"], "true");
  assert.deepEqual(s.unit.writeLog, []);
});

test("radio keys: arrows move and select, Space and Enter select the focused radio (Enter does not confirm), Scene works when it is allowed", async () => {
  const s = await setup();
  s.open();
  let p = s.parts();
  key(p.radios[1], "ArrowUp");
  p = s.parts();
  assert.equal(p.radios[0].attrs["aria-checked"], "true");
  assert.equal(s.doc.activeElement, p.radios[0]);
  assert.equal(p.radios[0].children[1].textContent, "Only the changes in scene 1");
  key(p.radios[0], "ArrowRight");
  assert.equal(s.parts().radios[1].attrs["aria-checked"], "true");
  p.radios[2].focus();
  key(p.radios[2], " ");
  assert.equal(s.parts().radios[2].attrs["aria-checked"], "true");
  key(s.parts().radios[2], "Enter");
  assert.equal(s.unit.writeLog.length, 0); // Enter on a radio only selects
  assert.equal(s.parts().input.parent.hidden, false);
});

test("Tab and Shift+Tab stay inside the dialog (chosen scope, name field when shown, Cancel, Write)", async () => {
  const s = await setup();
  s.open();
  let p = s.parts();
  const order = () => { const e = key(s.doc.activeElement, "Tab"); return s.doc.activeElement; };
  assert.equal(s.doc.activeElement, p.radios[1]);
  assert.equal(order(), cancelBtn(p));
  assert.equal(order(), confirmBtn(p));
  assert.equal(order(), p.radios[1]); // wraps
  const back = key(p.radios[1], "Tab", { shiftKey: true });
  assert.ok(back.prevented);
  assert.equal(s.doc.activeElement, confirmBtn(p));
  // with Save as new the name field joins
  s.wc.setScope(SCOPE.NEW);
  p = s.parts();
  p.radios[2].focus();
  assert.equal(order(), p.input);
  assert.equal(order(), cancelBtn(p));
});

test("Esc, Cancel and the backdrop close without sending anything and return the focus to the Write button", async () => {
  const s = await setup();
  s.open();
  s.parts().radios[1].focus();
  key(s.parts().radios[1], "Escape");
  assert.equal(s.parts().dialog, undefined);
  assert.equal(s.doc.activeElement, s.writeButton);
  s.open();
  cancelBtn(s.parts()).fire("click");
  assert.equal(s.parts().dialog, undefined);
  assert.equal(s.doc.activeElement, s.writeButton);
  s.open();
  s.parts().backdrop.fire("click");
  assert.equal(s.parts().dialog, undefined);
  assert.equal(s.doc.activeElement, s.writeButton);
  s.open();
  s.parts().dialog.fire("click"); // a click inside the dialog bubbles to the backdrop listener but its target is not the backdrop
  assert.ok(s.parts().dialog);
  assert.deepEqual(s.unit.writeLog, []);
});

test("Esc while an IME composes the name belongs to the IME (it does not close the dialog); Enter in the field confirms once the text is committed", async () => {
  const s = await setup();
  s.open();
  s.wc.setScope(SCOPE.NEW);
  const input = s.parts().input;
  input.fire("compositionstart");
  input.value = "Ballad こ";
  input.fire("input"); // mid-composition: the controller is not told
  assert.notEqual(s.wc.name, "Ballad こ");
  const e = key(input, "Escape", { isComposing: true });
  assert.ok(!e.prevented);
  assert.ok(s.parts().dialog);
  key(input, "Enter", { keyCode: 229 });
  assert.equal(s.unit.writeLog.length, 0);
  input.value = "Ballad こんにちは";
  input.fire("compositionend");
  assert.equal(s.wc.name, "Ballad こんにちは");
  const enter = key(input, "Enter");
  assert.ok(enter.prevented);
  await settle();
  assert.deepEqual(s.unit.writeLog, [{ scope: 3, new_name: "Ballad こんにちは" }]);
});

test("Save as new shows the suggested free name; a name in use shows the device message in an alert and puts the focus in the field", async () => {
  const s = await setup();
  s.unit.takenNames = new Set(["PERFECT 2"]);
  s.open();
  s.wc.setScope(SCOPE.NEW);
  let p = s.parts();
  assert.equal(p.input.value, "Perfect 3");
  p.input.value = "Perfect 2";
  p.input.fire("input");
  confirmBtn(p).fire("click");
  await settle();
  p = s.parts();
  const alert = p.alerts.find((a) => a.textContent.includes("already in this setlist"));
  assert.equal(alert.textContent, "A Live Set named 'Perfect 2' is already in this setlist");
  assert.equal(p.input.attrs["aria-invalid"], "true");
  assert.equal(s.doc.activeElement, p.input);
  assert.equal(p.input.value, "Perfect 3");
  assert.equal(p.input.attrs["aria-describedby"], alert.id);
  assert.equal(s.unit.writeLog.length, 0);
  confirmBtn(p).fire("click");
  await settle();
  assert.deepEqual(s.unit.writeLog, [{ scope: 3, new_name: "Perfect 3" }]);
});

test("while the Write is out: both buttons are disabled, the status says so, aria-busy is set, the focus rests on the dialog, Esc and the backdrop do nothing, a double click sends one Write", async () => {
  const s = await setup();
  s.open();
  let release;
  const gate = new Promise((r) => { release = r; });
  s.unit.writeHook = async () => { await gate; };
  confirmBtn(s.parts()).fire("click");
  confirmBtn(s.parts()).fire("click");
  confirmBtn(s.parts()).fire("click");
  await settle();
  const p = s.parts();
  assert.equal(confirmBtn(p).disabled, true);
  assert.equal(cancelBtn(p).disabled, true);
  assert.equal(p.status.textContent, "Writing to Velora…");
  assert.equal(p.dialog.attrs["aria-busy"], "true");
  assert.equal(s.doc.activeElement, p.dialog); // not lost to the page: Esc still reaches the dialog
  assert.equal(p.radios.every((r) => r.attrs["aria-disabled"] === "true"), true);
  const esc = key(s.doc.activeElement, "Escape");
  assert.ok(esc.prevented);
  assert.ok(s.parts().dialog, "Esc does not take a Write that is out back");
  s.parts().backdrop.fire("click");
  assert.ok(s.parts().dialog);
  key(s.doc.activeElement, "Tab");
  assert.equal(s.doc.activeElement, p.dialog);
  release();
  await settle();
  assert.equal(s.unit.writeLog.length, 1);
  assert.equal(s.parts().dialog, undefined);
});

test("after a successful Write the dialog is gone and the focus goes to the main area (the Write button is disabled now), never to nowhere", async () => {
  const s = await setup();
  s.open();
  confirmBtn(s.parts()).fire("click");
  await settle();
  assert.equal(s.parts().dialog, undefined);
  assert.equal(s.writeButton.disabled, true);
  assert.equal(s.doc.activeElement, s.main);
  assert.equal(announceText(s.ev.announces.at(-1), tr), "Written to Velora");
});

test("a failed Write closes the dialog; the Studio Banner explains it; Dismiss hides it", async () => {
  const s = await setup();
  s.unit.storageFull = true;
  s.open();
  confirmBtn(s.parts()).fire("click");
  await settle();
  assert.equal(s.parts().dialog, undefined);
  assert.equal(s.bannerEl.hidden, false);
  assert.equal(s.bannerEl.textContent.startsWith("Couldn't write to Velora 73. Not enough space to write."), true);
  assert.equal(s.doc.activeElement, s.writeButton); // still enabled: the edits are still there
  const dismiss = s.bannerEl.all((n) => n.tagName === "BUTTON" && n.textContent === "Dismiss")[0];
  const action = s.bannerEl.all((n) => n.className.includes("wr-banner-action"))[0];
  assert.equal(action.hidden, true); // nothing to check or reload for a refusal
  dismiss.fire("click");
  assert.equal(s.bannerEl.hidden, true);
});

test("the unconfirmed banner offers Check now; after the answer it is replaced; the stale banner offers Reload", async () => {
  const s = await setup({ rig: { wire: async (m, unit) => { const rep = await unit.request(m); if (m.body.WriteLiveSet) throw new Error("connection lost"); return rep; } } });
  s.open();
  confirmBtn(s.parts()).fire("click");
  await settle();
  assert.equal(s.bannerEl.hidden, false);
  assert.match(s.bannerEl.textContent, /^Couldn't confirm the write to Velora 73: connection lost\./);
  const action = s.bannerEl.all((n) => n.className.includes("wr-banner-action"))[0];
  assert.equal(action.hidden, false);
  assert.equal(action.textContent, "Check now");
  action.fire("click");
  await settle();
  assert.equal(s.bannerEl.hidden, true); // the unit says it landed: the banner goes
  assert.equal(s.writeButton.disabled, true);
  s.wc.showBanner("stale");
  assert.equal(s.bannerEl.hidden, false);
  assert.match(s.bannerEl.textContent, /^Changed on the Velora since you opened it\. Nothing was written\./);
  assert.equal(action.textContent, "Reload");
  action.fire("click");
  assert.equal(s.banner.reloaded, 1);
});

test("hostile Live Set names, error names and change texts are text, never markup", async () => {
  const s = await setup();
  const evil = '<img src=x onerror="alert(1)">';
  s.unit.liveSetName = evil;
  s.unit.changes[0] = { key: "future.key", text: evil, part: null, param_id: null, scene: null, slot: null };
  s.unit.push();
  s.open({ liveSetName: evil });
  let p = s.parts();
  assert.equal(p.dialog.children[1].textContent, `${evil}: 1 unsaved change`);
  assert.equal(p.changes[0].textContent, evil);
  s.wc.setScope(SCOPE.NEW);
  p = s.parts();
  p.input.value = evil;
  p.input.fire("input");
  s.unit.takenNames = new Set([evil.replace(/[a-z]/g, (c) => c.toUpperCase())]);
  s.wc.takenNames = () => s.unit.takenNames;
  confirmBtn(p).fire("click");
  await settle();
  assert.ok(s.parts().alerts.some((a) => a.textContent === `A Live Set named '${evil}' is already in this setlist`));
  // (the shim throws if innerHTML is ever assigned: nothing here used it)
});

test("the dialog follows State pushes while open: the change list grows, Scene turns into disabled when a timbre arrives, and it closes when the Live Set is written elsewhere", async () => {
  const s = await setup({ rig: { unitOptions: { empties: [5] } } });
  s.open();
  assert.equal(s.parts().changes.length, 1);
  await s.studioEdit("SetSendLevels", { part: 1, reverb: 0.5, delay: 0.1 });
  await settle();
  assert.deepEqual(s.parts().changes.map((c) => c.textContent), ["Part 1: volume", "Part 2: reverb send"]);
  assert.equal(s.parts().radios[0].attrs["aria-disabled"], "false");
  await s.studioEdit("SetPartTimbre", { part: 5, library_id: "lib-a", instrument_key: "grand" });
  await settle();
  assert.equal(s.parts().radios[0].attrs["aria-disabled"], "true");
  s.unit.dirty = false;
  s.unit.changes = [];
  s.unit.push();
  assert.equal(s.parts().dialog, undefined);
  assert.equal(s.doc.activeElement, s.main);
});

test("a long change list is cut with a count of the rest", async () => {
  const s = await setup();
  s.unit.changes = Array.from({ length: 32 }, (_, i) => ({ key: "part.volume", part: i % 8, scene: null, param_id: null, slot: null, text: "" }));
  const st = s.unit.state();
  st.edits.change_count = 45;
  s.wc.getState = () => st;
  s.open();
  const items = s.parts().changes;
  assert.equal(items.length, 33);
  assert.equal(items.at(-1).textContent, "and 13 more");
  assert.equal(s.parts().dialog.children[1].textContent, "Perfect: 45 unsaved changes");
});
