// Story 9.11: the "+ Add timbre" picker model (timbre_picker.js, no DOM) and its assignment through the Part Editor controller (part_editor.js assignTimbre) against
// tests/support/fake_unit.js: the list, the search (diacritics, words, category), the cap, navigation, every way a choice can fail (the engine refuses, the library vanished, a stale
// revision, a dropped link, a second choice) and the Live Set view / Footer after an assignment.
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";

import { MAX_VISIBLE, TimbrePickerController, buildItems, filterItems, fold, moveActive, visibleGroups } from "../src/timbre_picker.js";
import { PartEditorController } from "../src/part_editor.js";
import { LiveSetViewController, applyState, buildModel } from "../src/live_set_view.js";
import { partName } from "../src/live_set_dom.js";
import { liveEditFooter } from "../src/live_edit_state.js";
import { FakeUnit, CODE } from "./support/fake_unit.js";
import { CATALOGS } from "../src/catalogs.js";

const T = (library_id, instrument_key, name, category, library_name = "Lib " + library_id) => ({ library_id, instrument_key, name, category, library_name });
const settle = async () => { for (let i = 0; i < 8; i++) await new Promise((r) => setImmediate(r)); };
const tr = (k) => CATALOGS.en[k] || k;

// ---- text folding and search ---------------------------------------------------------------------------------------------------------------------------------

test("fold: case, diacritics, full-width forms and spacing are ignored; nothing throws", () => {
  assert.equal(fold("Rhôdes  MK I"), "rhodes mk i");
  assert.equal(fold("Señor Café"), "senor cafe");
  assert.equal(fold("ＰＩＡＮＯ"), "piano");               // full-width (an IME's output)
  assert.equal(fold("ﬁne"), "fine");                      // compatibility ligature
  assert.equal(fold("日本のピアノ"), "日本のピアノ");        // no decomposition: kept
  assert.equal(fold("é"), "e");                      // decomposed accent
  assert.equal(fold(null), "");
  assert.equal(fold(undefined), "");
  assert.equal(fold(42), "42");
});

test("buildItems drops rows that cannot be assigned, never uses a name as an identity, sorts totally and survives hostile text", () => {
  const items = buildItems({ timbres: [
    T("a", "k1", "Zebra", 1), T("a", "k2", "apple", 1), T("b", "k3", "Strings", 6), T("b", "k4", "Pad", 0), T("b", "k5", "Odd", 99),
    T("", "x", "no library", 1), T("c", "", "no key", 1), null, 7, { library_id: "d", instrument_key: "k6", name: "\u0007\u0000 ", category: 2 },
    T("e", "k7", '<img src=x onerror="alert(1)">', 1),
  ] });
  assert.deepEqual(items.map((i) => i.instrumentKey), ["k7", "k2", "k1", "k6", "k3", "k5", "k4"]); // category order, folded name; uncategorised last
  assert.equal(items.find((i) => i.instrumentKey === "k6").name, "k6");           // a name of control characters falls back to the key
  assert.equal(items.find((i) => i.instrumentKey === "k5").category, 0);           // an unknown category is "none"
  assert.equal(items.find((i) => i.instrumentKey === "k7").name, '<img src=x onerror="alert(1)">'); // text, kept as text
  assert.equal(new Set(items.map((i) => i.id)).size, items.length);
  // the same name in two libraries stays two rows with two identities
  const dup = buildItems({ timbres: [T("a", "grand", "Grand", 1), T("b", "grand", "Grand", 1)] });
  assert.equal(dup.length, 2);
  assert.notEqual(dup[0].id, dup[1].id);
  assert.deepEqual(buildItems(null), []);
  assert.deepEqual(buildItems({ timbres: "x" }), []);
});

