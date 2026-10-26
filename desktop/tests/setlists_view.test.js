// Story 9.13: the Setlists view model against a stand-in engine with the store's semantics. `cd desktop && node --test tests`
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";

import {
  ERROR_CODE, LIMITS, SENT_MESSAGES, SetlistsController, errorMessage, isPermutation, moveItem, moveTargets, nameProblem, nudgeTarget, parseReply, req, slotToIndex, utf8Length, validateName,
} from "../src/setlists_view.js";
import { CODE, FakeEngine, sample } from "./support/fake_engine.js";

function make(engine = sample(), extra = {}) {
  const events = { announce: [], errors: [], notices: [], changes: 0 };
  const ctl = new SetlistsController({
    request: (m) => engine.request(m),
    onChange: () => { events.changes++; },
    onError: (e) => events.errors.push(e),
    onAnnounce: (a) => events.announce.push(a),
    onNotice: (n) => events.notices.push(n),
    missingLabel: () => "missing",
    refreshMinMs: 0,
    ...extra,
  });
  return { engine, ctl, events };
}
const names = (list) => list.map((x) => x.name);
const sent = (engine) => engine.log.map(([n]) => n).filter((n) => n !== "GetSetlists");

test("open: lists setlists with tags, counts, missing counts, loaded marker, All Live Sets; the first setlist's entries in order", async () => {
  const { engine, ctl } = make();
  engine.loaded = "s-two";
  assert.equal(await ctl.refreshNow(), true);
  assert.deepEqual(names(ctl.setlists), ["Friday", "Saturday", "Sunday"]);
  assert.deepEqual(ctl.setlists.map((s) => s.count), [3, 1, 2]);
  assert.deepEqual(ctl.setlists.map((s) => s.missing), [0, 0, 1]);
  assert.deepEqual(ctl.setlists[0].tagIds, ["t-gig"]);
  assert.deepEqual(ctl.setlists.map((s) => s.loaded), [false, true, false]);
  assert.equal(ctl.selectedId, "s-one");
  assert.deepEqual(names(ctl.entries), ["Alpha", "Bravo", "Charlie"]);
  assert.equal(ctl.all.length, 5);
  assert.deepEqual(names(ctl.tags), ["Gig", "Jazz"]);
  // vacuity guard: a view that read nothing must fail this file
  assert.ok(ctl.setlists.length > 0 && ctl.entries.length > 0 && ctl.hasData);
});

test("select shows another setlist's Live Sets, and a missing Live Set is listed flagged", async () => {
  const { ctl } = make();
  await ctl.refreshNow();
  await ctl.select("s-three");
  assert.deepEqual(ctl.entries.map((e) => [e.name, e.missing]), [["", true], ["Echo", false]]);
});

test("read failure: inline error, previous data kept", async () => {
  const { engine, ctl, events } = make();
  await ctl.refreshNow();
  const before = ctl.setlists.length;
  const orig = engine.request.bind(engine);
  engine.request = async (m) => { if (m.body.GetSetlists) throw new Error("link down"); return orig(m); };
  assert.equal(await ctl.refreshNow(), false);
  assert.equal(ctl.setlists.length, before);
  assert.equal(events.errors.at(-1).kind, "transport");
  assert.equal(events.errors.at(-1).read, true);
});

test("selected setlist deleted by the device: the view falls back to a setlist that exists", async () => {
  const { engine, ctl } = make();
  await ctl.refreshNow();
  engine.setlists = engine.setlists.filter((s) => s.id !== "s-one");
  assert.equal(await ctl.refreshNow(), true);
  assert.equal(ctl.selectedId, "s-two");
  assert.deepEqual(names(ctl.entries), ["Delta"]);
});

test("create setlist: CreateSetlist sent, list refreshes, the new setlist is selected; limit and name errors", async () => {
  const { engine, ctl, events } = make();
  await ctl.refreshNow();
  const r = await ctl.createSetlist("  Opening night  ");
  assert.equal(r.ok, true);
  assert.deepEqual(engine.log.find(([n]) => n === "CreateSetlist")[1], { name: "Opening night" }); // trimmed client-side
  assert.equal(ctl.selected.name, "Opening night");
  assert.equal(ctl.setlists.length, 4);
  assert.equal(events.announce.at(-1).key, "studio.setlists.announce_setlist_created");
  // bad names never reach the engine
  const n = engine.log.length;
  for (const bad of ["", "   ", "a\u0001b", "x".repeat(65), "\ud800"]) assert.equal((await ctl.createSetlist(bad)).ok, false);
  assert.equal(engine.log.length, n);
  // 64 BYTES, not characters: 22 three-byte characters are 66 bytes
  assert.equal((await ctl.createSetlist("€".repeat(22))).ok, false);
  assert.equal((await ctl.createSetlist("€".repeat(21))).ok, true); // 63 bytes
  // the engine is the authority for the setlist limit: typed error shown, nothing broken
  while (engine.setlists.length < 100) engine.setlists.push({ id: "x" + engine.setlists.length, name: "x", ids: [], tagIds: [] });
  await ctl.refreshNow();
  const lim = await ctl.createSetlist("one more");
  assert.equal(lim.ok, false);
  assert.equal(errorMessage(lim.error).key, "studio.setlists.err.limit_setlists");
});

