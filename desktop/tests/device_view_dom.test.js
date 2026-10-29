// Story 9.21: DOM accessibility and layout tests for DeviceView
import test from "node:test";
import assert from "node:assert/strict";

import { DeviceViewController } from "../src/device_view.js";
import { DeviceView } from "../src/device_view_dom.js";
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
  removeAttribute(k) {
    delete this.attrs[k];
  }
  addEventListener(event, fn) {
    if (!this.listeners[event]) this.listeners[event] = [];
    this.listeners[event].push(fn);
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
  scrollIntoView() {}
}

class FakeDoc {
  constructor() {
    this.activeElement = null;
  }
  createElement(tag) {
    return new El(this, tag);
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

test("DeviceView renders offline card when unit is not paired", () => {
  const doc = new FakeDoc();
  const host = doc.createElement("section");
  const ctl = new DeviceViewController({ tr });

  const view = new DeviceView({ doc, host, tr, controller: ctl });
  view.show();

  assert.equal(host.hidden, false);
  const offlineCard = host.querySelector("#device-offline-card");
  assert.ok(offlineCard, "offline card should be rendered");
  assert.ok(offlineCard.textContent.includes(CATALOGS.en["studio.device.offline_title"]));
  assert.ok(offlineCard.textContent.includes(CATALOGS.en["studio.device.offline_body"]));
});

test("DeviceView renders hardware overview, storage breakdown, firmware, and backups when paired", () => {
  const doc = new FakeDoc();
  const host = doc.createElement("section");
  const ctl = new DeviceViewController({ tr });
  ctl.setDeviceStatus({
    configured: true,
    status: "paired",
    model: "Velora 73",
    firmware_version: "1.4.0",
    device_id: "velora-test-uuid",
  });
  ctl.setBackupTime("2026-10-28 15:30:00");
  ctl.setLibraries([
    { name: "Concert Grand", kind: "Factory", instrument_count: 3, size_bytes: 2_400_000_000 },
    { name: "Custom EP", kind: "User", instrument_count: 5, size_bytes: 600_000_000 },
  ]);

  let backedUp = false;
  let restored = false;
  let backedOut = false;

  const view = new DeviceView({
    doc,
    host,
    tr,
    controller: ctl,
    onBack: () => { backedOut = true; },
    onBackup: () => { backedUp = true; },
    onRestore: () => { restored = true; },
  });
  view.show();

  // 1. Overview section
  const ovSec = host.querySelector("#device-overview-section");
  assert.ok(ovSec);
  assert.equal(ovSec.getAttribute("role"), "region");
  assert.ok(ovSec.textContent.includes("Velora 73"));
  assert.ok(ovSec.textContent.includes("Stage"));
  assert.ok(ovSec.textContent.includes("1.4.0"));
  assert.ok(ovSec.textContent.includes("Production"));

  // 2. Storage section
  const stSec = host.querySelector("#device-storage-section");
  assert.ok(stSec);
  const meter = host.querySelector(".storage-meter");
  assert.ok(meter);
  assert.equal(meter.getAttribute("role"), "meter");
  assert.ok(meter.getAttribute("aria-valuenow"));
  assert.ok(meter.getAttribute("aria-valuetext"));

  const table = host.querySelector(".device-libraries-table");
  assert.ok(table);
  assert.equal(table.getAttribute("role"), "table");
  assert.ok(table.textContent.includes("Concert Grand"));
  assert.ok(table.textContent.includes("Custom EP"));

  // 3. Firmware section
  const fwSec = host.querySelector("#device-firmware-section");
  assert.ok(fwSec);
  assert.ok(fwSec.textContent.includes("1.4.0"));

  // 4. Backups section
  const bkSec = host.querySelector("#device-backups-section");
  assert.ok(bkSec);
  assert.ok(bkSec.textContent.includes("2026-10-28 15:30:00"));

  const btnBk = host.querySelector("#btn-device-create-backup");
  btnBk.click();
  assert.equal(backedUp, true);

  const btnRest = host.querySelector("#btn-device-restore-backup");
  btnRest.click();
  assert.equal(restored, true);

  // 5. Close button
  const btnClose = host.querySelector("#btn-close-device-view");
  btnClose.click();
  assert.equal(backedOut, true);
  assert.equal(ctl.isOpen, false);
});

test("DeviceView diagnostics export button initiates export", async () => {
  const doc = new FakeDoc();
  const host = doc.createElement("section");
  let exported = false;

  const ctl = new DeviceViewController({
    tr,
    saveDiagnostics: async () => {
      exported = true;
      return "/tmp/test_export.json";
    },
  });
  ctl.setDeviceStatus({
    configured: true,
    status: "paired",
    model: "Velora 73",
  });

  const view = new DeviceView({ doc, host, tr, controller: ctl });
  view.show();

  const exportBtn = host.querySelector("#btn-export-diagnostics");
  assert.ok(exportBtn);
  exportBtn.click();

  // Allow microtasks to complete
  await new Promise((r) => setTimeout(r, 10));
  assert.equal(exported, true);
});

test("DeviceView displays error banner when diagnostics export fails", async () => {
  const doc = new FakeDoc();
  const host = doc.createElement("section");

  const ctl = new DeviceViewController({
    tr,
    saveDiagnostics: async () => {
      throw new Error("Disk write error");
    },
  });
  ctl.setDeviceStatus({
    configured: true,
    status: "paired",
    model: "Velora 73",
  });

  const view = new DeviceView({ doc, host, tr, controller: ctl });
  view.show();

  const exportBtn = host.querySelector("#btn-export-diagnostics");
  exportBtn.click();

  await new Promise((r) => setTimeout(r, 10));
  assert.equal(ctl.diagnostics.error, "Disk write error");

  view.render();
  const errBanner = host.querySelector(".device-error-banner");
  assert.ok(errBanner, "Error banner should be visible");
  assert.equal(errBanner.getAttribute("role"), "alert");
  assert.ok(errBanner.textContent.includes("Disk write error"));
});
