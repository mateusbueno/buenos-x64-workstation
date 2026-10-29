import test from "node:test";
import assert from "node:assert/strict";
import { LibraryViewController, CANONICAL_CATEGORIES } from "../src/library_view.js";
import { LibraryView } from "../src/library_view_dom.js";

// Minimal DOM mock for Node environment
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
        const cleanSel = sel.replace(/:not\([^)]*\)/g, ""); // strip :not([disabled]) for mock
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

test("Canonical 17 categories are defined with ids 1..17", () => {
  assert.equal(CANONICAL_CATEGORIES.length, 17);
  assert.equal(CANONICAL_CATEGORIES[0].id, 1);
  assert.equal(CANONICAL_CATEGORIES[0].slug, "piano");
  assert.equal(CANONICAL_CATEGORIES[16].id, 17);
  assert.equal(CANONICAL_CATEGORIES[16].slug, "fx_ambient");
});

test("LibraryViewController: initial state and loading converted libraries", async () => {
  let invokedCmd = null;
  const mockLibs = [
    {
      library_id: "uuid-1",
      bundle_id: "b-1",
      name: "Concert Grand",
      category: "category.piano",
      category_id: 1,
      size_bytes: 104857600, // 100 MB
      instrument_count: 1,
    },
  ];

  const ctl = new LibraryViewController({
    invoke: async (cmd) => {
      invokedCmd = cmd;
      if (cmd === "converter_list_libraries") return mockLibs;
      return [];
    },
  });

  assert.equal(ctl.activeTab, "computer");
  assert.equal(ctl.onComputerLibraries.length, 0);

  await ctl.loadComputerLibraries();
  assert.equal(invokedCmd, "converter_list_libraries");
  assert.equal(ctl.onComputerLibraries.length, 1);
  assert.equal(ctl.onComputerLibraries[0].name, "Concert Grand");
});

test("LibraryViewController: deleteComputerLibrary removes item and announces", async () => {
  let deletedId = null;
  let announced = null;

  const ctl = new LibraryViewController({
    invoke: async (cmd, args) => {
      if (cmd === "converter_delete_library") {
        deletedId = args.libraryId;
        return;
      }
      return [];
    },
    onAnnounce: (msg) => { announced = msg; },
    tr: (k, ...args) => `${k}:${args.join(",")}`,
  });

  ctl.onComputerLibraries = [
    { library_id: "lib-99", name: "Vintage Wurli", category: "category.epiano" },
  ];

  await ctl.deleteComputerLibrary("lib-99");
  assert.equal(deletedId, "lib-99");
  assert.equal(ctl.onComputerLibraries.length, 0);
  assert(announced && announced.includes("Vintage Wurli"));
});

test("LibraryViewController: openConvertDialog suggests categories and supports applyCategoryToAll", async () => {
  const ctl = new LibraryViewController({
    invoke: async (cmd, args) => {
      if (cmd === "converter_suggest_categories") {
        return [
          { name: args.names[0], category: "category.piano", slug: "piano" },
          { name: args.names[1], category: "category.strings", slug: "strings" },
        ];
      }
      return [];
    },
  });

  await ctl.openConvertDialog(["Steinway D.sfz", "Violin Solo.sfz"]);
  assert.equal(ctl.isConvertDialogOpen, true);
  assert.equal(ctl.conversionTasks.length, 2);
  assert.equal(ctl.conversionTasks[0].category, "category.piano");
  assert.equal(ctl.conversionTasks[1].category, "category.strings");

  // User changes task 0 to organ
  ctl.setTaskCategory(0, "category.organ");
  assert.equal(ctl.conversionTasks[0].category, "category.organ");

  // User clicks Apply to all with brass
  ctl.applyCategoryToAll("category.brass");
  assert.equal(ctl.conversionTasks[0].category, "category.brass");
  assert.equal(ctl.conversionTasks[1].category, "category.brass");
});