test("filterItems: every word must match the name or library (diacritics and case ignored) or the category label; an empty query matches everything", () => {
  const items = buildItems({ timbres: [T("a", "r", "Rhôdes Mk I", 2, "Studio"), T("a", "g", "Concert Grand", 1, "Studio"), T("b", "s", "String Ensemble", 6, "Orchestra")] });
  const label = (it) => ({ 1: "Piano", 2: "E.Piano", 6: "Strings" }[it.category] || "");
  assert.equal(filterItems(items, "", label).length, 3);
  assert.deepEqual(filterItems(items, "rhodes", label).map((i) => i.instrumentKey), ["r"]);
  assert.deepEqual(filterItems(items, "RHÔDES mk", label).map((i) => i.instrumentKey), ["r"]);
  assert.deepEqual(filterItems(items, "studio", label).map((i) => i.instrumentKey), ["g", "r"]);       // the library name counts
  assert.deepEqual(filterItems(items, "grand studio", label).map((i) => i.instrumentKey), ["g"]);      // words in any order, all required
  assert.deepEqual(filterItems(items, "strings", label).map((i) => i.instrumentKey), ["s"]);           // the category label counts
  assert.deepEqual(filterItems(items, "ＧＲＡＮＤ", label).map((i) => i.instrumentKey), ["g"]);          // full-width IME text
  assert.deepEqual(filterItems(items, "zzz", label), []);
  assert.deepEqual(filterItems(items, "   ", label).length, 3);
  assert.deepEqual(filterItems(items, "e".repeat(5000), label), []);                                    // an absurd query is cut, not slow, and matches nothing
});

test("visibleGroups caps what is drawn and says so; a 5000-timbre catalog draws MAX_VISIBLE rows", () => {
  const many = Array.from({ length: 5000 }, (_, i) => T("lib" + (i % 7), "k" + i, "Timbre " + String(i).padStart(4, "0"), 1 + (i % 17)));
  const items = buildItems({ timbres: many });
  const v = visibleGroups(items);
  assert.equal(v.shown, MAX_VISIBLE);
  assert.equal(v.matched, 5000);
  assert.equal(v.capped, true);
  assert.equal(v.groups.reduce((n, g) => n + g.items.length, 0), MAX_VISIBLE);
  assert.ok(v.groups.every((g, i) => i === 0 || g.category !== v.groups[i - 1].category)); // one heading per run of one category
  const few = visibleGroups(items.slice(0, 3));
  assert.deepEqual([few.shown, few.matched, few.capped], [3, 3, false]);
  assert.deepEqual(visibleGroups([]), { groups: [], shown: 0, matched: 0, capped: false });
});

test("moveActive: a list, not a grid (no wrap); Home / End / PageUp / PageDown", () => {
  assert.equal(moveActive(-1, "ArrowDown", 5), 0);
  assert.equal(moveActive(0, "ArrowUp", 5), 0);
  assert.equal(moveActive(4, "ArrowDown", 5), 4);
  assert.equal(moveActive(2, "ArrowDown", 5), 3);
  assert.equal(moveActive(2, "ArrowUp", 5), 1);
  assert.equal(moveActive(2, "Home", 5), 0);
  assert.equal(moveActive(2, "End", 5), 4);
  assert.equal(moveActive(2, "PageDown", 50), 12);
  assert.equal(moveActive(2, "PageUp", 50), 0);
  assert.equal(moveActive(3, "x", 5), 3);
  assert.equal(moveActive(0, "ArrowDown", 0), -1);
});

// ---- the controller: loading and searching -----------------------------------------------------------------------------------------------------------------------

function pickerRig({ unit = new FakeUnit({ empties: [4, 5] }), list, assign } = {}) {
  const events = { changes: 0, closed: [] };
  const picker = new TimbrePickerController({
    listTimbres: list || (() => unit.listTimbres()),
    assign: assign || (async (a) => ({ ok: true, a })),
    categoryLabel: (it) => (it.categoryKey ? tr(it.categoryKey) : ""),
    onChange: () => { events.changes++; },
    onClose: (reason, detail, ctx) => events.closed.push([reason, detail, ctx]),
  });
  return { unit, picker, events };
}

test("open reads the list (read-only), groups it by category and starts on the first row; an empty catalog is its own state", async () => {
  const { unit, picker } = pickerRig();
  const p = picker.open({ liveSetId: "ls-1", part: 4 });
  assert.equal(picker.status, "loading");
  await p;
  assert.equal(picker.status, "ready");
  assert.deepEqual(picker.view.groups.map((g) => g.categoryKey), ["category.piano.short", "category.epiano.short", "category.strings.short"]);
  assert.equal(picker.rows.length, 4);
  assert.equal(picker.active, 0);
  assert.equal(picker.activeItem.name, "Concert Grand");
  assert.deepEqual(unit.log, [], "listing sends nothing to the engine");
  const none = pickerRig({ unit: new FakeUnit({ empties: [4] }) });
  none.unit.installed.clear();
  await none.picker.open({ liveSetId: "ls-1", part: 4 });
  assert.equal(none.picker.status, "empty");
  assert.equal(none.picker.rows.length, 0);
  assert.equal(await none.picker.choose({ id: "x" }), false);
  assert.equal(none.picker.cancel(), true);
});

