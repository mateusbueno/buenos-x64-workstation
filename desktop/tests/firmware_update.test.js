// Story 9.20: Unit and accessibility tests for firmware update flow
import test from "node:test";
import assert from "node:assert/strict";

import { DeviceViewController } from "../src/device_view.js";
import { DeviceView } from "../src/device_view_dom.js";
import { CATALOGS } from "../src/catalogs.js";

const tr = (key, ...args) => {
  const cat = CATALOGS.en || {};
  let s = cat[key] || key;
  for (const a of args) {
    if (s.includes("%s")) {
      s = s.replace("%s", a);
    } else if (s.includes("%d")) {
      s = s.replace("%d", a);
    }
  }
  return s;
};

class El {
  constructor(doc, tag) {
    this.ownerDocument = doc;
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.attrs = {};
    this.listeners = {};
    this.className = "";
    this.hidden = false;
    this.disabled = false;
    this.tabIndex = 0;
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
  addEventListener(event, fn) {
    if (!this.listeners[event]) this.listeners[event] = [];
    this.listeners[event].push(fn);
  }
  click() {
    if (this.disabled) return;
    (this.listeners["click"] || []).forEach((fn) => fn({ target: this }));
  }
  focus() {
    this.ownerDocument.activeElement = this;
  }
  querySelector(sel) {
    if (sel.startsWith("#")) {
      const id = sel.slice(1);
      if (this.id === id) return this;
      for (const c of this.children) {
        const found = c.querySelector(sel);
        if (found) return found;
      }
      return null;
    }
    if (sel.startsWith(".")) {
      const cls = sel.slice(1);
      if (this.className.split(/\s+/).includes(cls)) return this;
      for (const c of this.children) {
        const found = c.querySelector(sel);
        if (found) return found;
      }
      return null;
    }
    if (this.tagName.toLowerCase() === sel.toLowerCase()) return this;
    for (const c of this.children) {
      const found = c.querySelector(sel);
      if (found) return found;
    }
    return null;
  }
}

class Doc {
  constructor() {
    this.body = new El(this, "body");
    this.activeElement = null;
  }
  createElement(tag) {
    return new El(this, tag);
  }
}

function makePairedController(overrides = {}) {
  const ctl = new DeviceViewController({
    tr,
    ...overrides,
  });
  ctl.setDeviceStatus({
    configured: true,
    status: "paired",
    model: "Velora 73",
    firmware_version: "1.4.0",
    device_id: "velora-test-unit",
  });
  return ctl;
}

test("Firmware update: bundle selection and manifest inspection shows bundle vs current version", async () => {
  const ctl = makePairedController({
    invoke: async (cmd, args) => {
      if (cmd === "device_inspect_firmware_bundle") {
        return {
          valid: true,
          version: "1.5.0",
          compatible: "Velora 73",
          description: "Velora OS 1.5.0",
          file_size: 45 * 1024 * 1024,
          file_name: "velora-1.5.0.raucb",
        };
      }
      throw new Error(`unknown command: ${cmd}`);
    },
  });

  const doc = new Doc();
  const host = doc.createElement("div");
  const view = new DeviceView({ doc, host, tr, controller: ctl });
  view.show();

  // Initially: displays current version and select file button
  const currentFwEl = host.querySelector("#device-firmware-current");
  assert.ok(currentFwEl.textContent.includes("1.4.0"));
  const selectBtn = host.querySelector("#btn-select-firmware-file");
  assert.ok(selectBtn);

  // Pick file
  await ctl.selectFirmwareBundle("/path/to/velora-1.5.0.raucb");
  view.render();

  // Bundle version is displayed alongside current version
  assert.equal(ctl.firmware.state, "inspected");
  assert.equal(ctl.firmware.bundleInfo.version, "1.5.0");
  const bundleInfoEl = host.querySelector("#device-firmware-bundle-info");
  assert.ok(bundleInfoEl);
  assert.ok(bundleInfoEl.textContent.includes("1.5.0"));

  // Send to Velora button is present
  const sendBtn = host.querySelector("#btn-send-firmware");
  assert.ok(sendBtn);
});

test("Firmware update: streaming progress updates meter, aria attributes, and cancel resets state", async () => {
  let announcements = [];
  const ctl = makePairedController({
    onAnnounce: (msg) => announcements.push(msg),
    invoke: async (cmd) => {
      if (cmd === "device_send_firmware_update") {
        return { completed: true, status: "ready_to_install" };
      }
      if (cmd === "device_cancel_firmware_update") {
        return {};
      }
      return {};
    },
  });

  ctl.firmware.bundlePath = "/mock/update.raucb";
  ctl.firmware.bundleInfo = { version: "1.5.0", file_size: 100 * 1024 * 1024 };

  const doc = new Doc();
  const host = doc.createElement("div");
  const view = new DeviceView({ doc, host, tr, controller: ctl });

  // Simulate streaming chunk progress events
  ctl.handleProgress({
    status: "streaming",
    bytes_transferred: 45 * 1024 * 1024,
    total_bytes: 100 * 1024 * 1024,
    progress: 45,
    completed: false,
  });
  view.render();

  assert.equal(ctl.firmware.state, "sending");
  assert.equal(ctl.firmware.progress, 45);

  const meter = host.querySelector("#firmware-progress-meter");
  assert.ok(meter);
  assert.equal(meter.getAttribute("role"), "progressbar");
  assert.equal(meter.getAttribute("aria-valuenow"), "45");
  assert.equal(meter.getAttribute("aria-valuemin"), "0");
  assert.equal(meter.getAttribute("aria-valuemax"), "100");

  const progressText = host.querySelector("#firmware-progress-text");
  assert.ok(progressText.textContent.includes("45%"));

  // Cancel streaming
  const cancelBtn = host.querySelector("#btn-cancel-firmware-update");
  assert.ok(cancelBtn);
  cancelBtn.click();
  view.render();

  assert.equal(ctl.firmware.state, "idle");
  assert.equal(ctl.firmware.bundlePath, null);
});

test("Firmware update: ready to install announcement and Studio never triggers install", async () => {
  let announcements = [];
  let installCommandCalled = false;
  const ctl = makePairedController({
    onAnnounce: (msg) => announcements.push(msg),
    invoke: async (cmd) => {
      if (cmd.includes("install")) installCommandCalled = true;
      return {};
    },
  });

  ctl.firmware.bundleInfo = { version: "1.5.0" };
  ctl.handleProgress({
    status: "ready_to_install",
    completed: true,
  });

  const doc = new Doc();
  const host = doc.createElement("div");
  const view = new DeviceView({ doc, host, tr, controller: ctl });
  view.render();

  assert.equal(ctl.firmware.state, "ready_to_install");
  const readyBanner = host.querySelector("#firmware-ready-banner");
  assert.ok(readyBanner);
  assert.equal(readyBanner.getAttribute("role"), "status");
  assert.ok(readyBanner.textContent.includes("1.5.0 ready to install"));
  assert.ok(readyBanner.textContent.includes("WRITE · Install"));

  // Verify Studio provides no button to start the install itself
  assert.equal(host.querySelector("#btn-start-install"), null);
  assert.equal(installCommandCalled, false);
});

test("Firmware update: installing state, expected reboot anticipation, and clean reconnection", () => {
  let announcements = [];
  const ctl = makePairedController({
    onAnnounce: (msg) => announcements.push(msg),
  });

  const doc = new Doc();
  const host = doc.createElement("div");
  const view = new DeviceView({ doc, host, tr, controller: ctl });

  // Device initiates installation
  ctl.handleDeviceInstallingNotice("1.5.0");
  view.render();

  assert.equal(ctl.firmware.state, "installing");
  assert.equal(ctl.firmware.expectedReboot, true);
  const installBanner = host.querySelector("#firmware-installing-banner");
  assert.ok(installBanner);
  assert.ok(installBanner.textContent.includes("sound is off on the device until it restarts"));

  // Connection drops during reboot
  const wasExpected = ctl.handleDeviceDisconnectDuringReboot();
  assert.equal(wasExpected, true);
  assert.equal(ctl.firmware.state, "rebooting");
  view.render();

  const rebootBanner = host.querySelector("#firmware-rebooting-banner");
  assert.ok(rebootBanner);

  // Device reconnects with new firmware version 1.5.0
  ctl.setDeviceStatus({
    configured: true,
    status: "paired",
    model: "Velora 73",
    firmware_version: "1.5.0",
  });
  view.render();

  assert.equal(ctl.firmware.state, "idle");
  assert.equal(ctl.firmware.version, "1.5.0");
  assert.equal(ctl.firmware.expectedReboot, false);
  assert.ok(announcements.some((msg) => msg.includes("1.5.0")));
});

test("Firmware update: rejects corrupted / bad signature bundle with typed error", async () => {
  const ctl = makePairedController({
    invoke: async (cmd) => {
      if (cmd === "device_send_firmware_update") {
        throw new Error("bad_signature: signature verification failed");
      }
      return {};
    },
  });

  ctl.firmware.bundlePath = "/mock/bad_sig.raucb";
  ctl.firmware.bundleInfo = { version: "1.5.0" };

  const doc = new Doc();
  const host = doc.createElement("div");
  const view = new DeviceView({ doc, host, tr, controller: ctl });

  await assert.rejects(async () => {
    await ctl.sendFirmwareUpdate();
  });
  view.render();

  assert.equal(ctl.firmware.state, "error");
  const errEl = host.querySelector("#device-firmware-error");
  assert.ok(errEl);
  assert.equal(errEl.getAttribute("role"), "alert");
  assert.ok(errEl.textContent.includes("Invalid or corrupted bundle signature"));
});

test("Firmware update: rejects incompatible hardware bundle with typed error", async () => {
  const ctl = makePairedController({
    invoke: async (cmd) => {
      if (cmd === "device_send_firmware_update") {
        throw new Error("incompatible: incompatible hardware bundle");
      }
      return {};
    },
  });

  ctl.firmware.bundlePath = "/mock/incomp.raucb";
  ctl.firmware.bundleInfo = { version: "1.5.0" };

  const doc = new Doc();
  const host = doc.createElement("div");
  const view = new DeviceView({ doc, host, tr, controller: ctl });

  await assert.rejects(async () => {
    await ctl.sendFirmwareUpdate();
  });
  view.render();

  assert.equal(ctl.firmware.state, "error");
  const errEl = host.querySelector("#device-firmware-error");
  assert.ok(errEl);
  assert.ok(errEl.textContent.includes("incompatible with this hardware"));
});
