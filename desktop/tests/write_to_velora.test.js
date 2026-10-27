// Story 9.12: Write to Velora, the model and controller (no DOM) against tests/support/fake_unit.js (which keeps the rules of engine/host/src/edit_session.cpp do_write): when the button is
// usable, the device's scopes and their reasons, Save as new (suggested free name, name in use, the race), the exact command and its base_revision, flush before the Write, double click,
// STALE_REVISION, storage full, a connection that drops while the Write is out (the result is UNKNOWN: nothing is claimed, the truth is read back), typed refusals, and the locale keys.
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";

import { CATALOGS } from "../src/catalogs.js";
import {
  CODE,
  REASON_KEYS,
  SCOPE,
  WriteController,
  changeLabel,
  foldName,
  nameProblem,
  suggestFreeName,
  trimName,
  writeAvailability,
  writeSnapshot,
} from "../src/write_to_velora.js";
import { announceText, bannerText } from "../src/write_to_velora_dom.js";
import { FakeUnit } from "./support/fake_unit.js";
import { rig, settle } from "./support/write_rig.js";

const EN = CATALOGS.en;
const tr = (key, ...args) => {
  assert.ok(key in EN, `locale key missing: ${key}`);
  let i = 0;
  return EN[key].replace(/%[ds]/g, () => String(args[i++]));
};

const text = (a) => announceText(a, tr);

// ---- pure rules -------------------------------------------------------------------------------------------------------------------------------------------------

test("suggestFreeName is the engine's suggest_free_name (the same table as edit_session_test.cpp test_name_suggestion_pure)", () => {
  const taken = new Set();
  const add = (n) => taken.add(foldName(n));
  assert.equal(suggestFreeName("Perfect", taken), "Perfect 2");
  add("Perfect");
  add("Perfect 2");
  assert.equal(suggestFreeName("Perfect", taken), "Perfect 3");
  assert.equal(suggestFreeName("Perfect 2", taken), "Perfect 3");
  assert.equal(suggestFreeName("  perfect 7 ", taken), "perfect 3");
  assert.equal(suggestFreeName("Tune 12x", taken), "Tune 12x 2");
  assert.equal(suggestFreeName("", taken), "Live Set 2");
  assert.equal(suggestFreeName("42", taken), "42 2"); // no space before the digits: not a suffix
  assert.equal(suggestFreeName("x 1234567890", new Set()), "x 1234567890 2"); // more than 9 digits: not a suffix
  const big = new Set([foldName("Perfect")]);
  for (let i = 2; i <= 1000; i++) big.add(foldName(`Perfect ${i}`));
  assert.equal(suggestFreeName("Perfect 5", big), "Perfect 1001");
  const long = "é".repeat(32); // 64 bytes: " 2" does not fit, so it is cut on a character boundary
  const cut = suggestFreeName(long, new Set());
  assert.equal(new TextEncoder().encode(cut).length, 64);
  assert.ok(cut.endsWith(" 2") && nameProblem(cut) === "");
  assert.equal(suggestFreeName("perfect", new Set([foldName("PERFECT 2")])), "perfect 3"); // case-insensitive collision of the candidate itself
});

test("name rules: trimmed, 1..64 bytes of UTF-8, no control characters, ASCII-only folding", () => {
  assert.equal(trimName(" \t Perfect \n"), "Perfect");
  assert.equal(nameProblem(""), "empty");
  assert.equal(nameProblem("a".repeat(64)), "");
  assert.equal(nameProblem("a".repeat(65)), "too_long");
  assert.equal(nameProblem("é".repeat(33)), "too_long"); // 66 bytes
  assert.equal(nameProblem("é".repeat(32)), "");
  assert.equal(nameProblem("a\u0007b"), "invalid");
  assert.equal(nameProblem("a\u007fb"), "invalid");
  assert.equal(nameProblem("a\ud800b"), "invalid"); // a lone surrogate is not UTF-8
  assert.equal(nameProblem("😀"), "");
  assert.equal(foldName(" Perfect "), "PERFECT");
  assert.notEqual(foldName("é"), foldName("É")); // the engine folds ASCII only
});

