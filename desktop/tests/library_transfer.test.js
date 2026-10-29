import test from "node:test";
import assert from "node:assert/strict";
import { formatTransferLine, LibraryViewController } from "../src/library_view.js";
import { LibraryView } from "../src/library_view_dom.js";

function createMockDoc() {
  const elements = [];
  function createEl(tagName) {
    const el = {
      tagName: tagName.toUpperCase(),
      className: "",
      get textContent() {
        if (this._textContent !== undefined && this._textContent !== "") return this._textContent;
        if (this.children.length > 0) {
          return this.children.map(c => c.textContent).join("");
        }
        return this._textContent || "";
      },
      set textContent(v) {
        this._textContent = String(v);
        if (v === "") this.children = [];
      },
      hidden: false,
      tabIndex: -1,
      children: [],
      dataset: {},
      attributes: {},
      style: {},
      disabled: false,
      value: "",
      appendChild(child) {
        if (!child) return;
        this.children.push(child);
        child.parentElement = this;
      },
      setAttribute(k, v) { this.attributes[k] = String(v); },
      getAttribute(k) { return this.attributes[k]; },
      removeAttribute(k) { delete this.attributes[k]; },
      addEventListener(type, handler) {
        this.listeners = this.listeners || {};
        this.listeners[type] = this.listeners[type] || [];
        this.listeners[type].push(handler);
      },
      dispatchEvent(event) {
        if (this.listeners && this.listeners[event.type]) {
          for (const fn of this.listeners[event.type]) fn(event);
        }
      },
      querySelector(selector) {
        if (selector.startsWith(".")) {
          const cls = selector.slice(1);
          return findChild(this, (c) => c.className && c.className.split(" ").includes(cls));
        }
        if (selector.startsWith("#")) {
          const id = selector.slice(1);
          return findChild(this, (c) => c.id === id);
        }
        return findChild(this, (c) => c.tagName === selector.toUpperCase());
      },
      querySelectorAll(selector) {
        const results = [];
        findAllChildren(this, selector, results);
        return results;
      },
      focus() { this.focused = true; },
      click() {
        this.dispatchEvent({ type: "click", target: this });
      },
    };
    elements.push(el);
    return el;
  }

  function findChild(parent, predicate) {
    for (const c of parent.children) {
      if (predicate(c)) return c;
      const found = findChild(c, predicate);
      if (found) return found;
    }
    return null;
  }

  function findAllChildren(parent, selector, results) {
    const parts = selector.split(",").map(s => s.trim());
    for (const c of parent.children) {
      let match = false;
      for (const sel of parts) {
        const cleanSel = sel.replace(/:not\([^)]*\)/g, "");
        if (cleanSel.startsWith(".") && c.className && c.className.split(" ").includes(cleanSel.slice(1))) {
          match = true;
          break;
        } else if (cleanSel.startsWith("#") && c.id === cleanSel.slice(1)) {
          match = true;
          break;
        } else if (c.tagName === cleanSel.toUpperCase()) {
          match = true;
          break;
        }
      }
      if (match && !results.includes(c)) {
        results.push(c);
      }
      findAllChildren(c, selector, results);
    }
  }

  return {
    createElement: createEl,
    getElementById: (id) => elements.find((e) => e.id === id) || null,
    body: createEl("body"),
  };
}

// Translations fixture matching en.json
function mockTr(key, ...args) {
  const table = {
    "studio.library.title": "Sound Libraries",
    "studio.library.on_computer": "On Computer",
    "studio.library.on_velora": "On Velora",
    "studio.library.btn_send_to_velora": "Send to Velora",
    "studio.library.btn_cancel_transfer": "Cancel transfer",
    "studio.library.transfer_progress": "Transferring {0} · {1} of {2} GB",
    "studio.library.err_insufficient_space": "Cannot transfer library: required {0}, but only {1} free on Velora.",
    "studio.library.err_connection_lost": "Transfer stopped: connection lost. Nothing was installed. Start again when connected.",
    "studio.library.ready_notice": "{0} ready in Library",
    "studio.library.delete_btn": "Delete",
  };
  let val = table[key] || key;
  args.forEach((a, i) => {
    val = val.replace(`{${i}}`, a);
  });
  return val;
}

test("formatTransferLine formats required 'Transferring <Name> · <X> of <Y> GB'", () => {
  const line = formatTransferLine("Viola Caipira", 4.2 * 1024 * 1024 * 1024, 18.4 * 1024 * 1024 * 1024);
  assert.equal(line, "Transferring Viola Caipira · 4.2 of 18.4 GB");
});

test("LibraryViewController refuses to start transfer when insufficient free space", async () => {
  let announceMsg = "";
  let errorMsg = "";

  const ctl = new LibraryViewController({
    tr: mockTr,
    onAnnounce: (msg) => { announceMsg = msg; },
    onError: (err) => { errorMsg = err; },
    invoke: async (cmd, args) => {
      if (cmd === "device_check_library_space") {
        return {
          fits: false,
          required_bytes: 5000000000,
          free_bytes: 2000000000,
          reserve_bytes: 268435456,
        };
      }
      throw new Error("unexpected command " + cmd);
    },
  });

  ctl.setPaired(true);

  const library = {
    library_id: "viola-caipira-id",
    name: "Viola Caipira",
    size_bytes: 5000000000,
    file_path: "/tmp/viola.tar",
    category_id: 10,
  };

  await ctl.sendToVelora(library);

  assert.equal(ctl.transferState, null);
  assert.ok(ctl.transferError.includes("Cannot transfer library: required"));
  assert.ok(ctl.transferError.includes("free on Velora"));
  assert.equal(announceMsg, ctl.transferError);
  assert.equal(errorMsg, ctl.transferError);
});