test("LibraryViewController: startConversion and cancelConversion lifecycle", async () => {
  let startTasks = null;
  let cancelCalled = false;
  let announced = null;

  const ctl = new LibraryViewController({
    invoke: async (cmd, args) => {
      if (cmd === "converter_start_conversion") {
        startTasks = args.tasks;
        return [{ status: "imported" }];
      }
      if (cmd === "converter_cancel_conversion") {
        cancelCalled = true;
        return;
      }
      return [];
    },
    onAnnounce: (msg) => { announced = msg; },
    tr: (k, ...args) => `${k}:${args.join(",")}`,
  });

  ctl.conversionTasks = [
    { path: "/path/to/piano.sfz", name: "piano.sfz", category: "category.piano" },
  ];

  await ctl.startConversion();
  assert(startTasks !== null);
  assert.equal(startTasks[0].category, "piano");
  assert.equal(ctl.isConverting, false);
  assert(announced && announced.includes("studio.converter.completed"));

  // Cancel flow
  ctl.isConverting = true;
  await ctl.cancelConversion();
  assert.equal(cancelCalled, true);
  assert.equal(ctl.isConverting, false);
});

test("LibraryView: renders tabs, empty state, and table with category badges", () => {
  const doc = createMockDoc();
  const host = doc.createElement("div");
  const dialogHost = doc.createElement("div");

  const ctl = new LibraryViewController();
  const view = new LibraryView({
    doc,
    host,
    dialogHost,
    controller: ctl,
    tr: (k, ...args) => k,
  });

  view.show();
  assert.equal(host.hidden, false);

  // Initially empty state
  const emptyState = host.querySelector(".library-empty-state");
  assert(emptyState !== null);

  // Populate computer libraries and re-render
  ctl.onComputerLibraries = [
    {
      library_id: "lib-1",
      bundle_id: "b-1",
      name: "Steinway Grand",
      category: "category.piano",
      category_id: 1,
      size_bytes: 52428800,
      instrument_count: 1,
    },
  ];

  view.render();
  const table = host.querySelector(".library-table");
  assert(table !== null);

  const badge = host.querySelector(".badge-category");
  assert(badge !== null);

  const nameCell = host.querySelector(".library-cell-name");
  assert(nameCell.textContent.includes("Steinway Grand"));

  const deleteBtn = host.querySelector(".btn-danger");
  assert(deleteBtn !== null);
});

test("LibraryView: dialog renders category dropdown with 17 canonical options and progress bar", async () => {
  const doc = createMockDoc();
  const host = doc.createElement("div");
  const dialogHost = doc.createElement("div");

  const ctl = new LibraryViewController({
    invoke: async (cmd, args) => {
      if (cmd === "converter_suggest_categories") {
        return [{ name: args.names[0], category: "category.piano", slug: "piano" }];
      }
      return [];
    },
  });

  const view = new LibraryView({
    doc,
    host,
    dialogHost,
    controller: ctl,
    tr: (k, ...args) => k,
  });

  await ctl.openConvertDialog(["Grand Piano.sfz"]);
  view.renderDialog();

  const dialog = dialogHost.querySelector(".convert-dialog");
  assert(dialog !== null);

  const select = dialogHost.querySelector(".select-category");
  assert(select !== null);
  assert.equal(select.children.length, 17);

  const suggTag = dialogHost.querySelector(".badge-suggested");
  assert(suggTag !== null);

  // Trigger progress
  ctl.onProgressEvent({
    task_index: 1,
    total_tasks: 1,
    current_file: "Grand Piano.sfz",
    status: "converting",
  });
  view.renderDialog();

  const progBar = dialogHost.querySelector(".progress-bar");
  assert(progBar !== null);
  assert.equal(progBar.getAttribute("aria-valuenow"), "100");
});