test("writeAvailability: enabled only for the Live Set that sounds with unsaved edits; every other case says why", () => {
  const unit = new FakeUnit({ liveSetId: "ls-1" });
  const clean = unit.state();
  assert.deepEqual(writeAvailability(clean, "ls-1"), { enabled: false, reason: "studio.write.tooltip_nothing" });
  assert.deepEqual(writeAvailability(null, "ls-1"), { enabled: false, reason: "studio.write.tooltip_nothing" });
  unit.dirty = true;
  const dirty = unit.state();
  assert.deepEqual(writeAvailability(dirty, "ls-1"), { enabled: true, reason: "" });
  // the edits are on the Live Set that sounds: another Live Set shown has none of its own to write
  assert.deepEqual(writeAvailability(dirty, "ls-other"), { enabled: false, reason: "studio.write.tooltip_not_sounding" });
  assert.deepEqual(writeAvailability(clean, "ls-other"), { enabled: false, reason: "studio.write.tooltip_nothing" });
  unit.sounding = false;
  assert.equal(writeAvailability(unit.state(), "ls-1").enabled, false);
  assert.equal(writeAvailability(dirty, "ls-1", { writing: true }).reason, "studio.write.tooltip_writing");
  assert.equal(writeAvailability(dirty, "ls-1", { unconfirmed: true }).reason, "studio.write.tooltip_unconfirmed");
  // an older unit (no edit session) never offers a Write
  assert.equal(writeAvailability({ ...dirty, edits: { available: false, current_dirty: true } }, "ls-1").enabled, false);
  assert.equal(REASON_KEYS.timbre_change, "studio.write.scene_disabled_timbre");
});

test("changeLabel: localised from the stable key with 1-based numbers; an unknown key falls back to the unit's own text", () => {
  assert.equal(changeLabel({ key: "tempo" }, tr), "Tempo");
  assert.equal(changeLabel({ key: "part.volume", part: 1 }, tr), "Part 2: volume");
  assert.equal(changeLabel({ key: "part.timbre", part: 0 }, tr), "Part 1: timbre");
  assert.equal(changeLabel({ key: "scene.part.send_reverb", part: 2, scene: 1 }, tr), "Scene 2, part 3: reverb send");
  assert.equal(changeLabel({ key: "scene.name", scene: 0 }, tr), "Scene 1: name");
  assert.equal(changeLabel({ key: "assign.slot", slot: 0 }, tr), "ASSIGN K1");
  assert.equal(changeLabel({ key: "part.param", part: 3, param_id: 100 }, tr), `Part 4: ${tr("studio.param.drawbar_16")}`);
  assert.equal(changeLabel({ key: "part.something_new", part: 3, text: "Part 4 something new" }, tr), "Part 4 something new");
  assert.equal(changeLabel({ key: "part.volume", part: null, text: "Part ? volume" }, tr), "Part ? volume");
  assert.equal(changeLabel({ key: "future.key" }, tr), "future.key");
});

test("every locale key this feature can ask for exists in en, es and pt-BR", () => {
  const keys = new Set(Object.values(REASON_KEYS));
  for (const f of ["write_to_velora.js", "write_to_velora_dom.js"]) {
    const src = fs.readFileSync(new URL(`../src/${f}`, import.meta.url), "utf8");
    for (const m of src.matchAll(/"(studio\.(?:write|footer\.writing)[a-z0-9_.]*)"/g)) if (!m[1].endsWith("_") && !m[1].endsWith(".")) keys.add(m[1]);
  }
  for (const w of ["timbre", "engine", "zone", "octave", "transpose", "keyboard", "volume", "output", "inserts", "send_reverb", "send_delay", "arp", "name"]) keys.add(`studio.write.what.${w}`);
  for (const p of ["empty", "too_long", "invalid"]) keys.add(`studio.write.err.name_${p}`);
  assert.ok(keys.size > 50, `only ${keys.size} keys found`);
  for (const lang of Object.keys(CATALOGS)) for (const k of keys) assert.ok(k in CATALOGS[lang], `${lang}: ${k}`);
});

// ---- the dialog's content ---------------------------------------------------------------------------------------------------------------------------------------

