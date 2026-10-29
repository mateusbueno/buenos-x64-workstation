// Unit and DOM accessibility tests for ExportDialogView (Story 9.18, UX-DR45, UX-DR46, UX-DR71, UX-DR91, UX-DR114).
import test from "node:test";
import assert from "node:assert/strict";

import { CATALOGS } from "../src/catalogs.js";
import { ExportController, EXPORT_SCOPE } from "../src/export_dialog.js";
import { ExportDialogView } from "../src/export_dialog_dom.js";

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

function makeFixtureData() {
  const liveSets = [
    {
      id: "ls-1",
      name: "Gig Opener",
      parts: [
        {
          timbre: {
            library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8", // Factory
            instrument_key: "pianos/concert_grand.sfz",
          },
        },
        {
          timbre: {
            library_id: "user-lib-synth", // User
            instrument_key: "lead/lead.sfz",
          },
        },
      ],
    },
    {
      id: "ls-2",
      name: "Ballad",
      parts: [
        {
          timbre: {
            library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8",
            instrument_key: "pianos/concert_grand.sfz",
          },
        },
      ],
    },
  ];

  const timbres = [
    {
      library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8",
      instrument_key: "pianos/concert_grand.sfz",
      name: "Concert Grand",
      category: "Piano",
      size_bytes: 0,
      is_factory: true,
    },
    {
      library_id: "user-lib-synth",
      instrument_key: "lead/lead.sfz",
      name: "Analog Lead",
      category: "Synth Lead",
      size_bytes: 50 * 1024 * 1024,
      is_factory: false,
    },
  ];

  return { liveSets, timbres };
}

test("ExportDialogView: builds accessible dialog with role, title, and buttons", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const restoreBtn = el(doc, "button");
  const { liveSets, timbres } = makeFixtureData();

  const ctl = new ExportController({ liveSets, timbres });
  const view = new ExportDialogView({ doc, host, tr, controller: ctl, restore: restoreBtn });

  ctl.open({ scope: EXPORT_SCOPE.BUNDLE });
  view.open();

  const modal = host.all((n) => n.tagName === "DIALOG")[0];
  assert.ok(modal);
  assert.equal(modal.getAttribute("role"), "dialog");
  assert.equal(modal.getAttribute("aria-modal"), "true");
  assert.equal(modal.getAttribute("aria-labelledby"), "export-dialog-title");

  const title = host.all((n) => n.id === "export-dialog-title")[0];
  assert.ok(title);
  assert.equal(title.textContent, "Export");

  const confirmBtn = host.all((n) => n.id === "btn-confirm-export")[0];
  const cancelBtn = host.all((n) => n.id === "btn-cancel-export")[0];
  assert.ok(confirmBtn);
  assert.ok(cancelBtn);
  assert.equal(confirmBtn.textContent, "Export");
  assert.equal(cancelBtn.textContent, "Cancel");
});

test("ExportDialogView: renders scope buttons and switching scopes re-renders items", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const { liveSets, timbres } = makeFixtureData();

  const ctl = new ExportController({ liveSets, timbres });
  const view = new ExportDialogView({ doc, host, tr, controller: ctl });

  ctl.open({ scope: EXPORT_SCOPE.BUNDLE });
  view.open();

  const scopeBtns = host.all((n) => n.className && n.className.includes("scope-button"));
  assert.equal(scopeBtns.length, 3);

  // Default is bundle active
  const bundleBtn = scopeBtns.find((b) => b.dataset.scope === "bundle");
  assert.ok(bundleBtn.className.includes("active"));

  // Click Sets scope button
  const setsBtn = scopeBtns.find((b) => b.dataset.scope === "sets");
  setsBtn.fire("click");
  assert.equal(ctl.scope, "sets");

  const rows = host.all((n) => n.className === "export-row");
  // Sets scope shows only the 2 Live Sets
  assert.equal(rows.length, 2);
});

