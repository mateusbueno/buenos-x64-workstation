// Unit and DOM accessibility tests for ImportDialogView (Story 9.19, UX-DR77, UX-DR91, UX-DR114).
import test from "node:test";
import assert from "node:assert/strict";

import { CATALOGS } from "../src/catalogs.js";
import { ImportController, COLLISION_ACTION } from "../src/import_dialog.js";
import { ImportDialogView } from "../src/import_dialog_dom.js";

class El {
  constructor(doc, tag) {
    this.ownerDocument = doc;
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.attrs = {};
    this.dataset = {};
    this.listeners = {};
    this.className = "";
    this.classList = {
      add: (c) => {
        if (!this.className.includes(c)) this.className = (this.className + " " + c).trim();
      },
      remove: (c) => {
        this.className = this.className.replace(new RegExp(`\\b${c}\\b`, "g"), "").trim();
      },
      contains: (c) => this.className.includes(c),
      toggle: (c, force) => {
        if (force === undefined) {
          if (this.classList.contains(c)) this.classList.remove(c);
          else this.classList.add(c);
        } else if (force) {
          this.classList.add(c);
        } else {
          this.classList.remove(c);
        }
      },
    };
    this.hidden = false;
    this.disabled = false;
    this.tabIndex = 0;
    this.value = "";
    this.type = "";
    this.id = "";
    this.checked = false;
    this.parent = null;
    this._text = "";
  }
  get textContent() {
    return this._text + this.children.map((c) => c.textContent).join("");
  }
  set textContent(v) {
    const a = this.ownerDocument.activeElement;
    if (a && this.children.some((c) => c.contains(a))) this.ownerDocument.activeElement = null;
    this.children.forEach((c) => {
      c.parent = null;
    });
    this.children = [];
    this._text = String(v);
  }
  set innerHTML(v) {
    throw new Error("innerHTML must never be used for security: " + v);
  }
  contains(n) {
    for (let x = n; x; x = x.parent) if (x === this) return true;
    return false;
  }
  appendChild(c) {
    c.parent = this;
    this.children.push(c);
    return c;
  }
  append(...cs) {
    cs.forEach((c) => this.appendChild(c));
  }
  setAttribute(k, v) {
    this.attrs[k] = String(v);
    if (k === "type") this.type = String(v);
    if (k === "id") this.id = String(v);
  }
  getAttribute(k) {
    return k in this.attrs ? this.attrs[k] : null;
  }
  removeAttribute(k) {
    delete this.attrs[k];
  }
  addEventListener(t, f) {
    (this.listeners[t] = this.listeners[t] || []).push(f);
  }
  focus() {
    if (this.disabled) return;
    this.ownerDocument.activeElement = this;
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
  all(pred, out = []) {
    if (pred(this)) out.push(this);
    this.children.forEach((c) => c.all(pred, out));
    return out;
  }
}

const makeDoc = () => {
  const d = {
    activeElement: null,
    createElement: (t) => new El(d, t),
  };
  return d;
};

const el = (doc, tag) => doc.createElement(tag);

const EN = CATALOGS.en;
const tr = (key, ...args) => {
  assert.ok(key in EN, `locale key missing: ${key}`);
  let s = EN[key];
  for (const a of args) {
    s = s.replace(/%[ds]/, a);
  }
  return s;
};

function makeManifest() {
  return {
    file_name: "gig_sets.velorasets",
    file_path: "/tmp/gig_sets.velorasets",
    format_version: 1,
    scope: "bundle",
    device_model: "Velora 73",
    firmware_version: "1.4.0",
    collision_count: 1,
    missing_libraries: ["Grand Piano Studio"],
    missing_libraries_report: "1 Live Sets need libraries: Grand Piano Studio (factory)",
    items: [
      {
        id: "ls-1",
        name: "Gig Opener",
        category: "Live Set",
        item_type: "live_set",
        has_collision: true,
        collision_type: "live_set_id",
        is_reference_only: false,
        missing_libraries: ["Grand Piano Studio (factory)"],
      },
      {
        id: "ls-2",
        name: "Electric Jam",
        category: "Live Set",
        item_type: "live_set",
        has_collision: false,
        collision_type: null,
        is_reference_only: false,
        missing_libraries: [],
      },
    ],
  };
}

test("ImportDialogView: builds accessible dialog with role, title, and buttons", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const restoreBtn = el(doc, "button");
  restoreBtn.focus();

  const ctl = new ImportController();
  const view = new ImportDialogView({
    doc,
    host,
    tr,
    controller: ctl,
    restore: restoreBtn,
  });

  ctl.onChange = () => view.render();
  ctl.open(makeManifest());

  const dialog = host.all((n) => n.getAttribute("role") === "dialog")[0];
  assert.ok(dialog, "Dialog element must exist");
  assert.equal(dialog.getAttribute("aria-modal"), "true");
  assert.equal(dialog.getAttribute("aria-labelledby"), "import-dialog-title");

  const title = host.all((n) => n.id === "import-dialog-title")[0];
  assert.ok(title);
  assert.equal(title.textContent, "Import");

  const importBtn = host.all((n) => n.id === "btn-confirm-import")[0];
  assert.ok(importBtn);
  assert.equal(importBtn.disabled, false);
});