test("Send to Velora button disabled when unpaired and enabled when paired", () => {
  const doc = createMockDoc();
  const host = doc.createElement("div");

  const ctl = new LibraryViewController({
    tr: mockTr,
  });

  ctl.onComputerLibraries = [
    {
      library_id: "test-lib",
      name: "Test Piano",
      category: "category.piano",
      category_id: 1,
      size_bytes: 1000000,
      instrument_count: 1,
    },
  ];

  const view = new LibraryView({
    doc,
    host,
    controller: ctl,
    tr: mockTr,
  });

  // 1. Unpaired
  ctl.setPaired(false);
  view.render();
  const sendBtn1 = host.querySelector(".btn-send-to-velora");
  assert.ok(sendBtn1, "Send to Velora button should exist");
  assert.equal(sendBtn1.disabled, true, "Button must be disabled when unpaired");

  // 2. Paired
  ctl.setPaired(true);
  view.render();
  const sendBtn2 = host.querySelector(".btn-send-to-velora");
  assert.equal(sendBtn2.disabled, false, "Button must be enabled when paired");
});

test("LibraryViewController transfer lifecycle and ready notice", async () => {
  let announceMsg = "";
  let invoked = [];

  const ctl = new LibraryViewController({
    tr: mockTr,
    onAnnounce: (msg) => { announceMsg = msg; },
    invoke: async (cmd, args) => {
      invoked.push({ cmd, args });
      if (cmd === "device_check_library_space") {
        return {
          fits: true,
          required_bytes: 1000000,
          free_bytes: 50000000000,
          reserve_bytes: 268435456,
        };
      }
      if (cmd === "device_start_library_transfer") {
        return {
          transfer_id: "xfer-test-lib",
          completed: true,
          status: "completed",
        };
      }
      return null;
    },
  });

  ctl.setPaired(true);

  const library = {
    library_id: "test-lib",
    name: "Acoustic Grand",
    size_bytes: 1000000,
    file_path: "/tmp/grand.tar",
    category_id: 1,
  };

  await ctl.sendToVelora(library);

  assert.equal(ctl.transferState, null);
  assert.equal(announceMsg, "Acoustic Grand ready in Library");
  assert.equal(invoked.length, 2);
  assert.equal(invoked[0].cmd, "device_check_library_space");
  assert.equal(invoked[1].cmd, "device_start_library_transfer");
});

test("Connection lost mid-transfer displays required exact error message", () => {
  let errorMsg = "";
  let announceMsg = "";

  const ctl = new LibraryViewController({
    tr: mockTr,
    onAnnounce: (msg) => { announceMsg = msg; },
    onError: (err) => { errorMsg = err; },
  });

  ctl.setPaired(true);
  ctl.transferState = {
    transferId: "xfer-1",
    libraryId: "lib-1",
    libraryName: "Nord Lead",
    bytesTransferred: 5000,
    totalBytes: 10000,
    isTransferring: true,
  };

  // Connection dropped!
  ctl.setPaired(false);

  assert.equal(ctl.transferState, null);
  const expectedErr = "Transfer stopped: connection lost. Nothing was installed. Start again when connected.";
  assert.equal(ctl.transferError, expectedErr);
  assert.equal(errorMsg, expectedErr);
  assert.equal(announceMsg, expectedErr);
});

test("Transfer banner renders during transfer and cancel button calls cancelTransfer", async () => {
  const doc = createMockDoc();
  const host = doc.createElement("div");
  let cancelledTid = null;

  const ctl = new LibraryViewController({
    tr: mockTr,
    invoke: async (cmd, args) => {
      if (cmd === "device_cancel_library_transfer") {
        cancelledTid = args.transferId;
        return { ok: true };
      }
      return null;
    },
  });

  ctl.setPaired(true);
  ctl.transferState = {
    transferId: "xfer-abc",
    libraryId: "lib-abc",
    libraryName: "Rhodes Mark I",
    bytesTransferred: 1073741824, // 1 GB
    totalBytes: 2147483648, // 2 GB
    isTransferring: true,
  };

  const view = new LibraryView({
    doc,
    host,
    controller: ctl,
    tr: mockTr,
  });

  view.render();

  const banner = host.querySelector(".transfer-banner");
  assert.ok(banner, "Transfer banner must be present");
  assert.equal(banner.getAttribute("role"), "status");
  assert.equal(banner.getAttribute("aria-live"), "polite");

  const bannerText = host.querySelector(".transfer-banner-text");
  assert.equal(bannerText.textContent, "Transferring Rhodes Mark I · 1.0 of 2.0 GB");

  const cancelBtn = host.querySelector(".btn-cancel-transfer");
  assert.ok(cancelBtn, "Cancel button must exist");

  cancelBtn.click();
  await new Promise(resolve => setTimeout(resolve, 10));

  assert.equal(cancelledTid, "xfer-abc");
  assert.equal(ctl.transferState, null);
});

test("Transfer error banner renders on error and dismiss button clears it", () => {
  const doc = createMockDoc();
  const host = doc.createElement("div");

  const ctl = new LibraryViewController({
    tr: mockTr,
  });

  ctl.transferError = "Transfer stopped: connection lost. Nothing was installed. Start again when connected.";

  const view = new LibraryView({
    doc,
    host,
    controller: ctl,
    tr: mockTr,
  });

  view.render();

  const errBanner = host.querySelector(".transfer-error-banner");
  assert.ok(errBanner, "Error banner must be present");
  assert.equal(errBanner.getAttribute("role"), "alert");

  const dismissBtn = host.querySelector(".btn-dismiss-error");
  assert.ok(dismissBtn, "Dismiss button must exist");

  dismissBtn.click();
  assert.equal(ctl.transferError, null);
});
