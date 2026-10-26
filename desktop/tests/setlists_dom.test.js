// Story 9.13: the DOM side of the Setlists view against a minimal DOM shim (no jsdom here): rendering, textContent-only hostile names, roving tabindex, Alt+Up / Alt+Down,
// "Move to…", drag and drop with indicators, dialogs, tags, live-region announcements, disabled state.
import test from "node:test";
import assert from "node:assert/strict";

import { SetlistsView } from "../src/setlists_dom.js";
import { SetlistsController } from "../src/setlists_view.js";
import { CATALOGS } from "../src/catalogs.js";
import { sample } from "./support/fake_engine.js";

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
const settle = () => new Promise((r) => setTimeout(r, 15));

async function setup(engine = sample()) {
  const doc = makeDoc();
  const host = doc.createElement("section");
  const announced = [];
  const ctl = new SetlistsController({
    request: (m) => engine.request(m),
    onChange: () => view.render(),
    onError: (e) => view.showError(e),
    onAnnounce: (a) => announced.push(tr(a.key, ...a.args)),
    onNotice: (n) => view.showNotice(n),
    missingLabel: () => tr("studio.sidebar.entry_missing"),
    refreshMinMs: 0,
  });
  const view = new SetlistsView({ doc, host, tr, controller: ctl, announce: (t) => announced.push(t), onClose: () => { view.closed = true; } });
  view.show();
  await ctl.refreshNow();
  const rows = (list) => list.children.filter((r) => r.dataset.key !== undefined);
  return { doc, host, engine, ctl, view, announced, rowsA: () => rows(view.listA), rowsB: () => rows(view.listB) };
}
const key = (el, k, extra = {}) => el.fire("keydown", { key: k, ...extra });
const cmds = (engine) => engine.log.filter(([n]) => n !== "GetSetlists");