test("search: narrows by diacritic-free words, keeps the order, moves the active row to the first match, and 'no match' is not an error", async () => {
  const { picker } = pickerRig();
  await picker.open({ liveSetId: "ls-1", part: 4 });
  picker.setQuery("rhodes");
  assert.deepEqual(picker.rows.map((r) => r.instrumentKey), ["rhodes"]);
  assert.equal(picker.active, 0);
  picker.setQuery("grand");
  assert.deepEqual(picker.rows.map((r) => r.name), ["Concert Grand", "Soft Grand"]);
  picker.move("ArrowDown");
  assert.equal(picker.activeItem.name, "Soft Grand");
  picker.move("ArrowDown");
  assert.equal(picker.activeItem.name, "Soft Grand");
  picker.setQuery("grand strings");
  assert.deepEqual(picker.rows.map((r) => r.name), ["Soft Grand"]);
  picker.setQuery("nothing like this");
  assert.equal(picker.rows.length, 0);
  assert.equal(picker.active, -1);
  assert.equal(picker.status, "ready");
  assert.equal(picker.view.matched, 0);
  assert.equal(picker.error, null);
  picker.setQuery("");
  assert.equal(picker.rows.length, 4);
});

test("text typed while the list loads is kept and applied when it arrives", async () => {
  let release;
  const { picker } = pickerRig({ list: () => new Promise((r) => { release = r; }) });
  const p = picker.open({ liveSetId: "ls-1", part: 4 });
  picker.setQuery("string");
  release({ timbres: [T("a", "1", "String Ensemble", 6), T("a", "2", "Piano", 1)] });
  await p;
  assert.equal(picker.query, "string");
  assert.deepEqual(picker.rows.map((r) => r.name), ["String Ensemble"]);
});

test("a load that fails is an error with Try again; a reply that arrives for a picker that was closed or re-opened is dropped", async () => {
  const unit = new FakeUnit({ empties: [4] });
  unit.listFail = "not connected";
  const { picker, events } = pickerRig({ unit });
  assert.equal(await picker.open({ liveSetId: "ls-1", part: 4 }), false);
  assert.equal(picker.status, "error");
  assert.deepEqual(picker.error, { kind: "load", message: "not connected" });
  unit.listFail = "";
  assert.equal(await picker.retry(), true);
  assert.equal(picker.status, "ready");
  // late replies
  let releaseA;
  let releaseB;
  let calls = 0;
  const slow = pickerRig({ list: () => new Promise((r) => { if (++calls === 1) releaseA = r; else releaseB = r; }) });
  const a = slow.picker.open({ liveSetId: "ls-1", part: 4 });
  const b = slow.picker.open({ liveSetId: "ls-1", part: 5 }); // another part: the first picker is replaced
  releaseB({ timbres: [T("b", "kb", "B timbre", 1)] });
  assert.equal(await b, true);
  releaseA({ timbres: [T("a", "ka", "A timbre", 1)] });
  assert.equal(await a, false);
  assert.deepEqual(slow.picker.rows.map((r) => r.name), ["B timbre"], "the late reply of the first open never fills the second picker");
  assert.equal(slow.picker.context.part, 5);
  const closed = pickerRig({ list: () => new Promise((r) => { releaseA = r; }) });
  const c = closed.picker.open({ liveSetId: "ls-1", part: 4 });
  closed.picker.cancel();
  releaseA({ timbres: [T("a", "ka", "A timbre", 1)] });
  assert.equal(await c, false);
  assert.equal(closed.picker.status, "closed");
  assert.equal(events.closed.length, 0);
});

// ---- the controller: choosing -----------------------------------------------------------------------------------------------------------------------------------