test("nothing to write: the dialog does not open and nothing is sent", async () => {
  const r = await rig();
  assert.equal(r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" }), false);
  assert.equal(r.wc.isOpen, false);
  assert.deepEqual(r.unit.writeLog, []);
  assert.equal(r.wc.availability("ls-1").reason, "studio.write.tooltip_nothing");
});

test("opens on Whole Live Set with the change list and the device's scopes; Scene is enabled while no timbre changed", async () => {
  const r = await rig();
  await r.dirtyIt();
  await r.studioEdit("SetSendLevels", { part: 1, reverb: 0.2, delay: 0.1 });
  await settle();
  assert.equal(r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" }), true);
  const v = r.wc.view();
  assert.equal(v.scope, SCOPE.WHOLE);
  assert.deepEqual(v.scopes.map((s) => [s.id, s.enabled]), [[SCOPE.SCENE, true], [SCOPE.WHOLE, true], [SCOPE.NEW, true]]);
  assert.deepEqual(v.changes.map((c) => changeLabel(c, tr)), ["Part 1: volume", "Part 2: reverb send"]);
  assert.equal(v.count, 2);
  assert.equal(v.liveSetName, "Perfect");
  assert.equal(v.canConfirm, true);
  assert.equal(r.wc.open({ liveSetId: "ls-1" }), false); // already open
  assert.equal(r.wc.setScope(SCOPE.SCENE), true);
  assert.equal(r.wc.view().scope, SCOPE.SCENE);
  assert.deepEqual(r.unit.writeLog, []); // nothing is sent before the player confirms
});

test("Scene is disabled with the device's reason when the edits include a timbre change, and cannot be chosen", async () => {
  const r = await rig({ unitOptions: { empties: [5] } });
  await r.dirtyIt();
  await r.studioEdit("SetPartTimbre", { part: 5, library_id: "lib-a", instrument_key: "grand" });
  await settle();
  assert.equal(r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" }), true);
  const scene = r.wc.view().scopes[0];
  assert.equal(scene.enabled, false);
  assert.equal(scene.reasonKey, "studio.write.scene_disabled_timbre");
  assert.equal(tr(scene.reasonKey), "Timbre changes are saved with the Live Set"); // the device's own wording
  assert.equal(r.wc.setScope(SCOPE.SCENE), false);
  assert.equal(r.wc.view().scope, SCOPE.WHOLE);
  assert.equal(r.wc.setScope(SCOPE.NEW), true);
  assert.equal(r.wc.setScope(SCOPE.WHOLE), true);
  assert.deepEqual(r.unit.writeLog, []);
});

test("the other reasons Scene is unavailable are named too (no scene, no scenes, the scene is not in the saved file)", () => {
  const unit = new FakeUnit({ liveSetId: "ls-1" });
  unit.dirty = true;
  for (const [reason, key] of [["no_scene", "studio.write.scene_disabled_no_scene"], ["no_scenes", "studio.write.scene_disabled_no_scenes"], ["scene_not_in_saved", "studio.write.scene_disabled_not_saved"]]) {
    const st = unit.state();
    st.edits.scene_write_blocked_reason = reason;
    st.edits.scene_write_available = false;
    const wc = new WriteController({ request: async () => null, state: () => st });
    assert.ok(wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" }));
    assert.equal(wc.view().scopes[0].reasonKey, key);
    wc.cancel();
  }
});

test("a factory Live Set is saved with Save as new only: Scene and Whole are disabled with the reason and the dialog opens on Save as new", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Factory piano", factory: true });
  const v = r.wc.view();
  assert.equal(v.scope, SCOPE.NEW);
  assert.deepEqual(v.scopes.map((s) => s.enabled), [false, false, true]);
  assert.equal(v.scopes[1].reasonKey, "studio.write.scope_disabled_factory");
  assert.equal(r.wc.setScope(SCOPE.WHOLE), false);
});

test("a long change list says how many more there are (the unit sends at most 32 items)", async () => {
  const r = await rig();
  await r.dirtyIt();
  const st = r.unit.state();
  st.edits.change_count = 41;
  st.edits.change_summary = Array.from({ length: 32 }, (_, i) => ({ key: "part.volume", part: i % 8 }));
  const wc = new WriteController({ request: async () => null, state: () => st });
  wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  const v = wc.view();
  assert.equal(v.changes.length, 32);
  assert.equal(v.more, 9);
  assert.equal(v.count, 41);
});

// ---- Save as new -------------------------------------------------------------------------------------------------------------------------------------------------

test("Save as new suggests a free name, rejects a name in use with the device message and sends nothing for it", async () => {
  const r = await rig();
  r.unit.takenNames = new Set([foldName("Perfect"), foldName("Perfect 2")]);
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(r.wc.view().name, "Perfect 3"); // the first free name
  r.wc.setScope(SCOPE.NEW);
  r.wc.setName("  perfect 2 ");
  assert.equal(await r.wc.confirm(), "invalid");
  const v = r.wc.view();
  assert.equal(tr(v.nameError.key, ...v.nameError.args), "A Live Set named 'perfect 2' is already in this setlist"); // the device's message
  assert.equal(v.name, "perfect 3"); // the free name (built from what was typed, like the engine's suggestion) goes into the field
  assert.equal(r.wc.status, "open");
  assert.deepEqual(r.unit.writeLog, []);
  r.wc.setName("");
  assert.equal(await r.wc.confirm(), "invalid");
  assert.equal(r.wc.view().nameError.key, "studio.write.err.name_empty");
  r.wc.setName("x".repeat(65));
  assert.equal(await r.wc.confirm(), "invalid");
  assert.equal(r.wc.view().nameError.key, "studio.write.err.name_too_long");
  r.wc.setName("bad\u0001name");
  assert.equal(await r.wc.confirm(), "invalid");
  assert.equal(r.wc.view().nameError.key, "studio.write.err.name_invalid");
  assert.deepEqual(r.unit.writeLog, []);
});

test("Save as new sends the trimmed name; the sounding Live Set keeps its unsaved edits (the diamond stays) and the announcement says so", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  r.wc.setScope(SCOPE.NEW);
  r.wc.setName("  Ballad take  ");
  assert.equal(await r.wc.confirm(), "written");
  assert.deepEqual(r.unit.writeLog, [{ scope: 3, new_name: "Ballad take" }]);
  assert.equal(r.state.edits.current_dirty, true); // the engine's rule: the current Live Set keeps its edits
  assert.equal(r.unit.writesDone, 1);
  assert.equal(text({ parts: r.ev.announces.at(-1).parts }), "Written to Velora as the new Live Set 'Ballad take'. This Live Set still has its unsaved changes (◆).");
  assert.deepEqual(r.ev.closes, ["written"]);
});

test("a name that is free here but taken on the unit by the time the Write arrives (the race): the engine's NAME_IN_USE keeps the dialog open with its suggestion, and the next try works", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  r.wc.setScope(SCOPE.NEW);
  r.wc.setName("Encore");
  r.unit.takenNames = new Set([foldName("Encore")]); // someone created it meanwhile; Studio's hint (takenNames) does not know yet
  r.wc.takenNames = () => new Set();
  assert.equal(await r.wc.confirm(), "inline");
  assert.equal(r.wc.status, "open");
  const v = r.wc.view();
  assert.equal(tr(v.nameError.key, ...v.nameError.args), "A Live Set named 'Encore' is already in this setlist");
  assert.equal(v.name, "Encore 2");
  assert.equal(r.unit.writesDone, 0);
  assert.equal(await r.wc.confirm(), "written");
  assert.equal(r.unit.writeLog.length, 2);
  assert.equal(r.unit.writeLog[1].new_name, "Encore 2");
});

// ---- the Write ------------------------------------------------------------------------------------------------------------------------------------------------------

test("Whole Live Set: ONE WriteLiveSet with base_revision; the footer says WRITING while it is out; success clears the diamond on the unit and in the State; Written to Velora is announced", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  let release;
  const gate = new Promise((res) => { release = res; });
  r.unit.writeHook = async () => { await gate; }; // holds the Write out
  const p = r.wc.confirm();
  await settle();
  assert.equal(r.wc.writing, true); // Footer WRITING TO VELORA…, the Primary Buttons disabled
  assert.equal(r.wc.availability("ls-1").enabled, false);
  assert.equal(r.wc.view().canConfirm, false);
  assert.equal(r.wc.cancel(), false); // a Write that is out is not taken back
  assert.equal(await r.wc.confirm(), "ignored"); // the double click
  assert.equal(await r.wc.confirm(), "ignored");
  release();
  assert.equal(await p, "written");
  assert.equal(r.unit.writeLog.length, 1);
  assert.deepEqual(r.unit.writeLog[0], { scope: 2 });
  const sent = r.unit.log.find((l) => l[0] === "WriteLiveSet");
  assert.equal(sent[2], r.unit.liveRevision); // base_revision is the Live Set revision the view was loaded at
  assert.ok(sent[2] > 0);
  assert.equal(r.wc.writing, false);
  assert.equal(r.state.edits.current_dirty, false); // ◆ cleared on the device (State.edits) ...
  assert.deepEqual(r.state.edits.dirty_live_set_ids, []);
  assert.equal(r.unit.dirty, false);
  assert.equal(r.wc.availability("ls-1").reason, "studio.write.tooltip_nothing"); // ... and Studio's button follows
  assert.equal(text({ parts: r.ev.announces.at(-1).parts }), "Written to Velora");
  assert.deepEqual(r.ev.closes, ["written"]);
  assert.equal(r.ev.written.length, 1);
  assert.equal(r.ev.written[0].currentDirty, false);
  assert.equal(r.wc.banner, null);
  // and Studio's own edits still work after the Write: it did not move the Live Set revision
  assert.equal(r.pe.writeContext("ls-1").base, r.unit.liveRevision);
  assert.equal((await r.studioEdit("SetPartVolume", { part: 0, volume: 0.9 })).body.Ack !== undefined, true);
});