test("LibraryView: On Velora tab renders device libraries and responds to arrow keys", () => {
  const doc = createMockDoc();
  const host = doc.createElement("div");
  const dialogHost = doc.createElement("div");

  const ctl = new LibraryViewController();
  ctl.setDeviceLibraries([
    {
      library_id: "dev-lib-1",
      name: "Factory Concert Grand",
      category: "category.piano",
      size_bytes: 800000000,
      instrument_count: 1,
    },
  ]);

  const view = new LibraryView({
    doc,
    host,
    dialogHost,
    controller: ctl,
    tr: (k, ...args) => k,
  });

  view.show();

  // Switch to Velora tab via arrow key
  const compTab = host.querySelector("#tab-library-computer");
  compTab.dispatchEvent({ type: "keydown", key: "ArrowRight", preventDefault() {} });

  assert.equal(ctl.activeTab, "velora");
  const veloraTab = host.querySelector("#tab-library-velora");
  assert.equal(veloraTab.getAttribute("aria-selected"), "true");

  const table = host.querySelector(".library-table");
  assert(table !== null);

  const row = host.querySelector(".library-row");
  assert(row !== null);
  assert(row.textContent.includes("Factory Concert Grand"));

  // Device libraries cannot be deleted from computer; action column is "—"
  const deleteBtn = host.querySelector(".btn-danger");
  assert.equal(deleteBtn, null);
});

test("LibraryView: dialog traps Tab / Shift+Tab focus and restores focus to trigger on dismiss", async () => {
  const doc = createMockDoc();
  const host = doc.createElement("div");
  const dialogHost = doc.createElement("div");

  const ctl = new LibraryViewController({
    invoke: async () => [],
  });

  const view = new LibraryView({
    doc,
    host,
    dialogHost,
    controller: ctl,
    tr: (k, ...args) => k,
  });

  view.show();
  const convertBtn = host.querySelector("#btn-open-convert-dialog");
  convertBtn.click(); // Sets triggerEl

  await ctl.openConvertDialog(["MySound.sfz"]);
  view.renderDialog();

  const dialog = dialogHost.querySelector(".convert-dialog");
  assert(dialog !== null);

  // Collect focusables
  const focusables = dialog.querySelectorAll("button, select");
  assert(focusables.length >= 2);
  const first = focusables[0];
  const last = focusables[focusables.length - 1];

  // Shift+Tab on first wraps to last
  doc.activeElement = first;
  let prevented = false;
  dialog.dispatchEvent({
    type: "keydown",
    key: "Tab",
    shiftKey: true,
    preventDefault() { prevented = true; },
  });
  assert.equal(prevented, true);
  assert.equal(last.focused, true);

  // Tab on last wraps to first
  doc.activeElement = last;
  prevented = false;
  first.focused = false;
  dialog.dispatchEvent({
    type: "keydown",
    key: "Tab",
    shiftKey: false,
    preventDefault() { prevented = true; },
  });
  assert.equal(prevented, true);
  assert.equal(first.focused, true);

  // Escape key closes dialog and restores focus to triggerEl
  convertBtn.focused = false;
  dialog.dispatchEvent({
    type: "keydown",
    key: "Escape",
    stopPropagation() {},
  });

  assert.equal(ctl.isConvertDialogOpen, false);
  assert.equal(convertBtn.focused, true);
});

test("LibraryView: dialog renders error banner when conversion fails", async () => {
  const doc = createMockDoc();
  const host = doc.createElement("div");
  const dialogHost = doc.createElement("div");

  const ctl = new LibraryViewController({
    invoke: async (cmd) => {
      if (cmd === "converter_start_conversion") {
        throw new Error("Librarian parsing error: missing sample wave");
      }
      return [];
    },
  });

  const view = new LibraryView({
    doc,
    host,
    dialogHost,
    controller: ctl,
    tr: (k, ...args) => k,
  });

  await ctl.openConvertDialog(["Broken.sfz"]);
  await ctl.startConversion();
  view.renderDialog();

  const errAlert = dialogHost.querySelector(".alert-danger");
  assert(errAlert !== null);
  assert(errAlert.textContent.includes("missing sample wave"));
});