test("choose sends exactly the (library_id, instrument_key) of the row, once; a second Enter while it is out sends nothing", async () => {
  const sent = [];
  let release;
  const { picker, events } = pickerRig({ assign: (a) => { sent.push(a); return new Promise((r) => { release = r; }); } });
  await picker.open({ liveSetId: "ls-1", part: 4 });
  picker.setQuery("soft");
  const item = picker.activeItem;
  const p = picker.choose(item);
  assert.equal(picker.status, "assigning");
  assert.equal(await picker.choose(item), false, "a second choice while the first is out is refused");
  assert.equal(picker.cancel(), false, "Esc does not take back a request that is already out");
  picker.move("ArrowDown");
  picker.setQuery("rhodes"); // nothing changes the pending choice
  release({ ok: true });
  assert.equal(await p, true);
  assert.deepEqual(sent, [{ part: 4, libraryId: "lib-b", instrumentKey: "grand", liveSetId: "ls-1" }]);   // not lib-a's "grand": the same key in two libraries
  assert.equal(picker.status, "closed");
  assert.deepEqual(events.closed.map((c) => c[0]), ["assigned"]);
  assert.equal(events.closed[0][1].name, "Soft Grand");
});

test("a row that is not drawn (above the cap or filtered out) cannot be chosen", async () => {
  const sent = [];
  const many = Array.from({ length: 300 }, (_, i) => T("a", "k" + i, "Timbre " + String(i).padStart(3, "0"), 1));
  const { picker } = pickerRig({ list: async () => ({ timbres: many }), assign: async (a) => { sent.push(a); return { ok: true }; } });
  await picker.open({ liveSetId: "ls-1", part: 4 });
  assert.equal(picker.rows.length, MAX_VISIBLE);
  const hidden = picker.items.find((i) => !picker.rows.includes(i));
  assert.equal(await picker.choose(hidden), false);
  assert.equal(await picker.choose(null), false);
  assert.equal(await picker.choose({ id: "forged" }), false);
  assert.deepEqual(sent, []);
});

test("the engine refuses (BUSY, invalid): the picker stays open on the same list with the message, nothing is assigned, and another try works", async () => {
  let n = 0;
  const { picker, events } = pickerRig({ assign: async () => (++n === 1 ? { ok: false, error: { code: CODE.BUSY, message: "BUSY (reason=reload_in_progress)" } } : { ok: true }) });
  await picker.open({ liveSetId: "ls-1", part: 4 });
  picker.move("ArrowDown");
  const before = picker.activeItem.id;
  assert.equal(await picker.choose(picker.activeItem), false);
  assert.equal(picker.status, "ready");
  assert.deepEqual(picker.error, { kind: "assign", message: "BUSY (reason=reload_in_progress)" });
  assert.equal(picker.activeItem.id, before);
  assert.equal(picker.rows.length, 4);
  assert.equal(events.closed.length, 0);
  assert.equal(await picker.choose(picker.activeItem), true);
  assert.equal(picker.status, "closed");
});

test("a stale revision closes the picker as 'stale'; the link dropping closes it as 'aborted' and a reply that comes after is not believed", async () => {
  const stale = pickerRig({ assign: async () => ({ ok: false, stale: true }) });
  await stale.picker.open({ liveSetId: "ls-1", part: 4 });
  assert.equal(await stale.picker.choose(stale.picker.activeItem), false);
  assert.deepEqual(stale.events.closed.map((c) => c[0]), ["stale"]);
  let release;
  const gone = pickerRig({ assign: () => new Promise((r) => { release = r; }) });
  await gone.picker.open({ liveSetId: "ls-1", part: 4 });
  const p = gone.picker.choose(gone.picker.activeItem);
  assert.equal(gone.picker.abort("disconnected"), true);
  release({ ok: true });
  assert.equal(await p, false);
  assert.deepEqual(gone.events.closed.map((c) => [c[0], c[1]]), [["aborted", "disconnected"]]);
  assert.equal(gone.picker.abort("again"), false);
  // aborting while the list loads: the late list is dropped
  let late;
  const loading = pickerRig({ list: () => new Promise((r) => { late = r; }) });
  const l = loading.picker.open({ liveSetId: "ls-1", part: 4 });
  loading.picker.abort("disconnected");
  late({ timbres: [T("a", "k", "X", 1)] });
  assert.equal(await l, false);
  assert.equal(loading.picker.status, "closed");
});

test("cancel (Esc) closes without sending anything, from any state but an assignment in flight", async () => {
  const sent = [];
  const { picker, events } = pickerRig({ assign: async (a) => { sent.push(a); return { ok: true }; } });
  await picker.open({ liveSetId: "ls-1", part: 4 });
  picker.setQuery("grand");
  assert.equal(picker.cancel(), true);
  assert.deepEqual(events.closed.map((c) => c[0]), ["cancelled"]);
  assert.deepEqual(sent, []);
  assert.equal(picker.cancel(), false);
  assert.equal(picker.isOpen, false);
  // re-opening starts clean: no query, no stale list, the first row active
  await picker.open({ liveSetId: "ls-1", part: 5 });
  assert.equal(picker.query, "");
  assert.equal(picker.rows.length, 4);
});