test("Scene scope: writes the current scene only; what is left is announced and the diamond stays; nothing in the scene says so", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.unit.changes[0].scene = 0; // the unit stored the edit in scene 1
  r.unit.changes.push({ key: "master.param", part: null, param_id: 530, scene: null, slot: null, text: "" });
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  r.wc.setScope(SCOPE.SCENE);
  assert.equal(await r.wc.confirm(), "written");
  assert.deepEqual(r.unit.writeLog, [{ scope: 1 }]);
  assert.equal(r.state.edits.current_dirty, true); // the master edit is not in the scene: still unsaved
  assert.equal(text({ parts: r.ev.announces.at(-1).parts }), "Written to Velora Other changes are still unsaved (◆).");
  // the scene has nothing left: the unit says so and Studio does not claim a Write
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  r.wc.setScope(SCOPE.SCENE);
  assert.equal(await r.wc.confirm(), "nothing");
  assert.equal(text(r.ev.announces.at(-1)), "Nothing to write for this scope");
  assert.equal(r.unit.writesDone, 1);
});

test("a timbre change that arrives while the dialog is open takes Scene away: confirm refuses it with the reason and sends nothing", async () => {
  const r = await rig({ unitOptions: { empties: [5] } });
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  r.wc.setScope(SCOPE.SCENE);
  await r.studioEdit("SetPartTimbre", { part: 5, library_id: "lib-a", instrument_key: "grand" });
  await settle(); // State push: has_timbre_edit
  assert.equal(r.wc.view().scopes[0].enabled, false);
  assert.equal(await r.wc.confirm(), "invalid");
  assert.equal(tr(r.wc.view().inlineError.key), "Timbre changes are saved with the Live Set");
  assert.deepEqual(r.unit.writeLog, []);
  assert.equal(r.wc.status, "open");
  assert.equal(r.wc.setScope(SCOPE.WHOLE), true);
  assert.equal(await r.wc.confirm(), "written");
});

