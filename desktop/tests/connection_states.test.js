// Unit tests for ConnectionBannerController and ConnectionBannerView (Story 9.7, FR99, UX-DR47, UX-DR77, UX-DR91, AR25).
import test from "node:test";
import assert from "node:assert/strict";

import { CATALOGS } from "../src/catalogs.js";
import { ConnectionBannerController, ConnectionBannerView } from "../src/connection_banner.js";

const tr = (k) => CATALOGS.en[k] || k;
const localeMgr = { tr };

class MockElement {
  constructor(doc, tag) {
    this.ownerDocument = doc;
    this.tagName = tag.toUpperCase();
    this.children = [];
    const classes = new Set();
    this.classList = {
      add: (c) => classes.add(c),
      remove: (c) => classes.delete(c),
      contains: (c) => classes.has(c),
      toString: () => Array.from(classes).join(" "),
      [Symbol.iterator]: () => classes.values(),
    };
    this.attrs = {};
    this.dataset = {};
    this.listeners = {};
    this.hidden = false;
    this.disabled = false;
    this.tabIndex = 0;
    this.value = "";
    this.type = "";
    this.id = "";
    this.parent = null;
    this._text = "";
  }

  get className() {
    return this.classList.toString();
  }

  set className(v) {
    for (const c of Array.from(this.classList)) this.classList.remove(c);
    if (v) v.split(/\s+/).filter(Boolean).forEach((c) => this.classList.add(c));
  }

  get textContent() {
    return this._text + this.children.map((c) => c.textContent).join("");
  }

  set textContent(v) {
    this.children.forEach((c) => { c.parent = null; });
    this.children = [];
    this._text = String(v);
  }

  appendChild(c) {
    c.parent = this;
    this.children.push(c);
    return c;
  }

  append(...items) {
    items.forEach((item) => this.appendChild(item));
  }

  replaceChildren(...items) {
    this.children.forEach((c) => { c.parent = null; });
    this.children = [];
    this._text = "";
    this.append(...items);
  }

  setAttribute(k, v) {
    this.attrs[k] = String(v);
  }

  getAttribute(k) {
    return this.attrs[k] ?? null;
  }

  addEventListener(type, fn) {
    (this.listeners[type] = this.listeners[type] || []).push(fn);
  }

  dispatchEvent(evt) {
    const list = (this.listeners[evt.type] || []).slice();
    evt.target = evt.target || this;
    for (const fn of list) fn(evt);
  }

  click() {
    this.dispatchEvent({ type: "click", target: this });
  }
}

function makeDoc() {
  const doc = {
    createElement(tag) {
      return new MockElement(doc, tag);
    },
    activeElement: null,
  };
  doc.body = new MockElement(doc, "body");
  return doc;
}

test("ConnectionBannerController: starts in default unconfigured state", () => {
  const ctl = new ConnectionBannerController({
    localeMgr,
  });

  assert.equal(ctl.status, "disconnected");
  assert.equal(ctl.configured, false);
  assert.equal(ctl.isBannerVisible, false);
});

test("ConnectionBannerController: announces disconnect and shows banner when link drops", () => {
  const announcements = [];
  const ctl = new ConnectionBannerController({
    localeMgr,
    getModelName: () => "Velora 73",
    announceFn: (msg, isWarn) => announcements.push({ msg, isWarn }),
  });

  // Connect first
  ctl.setStatus({ status: "paired", configured: true, model: "Velora 73" });
  assert.equal(ctl.isBannerVisible, false);

  // Link drops
  ctl.setStatus({ status: "disconnected", configured: true, model: "Velora 73" });
  assert.equal(ctl.isBannerVisible, true);
  assert.equal(ctl.status, "disconnected");

  assert.equal(announcements.length, 2);
  assert.equal(announcements[0].msg, "Connected");
  assert.equal(announcements[1].msg, "Velora 73 disconnected");
  assert.equal(announcements[1].isWarn, true);
});

test("ConnectionBannerController: announces Connected when reconnecting from outage", () => {
  const announcements = [];
  const ctl = new ConnectionBannerController({
    localeMgr,
    getModelName: () => "Velora 73",
    announceFn: (msg, isWarn) => announcements.push({ msg, isWarn }),
  });

  ctl.setStatus({ status: "paired", configured: true, model: "Velora 73" });
  ctl.setStatus({ status: "disconnected", configured: true, model: "Velora 73" });
  assert.equal(ctl.isBannerVisible, true);

  // Reconnect
  ctl.setStatus({ status: "paired", configured: true, model: "Velora 73" });
  assert.equal(ctl.isBannerVisible, false);

  const connectedAnnounce = announcements.find((a) => a.msg === "Connected");
  assert.ok(connectedAnnounce, "Must announce Connected on reconnection");
  assert.equal(connectedAnnounce.isWarn, false);
});