// ---- the assignment against the unit ---------------------------------------------------------------------------------------------------------------------------

function unitRig(unitOptions = { empties: [4, 5] }) {
  const unit = new FakeUnit(unitOptions);
  const events = { stale: 0, errors: [] };
  const ctl = new PartEditorController({
    request: (m) => unit.request(m), setTimer: () => 0, clearTimer: () => {}, onChange: () => {}, onError: (e) => events.errors.push(e), onStale: () => { events.stale++; },
  });
  unit.onState((st) => ctl.onState(st));
  ctl.select({ liveSetId: unit.liveSetId, part: 4, engine: "sample" });
  ctl.onState(unit.state());
  return { unit, ctl, events };
}

test("assignTimbre sends ONE SetPartTimbre with base_revision, no engine type; the part is rebuilt, the Live Set is dirty and the Footer says UNSAVED: LIVE SET", async () => {
  const { unit, ctl } = unitRig();
  assert.equal(liveEditFooter(unit.state()).scope.kind, "none");
  const rev = unit.liveRevision;
  const res = await ctl.assignTimbre({ part: 4, libraryId: "lib-a", instrumentKey: "rhodes", liveSetId: unit.liveSetId });
  assert.deepEqual(res.ok, true);
  const sent = unit.log.filter((l) => l[0] === "SetPartTimbre");
  assert.equal(sent.length, 1);
  assert.deepEqual(sent[0], ["SetPartTimbre", { part: 4, library_id: "lib-a", instrument_key: "rhodes" }, rev]);
  assert.equal(unit.parts[4].part_state, 0);
  assert.equal(unit.liveRevision, rev, "Studio's own structural edit does not move the revision: the next edit is not stale");
  const f = liveEditFooter(unit.state());
  assert.deepEqual(f, { on: true, scope: { kind: "live_set" } });
  assert.equal(CATALOGS.en["studio.footer.unsaved_live_set"], "UNSAVED: LIVE SET");
  // the next edit on the same base is accepted (nothing went stale)
  const again = await ctl.assignTimbre({ part: 5, libraryId: "lib-b", instrumentKey: "ensemble", liveSetId: unit.liveSetId });
  assert.equal(again.ok, true);
});

test("the Live Set view after an assignment: the strip shows the chosen timbre (and never flicks back to '+ Add timbre' while the unit rebuilds), the diamond is lit", async () => {
  const { unit, ctl } = unitRig();
  const models = [];
  const view = new LiveSetViewController({ request: (m) => unit.request(m), onModel: (m) => models.push(m), refetchMinMs: 0 });
  await view.select(unit.liveSetId);
  view.onState(unit.state());
  assert.equal(view.model.parts[4].status, "empty");
  assert.equal(view.model.dirty, false);
  assert.equal((await ctl.assignTimbre({ part: 4, libraryId: "lib-a", instrumentKey: "rhodes", liveSetId: unit.liveSetId })).ok, true);
  // a State pushed while the unit rebuilds still reports the OLD generation (EMPTY_TIMBRE) and says it is loading
  const rebuilding = unit.state();
  rebuilding.parts[4].part_state = 1;
  rebuilding.loading = { loading: true };
  await view.select(unit.liveSetId); // the page reads the view again right after the Ack
  view.onState(rebuilding);
  assert.equal(view.model.parts[4].status, "ready", "the part keeps showing what was chosen during the rebuild");
  assert.equal(view.model.parts[4].timbreName, "Rhôdes Mk I");
  assert.equal(view.model.parts[4].addTimbre, false);
  view.onState(unit.state()); // the rebuild is done
  assert.equal(view.model.parts[4].status, "ready");
  assert.equal(view.model.parts[4].timbreName, "Rhôdes Mk I");
  assert.equal(view.model.dirty, true, "the diamond (structural change, Whole Live Set)");
  assert.equal(view.model.parts[5].status, "empty");
  // without the rebuild flag the old behaviour stands: the unit says EMPTY_TIMBRE, the strip is empty
  const plain = applyState(view.base, { ...unit.state(), parts: unit.state().parts.map((p, i) => (i === 4 ? { ...p, part_state: 1 } : p)) });
  assert.equal(plain.parts[4].status, "empty");
});