test("engine typed errors are shown (limit details, invalid name, storage)", async () => {
  const { engine, ctl, events } = make();
  await ctl.refreshNow();
  engine.fail = () => CODE.STORAGE;
  const r = await ctl.createSetlist("Z");
  assert.equal(r.ok, false);
  assert.equal(errorMessage(events.errors.at(-1)).key, "studio.setlists.err.storage_failed");
  assert.equal(errorMessage({ kind: "device", code: ERROR_CODE.LIMIT_REACHED, details: { limit_name: "tags", limit: "64" } }).key, "studio.setlists.err.limit_tags");
  assert.deepEqual(errorMessage({ kind: "device", code: ERROR_CODE.LIMIT_REACHED, details: { limit_name: "tags_per_setlist", limit: "8" } }).args, [8]);
  assert.equal(errorMessage({ kind: "device", code: 999, message: "huh" }).key, "studio.setlists.err.generic");
});

test("rename / delete setlist: commands sent, list converges; a typed error rolls back", async () => {
  const { engine, ctl } = make();
  await ctl.refreshNow();
  assert.equal((await ctl.renameSetlist("s-two", "Sat night")).ok, true);
  assert.equal(engine.setlists[1].name, "Sat night");
  engine.fail = (n) => (n === "RenameSetlist" ? CODE.NO_SUCH_SETLIST : 0);
  const seen = [];
  ctl.onChange = () => seen.push(ctl.setlists.find((s) => s.id === "s-two").name);
  assert.equal((await ctl.renameSetlist("s-two", "Nope")).ok, false);
  assert.ok(seen.includes("Nope")); // optimistic
  assert.equal(ctl.setlists.find((s) => s.id === "s-two").name, "Sat night"); // rolled back / converged
  engine.fail = null;
  assert.equal((await ctl.deleteSetlist("s-one")).ok, true);
  assert.deepEqual(engine.setlists.map((s) => s.id), ["s-two", "s-three"]);
  assert.equal(ctl.selectedId, "s-two");
  assert.deepEqual(names(ctl.entries), ["Delta"]);
});

test("reorder setlists: the FULL permutation is sent, announced, and an error rolls back then converges", async () => {
  const { engine, ctl, events } = make();
  await ctl.refreshNow();
  assert.equal((await ctl.nudgeSetlist("s-one", "ArrowDown")).ok, true);
  const cmd = engine.log.find(([n]) => n === "ReorderSetlists")[1];
  assert.deepEqual(cmd.ids, ["s-two", "s-one", "s-three"]); // all ids, never partial
  assert.deepEqual(ctl.setlists.map((s) => s.id), ["s-two", "s-one", "s-three"]);
  assert.deepEqual(events.announce.at(-1), { key: "studio.setlists.announce_setlist_moved", args: ["Friday", 2, 3] });
  // error: optimistic order shown, then rolled back to the engine's
  engine.fail = () => CODE.INVALID_ORDER;
  const orders = [];
  ctl.onChange = () => orders.push(ctl.setlists.map((s) => s.id).join());
  const r = await ctl.moveSetlist("s-three", 0);
  assert.equal(r.ok, false);
  assert.equal(orders[0], "s-three,s-two,s-one");
  assert.equal(ctl.setlists.map((s) => s.id).join(), "s-two,s-one,s-three");
  assert.equal(errorMessage(ctl.error).key, "studio.setlists.err.invalid_order");
  // edges announce and send nothing
  engine.fail = null;
  const n = engine.log.length;
  const e = await ctl.nudgeSetlist("s-two", "ArrowUp");
  assert.equal(e.noop, true);
  assert.equal(engine.log.length, n);
  assert.equal(events.announce.at(-1).key, "studio.setlists.announce_at_start");
});

