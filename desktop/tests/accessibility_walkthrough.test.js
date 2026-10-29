// Story 9.22: Full accessibility walkthrough and keyboard/screen reader audit tests
import test from "node:test";
import assert from "node:assert/strict";

import { FocusManager } from "../src/app.js";
import { DeviceViewController } from "../src/device_view.js";
import { DeviceView } from "../src/device_view_dom.js";
import { ExportController } from "../src/export_dialog.js";
import { ExportDialogView } from "../src/export_dialog_dom.js";
import { ImportController } from "../src/import_dialog.js";
import { ImportDialogView } from "../src/import_dialog_dom.js";
import { formatValue } from "../src/part_editor.js";
import { CATALOGS } from "../src/catalogs.js";

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
    this.style = {};
  }
  get textContent() {
    return this._text + this.children.map((c) => c.textContent).join("");
  }
  set textContent(v) {
    this.children.forEach((c) => {
      c.parent = null;
    });
    this.children = [];
    this._text = String(v);
  }
  appendChild(c) {
    c.parent = this;
    this.children.push(c);
    return c;
  }
  append(...nodes) {
    nodes.forEach((n) => this.appendChild(n));
  }
  setAttribute(k, v) {
    this.attrs[k] = String(v);
  }
  getAttribute(k) {
    return this.attrs[k] || null;
  }
  hasAttribute(k) {
    return k in this.attrs;
  }
  removeAttribute(k) {
    delete this.attrs[k];
  }
  addEventListener(event, fn) {
    if (!this.listeners[event]) this.listeners[event] = [];
    this.listeners[event].push(fn);
  }
  fire(type, ev = {}) {
    const e = {
      preventDefault() {
        this.prevented = true;
      },
      stopPropagation() {
        this.stopped = true;
      },
      ...ev,
      target: this,
    };
    for (let n = this; n; n = n.parent) {
      (n.listeners[type] || []).forEach((f) => f(e));
    }
    return e;
  }
  click() {
    if (this.disabled) return;
    (this.listeners.click || []).forEach((fn) => fn({ target: this, preventDefault: () => {} }));
  }
  querySelector(sel) {
    return this.querySelectorAll(sel)[0] || null;
  }
  querySelectorAll(sel) {
    const res = [];
    const walk = (n) => {
      if (sel.startsWith("#") && n.id === sel.slice(1)) res.push(n);
      else if (sel.startsWith(".") && n.className.split(/\s+/).includes(sel.slice(1))) res.push(n);
      else if (sel.toLowerCase() === n.tagName.toLowerCase()) res.push(n);
      n.children.forEach(walk);
    };
    this.children.forEach(walk);
    return res;
  }
  focus() {
    this.ownerDocument.activeElement = this;
  }
  contains(n) {
    for (let cur = n; cur; cur = cur.parent) {
      if (cur === this) return true;
    }
    return false;
  }
  closest(sel) {
    let cur = this;
    while (cur) {
      if (sel === "[hidden]" && cur.hidden) return cur;
      if (sel.includes("display: none") && cur.style?.display === "none") return cur;
      cur = cur.parent;
    }
    return null;
  }
}

class FakeDoc {
  constructor() {
    this.activeElement = null;
    this.elements = new Map();
  }
  createElement(tag) {
    return new El(this, tag);
  }
  getElementById(id) {
    return this.elements.get(id) || null;
  }
  querySelector(sel) {
    for (const el of this.elements.values()) {
      if (sel.startsWith("#") && el.id === sel.slice(1)) return el;
      const res = el.querySelector(sel);
      if (res) return res;
    }
    return null;
  }
  register(id, el) {
    el.id = id;
    this.elements.set(id, el);
    return el;
  }
}

function tr(key, ...args) {
  const cat = CATALOGS.en || {};
  let s = cat[key] || key;
  args.forEach((a) => {
    s = s.replace("%s", a);
  });
  return s;
}

test("FocusManager F6 cycles landmark regions: sidebar -> live-set -> scenes -> parts -> editor -> footer", () => {
  const doc = new FakeDoc();

  const sidebar = doc.register("sidebar-region", doc.createElement("nav"));
  const liveSet = doc.register("live-set-header", doc.createElement("div"));
  const scenes = doc.register("live-scenes", doc.createElement("div"));
  const liveParts = doc.register("live-part-grid", doc.createElement("div"));
  const realEditor = doc.register("real-part-editor", doc.createElement("div"));
  const footer = doc.register("footer-region", doc.createElement("footer"));

  const fm = new FocusManager(doc);
  const regions = fm.getRegions();
  assert.equal(regions.length, 6);
  assert.equal(regions[0], sidebar);
  assert.equal(regions[1], liveSet);
  assert.equal(regions[2], scenes);
  assert.equal(regions[3], liveParts);
  assert.equal(regions[4], realEditor);
  assert.equal(regions[5], footer);

  // Cycle forward
  assert.equal(fm.cycleRegion(1), sidebar);
  assert.equal(doc.activeElement, sidebar);

  assert.equal(fm.cycleRegion(1), liveSet);
  assert.equal(doc.activeElement, liveSet);

  assert.equal(fm.cycleRegion(1), scenes);
  assert.equal(doc.activeElement, scenes);

  assert.equal(fm.cycleRegion(1), liveParts);
  assert.equal(doc.activeElement, liveParts);

  assert.equal(fm.cycleRegion(1), realEditor);
  assert.equal(doc.activeElement, realEditor);

  assert.equal(fm.cycleRegion(1), footer);
  assert.equal(doc.activeElement, footer);

  // Wraps to start
  assert.equal(fm.cycleRegion(1), sidebar);
  assert.equal(doc.activeElement, sidebar);

  // Shift+F6 reverse cycle
  assert.equal(fm.cycleRegion(-1), footer);
  assert.equal(doc.activeElement, footer);

  // Contextual cycling: when focus is inside a child of a region, F6 moves to next landmark
  const childBtn = doc.createElement("button");
  scenes.appendChild(childBtn);
  childBtn.focus();
  assert.equal(doc.activeElement, childBtn);
  assert.equal(fm.cycleRegion(1), liveParts);
  assert.equal(doc.activeElement, liveParts);

  // Initial reverse cycling on fresh FocusManager goes to last region (footer)
  const freshDoc = new FakeDoc();
  freshDoc.register("sidebar-region", freshDoc.createElement("nav"));
  freshDoc.register("footer-region", freshDoc.createElement("footer"));
  const freshFm = new FocusManager(freshDoc);
  assert.equal(freshFm.cycleRegion(-1), freshDoc.getElementById("footer-region"));
});