test("a stale revision: the picker's assignment is refused by the unit, nothing is assigned, the editor raises its banner exactly as for a knob, and Reload makes the next pick work", async () => {
  const { unit, ctl, events } = unitRig();
  unit.deviceChange(); // the panel changed the Live Set after Studio loaded it
  const res = await ctl.assignTimbre({ part: 4, libraryId: "lib-a", instrumentKey: "grand", liveSetId: unit.liveSetId });
  assert.deepEqual(res, { ok: false, stale: true });
  assert.equal(unit.parts[4].part_state, 1, "the part is still empty");
  assert.equal(unit.parts[4].timbre, null);
  assert.equal(events.stale, 1);
  assert.ok(ctl.stale);
  // while the banner is up nothing is sent
  const logged = unit.log.length;
  assert.deepEqual(await ctl.assignTimbre({ part: 4, libraryId: "lib-a", instrumentKey: "grand", liveSetId: unit.liveSetId }), { ok: false, stale: true });
  assert.equal(unit.log.length, logged);
  assert.equal(await ctl.reload(), true);
  assert.equal((await ctl.assignTimbre({ part: 4, libraryId: "lib-a", instrumentKey: "grand", liveSetId: unit.liveSetId })).ok, true);
  assert.equal(unit.parts[4].part_state, 0);
});

test("stale while the picker is open: the picker closes as 'stale', the part stays empty", async () => {
  const { unit, ctl } = unitRig();
  const closed = [];
  const picker = new TimbrePickerController({ listTimbres: () => unit.listTimbres(), assign: (a) => ctl.assignTimbre(a), onClose: (r) => closed.push(r) });
  await picker.open({ liveSetId: unit.liveSetId, part: 4 });
  unit.deviceChange(); // someone turns a knob on the unit while the player is choosing
  assert.equal(await picker.choose(picker.activeItem), false);
  assert.deepEqual(closed, ["stale"]);
  assert.equal(unit.parts[4].timbre, null);
});

test("the library vanished between the list and the assignment: the unit takes the reference, the part reads MISSING_LIBRARY with no name, never another timbre", async () => {
  const { unit, ctl } = unitRig();
  const picker = new TimbrePickerController({ listTimbres: () => unit.listTimbres(), assign: (a) => ctl.assignTimbre(a) });
  await picker.open({ liveSetId: unit.liveSetId, part: 4 });
  picker.setQuery("string");
  assert.equal(picker.activeItem.libraryId, "lib-b");
  unit.installed.delete("lib-b"); // removed from the unit after the list was read
  assert.equal(await picker.choose(picker.activeItem), true);
  assert.equal(unit.parts[4].timbre.library_id, "lib-b");
  assert.equal(unit.parts[4].part_state, 2);
  const view = new LiveSetViewController({ request: (m) => unit.request(m), onModel: () => {}, refetchMinMs: 0 });
  await view.select(unit.liveSetId);
  view.onState(unit.state());
  const p = view.model.parts[4];
  assert.equal(p.status, "missing");
  assert.equal(p.timbreName, "");
  assert.equal(partName(p, tr), "Missing library");
  assert.equal(unit.log.filter((l) => l[0] === "SetPartTimbre").length, 1, "one request, for the chosen timbre only");
  // and the same strip when the Live Set is shown again later (a model built from a view that says missing, even if it still carries a name)
  const again = buildModel({ live_set_id: "x", name: "N", parts: [{ index: 0, missing_library: true, timbre_name: "Concert Grand", engine_type_id: 1 }] });
  assert.equal(again.parts[0].status, "missing");
  assert.equal(again.parts[0].timbreName, "");
});

test("BUSY while the unit still rebuilds (a second pick right away) is a typed error the picker shows; the part is unchanged", async () => {
  const { unit, ctl } = unitRig();
  const picker = new TimbrePickerController({ listTimbres: () => unit.listTimbres(), assign: (a) => ctl.assignTimbre(a) });
  await picker.open({ liveSetId: unit.liveSetId, part: 5 });
  unit.busyRebuild = true;
  assert.equal(await picker.choose(picker.activeItem), false);
  assert.equal(picker.status, "ready");
  assert.equal(picker.error.kind, "assign");
  assert.equal(unit.parts[5].timbre, null);
  unit.busyRebuild = false;
  assert.equal(await picker.choose(picker.activeItem), true);
});