test("ExportDialogView: Bundle scope shows locked factory timbre reference at 0 B", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const { liveSets, timbres } = makeFixtureData();

  const ctl = new ExportController({ liveSets, timbres });
  const view = new ExportDialogView({ doc, host, tr, controller: ctl });

  ctl.open({ scope: EXPORT_SCOPE.BUNDLE, preselectLiveSetId: "ls-1" });
  view.open();

  const rows = host.all((n) => n.className === "export-row");
  // 2 Live Sets (ls-1 checked, ls-2 unchecked) + 2 locked timbres (1 factory + 1 user)
  assert.equal(rows.length, 4);

  const factoryRow = rows.find((r) => r.textContent.includes("Reference only"));
  assert.ok(factoryRow, "Factory timbre row must carry 'Reference only' badge");
  assert.ok(factoryRow.textContent.includes("0 B"), "Factory timbre must display 0 B");

  const factoryCb = factoryRow.all((n) => n.type === "checkbox")[0];
  assert.equal(factoryCb.disabled, true, "Locked dependency checkbox must be disabled");
  assert.equal(factoryCb.checked, true, "Locked dependency checkbox must be checked");
});

test("ExportDialogView: Space key toggles selection on focused row", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const { liveSets, timbres } = makeFixtureData();

  const ctl = new ExportController({ liveSets, timbres });
  const view = new ExportDialogView({ doc, host, tr, controller: ctl });

  ctl.open({ scope: EXPORT_SCOPE.SETS });
  view.open();

  const rows = host.all((n) => n.className === "export-row");
  assert.equal(ctl.selectedLiveSetIds.has("ls-1"), true);

  // Press Space on row 0 (ls-1)
  rows[0].fire("keydown", { key: " " });
  assert.equal(ctl.selectedLiveSetIds.has("ls-1"), false);

  // Press Space again to toggle back
  rows[0].fire("keydown", { key: " " });
  assert.equal(ctl.selectedLiveSetIds.has("ls-1"), true);
});

test("ExportDialogView: live size estimate and 2 GB warning line appear", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const { liveSets } = makeFixtureData();
  const bigTimbres = [
    {
      library_id: "user-huge",
      instrument_key: "huge.sfz",
      name: "Huge Instrument",
      category: "Keys",
      size_bytes: 3 * 1024 * 1024 * 1024, // 3 GB
      is_factory: false,
    },
  ];
  const bigSets = [
    {
      id: "ls-big",
      name: "Big Set",
      parts: [{ timbre: { library_id: "user-huge", instrument_key: "huge.sfz" } }],
    },
  ];

  const ctl = new ExportController({ liveSets: bigSets, timbres: bigTimbres });
  const view = new ExportDialogView({ doc, host, tr, controller: ctl });

  ctl.open({ scope: EXPORT_SCOPE.BUNDLE });
  view.open();

  const estimateLine = host.all((n) => n.id === "export-estimate-summary")[0];
  assert.ok(estimateLine);
  assert.ok(estimateLine.textContent.includes("About 3.0 GB · 1 Live Sets, 1 timbres (0 by reference)"));

  const warn2gb = host.all((n) => n.className && n.className.includes("warn-2gb"))[0];
  assert.ok(warn2gb);
  assert.equal(warn2gb.hidden, false, "2 GB warning line must be visible");
  assert.ok(warn2gb.textContent.includes("3.0 GB"));
});

test("ExportDialogView: insufficient free space shows error warning and disables Export", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const { liveSets, timbres } = makeFixtureData();

  const ctl = new ExportController({
    liveSets,
    timbres,
    freeSpaceBytes: 1024, // only 1 KB free!
  });
  const view = new ExportDialogView({ doc, host, tr, controller: ctl });

  ctl.open({ scope: EXPORT_SCOPE.BUNDLE }); // needs 50 MB
  view.open();

  const warnSpace = host.all((n) => n.className && n.className.includes("warn-space"))[0];
  assert.ok(warnSpace);
  assert.equal(warnSpace.hidden, false, "Free space warning must be visible");
  assert.ok(warnSpace.textContent.includes("Not enough free space"));

  const exportBtn = host.all((n) => n.id === "btn-confirm-export")[0];
  assert.equal(exportBtn.disabled, true, "Export button must be disabled when free space is insufficient");
});

test("ExportDialogView: Esc key, Cancel button, and backdrop close dialog and restore focus", () => {
  const doc = makeDoc();
  const host = el(doc, "div");
  const restoreBtn = el(doc, "button");
  const { liveSets, timbres } = makeFixtureData();

  const ctl = new ExportController({ liveSets, timbres });
  const view = new ExportDialogView({ doc, host, tr, controller: ctl, restore: restoreBtn });

  ctl.open({ scope: EXPORT_SCOPE.SETS });
  view.open();

  const modal = host.all((n) => n.tagName === "DIALOG")[0];
  modal.fire("keydown", { key: "Escape" });
  assert.equal(ctl.isOpen, false);
  assert.equal(doc.activeElement, restoreBtn);
});