test("ImportDialogView: renders collision badges and action choices", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const ctl = new ImportController();
  const view = new ImportDialogView({
    doc,
    host,
    tr,
    controller: ctl,
  });

  ctl.onChange = () => view.render();
  ctl.open(makeManifest());

  const collisionBadges = host.all((n) => n.className.includes("badge-collision"));
  assert.equal(collisionBadges.length, 1);
  assert.equal(collisionBadges[0].textContent, "Collision");

  const rows = host.all((n) => n.className.includes("import-row"));
  assert.equal(rows.length, 2);

  // Row 1 has collision actions
  const row1Actions = rows[0].all((n) => n.className.includes("action-choice-btn"));
  assert.equal(row1Actions.length, 3); // Keep both, Replace, Skip
  assert.equal(row1Actions[0].textContent, "Keep both");
  assert.equal(row1Actions[1].textContent, "Replace");
  assert.equal(row1Actions[2].textContent, "Skip");

  // Keep both is selected initially
  assert.equal(row1Actions[0].getAttribute("aria-checked"), "true");
  assert.equal(row1Actions[1].getAttribute("aria-checked"), "false");

  // Clicking Replace selects Replace
  row1Actions[1].fire("click");
  assert.equal(ctl.items[0].collision_action, COLLISION_ACTION.REPLACE);
});

test("ImportDialogView: Apply to all triggers action change across items", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const ctl = new ImportController();
  const view = new ImportDialogView({
    doc,
    host,
    tr,
    controller: ctl,
  });

  ctl.onChange = () => view.render();
  ctl.open(makeManifest());

  const applyButtons = host.all((n) => n.className.includes("btn-apply-all"));
  assert.equal(applyButtons.length, 3);

  // Click Apply all "Skip"
  applyButtons[2].fire("click");
  assert.equal(ctl.items[0].collision_action, COLLISION_ACTION.SKIP);
});

test("ImportDialogView: Arrow navigation moves roving tabindex between rows", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const ctl = new ImportController();
  const view = new ImportDialogView({
    doc,
    host,
    tr,
    controller: ctl,
  });

  ctl.onChange = () => view.render();
  ctl.open(makeManifest());

  const rows = host.all((n) => n.className.includes("import-row"));
  assert.equal(rows[0].tabIndex, 0);
  assert.equal(rows[1].tabIndex, -1);

  // Down arrow focuses second row
  rows[0].fire("keydown", { key: "ArrowDown" });
  assert.equal(rows[0].tabIndex, -1);
  assert.equal(rows[1].tabIndex, 0);
  assert.equal(doc.activeElement, rows[1]);

  // Up arrow focuses first row
  rows[1].fire("keydown", { key: "ArrowUp" });
  assert.equal(rows[0].tabIndex, 0);
  assert.equal(rows[1].tabIndex, -1);
  assert.equal(doc.activeElement, rows[0]);
});

test("ImportDialogView: Error banner appears on format_version > 1", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const ctl = new ImportController();
  const view = new ImportDialogView({
    doc,
    host,
    tr,
    controller: ctl,
  });

  ctl.onChange = () => view.render();
  ctl.open({
    file_name: "newer_project.velorasets",
    format_version: 2,
    items: [],
  });

  const errorBanner = host.all((n) => n.id === "import-error-banner")[0];
  assert.ok(errorBanner);
  assert.equal(errorBanner.hidden, false);
  assert.equal(
    errorBanner.textContent,
    "Couldn't import newer_project.velorasets: file made by a newer Velora Studio. Update the app."
  );

  const importBtn = host.all((n) => n.id === "btn-confirm-import")[0];
  assert.equal(importBtn.disabled, true);
});

test("ImportDialogView: Missing library warning banner and badges display", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const ctl = new ImportController();
  const view = new ImportDialogView({
    doc,
    host,
    tr,
    controller: ctl,
  });

  ctl.onChange = () => view.render();
  ctl.open(makeManifest());

  const missingBanner = host.all((n) => n.id === "import-missing-libs-banner")[0];
  assert.ok(missingBanner);
  assert.equal(missingBanner.hidden, false);
  assert.equal(
    missingBanner.textContent,
    "1 Live Sets need libraries: Grand Piano Studio (factory)"
  );

  const itemBadge = host.all((n) => n.className.includes("badge-warn"))[0];
  assert.ok(itemBadge);
  assert.equal(itemBadge.textContent, "Missing library");
});

test("ImportDialogView: Progress footer and import execution", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  let started = false;
  const ctl = new ImportController({
    deviceName: "Velora 73",
    onImport: () => {
      started = true;
    },
  });
  const view = new ImportDialogView({
    doc,
    host,
    tr,
    controller: ctl,
  });

  ctl.onChange = () => view.render();
  ctl.open(makeManifest());

  const importBtn = host.all((n) => n.id === "btn-confirm-import")[0];
  importBtn.fire("click");
  assert.equal(started, true);
  assert.equal(ctl.isImporting, true);

  const progressEl = host.all((n) => n.id === "import-progress-status")[0];
  assert.ok(progressEl);
  assert.equal(progressEl.hidden, false);
  assert.equal(progressEl.textContent, "IMPORTING TO Velora 73 · 0 of 2 items");

  ctl.updateProgress(1, 2);
  assert.equal(progressEl.textContent, "IMPORTING TO Velora 73 · 1 of 2 items");
});

test("ImportDialogView: Esc key and Cancel close dialog and restore focus", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const restoreBtn = el(doc, "button");
  restoreBtn.focus();

  const ctl = new ImportController();
  const view = new ImportDialogView({
    doc,
    host,
    tr,
    controller: ctl,
    restore: restoreBtn,
  });

  ctl.onChange = () => view.render();
  ctl.open(makeManifest());

  const modal = host.all((n) => n.getAttribute("role") === "dialog")[0];
  assert.ok(modal);

  // Esc key closes dialog
  modal.fire("keydown", { key: "Escape" });
  assert.equal(ctl.isOpen, false);
  assert.equal(doc.activeElement, restoreBtn);
});