test("ConnectionBannerController: version-mismatch displays warning banner", () => {
  const ctl = new ConnectionBannerController({
    localeMgr,
    getModelName: () => "Velora 73",
  });

  ctl.setStatus({
    status: "version-mismatch",
    configured: true,
    model: "Velora 73",
    firmware_version: "1.2",
  });

  assert.equal(ctl.isBannerVisible, true);
  assert.equal(ctl.status, "version-mismatch");
});

test("ConnectionBannerView: renders disconnect banner with Reconnect button", async () => {
  const doc = makeDoc();
  const bannerEl = doc.createElement("div");
  let reconnected = false;

  const ctl = new ConnectionBannerController({
    localeMgr,
    getModelName: () => "Velora 73",
    reconnectFn: () => {
      reconnected = true;
    },
  });

  const view = new ConnectionBannerView({
    doc,
    el: bannerEl,
    controller: ctl,
    tr,
  });

  // Initially hidden
  assert.equal(bannerEl.hidden, true);

  // Connect and then disconnect
  ctl.setStatus({ status: "paired", configured: true, model: "Velora 73" });
  assert.equal(bannerEl.hidden, true);

  ctl.setStatus({ status: "disconnected", configured: true, model: "Velora 73" });
  assert.equal(bannerEl.hidden, false);
  assert.ok(bannerEl.textContent.includes("Velora 73 disconnected"));
  assert.ok(bannerEl.textContent.includes("Reconnect"));

  // Click action button
  const actionBtn = bannerEl.children.find((c) => c.tagName === "BUTTON");
  assert.ok(actionBtn);
  actionBtn.click();
  await Promise.resolve();
  assert.equal(reconnected, true);

  // Reconnect hides banner
  ctl.setStatus({ status: "paired", configured: true, model: "Velora 73" });
  assert.equal(bannerEl.hidden, true);

  view.destroy();
});

test("ConnectionBannerView: banner stays visible during connecting with disabled button", () => {
  const doc = makeDoc();
  const bannerEl = doc.createElement("div");

  const ctl = new ConnectionBannerController({
    localeMgr,
    getModelName: () => "Velora 73",
  });

  const view = new ConnectionBannerView({
    doc,
    el: bannerEl,
    controller: ctl,
    tr,
  });

  // Paired -> Disconnected -> Connecting
  ctl.setStatus({ status: "paired", configured: true, model: "Velora 73" });
  ctl.setStatus({ status: "disconnected", configured: true, model: "Velora 73" });
  assert.equal(bannerEl.hidden, false);

  ctl.setStatus({ status: "connecting", configured: true, model: "Velora 73" });
  assert.equal(bannerEl.hidden, false, "Banner must stay until connection is resolved");

  const actionBtn = bannerEl.children.find((c) => c.tagName === "BUTTON");
  assert.ok(actionBtn);
  assert.equal(actionBtn.disabled, true);
  assert.equal(actionBtn.textContent, "Connecting to your Velora…");

  // Restored -> hidden
  ctl.setStatus({ status: "paired", configured: true, model: "Velora 73" });
  assert.equal(bannerEl.hidden, true);

  view.destroy();
});

test("ConnectionBannerView: version mismatch renders exact message and triggers updateFirmware", () => {
  const doc = makeDoc();
  const bannerEl = doc.createElement("div");
  let firmwareUpdated = false;

  const ctl = new ConnectionBannerController({
    localeMgr,
    getModelName: () => "Velora 73",
    updateFirmwareFn: () => {
      firmwareUpdated = true;
    },
  });

  const view = new ConnectionBannerView({
    doc,
    el: bannerEl,
    controller: ctl,
    tr,
  });

  ctl.setStatus({
    status: "version-mismatch",
    configured: true,
    model: "Velora 73",
    firmware_version: "1.2",
  });

  assert.equal(bannerEl.hidden, false);
  assert.ok(
    bannerEl.textContent.includes(
      "Velora 73 runs 1.2; this Velora Studio needs 1.3 for some features. Update firmware / Update app."
    )
  );

  const actionBtn = bannerEl.children.find((c) => c.tagName === "BUTTON");
  assert.ok(actionBtn);
  assert.equal(actionBtn.textContent, "Update Firmware");
  assert.equal(actionBtn.disabled, false);

  actionBtn.click();
  assert.equal(firmwareUpdated, true);

  view.destroy();
});