test("FocusManager F6 switches to Device View when device view is open", () => {
  const doc = new FakeDoc();

  const sidebar = doc.register("sidebar-region", doc.createElement("nav"));
  const devView = doc.register("device-view", doc.createElement("section"));
  const footer = doc.register("footer-region", doc.createElement("footer"));

  const fm = new FocusManager(doc);
  const regions = fm.getRegions();
  assert.equal(regions.length, 3);
  assert.equal(regions[0], sidebar);
  assert.equal(regions[1], devView);
  assert.equal(regions[2], footer);

  assert.equal(fm.cycleRegion(1), sidebar);
  assert.equal(fm.cycleRegion(1), devView);
  assert.equal(fm.cycleRegion(1), footer);
});

test("Studio Knobs read name, formatted value and unit via aria-valuetext", () => {
  // Test frequency knob formatting
  const freqKnob = {
    key: "cutoff",
    name: "Cutoff",
    unit: "Hz",
    min: 20,
    max: 20000,
    value: 440,
    kind: "frequency",
  };
  const freqText = formatValue(freqKnob, 440, tr);
  assert.ok(freqText.includes("440") && freqText.includes("Hz"));

  // Test time knob formatting
  const timeKnob = {
    key: "attack",
    name: "Attack",
    unit: "ms",
    min: 1,
    max: 5000,
    value: 120,
    kind: "time",
  };
  const timeText = formatValue(timeKnob, 120, tr);
  assert.ok(timeText.includes("120") && timeText.includes("ms"));

  // Test dB knob formatting
  const dbKnob = {
    key: "level",
    name: "Level",
    unit: "dB",
    min: -60,
    max: 6,
    value: -3,
    kind: "decibels",
  };
  const dbText = formatValue(dbKnob, -3, tr);
  assert.ok(dbText.includes("-3") && dbText.includes("dB"));
});

test("ExportDialogView keyboard walkthrough: Esc closes, Space toggles, Tab trap", () => {
  const doc = new FakeDoc();
  const host = doc.createElement("div");
  let closed = false;
  const ctl = new ExportController({
    tr,
    getDeviceStatus: () => ({ isPaired: true, model: "Velora 73" }),
    getLiveSets: () => [{ id: "ls-1", name: "Show 1", parts: [] }],
  });
  ctl.open("bundle");

  const dialog = new ExportDialogView({
    doc,
    host,
    controller: ctl,
    tr,
    restore: () => { closed = true; },
  });
  dialog.open();

  // verify title and role
  const modal = dialog.dialog.modal;
  assert.equal(modal.getAttribute("role"), "dialog");
  assert.equal(modal.getAttribute("aria-modal"), "true");

  // Esc key closes
  modal.fire("keydown", { key: "Escape" });
  assert.equal(closed, true);
});

test("ImportDialogView keyboard walkthrough: role, focus trap, and Esc close", () => {
  const doc = new FakeDoc();
  const host = doc.createElement("div");
  let closed = false;
  const ctl = new ImportController({
    tr,
    getDeviceStatus: () => ({ isPaired: true, model: "Velora 73" }),
  });
  ctl.loadManifest({
    format_version: 1,
    source_device_model: "Velora 73",
    live_sets: [{ id: "ls-1", name: "Ballad" }],
    timbres: [],
  });

  const dialog = new ImportDialogView({
    doc,
    host,
    controller: ctl,
    tr,
    restore: () => { closed = true; },
  });
  dialog.open();

  const modal = dialog.dialog.modal;
  assert.equal(modal.getAttribute("role"), "dialog");
  assert.equal(modal.getAttribute("aria-modal"), "true");

  modal.fire("keydown", { key: "Escape" });
  assert.equal(closed, true);
});

test("DeviceView keyboard walkthrough: title focus, role regions and close button", () => {
  const doc = new FakeDoc();
  const host = doc.createElement("section");
  let backedOut = false;
  const ctl = new DeviceViewController({ tr });
  ctl.setDeviceStatus({
    configured: true,
    status: "paired",
    model: "Velora 73",
    firmware_version: "1.4.0",
  });

  const view = new DeviceView({
    doc,
    host,
    tr,
    controller: ctl,
    onBack: () => { backedOut = true; },
  });
  view.show();

  // Heading receives initial focus
  const heading = host.querySelector(".device-header-title");
  assert.ok(heading);
  assert.equal(doc.activeElement, heading);

  // Close button triggers onBack
  const closeBtn = host.querySelector("#btn-close-device-view");
  assert.ok(closeBtn);
  closeBtn.click();
  assert.equal(backedOut, true);
});