test("add / remove Live Sets: AddLiveSetToSetlist then re-read; duplicate and limit errors; removal never deletes the Live Set", async () => {
  const { engine, ctl } = make();
  await ctl.refreshNow();
  assert.equal((await ctl.addLiveSet("s-one", "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee")).ok, true);
  assert.deepEqual(names(ctl.entries), ["Alpha", "Bravo", "Charlie", "Echo"]);
  assert.deepEqual(engine.log.find(([n]) => n === "AddLiveSetToSetlist")[1], { setlist_id: "s-one", live_set_id: "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee" }); // absent index = append
  const dup = await ctl.addLiveSet("s-one", "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
  assert.equal(dup.ok, false);
  assert.equal(errorMessage(dup.error).key, "studio.setlists.err.duplicate");
  assert.equal((await ctl.removeLiveSet("s-one", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")).ok, true);
  assert.deepEqual(names(ctl.entries), ["Alpha", "Charlie", "Echo"]);
  assert.ok(engine.liveSets.some((l) => l.id === "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"), "the Live Set file stays");
  assert.ok(!sent(engine).includes("DeleteLiveSet"));
  // a missing Live Set can be removed
  await ctl.select("s-three");
  assert.equal((await ctl.removeLiveSet("s-three", "ffffffff-ffff-4fff-8fff-ffffffffffff")).ok, true);
  assert.deepEqual(names(ctl.entries), ["Echo"]);
  assert.equal(ctl.setlists.find((s) => s.id === "s-three").missing, 0);
});

test("reorder Live Sets: full ordered ids, never blocked by a missing one, rollback on error, announcement text args", async () => {
  const { engine, ctl, events } = make();
  await ctl.refreshNow();
  await ctl.select("s-three"); // [missing, Echo]
  assert.equal((await ctl.nudgeLiveSet("ffffffff-ffff-4fff-8fff-ffffffffffff", "ArrowDown")).ok, true);
  assert.deepEqual(engine.log.find(([n]) => n === "ReorderLiveSetsInSetlist")[1], { setlist_id: "s-three", live_set_ids: ["eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", "ffffffff-ffff-4fff-8fff-ffffffffffff"] });
  await ctl.select("s-one");
  assert.equal((await ctl.moveLiveSet("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", 2)).ok, true);
  assert.deepEqual(names(ctl.entries), ["Bravo", "Charlie", "Alpha"]);
  assert.deepEqual(events.announce.at(-1), { key: "studio.setlists.announce_moved", args: ["Alpha", 3, 3] });
  engine.fail = () => CODE.INVALID_ORDER;
  assert.equal((await ctl.moveLiveSet("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", 0)).ok, false);
  assert.deepEqual(names(ctl.entries), ["Bravo", "Charlie", "Alpha"]); // converged to the engine's order
});

test("Move to... another setlist: Add (at the start or end) THEN Remove; both commands, in that order", async () => {
  const { engine, ctl, events } = make();
  await ctl.refreshNow();
  const before = engine.log.length;
  assert.equal((await ctl.moveLiveSetToSetlist("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "s-two", "start")).ok, true);
  const cmds = engine.log.slice(before).filter(([n]) => n !== "GetSetlists");
  assert.deepEqual(cmds.map(([n]) => n), ["AddLiveSetToSetlist", "RemoveLiveSetFromSetlist"]);
  assert.deepEqual(cmds[0][1], { setlist_id: "s-two", live_set_id: "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", index: 0 });
  assert.deepEqual(engine.setlists[1].ids, ["bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "dddddddd-dddd-4ddd-8ddd-dddddddddddd"]);
  assert.deepEqual(engine.setlists[0].ids, ["aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "cccccccc-cccc-4ccc-8ccc-cccccccccccc"]);
  assert.deepEqual(events.announce.at(-1), { key: "studio.setlists.announce_moved_to_setlist", args: ["Bravo", "Saturday"] });
  // end of the target: no index
  await ctl.moveLiveSetToSetlist("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "s-two", "end");
  assert.deepEqual(engine.setlists[1].ids, ["bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "dddddddd-dddd-4ddd-8ddd-dddddddddddd", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"]);
});

test("Move to... partial failure (Add ok, Remove fails) leaves a consistent re-read state: in both setlists, never in neither", async () => {
  const { engine, ctl } = make();
  await ctl.refreshNow();
  engine.fail = (n) => (n === "RemoveLiveSetFromSetlist" ? CODE.STORAGE : 0);
  const r = await ctl.moveLiveSetToSetlist("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "s-two", "end");
  assert.equal(r.ok, false);
  assert.equal(errorMessage(r.error).key, "studio.setlists.err.partial_move");
  assert.deepEqual(engine.setlists[0].ids, ["aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "cccccccc-cccc-4ccc-8ccc-cccccccccccc"]);
  assert.deepEqual(engine.setlists[1].ids, ["dddddddd-dddd-4ddd-8ddd-dddddddddddd", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"]);
  assert.deepEqual(names(ctl.entries), ["Alpha", "Bravo", "Charlie"]); // what the engine says
  assert.equal(ctl.setlists[1].count, 2);
  // Add refused (duplicate): nothing removed
  engine.fail = null;
  const dup = await ctl.moveLiveSetToSetlist("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "s-two", "end");
  assert.equal(dup.ok, false);
  assert.deepEqual(engine.setlists[0].ids, ["aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "cccccccc-cccc-4ccc-8ccc-cccccccccccc"]);
});

test("create Live Set: CreateLiveSet, optionally added to the selected setlist; typed error shown", async () => {
  const { engine, ctl, events } = make();
  await ctl.refreshNow();
  const r = await ctl.createLiveSet("Foxtrot", "s-one");
  assert.equal(r.ok, true);
  assert.deepEqual(sent(engine).slice(-2), ["CreateLiveSet", "AddLiveSetToSetlist"]);
  assert.deepEqual(names(ctl.entries), ["Alpha", "Bravo", "Charlie", "Foxtrot"]);
  assert.ok(ctl.all.some((l) => l.name === "Foxtrot"));
  assert.equal(events.announce.at(-1).key, "studio.setlists.announce_live_set_created_in");
  const r2 = await ctl.createLiveSet("Golf", "");
  assert.equal(r2.ok, true);
  assert.equal(names(ctl.entries).includes("Golf"), false);
  assert.ok(ctl.all.some((l) => l.name === "Golf"));
  engine.fail = (n) => (n === "AddLiveSetToSetlist" ? CODE.LIMIT : 0);
  const r3 = await ctl.createLiveSet("Hotel", "s-one");
  assert.equal(r3.ok, false);
  assert.ok(ctl.all.some((l) => l.name === "Hotel"), "the Live Set exists even if adding it failed");
});

test("tags: create / rename / delete, set per setlist, cap of 8 (client) and the engine's typed error", async () => {
  const { engine, ctl } = make();
  await ctl.refreshNow();
  const made = [];
  for (let i = 0; i < 8; i++) made.push((await ctl.createTag("T" + i)).id);
  assert.equal(ctl.tags.length, 10);
  assert.equal((await ctl.setSetlistTags("s-two", made)).ok, true);
  assert.deepEqual(engine.setlists[1].tagIds, made);
  const n = engine.log.length;
  const nine = await ctl.setSetlistTags("s-two", [...made, "t-gig"]);
  assert.equal(nine.ok, false);
  assert.equal(engine.log.length, n, "never sent: 9 tags");
  assert.equal(errorMessage(nine.error).key, "studio.setlists.err.limit_tags_per_setlist");
  assert.equal((await ctl.renameTag(made[0], "Renamed")).ok, true);
  assert.equal(ctl.tags.find((t) => t.id === made[0]).name, "Renamed");
  assert.equal((await ctl.deleteTag(made[1])).ok, true);
  assert.equal(ctl.setlists[1].tagIds.includes(made[1]), false); // removed from every setlist
  assert.equal((await ctl.renameTag("t-nope", "X")).ok, false);
  engine.tags = engine.tags.slice(0, 2);
  while (engine.tags.length < 64) engine.tags.push({ id: "tt" + engine.tags.length, name: "q" });
  await ctl.refreshNow();
  assert.equal(errorMessage((await ctl.createTag("one more")).error).key, "studio.setlists.err.limit_tags");
});

test("a device change meanwhile: State.setlists.library_revision refreshes (throttled), selection kept; never while a mutation is pending", async () => {
  const { engine, ctl } = make();
  await ctl.refreshNow();
  await ctl.select("s-two");
  const reads = () => engine.log.filter(([n]) => n === "GetSetlists").length;
  const r0 = reads();
  ctl.onState({ setlists: { library_revision: ctl.libraryRevision, loaded_setlist_id: "" } });
  await new Promise((r) => setTimeout(r, 5));
  assert.equal(reads(), r0, "same key: no read");
  engine.setlists[0].name = "Friday (edited on the unit)";
  engine.revision++;
  ctl.onState({ setlists: { library_revision: engine.revision, loaded_setlist_id: "" } });
  await new Promise((r) => setTimeout(r, 20));
  assert.equal(ctl.setlists[0].name, "Friday (edited on the unit)");
  assert.equal(ctl.selectedId, "s-two", "selection kept while the setlist still exists");
  // a loaded-setlist change alone refreshes too
  engine.loaded = "s-three";
  ctl.onState({ setlists: { library_revision: engine.revision, loaded_setlist_id: "s-three" } });
  await new Promise((r) => setTimeout(r, 20));
  assert.equal(ctl.setlists[2].loaded, true);
});

test("serialisation: queued mutations go out one at a time in order; a read reply that arrives meanwhile is dropped", async () => {
  const engine = sample();
  engine.delay = 3;
  const { ctl } = make(engine);
  await ctl.refreshNow();
  const a = ctl.nudgeLiveSet("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "ArrowDown");
  const b = ctl.nudgeLiveSet("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "ArrowDown");
  ctl.refreshNow(); // a stale read in the middle of it all
  const [ra, rb] = await Promise.all([a, b]);
  assert.equal(ra.ok && rb.ok, true);
  assert.deepEqual(engine.setlists[0].ids, ["bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"]);
  assert.deepEqual(names(ctl.entries), ["Bravo", "Charlie", "Alpha"]);
  const reorders = engine.log.filter(([n]) => n === "ReorderLiveSetsInSetlist").map(([, b2]) => b2.live_set_ids.join());
  assert.deepEqual(reorders, ["bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb,aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa,cccccccc-cccc-4ccc-8ccc-cccccccccccc", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb,cccccccc-cccc-4ccc-8ccc-cccccccccccc,aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"]);
});

test("not-durable replies are surfaced as a notice, not an error", async () => {
  const { engine, ctl, events } = make();
  await ctl.refreshNow();
  const orig = engine.handle.bind(engine);
  engine.handle = (n, b) => { const r = orig(n, b); if (r.body.Ack) r.body.Ack.not_durable = true; return r; };
  assert.equal((await ctl.renameSetlist("s-one", "Fri")).ok, true);
  assert.equal(events.notices.at(-1).key, "studio.setlists.not_durable");
  assert.equal(ctl.error, null);
});

test("reset drops everything and ignores a late reply", async () => {
  const engine = sample();
  engine.delay = 5;
  const { ctl } = make(engine);
  const p = ctl.refreshNow();
  ctl.reset();
  await p;
  assert.equal(ctl.setlists.length, 0);
  assert.equal(ctl.hasData, false);
});

// ---- pure helpers ----

test("validateName: trim, 1..64 UTF-8 bytes, no control characters, well-formed UTF-16", () => {
  assert.deepEqual(validateName("  Set A "), { ok: true, name: "Set A" });
  assert.equal(validateName("").reason, "empty");
  assert.equal(validateName("\t \n").reason, "empty");
  assert.equal(validateName("a\u0000b").reason, "control");
  assert.equal(validateName("a\u007fb").reason, "control");
  assert.equal(validateName("x".repeat(64)).ok, true);
  assert.equal(validateName("x".repeat(65)).reason, "too_long");
  assert.equal(validateName("🎹".repeat(16)).ok, true); // 4 bytes each: 64
  assert.equal(validateName("🎹".repeat(17)).reason, "too_long");
  assert.equal(validateName("a\udc00").reason, "encoding");
  assert.equal(validateName(undefined).reason, "empty");
  assert.equal(utf8Length("é€🎹"), 2 + 3 + 4);
  assert.equal(nameProblem("too_long").args[0], 64);
  assert.equal(LIMITS.maxSetlists, 100);
  assert.equal(LIMITS.maxTags, 64);
  assert.equal(LIMITS.maxLiveSetsPerSetlist, 256);
  assert.equal(LIMITS.maxTagsPerSetlist, 8);
});

test("ordering helpers: moveItem, nudgeTarget, slotToIndex (drag gaps), isPermutation, moveTargets", () => {
  assert.deepEqual(moveItem(["a", "b", "c", "d"], 0, 2), ["b", "c", "a", "d"]);
  assert.deepEqual(moveItem(["a", "b", "c", "d"], 3, 0), ["d", "a", "b", "c"]);
  assert.deepEqual(moveItem(["a", "b"], 0, 9), ["b", "a"]);
  assert.deepEqual(moveItem(["a", "b"], 5, 0), ["a", "b"]);
  assert.equal(nudgeTarget(0, 3, "ArrowUp"), -1);
  assert.equal(nudgeTarget(1, 3, "ArrowUp"), 0);
  assert.equal(nudgeTarget(2, 3, "ArrowDown"), -1);
  assert.equal(nudgeTarget(1, 3, "ArrowDown"), 2);
  assert.equal(nudgeTarget(1, 3, "Home"), -1);
  // gaps: dropping on the gap before the item itself or right after it changes nothing
  assert.equal(slotToIndex(2, 2), 2);
  assert.equal(slotToIndex(2, 3), 2);
  assert.equal(slotToIndex(2, 0), 0);
  assert.equal(slotToIndex(0, 4), 3);
  assert.ok(isPermutation(["a", "b", "c"], ["c", "a", "b"]));
  assert.ok(!isPermutation(["a", "b", "c"], ["c", "a"]));
  assert.ok(!isPermutation(["a", "b"], ["a", "a"]));
  const items = moveTargets({
    setlists: [{ id: "s1", count: 3 }, { id: "s2", count: 1 }, { id: "s3", count: 256 }],
    selectedId: "s1", entries: [{ id: "a" }, { id: "b" }, { id: "c" }], liveSetId: "b", knownMembers: new Map([["s2", new Set(["b"])]]),
  });
  assert.deepEqual(items.filter((i) => i.kind === "position").map((i) => i.index), [0, 2]); // every other position of the same setlist
  const s2 = items.filter((i) => i.setlistId === "s2");
  assert.ok(s2.every((i) => i.disabled)); // it is already there
  assert.ok(items.filter((i) => i.setlistId === "s3").every((i) => i.disabled)); // full
  assert.ok(!items.some((i) => i.setlistId === "s1"));
});

test("parseReply / request builders: shapes the Rust client and the engine expect", () => {
  assert.deepEqual(parseReply({ body: { Ack: { not_durable: false } } }), { ok: true, kind: "Ack", id: "", name: "", notDurable: false, body: { not_durable: false } });
  assert.equal(parseReply({ body: { CreatedReply: { id: "x", name: "n", not_durable: true } } }).notDurable, true);
  const e = parseReply({ body: { Error: { code: 29, message: "m", details: { limit_name: "tags", limit: "64" } } } });
  assert.equal(e.ok, false);
  assert.equal(e.error.details.limit, "64");
  assert.equal(parseReply(null).ok, false);
  assert.deepEqual(req.addLiveSet("s", "l"), { body: { AddLiveSetToSetlist: { setlist_id: "s", live_set_id: "l" } } });
  assert.deepEqual(req.addLiveSet("s", "l", 0), { body: { AddLiveSetToSetlist: { setlist_id: "s", live_set_id: "l", index: 0 } } });
  assert.deepEqual(req.reorderSetlists(["a", "b"]), { body: { ReorderSetlists: { ids: ["a", "b"] } } });
  assert.deepEqual(req.setSetlistTags("s", ["t"]), { body: { SetSetlistTags: { setlist_id: "s", tag_ids: ["t"] } } });
});

test("never sends a load, step, jump, recall, write or Live Set deletion; every message used is a paired allow-listed bridge row", () => {
  const forbidden = ["LoadSetlist", "UnloadSetlist", "NextLiveSet", "PreviousLiveSet", "JumpToLiveSet", "RecallScene", "WriteLiveSet", "DeleteLiveSet"];
  for (const f of forbidden) assert.ok(!SENT_MESSAGES.includes(f), f);
  const src = fs.readFileSync(new URL("../src/setlists_view.js", import.meta.url), "utf8");
  for (const f of forbidden) assert.ok(!new RegExp(`body:\\s*\\{\\s*${f}\\b`).test(src), `${f} must not be built`);
  // every message in SENT_MESSAGES appears as a row of the bridge's message table (sysd/src/bridge_server.cpp), the paired allow-list
  const bridge = fs.readFileSync(new URL("../../sysd/src/bridge_server.cpp", import.meta.url), "utf8");
  const snake = (s) => s.replace(/([a-z0-9])([A-Z])/g, "$1_$2").toLowerCase();
  for (const m of SENT_MESSAGES) {
    const row = new RegExp(`\\{B::k${m},\\s*"${snake(m)}",\\s*"liveset\\.v1",\\s*R::kEngine,\\s*false\\}`);
    assert.ok(row.test(bridge), `${m}: no paired row in the bridge message table`);
  }
  // and the request builders only produce those names
  const built = new Set();
  for (const fn of Object.values(req)) {
    const msg = fn("a", "b", "c") ;
    built.add(Object.keys(msg.body)[0]);
  }
  assert.deepEqual([...built].sort(), [...SENT_MESSAGES].sort());
});

const A = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", B = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", C = "cccccccc-cccc-4ccc-8ccc-cccccccccccc", D = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";

test("a throwing callback (renderer bug) never freezes the controller: pending returns to 0 and the final read still happens", async () => {
  const engine = sample();
  const ctl = new SetlistsController({
    request: (m) => engine.request(m),
    onChange: () => { throw new Error("render bug"); },
    onError: () => { throw new Error("error banner bug"); },
    onAnnounce: () => { throw new Error("announce bug"); },
    onNotice: () => { throw new Error("notice bug"); },
    refreshMinMs: 0,
  });
  await ctl.refreshNow();
  assert.equal(ctl.hasData, true);
  assert.equal((await ctl.nudgeLiveSet(A, "ArrowDown")).ok, true);
  assert.equal(ctl.pending, 0);
  assert.deepEqual(names(ctl.entries), ["Bravo", "Alpha", "Charlie"]);
  engine.fail = () => CODE.INVALID_ORDER;
  assert.equal((await ctl.nudgeLiveSet(A, "ArrowDown")).ok, false);
  assert.equal(ctl.pending, 0);
  engine.fail = null;
  engine.setlists[0].name = "from the unit";
  ctl.onState({ setlists: { library_revision: 99, loaded_setlist_id: "" } }); // reads are not dropped any more
  await new Promise((r) => setTimeout(r, 20));
  assert.equal(ctl.setlists[0].name, "from the unit");
});

test("a failed mutation restores only what it changed: a selection the user made meanwhile is kept", async () => {
  const engine = sample();
  engine.delay = 8;
  const { ctl } = make(engine);
  await ctl.refreshNow();
  engine.fail = (n) => (n === "ReorderLiveSetsInSetlist" ? CODE.INVALID_ORDER : 0);
  const moving = ctl.moveLiveSet(A, 2);
  assert.deepEqual(names(ctl.entries), ["Bravo", "Charlie", "Alpha"]); // optimistic
  const sel = ctl.select("s-two"); // the user switches setlists while the command is in flight
  const [r] = await Promise.all([moving, sel]);
  assert.equal(r.ok, false);
  assert.equal(ctl.selectedId, "s-two", "the rollback did not yank the selection back");
  assert.deepEqual(names(ctl.entries), ["Delta"]);
  assert.deepEqual(engine.setlists[0].ids, [A, B, C]);
});

test("rollback is observable: a failed removal shows the row gone, then back", async () => {
  const { engine, ctl } = make();
  await ctl.refreshNow();
  engine.fail = (n) => (n === "RemoveLiveSetFromSetlist" ? CODE.STORAGE : 0);
  const seen = [];
  ctl.onChange = () => seen.push(names(ctl.entries).join());
  assert.equal((await ctl.removeLiveSet("s-one", B)).ok, false);
  assert.equal(seen[0], "Alpha,Charlie");
  assert.equal(names(ctl.entries).join(), "Alpha,Bravo,Charlie");
  assert.equal(ctl.setlists[0].count, 3);
});

test("a move queued behind an add / create is built from the engine's list (no INVALID_ORDER)", async () => {
  const engine = sample();
  engine.delay = 4;
  const { ctl } = make(engine);
  await ctl.refreshNow();
  const add = ctl.addLiveSet("s-one", D);
  const move = ctl.nudgeLiveSet(A, "ArrowDown"); // called before the add is answered
  const [a, m] = await Promise.all([add, move]);
  assert.equal(a.ok && m.ok, true, JSON.stringify(m.error));
  assert.deepEqual(engine.setlists[0].ids, [B, A, C, D]);
  // the new Live Set itself moved straight after being created and added
  const made = ctl.createLiveSet("Zulu", "s-one");
  const last = ctl.moveLiveSet(D, 0);
  const [c, l] = await Promise.all([made, last]);
  assert.equal(c.ok && l.ok, true);
  assert.equal(engine.setlists[0].ids[0], D);
  assert.equal(engine.setlists[0].ids.length, 5);
  assert.ok(!engine.log.some(([n, b]) => n === "ReorderLiveSetsInSetlist" && b.live_set_ids.length === 4 && b.live_set_ids.includes(engine.liveSets.at(-1).id)), "no permutation was sent that predates the new id");
  // a new setlist, then a setlist reorder
  const mk = ctl.createSetlist("Fresh");
  const mv = ctl.moveSetlist("s-three", 0);
  const [x, y] = await Promise.all([mk, mv]);
  assert.equal(x.ok && y.ok, true);
  assert.equal(engine.setlists[0].id, "s-three");
  assert.equal(engine.setlists.length, 4);
});

test("a setlist reorder queued behind a delete sends the permutation without it", async () => {
  const engine = sample();
  engine.delay = 4;
  const { ctl } = make(engine);
  await ctl.refreshNow();
  const del = ctl.deleteSetlist("s-two");
  const mv = ctl.moveSetlist("s-three", 0);
  const [d, m] = await Promise.all([del, mv]);
  assert.equal(d.ok && m.ok, true, JSON.stringify(m.error));
  assert.deepEqual(engine.setlists.map((s) => s.id), ["s-three", "s-one"]);
});

test("missing Live Sets: no cross-setlist move, announcements and names use a fallback", async () => {
  const { engine, ctl, events } = make();
  await ctl.refreshNow();
  await ctl.select("s-three");
  const GONE = "ffffffff-ffff-4fff-8fff-ffffffffffff";
  const n = engine.log.length;
  assert.equal((await ctl.moveLiveSetToSetlist(GONE, "s-one", "end")).noop, true);
  assert.equal(engine.log.length, n);
  const t = moveTargets({ setlists: ctl.setlists, selectedId: "s-three", entries: ctl.entries, liveSetId: GONE });
  assert.deepEqual(t.map((x) => x.kind), ["position"]);
  assert.equal((await ctl.nudgeLiveSet(GONE, "ArrowDown")).ok, true);
  assert.deepEqual(events.announce.at(-1).args[0], "missing ffffffff");
});

test("reset clears the error through onError(null)", async () => {
  const { engine, ctl, events } = make();
  await ctl.refreshNow();
  engine.fail = () => CODE.STORAGE;
  await ctl.renameSetlist("s-one", "x");
  assert.notEqual(events.errors.at(-1), null);
  ctl.reset();
  assert.equal(events.errors.at(-1), null);
});

test("error mapping: storage full, busy, upstream unavailable, partial wins over transport, a mutation timeout says it may have been applied", () => {
  assert.equal(errorMessage({ kind: "device", code: 36 }).key, "studio.setlists.err.storage_failed");
  assert.equal(errorMessage({ kind: "device", code: 13 }).key, "studio.setlists.err.busy");
  assert.equal(errorMessage({ kind: "device", code: 44 }).key, "studio.setlists.err.upstream");
  assert.equal(errorMessage({ kind: "transport", message: "timeout", partial: true }).key, "studio.setlists.err.partial_move");
  assert.equal(errorMessage({ kind: "transport", message: "timeout" }).key, "studio.setlists.err.transport_changed");
  assert.equal(errorMessage({ kind: "transport", message: "timeout", read: true }).key, "studio.setlists.err.transport");
});

test("the engine's busy / storage-full / upstream errors roll the change back and are shown", async () => {
  for (const [code, key] of [[CODE.STORAGE_FULL, "storage_failed"], [CODE.BUSY, "busy"], [CODE.UPSTREAM, "upstream"]]) {
    const { engine, ctl } = make();
    await ctl.refreshNow();
    engine.fail = () => code;
    const r = await ctl.renameSetlist("s-one", "New name");
    assert.equal(r.ok, false);
    assert.equal(errorMessage(r.error).key, "studio.setlists.err." + key);
    assert.equal(ctl.setlists[0].name, "Friday");
  }
});

test("a Remove that times out after a good Add is reported as a partial move, and a transport timeout on a mutation says it may have applied", async () => {
  const { engine, ctl } = make();
  await ctl.refreshNow();
  const orig = engine.request.bind(engine);
  engine.request = async (m) => { if (m.body.RemoveLiveSetFromSetlist) throw new Error("timeout"); return orig(m); };
  const r = await ctl.moveLiveSetToSetlist(B, "s-two", "end");
  assert.equal(errorMessage(r.error).key, "studio.setlists.err.partial_move");
  engine.request = async (m) => { if (m.body.RenameSetlist) throw new Error("timeout"); return orig(m); };
  const r2 = await ctl.renameSetlist("s-one", "Z");
  assert.equal(errorMessage(r2.error).key, "studio.setlists.err.transport_changed");
});

test("the fake engine mirrors the store: limit before NO_SUCH_TAG, duplicate tag ids, non-UUID Live Set ids, All Live Sets order", async () => {
  const engine = sample();
  const nine = Array.from({ length: 9 }, (_, i) => "nope" + i);
  assert.equal((await engine.request(req.setSetlistTags("s-one", nine))).body.Error.code, CODE.LIMIT);
  assert.equal((await engine.request(req.setSetlistTags("s-one", ["t-gig", "t-gig"]))).body.Error.code, CODE.INVALID_ORDER);
  assert.equal((await engine.request(req.setSetlistTags("s-one", ["nope"]))).body.Error.code, 23);
  assert.equal((await engine.request(req.addLiveSet("s-two", "ls-1"))).body.Error.code, CODE.NO_SUCH_LIVE_SET);
  engine.liveSets.push({ id: "00000000-0000-4000-8000-000000000001", name: "alpha" }, { id: "00000000-0000-4000-8000-000000000002", name: "ALPHA" });
  const all = (await engine.request(req.get(""))).body.SetlistsReply.all_live_sets.map((l) => l.name);
  assert.deepEqual(all.slice(0, 3), ["alpha", "ALPHA", "Alpha"], "ASCII-folded names tie, then the id decides (0000... before aaaa...)");
  await assert.rejects(() => engine.request({ body: { LoadSetlist: { id: "s-one" } } }), /must never send/);
  await assert.rejects(() => engine.request({ body: { NextLiveSet: {} } }), /must never send/);
});

test("tags_set announcement is singular for one tag", async () => {
  const { ctl, events } = make();
  await ctl.refreshNow();
  await ctl.setSetlistTags("s-two", ["t-jazz"]);
  assert.equal(events.announce.at(-1).key, "studio.setlists.announce_tags_set_one");
  await ctl.setSetlistTags("s-two", []);
  assert.deepEqual(events.announce.at(-1), { key: "studio.setlists.announce_tags_set", args: ["Saturday", 0] });
});