test("assignTimbre refuses what would land elsewhere: another Live Set, a Live Set that does not sound, bad arguments", async () => {
  const { unit, ctl } = unitRig();
  for (const a of [
    { part: 4, libraryId: "lib-a", instrumentKey: "grand", liveSetId: "another" },
    { part: -1, libraryId: "lib-a", instrumentKey: "grand", liveSetId: unit.liveSetId },
    { part: 4, libraryId: "", instrumentKey: "grand", liveSetId: unit.liveSetId },
    { part: 4, libraryId: "lib-a", instrumentKey: "", liveSetId: unit.liveSetId },
    {},
  ]) {
    const r = await ctl.assignTimbre(a);
    assert.equal(r.ok, false);
    assert.ok(r.error && r.error.message);
  }
  unit.sounding = false;
  ctl.onState(unit.state());
  assert.equal((await ctl.assignTimbre({ part: 4, libraryId: "lib-a", instrumentKey: "grand", liveSetId: unit.liveSetId })).ok, false);
  assert.equal(unit.log.filter((l) => l[0] === "SetPartTimbre").length, 0, "nothing reached the unit");
});

test("the link drops while the assignment is out: a stale answer that arrives after the reset is not believed (no banner on a view that is gone)", async () => {
  const { unit, ctl, events } = unitRig();
  unit.delay = 3;
  const p = ctl.assignTimbre({ part: 4, libraryId: "lib-a", instrumentKey: "grand", liveSetId: unit.liveSetId });
  unit.liveRevision++; // changed meanwhile: the unit will answer STALE_REVISION
  ctl.reset(); // the cable was pulled: the editor drops everything
  const r = await p;
  assert.equal(r.ok, false);
  assert.equal(r.stale, undefined);
  assert.equal(events.stale, 0);
  assert.equal(ctl.stale, null);
});

test("a transport failure is an error, not an assignment", async () => {
  const { ctl, unit } = unitRig();
  const broken = new PartEditorController({ request: async () => { throw new Error("not connected"); }, setTimer: () => 0, clearTimer: () => {}, onChange: () => {} });
  broken.onState(unit.state());
  broken.select({ liveSetId: unit.liveSetId, part: 4, engine: "sample" });
  const r = await broken.assignTimbre({ part: 4, libraryId: "lib-a", instrumentKey: "grand", liveSetId: unit.liveSetId });
  assert.deepEqual(r, { ok: false, error: { code: 0, message: "not connected", details: {} } });
  void ctl;
  await settle();
});

test("an older unit (no live_set_revision): no base_revision is sent, nothing is ever stale", async () => {
  const unit = new FakeUnit({ empties: [4] });
  const ctl = new PartEditorController({ request: (m) => unit.request(m), setTimer: () => 0, clearTimer: () => {}, onChange: () => {} });
  const st = unit.state();
  delete st.edits.live_set_revision;
  ctl.select({ liveSetId: unit.liveSetId, part: 4, engine: "sample" });
  ctl.onState(st);
  assert.equal((await ctl.assignTimbre({ part: 4, libraryId: "lib-a", instrumentKey: "grand", liveSetId: unit.liveSetId })).ok, true);
  assert.equal(unit.log.find((l) => l[0] === "SetPartTimbre")[2], 0);
});

test("locale: every key the picker, the strip and the app wiring use exists in en, es and pt-BR with the same placeholders", () => {
  const keys = new Set();
  for (const f of ["timbre_picker_dom.js", "live_set_dom.js", "app.js"]) {
    const src = fs.readFileSync(new URL(`../src/${f}`, import.meta.url), "utf8");
    for (const m of src.matchAll(/"(studio\.(?:timbre_picker|live_set\.add_timbre[a-z_]*)[a-z_.]*)"/g)) keys.add(m[1]);
  }
  assert.ok(keys.size >= 20, `found ${keys.size} keys`);
  const seq = (t) => (t.match(/%[ds]/g) || []).join("");
  for (const lang of ["en", "es", "pt-BR"]) for (const k of keys) {
    assert.ok(CATALOGS[lang][k], `${lang}: ${k}`);
    assert.equal(seq(CATALOGS[lang][k]), seq(CATALOGS.en[k]), `${lang}: ${k} placeholders`);
  }
  assert.equal("studio.live_set.add_timbre_soon" in CATALOGS.en, false, "the disabled placeholder text is gone");
});