test("renders setlists with name, tags, counts, missing and loaded marker, and the selected setlist's Live Sets in order", async () => {
  const eng = sample();
  eng.loaded = "s-one";
  const { view, rowsA, rowsB } = await setup(eng);
  assert.equal(rowsA().length, 3);
  assert.deepEqual(rowsA().map((r) => r.dataset.key), ["s-one", "s-two", "s-three"]);
  const first = rowsA()[0];
  assert.equal(first.getAttribute("aria-selected"), "true");
  assert.match(first.textContent, /Friday/);
  assert.match(first.textContent, /Gig/);
  assert.match(first.textContent, /3 Live Sets/);
  assert.match(first.textContent, /LOADED/);
  assert.match(first.getAttribute("aria-label"), /loaded on the Velora/);
  assert.match(rowsA()[2].textContent, /1 missing/);
  assert.deepEqual(rowsB().map((r) => r.dataset.key), ["aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "cccccccc-cccc-4ccc-8ccc-cccccccccccc"]);
  assert.match(rowsB()[1].getAttribute("aria-label"), /Bravo, position 2 of 3/);
  assert.equal(view.listA.getAttribute("role"), "listbox");
  assert.ok(view.listA.getAttribute("aria-label").length > 0);
  // drag handles have accessible names
  const handles = first.all((n) => n.attrs.role === "img");
  assert.equal(handles.length, 1);
  assert.match(handles[0].getAttribute("aria-label"), /Drag to reorder Friday/);
});

test("hostile names are rendered with textContent only (never as markup) and a missing Live Set is flagged", async () => {
  const eng = sample();
  eng.setlists[0].name = "<img src=x onerror=alert(1)>";
  eng.liveSets[0].name = "<script>boom()</script>";
  eng.tags[0].name = "<b>tag</b>";
  const { rowsA, rowsB, ctl } = await setup(eng);
  assert.equal(rowsA()[0].children.find((c) => c.className.includes("sv-name")).textContent, "<img src=x onerror=alert(1)>");
  assert.match(rowsB()[0].textContent, /<script>boom\(\)<\/script>/);
  assert.match(rowsA()[0].textContent, /<b>tag<\/b>/);
  await ctl.select("s-three");
  assert.match(ctl.entries[0].name, /^$/);
  assert.match(rowsB()[0].textContent, /missing/);
  assert.match(rowsB()[0].className, /missing/);
});

test("roving tabindex: exactly one row of each list is a tab stop; arrows move the focus, Home / End jump, Enter selects", async () => {
  const { view, rowsA, ctl } = await setup();
  assert.equal(rowsA().filter((r) => r.tabIndex === 0).length, 1);
  rowsA()[0].focus();
  key(rowsA()[0], "ArrowDown");
  assert.equal(view.doc.activeElement, rowsA()[1]);
  assert.deepEqual(rowsA().map((r) => r.tabIndex), [-1, 0, -1]);
  key(rowsA()[1], "End");
  assert.equal(view.doc.activeElement, rowsA()[2]);
  key(rowsA()[2], "Home");
  assert.equal(view.doc.activeElement, rowsA()[0]);
  key(rowsA()[0], "ArrowDown");
  const e = key(rowsA()[1], "Enter");
  assert.equal(e.prevented, true);
  await settle();
  assert.equal(ctl.selectedId, "s-two");
  assert.match(view.headingB.textContent, /LIVE SETS IN Saturday/);
});

test("Alt+ArrowDown / Alt+ArrowUp move a setlist: full permutation sent, focus stays on it, announced", async () => {
  const { engine, rowsA, view, announced } = await setup();
  rowsA()[0].focus();
  const e = key(rowsA()[0], "ArrowDown", { altKey: true });
  assert.equal(e.prevented, true);
  await settle();
  assert.deepEqual(cmds(engine)[0], ["ReorderSetlists", { ids: ["s-two", "s-one", "s-three"] }]);
  assert.deepEqual(rowsA().map((r) => r.dataset.key), ["s-two", "s-one", "s-three"]);
  assert.equal(view.doc.activeElement.dataset.key, "s-one");
  assert.ok(announced.includes("Moved setlist Friday to position 2 of 3"), announced.join("|"));
  key(view.doc.activeElement, "ArrowUp", { altKey: true });
  await settle();
  assert.deepEqual(rowsA().map((r) => r.dataset.key), ["s-one", "s-two", "s-three"]);
  // at the edge: nothing sent, announced
  const n = engine.log.length;
  key(view.doc.activeElement, "ArrowUp", { altKey: true });
  await settle();
  assert.equal(engine.log.length, n);
  assert.equal(announced.at(-1), "Already first");
});

test("Alt+ArrowDown on a Live Set: ReorderLiveSetsInSetlist with the full list, announcement 'Moved Song X to position 3 of 12' form", async () => {
  const { engine, rowsB, view, announced } = await setup();
  rowsB()[1].focus();
  key(rowsB()[1], "ArrowDown", { altKey: true });
  await settle();
  assert.deepEqual(cmds(engine)[0], ["ReorderLiveSetsInSetlist", { setlist_id: "s-one", live_set_ids: ["aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"] }]);
  assert.ok(announced.includes("Moved Bravo to position 3 of 3"), announced.join("|"));
  assert.equal(view.doc.activeElement.dataset.key, "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
});

test("an engine error rolls the optimistic move back and shows the typed error inline", async () => {
  const { engine, rowsB, view } = await setup();
  engine.fail = () => 31;
  rowsB()[0].focus();
  key(rowsB()[0], "ArrowDown", { altKey: true });
  await settle();
  assert.deepEqual(rowsB().map((r) => r.dataset.key), ["aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "cccccccc-cccc-4ccc-8ccc-cccccccccccc"]);
  assert.equal(view.errorEl.hidden, false);
  assert.match(view.errorEl.textContent, /no longer valid/);
  assert.equal(view.errorEl.getAttribute("role"), "alert");
});

test("Move to... menu: labelled menu, arrows / Enter / Escape; moves to a position and to another setlist; focus returns", async () => {
  const { engine, rowsB, view, doc, announced } = await setup();
  rowsB()[0].focus();
  key(rowsB()[0], "KeyM", { altKey: true, code: "KeyM" });
  const menu = view.dialogHost.all((n) => n.attrs.role === "menu")[0];
  assert.ok(menu, "menu opened");
  assert.match(menu.getAttribute("aria-label"), /Move Alpha to/);
  const items = menu.all((n) => n.attrs.role === "menuitem");
  assert.match(items[0].textContent, /Position 2/);
  assert.equal(doc.activeElement, items[0]);
  key(items[0], "ArrowDown");
  assert.equal(doc.activeElement, items[1]);
  key(items[1], "ArrowUp");
  key(items[0], "Escape");
  assert.equal(view.dialogHost.children.length, 0);
  assert.equal(doc.activeElement, rowsB()[0], "focus returns to the row");
  // pick "Position 3"
  key(rowsB()[0], "ContextMenu", { key: "ContextMenu" });
  const m2 = view.dialogHost.all((n) => n.attrs.role === "menu")[0];
  const it2 = m2.all((n) => n.attrs.role === "menuitem");
  key(it2[0], "ArrowDown");
  key(doc.activeElement, "Enter");
  await settle();
  assert.deepEqual(cmds(engine)[0][0], "ReorderLiveSetsInSetlist");
  assert.deepEqual(cmds(engine)[0][1].live_set_ids, ["bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"]);
  assert.ok(announced.includes("Moved Alpha to position 3 of 3"));
  // to another setlist, at the end
  rowsB()[0].focus();
  key(rowsB()[0], "KeyM", { altKey: true, code: "KeyM" });
  const m3 = view.dialogHost.all((n) => n.attrs.role === "menu")[0];
  const it3 = m3.all((n) => n.attrs.role === "menuitem");
  const end = it3.find((i) => /Saturday: at the end/.test(i.textContent));
  assert.ok(end && it3.some((i) => /Saturday: at the start/.test(i.textContent)));
  end.fire("click");
  await settle();
  const c = cmds(engine).slice(1).map(([n]) => n);
  assert.deepEqual(c, ["AddLiveSetToSetlist", "RemoveLiveSetFromSetlist"]);
  assert.deepEqual(engine.setlists[1].ids, ["dddddddd-dddd-4ddd-8ddd-dddddddddddd", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"]);
  assert.ok(announced.includes("Moved Bravo to Saturday"));
});

test("Add Live Set... offers only the Live Sets not in the setlist, and adds the chosen one", async () => {
  const { engine, view } = await setup();
  view.btnAdd.fire("click");
  const menu = view.dialogHost.all((n) => n.attrs.role === "menu")[0];
  const labels = menu.all((n) => n.attrs.role === "menuitem").map((n) => n.textContent);
  assert.deepEqual(labels, ["Delta", "Echo"]);
  menu.all((n) => n.attrs.role === "menuitem")[1].fire("click");
  await settle();
  assert.deepEqual(cmds(engine)[0], ["AddLiveSetToSetlist", { setlist_id: "s-one", live_set_id: "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee" }]);
});

function dragOver(list, row, clientY) {
  row._rect = { top: 100, height: 20 };
  return row.fire("dragover", { clientY, dataTransfer: {} }); // fired on the row, it bubbles to the list as in a browser
}

test("drag and drop of Live Sets: dragover shows a drop indicator before / after the row, drop sends the full order, dragend clears", async () => {
  const { engine, rowsB, view } = await setup();
  const [a, b, c] = rowsB();
  a.fire("dragstart", { dataTransfer: { setData() {}, effectAllowed: "" } });
  assert.match(a.className, /dragging/);
  const over = dragOver(view.listB, c, 118); // lower half of the last row: after it
  assert.equal(over.prevented, true);
  assert.match(c.className, /drop-after/);
  assert.ok(!/drop-before/.test(c.className));
  dragOver(view.listB, b, 101); // upper half of the middle row: before it
  assert.match(b.className, /drop-before/);
  assert.ok(!/drop-after/.test(c.className), "the previous indicator is gone");
  dragOver(view.listB, c, 118);
  c.fire("drop", { clientY: 118 });
  await settle();
  assert.deepEqual(cmds(engine)[0], ["ReorderLiveSetsInSetlist", { setlist_id: "s-one", live_set_ids: ["bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"] }]);
  assert.deepEqual(rowsB().map((r) => r.dataset.key), ["bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"]);
  assert.ok(rowsB().every((r) => !/drop-|dragging/.test(r.className)));
});

test("drag and drop of setlists, and dropping a Live Set on another setlist moves it there; a drop on itself sends nothing", async () => {
  const { engine, rowsA, rowsB, view } = await setup();
  const [s1, s2, s3] = rowsA();
  s3.fire("dragstart", { dataTransfer: {} });
  dragOver(view.listA, s1, 101);
  s1.fire("drop", { clientY: 101 });
  await settle();
  assert.deepEqual(cmds(engine)[0], ["ReorderSetlists", { ids: ["s-three", "s-one", "s-two"] }]);
  // Live Set onto a setlist row
  rowsB()[0].fire("dragstart", { dataTransfer: {} });
  const target = rowsA().find((r) => r.dataset.key === "s-two");
  const o = dragOver(view.listA, target, 110);
  assert.equal(o.prevented, true);
  assert.match(target.className, /drop-into/);
  view.listA.fire("drop", { target, clientY: 110 });
  await settle();
  const names = cmds(engine).map(([n]) => n);
  assert.deepEqual(names.slice(1), ["AddLiveSetToSetlist", "RemoveLiveSetFromSetlist"]);
  assert.deepEqual(engine.setlists.find((s) => s.id === "s-two").ids, ["dddddddd-dddd-4ddd-8ddd-dddddddddddd", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"]);
  // dropping onto the gap right after itself: no command
  const n = engine.log.length;
  const row = rowsB()[0];
  row.fire("dragstart", { dataTransfer: {} });
  dragOver(view.listB, row, 118);
  row.fire("drop", { clientY: 118 });
  await settle();
  assert.equal(engine.log.length, n);
  // a drop of a Live Set on its own setlist row is refused (no indicator)
  rowsB()[0].fire("dragstart", { dataTransfer: {} });
  const own = rowsA().find((r) => r.dataset.key === "s-one");
  const o2 = dragOver(view.listA, own, 110);
  assert.ok(!o2.prevented);
  rowsB()[0].fire("dragend", {});
});

test("a render while a drag is in progress is deferred (the dragged row is never rebuilt under the pointer)", async () => {
  const { rowsB, view, ctl } = await setup();
  const a = rowsB()[0];
  a.fire("dragstart", { dataTransfer: {} });
  ctl.entries = ctl.entries.slice().reverse();
  view.render();
  assert.equal(rowsB()[0], a, "same element: nothing rebuilt");
  a.fire("dragend", {});
  assert.notEqual(rowsB()[0], a);
});

test("New setlist dialog: name validated client-side (inline, nothing sent), created, selected, dialog closed, focus restored", async () => {
  const { engine, view, doc } = await setup();
  view.btnNewSetlist.fire("click");
  const dlg = view.dialogHost.all((n) => n.attrs.role === "dialog")[0];
  assert.ok(dlg);
  assert.equal(dlg.getAttribute("aria-modal"), "true");
  const input = dlg.all((n) => n.tagName === "INPUT")[0];
  assert.equal(doc.activeElement, input);
  input.value = "   ";
  key(input, "Enter");
  await settle();
  assert.equal(cmds(engine).length, 0);
  assert.match(dlg.all((n) => n.attrs.role === "alert")[0].textContent, /Enter a name/);
  input.value = "x".repeat(65);
  key(input, "Enter");
  await settle();
  assert.match(dlg.all((n) => n.attrs.role === "alert")[0].textContent, /64 bytes/);
  input.value = " Encore ";
  key(input, "Enter");
  await settle();
  assert.deepEqual(cmds(engine)[0], ["CreateSetlist", { name: "Encore" }]);
  assert.equal(view.dialogHost.children.length, 0);
  assert.equal(doc.activeElement, view.btnNewSetlist);
  assert.equal(view.ctl.selected.name, "Encore");
});

test("a dialog traps Tab and closes on Escape", async () => {
  const { view, doc } = await setup();
  view.btnNewSetlist.fire("click");
  const dlg = view.dialogHost.all((n) => n.attrs.role === "dialog")[0];
  const f = dlg.all((n) => n.tagName === "BUTTON" || n.tagName === "INPUT");
  f[f.length - 1].focus();
  key(f[f.length - 1], "Tab");
  assert.equal(doc.activeElement, f[0]);
  key(f[0], "Tab", { shiftKey: true });
  assert.equal(doc.activeElement, f[f.length - 1]);
  key(f[0], "Escape");
  assert.equal(view.dialogHost.children.length, 0);
});

test("Rename setlist keeps focus flow and sends RenameSetlist; an engine error stays inside the dialog", async () => {
  const { engine, view } = await setup();
  view.btnRename.fire("click");
  let dlg = view.dialogHost.all((n) => n.attrs.role === "dialog")[0];
  let input = dlg.all((n) => n.tagName === "INPUT")[0];
  assert.equal(input.value, "Friday");
  engine.fail = () => 25;
  input.value = "Fri";
  key(input, "Enter");
  await settle();
  assert.match(dlg.all((n) => n.attrs.role === "alert")[0].textContent, /refused this name/);
  assert.equal(view.dialogHost.children.length, 1);
  engine.fail = null;
  key(input, "Enter");
  await settle();
  assert.equal(engine.setlists[0].name, "Fri");
  assert.equal(view.dialogHost.children.length, 0);
});

test("Delete setlist and Remove from setlist ask for confirmation (Cancel has the focus); neither deletes a Live Set file", async () => {
  const { engine, view, doc, rowsB } = await setup();
  view.btnDelete.fire("click");
  let dlg = view.dialogHost.all((n) => n.attrs.role === "dialog")[0];
  assert.match(dlg.textContent, /Its Live Sets are not deleted/);
  const btns = dlg.all((n) => n.tagName === "BUTTON");
  assert.equal(doc.activeElement, btns[0]);
  assert.equal(btns[0].textContent, "Cancel");
  btns[0].fire("click");
  assert.equal(cmds(engine).length, 0);
  // Remove via the Delete key on an entry
  rowsB()[1].focus();
  key(rowsB()[1], "Delete");
  dlg = view.dialogHost.all((n) => n.attrs.role === "dialog")[0];
  assert.match(dlg.textContent, /Remove Bravo from Friday/);
  dlg.all((n) => n.tagName === "BUTTON")[1].fire("click");
  await settle();
  assert.deepEqual(cmds(engine)[0], ["RemoveLiveSetFromSetlist", { setlist_id: "s-one", live_set_id: "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb" }]);
  assert.ok(engine.liveSets.some((l) => l.id === "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"));
  // delete the setlist for real
  view.btnDelete.fire("click");
  dlg = view.dialogHost.all((n) => n.attrs.role === "dialog")[0];
  dlg.all((n) => n.tagName === "BUTTON")[1].fire("click");
  await settle();
  assert.equal(cmds(engine).at(-1)[0], "DeleteSetlist");
  assert.ok(!engine.setlists.some((s) => s.id === "s-one"));
  assert.equal(engine.liveSets.length, 5);
});

test("New Live Set dialog: name + 'add to the selected setlist' checkbox", async () => {
  const { engine, view } = await setup();
  view.btnNewLiveSet.fire("click");
  const dlg = view.dialogHost.all((n) => n.attrs.role === "dialog")[0];
  const [input, check] = dlg.all((n) => n.tagName === "INPUT");
  assert.equal(check.checked, true);
  assert.match(dlg.textContent, /Add it to Friday/);
  input.value = "Foxtrot";
  key(input, "Enter");
  await settle();
  assert.deepEqual(cmds(engine).map(([n]) => n), ["CreateLiveSet", "AddLiveSetToSetlist"]);
  assert.deepEqual(engine.setlists[0].ids.at(-1), engine.liveSets.at(-1).id);
  assert.match(engine.liveSets.at(-1).id, /^00000000-0000-4000-8000-/);
  // unchecked: created only
  view.btnNewLiveSet.fire("click");
  const dlg2 = view.dialogHost.all((n) => n.attrs.role === "dialog")[0];
  const [i2, c2] = dlg2.all((n) => n.tagName === "INPUT");
  c2.checked = false;
  i2.value = "Golf";
  key(i2, "Enter");
  await settle();
  assert.deepEqual(cmds(engine).slice(2).map(([n]) => n), ["CreateLiveSet"]);
});

test("tag editor: tick tags (8 at most, the rest disabled), create, rename, delete with a second click, Done sends SetSetlistTags", async () => {
  const eng = sample();
  for (let i = 0; i < 8; i++) eng.tags.push({ id: "x" + i, name: "X" + i });
  const { engine, view, announced } = await setup(eng);
  view.btnTags.fire("click");
  const dlg = view.dialogHost.all((n) => n.attrs.role === "dialog")[0];
  assert.match(dlg.textContent, /Tags of Friday/);
  const boxes = () => dlg.all((n) => n.tagName === "INPUT" && n.type === "checkbox");
  assert.equal(boxes().length, 10);
  assert.equal(boxes()[0].checked, true); // Gig
  // tick 7 more: 8 in all, then the others are disabled
  for (let i = 2; i < 9; i++) { boxes()[i].checked = true; boxes()[i].fire("change"); }
  assert.equal(boxes().filter((b) => b.checked).length, 8);
  assert.equal(boxes().filter((b) => !b.checked).every((b) => b.disabled), true);
  // untick one, then Done
  boxes()[2].checked = false; boxes()[2].fire("change");
  const done = dlg.all((n) => n.tagName === "BUTTON" && n.textContent === "Done")[0];
  done.fire("click");
  await settle();
  const cmd = cmds(engine)[0];
  assert.equal(cmd[0], "SetSetlistTags");
  assert.equal(cmd[1].tag_ids.length, 7);
  assert.equal(view.dialogHost.children.length, 0);
  assert.ok(announced.some((t) => /Tags of Friday updated, 7 tags/.test(t)));

  // create / rename / delete
  view.btnTags.fire("click");
  const d2 = view.dialogHost.all((n) => n.attrs.role === "dialog")[0];
  const add = d2.all((n) => n.tagName === "INPUT" && n.type === "text")[0];
  add.value = "Brand new";
  key(add, "Enter");
  await settle();
  assert.ok(engine.tags.some((t) => t.name === "Brand new"));
  assert.ok(d2.all((n) => n.tagName === "INPUT" && n.type === "checkbox").length === 11);
  const ren = d2.all((n) => n.tagName === "BUTTON" && /^Rename$/.test(n.textContent))[0];
  ren.fire("click");
  const edit = d2.all((n) => n.tagName === "INPUT" && n.type === "text")[0];
  edit.value = "Gig night";
  key(edit, "Enter");
  await settle();
  assert.equal(engine.tags[0].name, "Gig night");
  const del = d2.all((n) => n.tagName === "BUTTON" && /^Delete$/.test(n.textContent))[0];
  del.fire("click");
  assert.equal(engine.tags.length, 11, "first click only arms it");
  assert.equal(del.textContent, "Confirm delete");
  del.fire("click");
  await settle();
  assert.equal(engine.tags.length, 10);
  assert.ok(!engine.tags.some((t) => t.name === "Gig night"));
});

test("offline / disabled: every action is inert and nothing is sent", async () => {
  const { engine, view, rowsA } = await setup();
  view.setDisabled(true);
  view.render();
  assert.ok([view.btnNewSetlist, view.btnNewLiveSet, view.btnRename, view.btnTags, view.btnDelete, view.btnAdd, view.btnMove, view.btnRemove].every((b) => b.disabled));
  rowsA()[0].focus();
  key(rowsA()[0], "ArrowDown", { altKey: true });
  view.openNewSetlist();
  view.openMoveTo();
  await settle();
  assert.equal(cmds(engine).length, 0);
  assert.equal(view.dialogHost.children.length, 0);
});

test("hide() closes any dialog and hides the view; show() draws it again", async () => {
  const { view } = await setup();
  view.btnNewSetlist.fire("click");
  view.hide();
  assert.equal(view.host.hidden, true);
  assert.equal(view.dialogHost.children.length, 0);
  view.show();
  assert.equal(view.host.hidden, false);
});

test("empty library: the empty hint, no rows, create still available", async () => {
  const eng = sample();
  eng.setlists = [];
  const { view, rowsA } = await setup(eng);
  assert.equal(rowsA().length, 0);
  assert.match(view.emptyA.textContent, /No setlists yet/);
  assert.equal(view.btnNewSetlist.disabled, false);
  assert.equal(view.btnRename.disabled, true);
});

const GONE = "ffffffff-ffff-4fff-8fff-ffffffffffff";

test("a double Enter in a name dialog sends ONE command (re-entry is ignored while it is in flight)", async () => {
  const eng = sample();
  eng.delay = 10;
  const { engine, view } = await setup(eng);
  view.btnNewSetlist.fire("click");
  const input = view.dialogHost.all((n) => n.tagName === "INPUT")[0];
  input.value = "Once";
  key(input, "Enter");
  key(input, "Enter");
  key(input, "Enter");
  await new Promise((r) => setTimeout(r, 80));
  assert.equal(cmds(engine).filter(([n]) => n === "CreateSetlist").length, 1);
});

test("a double Enter in the tag editor (add tag, rename tag) sends one command each", async () => {
  const eng = sample();
  eng.delay = 10;
  const { engine, view } = await setup(eng);
  view.btnTags.fire("click");
  const dlg = view.dialogHost.all((n) => n.attrs.role === "dialog")[0];
  const add = dlg.all((n) => n.tagName === "INPUT" && n.type === "text")[0];
  add.value = "Fresh";
  key(add, "Enter");
  key(add, "Enter");
  await new Promise((r) => setTimeout(r, 80));
  assert.equal(cmds(engine).filter(([n]) => n === "CreateTag").length, 1);
  dlg.all((n) => n.tagName === "BUTTON" && n.textContent === "Rename")[0].fire("click");
  const edit = dlg.all((n) => n.tagName === "INPUT" && n.type === "text")[0];
  edit.value = "Gig 2";
  key(edit, "Enter");
  key(edit, "Enter");
  await new Promise((r) => setTimeout(r, 80));
  assert.equal(cmds(engine).filter(([n]) => n === "RenameTag").length, 1);
});

test("Backspace (the Delete key of a Mac keyboard) asks to remove a Live Set / delete a setlist, like Delete", async () => {
  const { view, rowsA, rowsB } = await setup();
  rowsB()[0].focus();
  const e = key(rowsB()[0], "Backspace");
  assert.equal(e.prevented, true);
  assert.match(view.dialogHost.all((n) => n.attrs.role === "dialog")[0].textContent, /Remove Alpha from Friday/);
  view.closeDialog(true);
  rowsA()[1].focus();
  key(rowsA()[1], "Backspace");
  await settle();
  assert.match(view.dialogHost.all((n) => n.attrs.role === "dialog")[0].textContent, /Delete Saturday/);
});

test("focus is never lost: after a remove, a delete, a move away or a Move to menu it lands on the row at the nearest index", async () => {
  const { engine, view, doc, rowsA, rowsB } = await setup();
  // remove the middle Live Set through the dialog
  rowsB()[1].focus();
  key(rowsB()[1], "Delete");
  view.dialogHost.all((n) => n.tagName === "BUTTON")[1].fire("click");
  await settle();
  assert.ok(doc.activeElement, "focus is somewhere");
  assert.equal(doc.activeElement.dataset.key, rowsB()[1].dataset.key); // the row now at index 1 (Charlie)
  // remove the last remaining ones: the list's button takes the focus when it is empty
  rowsB()[0].focus();
  key(rowsB()[0], "Delete");
  view.dialogHost.all((n) => n.tagName === "BUTTON")[1].fire("click");
  await settle();
  rowsB()[0].focus();
  key(rowsB()[0], "Delete");
  view.dialogHost.all((n) => n.tagName === "BUTTON")[1].fire("click");
  await settle();
  assert.equal(rowsB().length, 0);
  assert.equal(doc.activeElement, view.btnAdd);
  // delete a setlist: the focus goes to the setlist now at its place
  rowsA()[0].focus();
  view.btnDelete.fire("click");
  view.dialogHost.all((n) => n.tagName === "BUTTON")[1].fire("click");
  await settle();
  assert.equal(doc.activeElement, rowsA()[0]);
  assert.ok(!engine.setlists.some((x) => x.id === "s-one"));
});

test("focus after a Live Set moves to another setlist (menu) lands on the row now at its place", async () => {
  const { doc, view, rowsB } = await setup();
  rowsB()[0].focus();
  key(rowsB()[0], "KeyM", { altKey: true, code: "KeyM" });
  const end = view.dialogHost.all((n) => n.attrs.role === "menuitem").find((i) => /Saturday: at the end/.test(i.textContent));
  end.fire("click");
  await settle();
  assert.ok(doc.activeElement && doc.activeElement.dataset.list === "liveset");
  assert.equal(doc.activeElement, rowsB()[0]);
});

test("a missing Live Set shows a fallback name (the missing label and a short id) everywhere, and Move to offers positions only", async () => {
  const { ctl, view, rowsB, announced } = await setup();
  await ctl.select("s-three");
  const row = rowsB()[0];
  assert.equal(row.dataset.key, GONE);
  assert.match(row.textContent, /missing ffffffff/);
  assert.match(row.getAttribute("aria-label"), /missing ffffffff, position 1 of 2, missing/);
  row.focus();
  key(row, "KeyM", { altKey: true, code: "KeyM" });
  const items = view.dialogHost.all((n) => n.attrs.role === "menuitem").map((n) => n.textContent);
  assert.deepEqual(items, ["Position 2"], "no setlist target for a missing Live Set");
  assert.match(view.dialogHost.all((n) => n.attrs.role === "menu")[0].getAttribute("aria-label"), /Move missing ffffffff to/);
  view.closeDialog(true);
  key(row, "Delete");
  assert.match(view.dialogHost.all((n) => n.attrs.role === "dialog")[0].textContent, /Remove missing ffffffff from Sunday/);
  view.dialogHost.all((n) => n.tagName === "BUTTON")[1].fire("click");
  await settle();
  assert.ok(announced.includes("Removed missing ffffffff from Sunday"), announced.join("|"));
});

test("singular count in the row text, the aria-label and the selection announcement", async () => {
  const { rowsA, view, announced } = await setup();
  assert.match(rowsA()[1].textContent, /1 Live Set(?!s)/);
  assert.match(rowsA()[1].getAttribute("aria-label"), /^Saturday, 1 Live Set(?!s)/);
  rowsA()[1].focus();
  key(rowsA()[1], "Enter");
  await settle();
  assert.ok(announced.includes("Setlist Saturday selected, 1 Live Set"), announced.join("|"));
  void view;
});

test("the armed tag delete disarms itself on a timer (a click does not focus a button in WKWebView, so blur is not used)", async () => {
  const { view } = await setup();
  const real = globalThis.setTimeout;
  const timers = [];
  globalThis.setTimeout = (f, ms) => { timers.push([f, ms]); return 0; };
  try {
    view.btnTags.fire("click");
    const dlg = view.dialogHost.all((n) => n.attrs.role === "dialog")[0];
    const del = dlg.all((n) => n.tagName === "BUTTON" && /^Delete$/.test(n.textContent))[0];
    del.fire("click");
    assert.equal(del.textContent, "Confirm delete");
    assert.equal(timers.length, 1);
    timers[0][0]();
    assert.equal(del.textContent, "Delete");
    assert.equal(del.dataset.armed, "0");
  } finally {
    globalThis.setTimeout = real;
  }
});

test("Backspace / Delete never act from inside a dialog input (only from a list row)", async () => {
  const { engine, view } = await setup();
  view.btnNewSetlist.fire("click");
  const input = view.dialogHost.all((n) => n.tagName === "INPUT")[0];
  key(input, "Backspace");
  key(input, "Delete");
  assert.equal(cmds(engine).length, 0);
  assert.equal(view.dialogHost.all((n) => n.attrs.role === "dialog").length, 1);
});