test("the engine's own refusal of Scene (a timbre change Studio had not heard of yet) keeps the dialog open on Whole with the reason", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  r.wc.setScope(SCOPE.SCENE);
  r.unit.hasTimbreEdit = true; // the unit has it; no State push told Studio yet
  assert.equal(await r.wc.confirm(), "inline");
  assert.equal(r.wc.status, "open");
  assert.equal(r.wc.view().scope, SCOPE.WHOLE);
  assert.equal(r.wc.view().scopes[0].enabled, false);
  assert.equal(tr(r.wc.view().inlineError.key), "Timbre changes are saved with the Live Set");
  assert.equal(r.unit.writesDone, 0);
  assert.equal(r.unit.writeLog.length, 1);
  assert.equal(await r.wc.confirm(), "written"); // Whole goes through
});

test("a flush comes first: a knob edit still waiting for its turn reaches the unit before the Write does", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  r.unit.delay = 3;
  r.pe.edit("p:100", 4); // sent coalesced: the request goes out on the sender's schedule
  r.pe.edit("p:100", 6);
  r.pe.edit("p:100", 7);
  assert.equal(r.pe.sender.busy, true);
  const p = r.wc.confirm();
  await r.clock.advance(400);
  assert.equal(await p, "written");
  const names = r.unit.log.map((l) => l[0]);
  const lastEdit = names.lastIndexOf("SetPerformanceParam");
  const write = names.indexOf("WriteLiveSet");
  assert.ok(lastEdit >= 0 && write > lastEdit, `order: ${names.join(",")}`);
  assert.equal(r.unit.log.filter((l) => l[0] === "SetPerformanceParam").at(-1)[1].value, 7); // the LAST value is what was on the unit when it saved
  assert.equal(r.unit.writeLog.length, 1);
});

test("a unit that does not answer the flush in time gets no Write: the dialog stays open and says so", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  r.wc.flush = async () => false;
  assert.equal(await r.wc.confirm(), "inline");
  assert.equal(r.wc.status, "open");
  assert.equal(r.wc.view().inlineError.key, "studio.write.err.flush");
  assert.deepEqual(r.unit.writeLog, []);
});

test("EditSender.whenIdle resolves when everything is answered, and false on its own timeout", async () => {
  const r = await rig();
  assert.equal(await r.pe.flush(50), true); // nothing waiting
  r.unit.delay = 40;
  r.pe.edit("p:100", 3);
  const slow = r.pe.flush(10);
  await r.clock.advance(20);
  assert.equal(await slow, false); // the unit has not answered within the bound
  assert.equal(await (async () => { const f = r.pe.flush(5000); await r.clock.advance(2000); return f; })(), true);
});

// ---- stale ---------------------------------------------------------------------------------------------------------------------------------------------------------------

test("a Write on an older view is refused (STALE_REVISION): nothing is written, the Reload banner of the Part Editor is raised, the edits stay on the unit", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  r.unit.deviceChange(); // a knob turned on the unit after Studio loaded the view
  await settle();
  assert.equal(await r.wc.confirm(), "stale");
  assert.equal(r.unit.staleRejected, 1);
  assert.equal(r.unit.writesDone, 0);
  assert.equal(r.unit.dirty, true);
  assert.ok(r.pe.stale, "the knobs are read-only and the banner is up");
  assert.equal(r.ev.stale.length, 1);
  assert.deepEqual(r.ev.closes, ["stale"]);
  assert.equal(text(r.ev.announces.at(-1)), "Changed on the Velora since you opened it. Nothing was written.");
  // after Reload the Write works (the view is current again)
  assert.equal(await r.pe.reload(), true);
  assert.equal(r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" }), true);
  assert.equal(await r.wc.confirm(), "written");
});

test("while the view is already known to be stale nothing is sent at all", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.unit.deviceChange();
  await settle();
  await r.studioEdit("SetPartVolume", { part: 0, volume: 0.2 }).catch(() => {});
  r.pe.edit("p:100", 3);
  await r.clock.advance(200);
  assert.ok(r.pe.stale);
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(await r.wc.confirm(), "stale");
  assert.deepEqual(r.unit.writeLog, []);
});

test("noteStale is ignored when the view has moved on since the Write was sent (Reload, another Live Set)", async () => {
  const r = await rig();
  const ctx = r.pe.writeContext("ls-1");
  assert.equal(r.pe.noteStale({ code: 47, details: {} }, ctx), true); // same view: the banner is raised
  assert.ok(r.pe.stale);
  assert.equal(await r.pe.reload(), true);
  assert.equal(r.pe.stale, null);
  assert.equal(r.pe.noteStale({ code: 47, details: {} }, ctx), false); // an answer about the view as it was before the Reload says nothing about the one shown now
  assert.equal(r.pe.stale, null);
  assert.equal(r.pe.noteStale({ code: 47, details: {} }, null), false);
});

test("a view whose revision is unknown (not adopted yet) is not written from: the dialog asks to wait", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.wc.writeContext = () => ({ base: 0, known: false, stale: false, token: 0 });
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(await r.wc.confirm(), "inline");
  assert.equal(r.wc.view().inlineError.key, "studio.write.err.reading");
  assert.deepEqual(r.unit.writeLog, []);
});

// ---- failures --------------------------------------------------------------------------------------------------------------------------------------------------------

test("storage full: a Studio Banner says so, the edits stay on the unit with the diamond, nothing was written; with space back the same Write works", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.unit.storageFull = true;
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(await r.wc.confirm(), "failed");
  assert.deepEqual(r.wc.banner, { kind: "storage_full", device: "Velora 73" });
  assert.equal(bannerText(r.wc.banner, tr), "Couldn't write to Velora 73. Not enough space to write. Free space in Settings › Storage & Library. Your edits are still on the Velora (◆).");
  assert.equal(r.unit.dirty, true);
  assert.equal(r.state.edits.current_dirty, true);
  assert.equal(r.unit.writesDone, 0);
  assert.equal(r.wc.writing, false);
  assert.deepEqual(r.ev.closes, ["failed"]);
  assert.equal(text(r.ev.announces.at(-1)).startsWith("Couldn't write to Velora 73."), true);
  assert.equal(r.wc.availability("ls-1").enabled, true); // write again
  r.unit.storageFull = false;
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(await r.wc.confirm(), "written");
  assert.equal(r.wc.banner, null); // a success clears the banner
});

test("typed refusals: busy, storage failure, no Live Set... each is a banner with its reason, the edits stay, and none is reported as written", async () => {
  for (const [code, reasonKey] of [[13, "studio.write.reason.busy"], [33, "studio.write.reason.storage_failed"], [17, "studio.write.reason.no_live_set"], [29, "studio.write.reason.limit_reached"], [5, "studio.write.reason.unavailable"], [99, "studio.write.reason.other"]]) {
    const r = await rig();
    await r.dirtyIt();
    r.unit.writeHook = async () => ({ request_id: 0, revision: 1, body: { Error: { code, message: "injected", details: {} } } });
    r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
    assert.equal(await r.wc.confirm(), "failed", `code ${code}`);
    assert.deepEqual(r.wc.banner, { kind: "error", reasonKey, device: "Velora 73" });
    assert.match(bannerText(r.wc.banner, tr), /^Couldn't write to Velora 73: .+\. Your edits are still on the Velora \(◆\)\.$/);
    assert.equal(r.ev.written.length, 0);
    assert.equal(r.unit.dirty, true);
  }
});

test("an Ack or an empty answer is not a Write result: success is never claimed for it", async () => {
  for (const answer of [{ request_id: 0, revision: 1, body: { Ack: {} } }, { request_id: 0, revision: 1 }, null]) {
    const r = await rig();
    await r.dirtyIt();
    r.unit.writeHook = async () => answer || { request_id: 0, revision: 1, body: {} };
    r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
    assert.equal(await r.wc.confirm(), "failed");
    assert.equal(r.wc.banner.reasonKey, "studio.write.reason.no_answer");
    assert.equal(r.ev.written.length, 0);
    assert.ok(!r.ev.announces.some((a) => a.parts));
  }
});

test("the bridge could not reach the engine (connect_failed): definitely not written, the banner is the device's connection-lost sentence", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.unit.writeHook = async () => ({ request_id: 0, revision: 1, body: { Error: { code: CODE.UPSTREAM_UNAVAILABLE, message: "no", details: { upstream: "engine", reason: "connect_failed" } } } });
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(await r.wc.confirm(), "failed");
  assert.equal(bannerText(r.wc.banner, tr), "Couldn't write to Velora 73: connection lost. Your edits are still on the Velora (◆). Reconnect and write again.");
  assert.equal(r.wc.unconfirmed, false);
});

test("the link was already down when Write was pressed (the request never left): the same definite banner", async () => {
  const r = await rig({ wire: async () => { throw new Error("not connected"); } });
  await r.dirtyIt();
  r.setPaired(false);
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(await r.wc.confirm(), "failed");
  assert.equal(r.wc.banner.kind, "connection");
  assert.equal(r.wc.unconfirmed, false);
});

// ---- the connection drops mid-write: the result is unknown ---------------------------------------------------------------------------------------------------

test("the connection drops while the Write is out and it did NOT land: nothing is claimed; after the reconnect Studio reads the unit and says the edits are still there", async () => {
  const r = await rig({ wire: async (m, unit) => { if (m.body.WriteLiveSet) throw new Error("connection lost"); return unit.request(m); } });
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(await r.wc.confirm(), "unknown");
  assert.equal(r.wc.banner.kind, "unknown");
  assert.match(bannerText(r.wc.banner, tr), /^Couldn't confirm the write to Velora 73: connection lost\. If it did not go through, your edits are still on the Velora \(◆\)\./);
  assert.equal(r.ev.written.length, 0);
  assert.ok(!r.ev.announces.some((a) => a.parts), "no success is announced");
  assert.equal(r.wc.writing, false);
  assert.equal(r.wc.unconfirmed, true);
  assert.equal(r.wc.availability("ls-1").reason, "studio.write.tooltip_unconfirmed"); // no second Write meanwhile
  assert.equal(r.wc.open({ liveSetId: "ls-1" }), false);
  // a State that arrives while the link has not been seen to drop and come back judges nothing
  r.wc.onState(r.state);
  assert.equal(r.wc.banner.kind, "unknown");
  // the cable is pulled and put back: the first State after the reconnect is the truth
  r.setPaired(false);
  r.wc.onLinkLost();
  r.setPaired(true);
  r.wc.onState(r.unit.state());
  assert.equal(r.wc.banner.kind, "not_landed");
  assert.equal(bannerText(r.wc.banner, tr), "The write did not reach Velora 73. Your edits are still on the Velora (◆). Write again.");
  assert.equal(r.wc.unconfirmed, false);
  assert.equal(r.ev.written.length, 0);
  assert.equal(r.wc.availability("ls-1").enabled, true);
});

test("the Write landed but its answer was lost with the cable: after the reconnect Studio finds writes_done moved, clears the banner and says it was written", async () => {
  const r = await rig({ wire: async (m, unit) => { const rep = await unit.request(m); if (m.body.WriteLiveSet) throw new Error("connection lost"); return rep; } });
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(await r.wc.confirm(), "unknown");
  assert.equal(r.unit.writesDone, 1); // it did land
  assert.equal(r.wc.banner.kind, "unknown");
  assert.equal(r.ev.written.length, 0); // but Studio did not know
  r.wc.onLinkLost();
  r.wc.onState(r.unit.state());
  assert.equal(r.wc.banner, null);
  assert.equal(r.ev.written.length, 1);
  assert.equal(r.ev.written[0].recovered, true);
  assert.equal(text(r.ev.announces.at(-1)), "Written to Velora. Studio confirmed it after reconnecting.");
  assert.equal(r.wc.unconfirmed, false);
});

test("the unit restarted meanwhile (its write counter went back): Studio says so and never claims the Write", async () => {
  const r = await rig({ wire: async (m, unit) => { if (m.body.WriteLiveSet) throw new Error("connection lost"); return unit.request(m); } });
  r.unit.writesDone = 5;
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(await r.wc.confirm(), "unknown");
  r.unit.writesDone = 0;
  r.unit.dirty = false;
  r.unit.changes = []; // a power cycle: the edits in memory are gone
  r.wc.onLinkLost();
  r.wc.onState(r.unit.state());
  assert.equal(r.wc.banner.kind, "restarted");
  assert.equal(r.ev.written.length, 0);
});

test("nothing landed and nothing is unsaved any more (someone saved on the unit): the banner says there is nothing left, not that edits remain", async () => {
  const r = await rig({ wire: async (m, unit) => { if (m.body.WriteLiveSet) throw new Error("connection lost"); return unit.request(m); } });
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  await r.wc.confirm();
  r.unit.dirty = false;
  r.unit.changes = [];
  r.wc.onLinkLost();
  r.wc.onState(r.unit.state());
  assert.equal(r.wc.banner.kind, "clean");
  assert.equal(bannerText(r.wc.banner, tr), "Velora 73 has nothing left to write.");
});

test("Check now reads the unit without waiting for a reconnect (a Write that timed out on a live link)", async () => {
  const r = await rig({ wire: async (m, unit) => { if (m.body.WriteLiveSet) { await unit.request(m); throw new Error("timeout"); } return unit.request(m); } });
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(await r.wc.confirm(), "unknown");
  assert.equal(await r.wc.recheck(), true);
  assert.equal(r.wc.banner, null);
  assert.equal(r.ev.written[0].recovered, true);
  // an unreachable unit leaves the judgement for later
  const q = await rig({ wire: async () => { throw new Error("timeout"); } });
  await q.dirtyIt();
  q.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(await q.wc.confirm(), "unknown");
  assert.equal(await q.wc.recheck(), false);
  assert.equal(q.wc.banner.kind, "unknown");
});

test("the bridge's own timeout (the engine never answered) is unknown too, never a refusal", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.unit.writeHook = async () => ({ request_id: 0, revision: 1, body: { Error: { code: CODE.UPSTREAM_UNAVAILABLE, message: "no reply", details: { upstream: "engine", reason: "timeout" } } } });
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(await r.wc.confirm(), "unknown");
  assert.equal(r.wc.banner.kind, "unknown");
});

test("the dialog closes when the link drops (nothing was sent); a Write that is out when the link drops stays out and is judged after", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  r.wc.onLinkLost();
  assert.equal(r.wc.isOpen, false);
  assert.deepEqual(r.ev.closes, ["changed"]);
  assert.equal(text(r.ev.announces.at(-1)), "Write dialog closed: the Velora disconnected. Nothing was written.");
  assert.deepEqual(r.unit.writeLog, []);
  // while a Write is out the dialog is not closed under it
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  r.unit.writeHook = () => new Promise(() => {}); // never answers
  r.wc.confirm();
  await settle();
  assert.equal(r.wc.writing, true);
  r.wc.onLinkLost();
  assert.equal(r.wc.writing, true);
});

// ---- the dialog follows the unit ------------------------------------------------------------------------------------------------------------------------------

test("the dialog closes by itself when what it was about is gone: the edits were written on the unit, or another Live Set sounds", async () => {
  let r = await rig();
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  r.unit.dirty = false;
  r.unit.changes = [];
  r.unit.push();
  assert.equal(r.wc.isOpen, false);
  assert.deepEqual(r.ev.closes, ["nothing"]);
  assert.equal(text(r.ev.announces.at(-1)), "Write dialog closed: nothing to write.");
  r = await rig();
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  r.unit.liveSetId = "ls-2";
  r.unit.push();
  assert.equal(r.wc.isOpen, false);
  assert.deepEqual(r.ev.closes, ["changed"]);
  assert.deepEqual(r.unit.writeLog, []);
});

test("Cancel closes without sending anything; the write button then opens it again", async () => {
  const r = await rig();
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(r.wc.cancel(), true);
  assert.deepEqual(r.ev.closes, ["cancelled"]);
  assert.deepEqual(r.unit.writeLog, []);
  assert.equal(r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" }), true);
});

test("writeSnapshot reads the State defensively (hostile and missing fields never throw)", () => {
  assert.equal(writeSnapshot(null, "x").known, false);
  assert.equal(writeSnapshot({ edits: null }, "x").dirty, false);
  const s = writeSnapshot({ live_set_loaded: true, live_set_id: "x", edits: { available: true, current_dirty: true, change_summary: [null, 5, { key: "tempo" }], change_count: "many" } }, "x");
  assert.equal(s.items.length, 1);
  assert.equal(s.count, 1);
  assert.equal(s.sounding && s.dirty, true);
});

test("the link dropped and came back before the failed request was reported (the drop was seen first): the next State judges the Write at once", async () => {
  let wcRef;
  const r = await rig({ wire: async (m, unit) => { if (m.body.WriteLiveSet) { wcRef.onLinkLost(); throw new Error("connection lost"); } return unit.request(m); } });
  wcRef = r.wc;
  await r.dirtyIt();
  r.wc.open({ liveSetId: "ls-1", liveSetName: "Perfect" });
  assert.equal(await r.wc.confirm(), "unknown");
  assert.equal(r.wc.unknown.linkLost, true); // paired again by now, but the drop happened while the Write was out
  r.wc.onState(r.unit.state());
  assert.equal(r.wc.banner.kind, "not_landed");
});
