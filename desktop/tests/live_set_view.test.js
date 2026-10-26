// Story 9.8: the Live Set view model. `cd desktop && node --test tests`
import test from "node:test";
import assert from "node:assert/strict";

import {
  PART_SLOTS,
  SCENE_SLOTS,
  LiveSetViewController,
  createThrottledRunner,
  allLiveSetRows,
  applyState,
  buildModel,
  categoryLabelKey,
  diffModels,
  entryRows,
  keyRangeText,
  moveFocus,
  noteName,
  setlistRows,
  structuralSignature,
  viewRequest,
  setlistsRequest,
} from "../src/live_set_view.js";

const ID = "11111111-1111-4111-8111-111111111111";
const OTHER = "22222222-2222-4222-8222-222222222222";

function part(i, over = {}) {
  return { index: i, empty: false, on: true, engine_type_id: 1, timbre_name: `Timbre ${i}`, category: 1, volume: 0.5, low_key: 24, high_key: 96, organ_registration: "", missing_library: false, ...over };
}

// A mixed Live Set: piano, pad, organ (off), missing library, then empty parts; 3 scenes.
function view(over = {}) {
  return {
    live_set_id: ID,
    name: "Perfect",
    dirty: false,
    sounding: true,
    scenes: Array.from({ length: 8 }, (_, i) => ({ index: i, name: ["Intro", "Verse", "Chorus"][i] || "", empty: i >= 3 })),
    parts: [
      part(0, { timbre_name: "Concert Grand", category: 1, volume: 0.78 }),
      part(1, { timbre_name: "Warm Pad", category: 13, volume: 0.4, low_key: 0, high_key: 127 }),
      part(2, { engine_type_id: 2, timbre_name: "", category: 3, on: false, organ_registration: "88 8600 000", volume: 0.62 }),
      part(3, { timbre_name: "", category: 0, missing_library: true }),
      part(4, { empty: true, timbre_name: "", category: 0 }),
      part(5, { empty: true, timbre_name: "", category: 0 }),
      part(6, { empty: true, timbre_name: "", category: 0 }),
      part(7, { empty: true, timbre_name: "", category: 0 }),
    ],
    part_count: 8,
    scene_count: 3,
    factory: false,
    ...over,
  };
}

function state(over = {}) {
  return {
    revision: 5,
    live_set_loaded: true,
    live_set_id: ID,
    current_scene: 1,
    scene_count: 3,
    parts: Array.from({ length: 8 }, (_, i) => ({ part: i, volume: 0.5, keyboard_on: true, low_key: 24, high_key: 96, engine_type_id: 1, part_state: i === 3 ? 2 : i >= 4 ? 1 : 0, view_fields: true })),
    edits: { available: true, dirty_live_set_ids: [], current_dirty: false, change_summary: [], writes_done: 0 },
    ...over,
  };
}

test("note names: middle C is C4, the ends of the range, invalid input", () => {
  assert.equal(noteName(60), "C4");
  assert.equal(noteName(36), "C2");
  assert.equal(noteName(0), "C-1");
  assert.equal(noteName(127), "G9");
  assert.equal(noteName(61), "C#4");
  for (const bad of [-1, 128, 1.5, NaN, undefined, null, "60"]) assert.equal(noteName(bad), "");
  assert.equal(keyRangeText(36, 96), "C2–C7");
  assert.equal(keyRangeText(0, 127), "C-1–G9");
  assert.equal(keyRangeText(96, 36), "C7–C2"); // shown as given, not repaired
  assert.equal(keyRangeText(300, 1), "");
});

test("category label keys follow the locale stems; none and unknown are null", () => {
  assert.equal(categoryLabelKey(1), "category.piano.short");
  assert.equal(categoryLabelKey(13), "category.synth_pad.short");
  assert.equal(categoryLabelKey(17), "category.fx_ambient.short");
  for (const bad of [0, 18, -1, null, undefined, "1"]) assert.equal(categoryLabelKey(bad), null);
});

test("buildModel: header, 8 scene tiles (current none, empty dashed), 8 part strips with the specified content", () => {
  const m = buildModel(view({ dirty: true }));
  assert.equal(m.id, ID);
  assert.equal(m.name, "Perfect");
  assert.equal(m.dirty, true);
  assert.equal(m.scenes.length, SCENE_SLOTS);
  assert.equal(m.parts.length, PART_SLOTS);
  assert.deepEqual(m.scenes.map((s) => s.empty), [false, false, false, true, true, true, true, true]);
  assert.equal(m.scenes[1].name, "Verse");
  assert.equal(m.scenes[5].name, "");
  assert.ok(m.scenes.every((s) => !s.current)); // the static view has no current scene
  const [p0, p1, p2, p3, p4] = m.parts;
  assert.deepEqual([p0.status, p0.timbreName, p0.categoryKey, p0.volumePct, p0.keyRange, p0.engine], ["ready", "Concert Grand", "category.piano.short", 78, "C1–C7", "sample"]);
  assert.equal(p1.categoryKey, "category.synth_pad.short");
  assert.equal(p1.keyRange, "C-1–G9");
  assert.deepEqual([p2.engine, p2.status, p2.on, p2.registration, p2.categoryKey], ["organ", "off", false, "88 8600 000", "category.organ.short"]);
  assert.deepEqual([p3.status, p3.missing, p3.timbreName, p3.addTimbre], ["missing", true, "", false]);
  assert.deepEqual([p4.status, p4.addTimbre, p4.registration], ["empty", true, ""]);
  assert.equal(m.activeParts, 2); // piano + pad: the organ is off, one library is missing, four are empty
  assert.equal(m.hasMoreParts, false);
});

test("buildModel: a missing library never shows a name, even if the reply carries one", () => {
  const m = buildModel(view({ parts: [part(0, { missing_library: true, timbre_name: "Somebody Else's Timbre" })] }));
  assert.equal(m.parts[0].timbreName, "");
  assert.equal(m.parts[0].status, "missing");
});

test("buildModel: short, absent and hostile replies still give 8 + 8 tiles", () => {
  for (const v of [undefined, null, {}, { parts: "nope", scenes: 7 }, { parts: [null, 3, "x"], scenes: [null] }]) {
    const m = buildModel(v);
    assert.equal(m.scenes.length, 8);
    assert.equal(m.parts.length, 8);
    assert.ok(m.parts.every((p) => p.status === "empty"));
    assert.ok(m.scenes.every((s) => s.empty));
  }
  const m = buildModel({ live_set_id: ID, parts: [part(2)], scenes: [] });
  assert.equal(m.parts[2].status, "ready");
  assert.equal(m.parts[0].status, "empty");
  // volume out of range is clamped, not trusted
  assert.equal(buildModel({ parts: [part(0, { volume: 7 })] }).parts[0].volumePct, 100);
  assert.equal(buildModel({ parts: [part(0, { volume: -1 })] }).parts[0].volumePct, 0);
  assert.equal(buildModel({ parts: [part(0, { volume: NaN })] }).parts[0].volumePct, 0);
});

test("buildModel: Pro (16 parts) shows the first 8 and flags the rest", () => {
  const parts = Array.from({ length: 16 }, (_, i) => part(i));
  const m = buildModel(view({ parts, part_count: 16 }));
  assert.equal(m.parts.length, 8);
  assert.equal(m.partCount, 16);
  assert.equal(m.hasMoreParts, true);
  assert.equal(m.parts[7].index, 7);
});

test("buildModel does not modify the reply", () => {
  const v = view();
  const before = JSON.stringify(v);
  buildModel(v);
  assert.equal(JSON.stringify(v), before);
});

test("applyState: the sounding set follows scene, on/off, volume and zone; the base is untouched", () => {
  const base = buildModel(view());
  const frozen = JSON.stringify(base);
  const s = state({ current_scene: 2 });
  s.parts[0] = { ...s.parts[0], volume: 0.9, keyboard_on: false, low_key: 36, high_key: 84 };
  const m = applyState(base, s);
  assert.equal(JSON.stringify(base), frozen);
  assert.equal(m.sounding, true);
  assert.equal(m.currentScene, 2);
  assert.deepEqual(m.scenes.map((x) => x.current), [false, false, true, false, false, false, false, false]);
  assert.deepEqual([m.parts[0].on, m.parts[0].status, m.parts[0].volumePct, m.parts[0].keyRange], [false, "off", 90, "C2–C6"]);
  // an organ part turned on by the device
  const s2 = state();
  s2.parts[2] = { ...s2.parts[2], keyboard_on: true, engine_type_id: 2 };
  assert.equal(applyState(base, s2).parts[2].status, "ready");
  assert.equal(applyState(base, s2).activeParts, 3);
});

test("applyState: a Live Set that does not sound stays static (no current scene, no overlay), whatever the State says", () => {
  const base = buildModel(view({ sounding: false }));
  const m = applyState(base, state({ live_set_id: OTHER, current_scene: 1 }));
  assert.equal(m.sounding, false);
  assert.equal(m.currentScene, -1);
  assert.ok(m.scenes.every((s) => !s.current));
  assert.equal(m.parts[0].volumePct, 78); // the stored value, not the other set's State
  // ... and goes back to the static picture when the sounding set changes away
  const sounding = applyState(base, state({ current_scene: 1 }));
  assert.equal(sounding.currentScene, 1);
  const away = applyState(base, state({ live_set_id: OTHER }));
  assert.equal(away.currentScene, -1);
  assert.equal(away.parts[0].volumePct, 78);
  // nothing loaded
  assert.equal(applyState(base, state({ live_set_loaded: false })).sounding, false);
});

test("applyState: the unsaved flag follows State.edits; a view that already said dirty stays dirty without edits info", () => {
  const base = buildModel(view({ dirty: false }));
  assert.equal(applyState(base, state({ edits: { available: true, dirty_live_set_ids: [ID], current_dirty: true } })).dirty, true);
  assert.equal(applyState(base, state({ live_set_id: OTHER, edits: { available: true, dirty_live_set_ids: [ID], current_dirty: false } })).dirty, true); // dirty but not sounding
  assert.equal(applyState(base, state({ edits: { available: true, dirty_live_set_ids: [], current_dirty: false } })).dirty, false);
  const dirtyBase = buildModel(view({ dirty: true }));
  assert.equal(applyState(dirtyBase, state({ edits: { available: false } })).dirty, true); // no edit session: keep the view's flag
  assert.equal(applyState(dirtyBase, null).dirty, true);
  assert.equal(applyState(dirtyBase, { edits: { available: true, dirty_live_set_ids: [] } }).dirty, false); // the device says nothing is dirty: that wins
});

test("applyState: missing library from the State wins over a name; an empty sample timbre becomes '+ Add timbre'", () => {
  const base = buildModel(view());
  const s = state();
  s.parts[1] = { ...s.parts[1], part_state: 2 };
  s.parts[4] = { ...s.parts[4], part_state: 1 };
  const m = applyState(base, s);
  assert.deepEqual([m.parts[1].status, m.parts[1].timbreName], ["missing", ""]);
  assert.equal(m.parts[4].addTimbre, true);
  // part_state 1 on an organ part is not "empty"
  const s3 = state();
  s3.parts[2] = { ...s3.parts[2], part_state: 1, keyboard_on: false };
  assert.equal(applyState(base, s3).parts[2].status, "off");
});

test("applyState ignores hostile State parts (out of range, no numbers, not an object)", () => {
  const base = buildModel(view());
  const s = state({ parts: [null, 4, "x", { part: 99, volume: 1 }, { part: -1 }, { part: 0, volume: "loud", keyboard_on: "yes", low_key: "a" }] });
  const m = applyState(base, s);
  assert.equal(m.parts[0].volumePct, 78);
  assert.equal(m.parts[0].on, true);
  assert.equal(applyState(base, "nonsense").sounding, true); // no usable State: the view reply's own flag stands
  assert.equal(applyState(null, state()), null);
});

test("diffModels lists only the tiles that changed", () => {
  const base = buildModel(view());
  const a = applyState(base, state({ current_scene: 0 }));
  const s = state({ current_scene: 1 });
  s.parts[3] = { ...s.parts[3], volume: 0.1 }; // part 4 is missing: its volume still changes the strip
  const b = applyState(base, s);
  const d = diffModels(a, b);
  assert.deepEqual(d.scenes, [0, 1]);
  assert.deepEqual(d.parts, [3]);
  assert.equal(d.header, false);
  assert.equal(d.any, true);
  const none = diffModels(b, applyState(base, s));
  assert.deepEqual([none.header, none.scenes, none.parts, none.any], [false, [], [], false]);
  // first draw, and another Live Set: everything
  const first = diffModels(null, b);
  assert.equal(first.scenes.length, 8);
  assert.equal(first.parts.length, 8);
  assert.equal(first.header, true);
  const other = diffModels(b, buildModel(view({ live_set_id: OTHER })));
  assert.equal(other.parts.length, 8);
  // dirty flips: header only
  const dirty = applyState(base, state({ edits: { available: true, dirty_live_set_ids: [ID], current_dirty: true } }));
  const dh = diffModels(applyState(base, state()), dirty);
  assert.deepEqual([dh.header, dh.scenes, dh.parts], [true, [], []]);
});

test("moveFocus: arrows wrap in the one-row grids, Home / End jump, Up / Down stay, other keys are not handled", () => {
  assert.equal(moveFocus(0, "ArrowRight", 8), 1);
  assert.equal(moveFocus(7, "ArrowRight", 8), 0);
  assert.equal(moveFocus(0, "ArrowLeft", 8), 7);
  assert.equal(moveFocus(3, "ArrowLeft", 8), 2);
  assert.equal(moveFocus(5, "Home", 8), 0);
  assert.equal(moveFocus(2, "End", 8), 7);
  assert.equal(moveFocus(4, "ArrowUp", 8), 4);
  assert.equal(moveFocus(4, "ArrowDown", 8), 4);
  assert.equal(moveFocus(4, "Enter", 8), null);
  assert.equal(moveFocus(4, "a", 8), null);
  assert.equal(moveFocus(0, "ArrowRight", 0), null);
  assert.equal(moveFocus(NaN, "ArrowRight", 8), 1); // a lost focus index starts from the first
  assert.equal(moveFocus(99, "ArrowLeft", 8), 7);
});

test("structuralSignature moves only for what the in-place update cannot follow", () => {
  const sig = (s) => structuralSignature(s, ID);
  const base = state();
  assert.equal(sig(base), sig(state({ current_scene: 2, revision: 9 }))); // a scene change is applied in place
  const vol = state();
  vol.parts[0] = { ...vol.parts[0], volume: 0.1, keyboard_on: false };
  assert.equal(sig(base), sig(vol)); // volume and on/off too
  const dirty = state({ edits: { available: true, dirty_live_set_ids: [ID], current_dirty: true, change_summary: [{ key: "part.volume", part: 0 }], writes_done: 0 } });
  assert.notEqual(sig(base), sig(dirty)); // the flag flips
  const dirtyVol2 = state({ edits: { available: true, dirty_live_set_ids: [ID], current_dirty: true, change_summary: [{ key: "part.volume", part: 1 }], writes_done: 0 } });
  assert.equal(sig(dirty), sig(dirtyVol2)); // another volume edit does not
  const timbre = state({ edits: { available: true, dirty_live_set_ids: [ID], current_dirty: true, change_summary: [{ key: "part.volume", part: 0 }, { key: "part.timbre", part: 2 }], writes_done: 0 } });
  assert.notEqual(sig(dirty), sig(timbre)); // a timbre edit needs a re-read
  const written = state({ edits: { available: true, dirty_live_set_ids: [], current_dirty: false, change_summary: [], writes_done: 1 } });
  assert.notEqual(sig(base), sig(written)); // a Write
  assert.notEqual(sig(base), sig(state({ live_set_id: OTHER }))); // a jump away
  assert.equal(sig(null), "");
});

test("sidebar rows: setlists, entries, All Live Sets, hostile input", () => {
  const reply = {
    setlists: [{ id: "s1", name: "Wedding", live_set_count: 4, missing_count: 1, loaded: true }, { id: "s2", name: "Sunday" }],
    detail_setlist_id: "s1",
    entries: [{ live_set_id: ID, name: "Perfect" }, { live_set_id: OTHER, name: "Gone", missing: true }],
    all_live_sets: [{ live_set_id: ID, name: "Perfect" }],
  };
  assert.deepEqual(setlistRows(reply), [
    { id: "s1", name: "Wedding", count: 4, missing: 1, loaded: true },
    { id: "s2", name: "Sunday", count: 0, missing: 0, loaded: false },
  ]);
  assert.deepEqual(entryRows(reply), { setlistId: "s1", rows: [{ id: ID, name: "Perfect", missing: false }, { id: OTHER, name: "Gone", missing: true }] });
  assert.deepEqual(allLiveSetRows(reply), [{ id: ID, name: "Perfect", missing: false }]);
  for (const bad of [undefined, null, {}, { setlists: 3, entries: "x", all_live_sets: null }]) {
    assert.deepEqual(setlistRows(bad), []);
    assert.deepEqual(entryRows(bad).rows, []);
    assert.deepEqual(allLiveSetRows(bad), []);
  }
});

test("requests: the view request is GetLiveSetView only; the setlists request is a read", () => {
  assert.deepEqual(viewRequest(ID), { body: { GetLiveSetView: { live_set_id: ID } } });
  assert.deepEqual(setlistsRequest("", true), { body: { GetSetlists: { setlist_id: "", include_all_live_sets: true } } });
  assert.deepEqual(setlistsRequest("s1", false), { body: { GetSetlists: { setlist_id: "s1", include_all_live_sets: false } } });
});

// ---- the controller ----------------------------------------------------------------------------------------------------------------------------------------

function fakeClock() {
  const c = { t: 1000, now: () => c.t, advance(ms) { c.t += ms; } };
  return c;
}

function rig(over = {}) {
  const clock = fakeClock();
  const sent = [];
  const replies = new Map([[ID, { request_id: 1, body: { LiveSetViewReply: view() } }]]);
  const log = { models: [], errors: [], parts: [], loading: [], fetched: 0 };
  const timers = [];
  const ctl = new LiveSetViewController({
    clock: clock.now,
    setTimer: (f, ms) => { const t = { f, ms }; timers.push(t); return t; },
    clearTimer: (t) => { const i = timers.indexOf(t); if (i >= 0) timers.splice(i, 1); },
    onFetched: () => { log.fetched++; },
    request: async (r) => {
      sent.push(r);
      const id = r.body.GetLiveSetView && r.body.GetLiveSetView.live_set_id;
      if (over.request) return over.request(r, replies);
      if (!replies.has(id)) return { request_id: 1, body: { Error: { code: 20, message: "unknown Live Set id", details: { id } } } };
      return replies.get(id);
    },
    onModel: (m, ch) => {
      clock.advance(over.renderMs ?? 3); // rendering takes a few ms
      log.models.push({ m, ch });
    },
    onError: (e) => log.errors.push(e),
    onSelectPart: (i, p) => log.parts.push([i, p && p.status]),
    onLoading: (on, id) => log.loading.push([on, id]),
  });
  // Advances the fake clock and runs the timers that are due (the throttle's trailing run), then lets the promises settle.
  const tick = async (ms = 400) => {
    clock.advance(ms);
    const due = timers.splice(0);
    due.forEach((t) => t.f());
    await new Promise((r) => setTimeout(r, 0));
  };
  return { clock, sent, replies, log, ctl, timers, tick };
}

test("controller.select: asks for the view (read-only), shows it, and sends nothing else", async () => {
  const { ctl, sent, log } = rig();
  assert.equal(await ctl.select(ID), true);
  assert.equal(sent.length, 1);
  assert.deepEqual(Object.keys(sent[0].body), ["GetLiveSetView"]); // never Jump / Recall / Load / Write
  assert.equal(log.models.length, 1);
  assert.equal(log.models[0].m.name, "Perfect");
  assert.equal(log.models[0].ch.parts.length, 8);
  assert.deepEqual(log.loading, [[true, ID], [false, ID]]);
  assert.equal(ctl.selectedPart, 0);
  assert.ok(!/Jump|Recall|Load|Write|Next|Previous|Create|Delete/.test(JSON.stringify(sent)));
});

test("controller: a device error keeps the previous view and reports it; a transport failure too", async () => {
  const { ctl, log, replies } = rig();
  await ctl.select(ID);
  assert.equal(await ctl.select(OTHER), false); // unknown id
  assert.equal(log.errors.length, 1);
  assert.deepEqual([log.errors[0].kind, log.errors[0].code, log.errors[0].message], ["device", 20, "unknown Live Set id"]);
  assert.equal(ctl.model.id, ID); // previous view kept
  assert.equal(ctl.liveSetId, ID); // ... and it is still the Live Set shown (the sidebar highlight follows this)
  assert.equal(ctl.pendingId, "");
  const t = rig({ request: async () => { throw new Error("timeout"); } });
  assert.equal(await t.ctl.select(ID), false);
  assert.equal(t.log.errors[0].kind, "transport");
  assert.equal(t.log.errors[0].message, "timeout");
  assert.equal(t.ctl.model, null);
  // a reply for another Live Set than the one asked
  const w = rig({ request: async () => ({ body: { LiveSetViewReply: view({ live_set_id: OTHER }) } }) });
  assert.equal(await w.ctl.select(ID), false);
  assert.equal(w.log.errors[0].kind, "mismatch");
  // nothing set / garbage
  const g = rig({ request: async () => ({}) });
  assert.equal(await g.ctl.select(ID), false);
  assert.equal(g.log.errors[0].kind, "device");
  assert.equal(await g.ctl.select(""), false);
  void replies;
});

test("controller: a slow reply for an older selection never replaces the newer one", async () => {
  const pending = [];
  const { ctl, log } = rig({
    request: (r) =>
      new Promise((resolve) => {
        pending.push({ id: r.body.GetLiveSetView.live_set_id, resolve });
      }),
  });
  const a = ctl.select(ID);
  const b = ctl.select(OTHER);
  assert.equal(ctl.pendingId, OTHER);
  assert.equal(ctl.liveSetId, "");
  pending[1].resolve({ body: { LiveSetViewReply: view({ live_set_id: OTHER, name: "Other" }) } });
  await b;
  pending[0].resolve({ body: { LiveSetViewReply: view() } });
  await a;
  assert.equal(ctl.model.id, OTHER);
  assert.equal(log.models.length, 1);
});

test("controller.onState: deltas update in place and only touch what changed", async () => {
  const { ctl, log, clock } = rig();
  await ctl.select(ID);
  ctl.onState(state({ current_scene: 0 }));
  assert.equal(ctl.model.currentScene, 0);
  const n = log.models.length;
  // scene change
  clock.advance(500);
  ctl.onState(state({ current_scene: 2, revision: 6 }));
  assert.deepEqual(log.models.at(-1).ch.scenes, [0, 2]);
  assert.deepEqual(log.models.at(-1).ch.parts, []);
  // part off + volume
  const s = state({ current_scene: 2, revision: 7 });
  s.parts[0] = { ...s.parts[0], keyboard_on: false };
  s.parts[1] = { ...s.parts[1], volume: 0.9 };
  clock.advance(500);
  ctl.onState(s);
  assert.deepEqual(log.models.at(-1).ch.parts, [0, 1]);
  assert.deepEqual(log.models.at(-1).ch.scenes, []);
  assert.equal(ctl.model.parts[1].volumePct, 90);
  // an identical State changes nothing and does not call the renderer
  const count = log.models.length;
  ctl.onState(s);
  assert.equal(log.models.length, count);
  assert.ok(log.models.length > n);
});

test("controller.onState before the first view just remembers the State; the view then starts from it", async () => {
  const { ctl, log } = rig();
  ctl.onState(state({ current_scene: 2 }));
  assert.equal(log.models.length, 0);
  await ctl.select(ID);
  assert.equal(ctl.model.currentScene, 2);
  assert.equal(ctl.model.sounding, true);
});

const mkTimbre = (n, extra = {}) => state({ edits: { available: true, dirty_live_set_ids: [ID], current_dirty: true, change_summary: [{ key: "part.timbre", part: n }], writes_done: 0, ...extra } });

test("controller: a State whose structure changed re-reads the view; scene / volume / on-off never do", async () => {
  const { ctl, sent, replies, tick } = rig();
  await ctl.select(ID);
  ctl.onState(state());
  await tick();
  assert.equal(sent.length, 2); // the reply was sent with no State known: the first State is checked against it (one re-read), never assumed equal
  ctl.onState(state({ current_scene: 1 }));
  const vol = state();
  vol.parts[0] = { ...vol.parts[0], volume: 0.2 };
  ctl.onState(vol);
  await tick();
  assert.equal(sent.length, 2); // in place only
  replies.set(ID, { body: { LiveSetViewReply: view({ dirty: true }) } });
  ctl.onState(state({ edits: { available: true, dirty_live_set_ids: [ID], current_dirty: true, change_summary: [{ key: "part.volume", part: 0 }], writes_done: 0 } }));
  assert.equal(ctl.model.dirty, true); // applied at once from the State, before the re-read answers
  await tick();
  assert.equal(sent.length, 3);
  assert.equal(ctl.refetches, 2);
  ctl.onState(state({ edits: { available: true, dirty_live_set_ids: [ID], current_dirty: true, change_summary: [{ key: "part.volume", part: 1 }], writes_done: 0 } }));
  await tick();
  assert.equal(sent.length, 3); // another volume edit: no re-read
});

test("structuralSignature also folds has_timbre_edit, change_count (the summary is capped at 32), the name and the library revision", () => {
  const sig = (s) => structuralSignature(s, ID);
  const base = state();
  assert.notEqual(sig(base), sig(state({ edits: { ...base.edits, has_timbre_edit: true } })));
  assert.notEqual(sig(base), sig(state({ edits: { ...base.edits, change_count: 40 } })));
  assert.notEqual(sig(base), sig(state({ live_set_name: "Renamed" })));
  assert.notEqual(sig(base), sig(state({ setlists: { library_revision: 9 } })));
  assert.equal(sig(base), sig(state({ live_set_id: ID, current_scene: 2 })));
});

test("throttle: alternating change keys cannot make a continuous request stream (one re-read per interval, single flight)", async () => {
  const { ctl, sent, tick, clock } = rig({ renderMs: 0 });
  ctl.onState(state());
  await ctl.select(ID);
  await tick(0);
  const base = sent.length;
  for (let i = 0; i < 50; i++) {
    ctl.onState(mkTimbre(i % 2, { change_count: i + 1 }));
    clock.advance(5);
  }
  await new Promise((r) => setTimeout(r, 0));
  assert.ok(sent.length - base <= 1, `sent ${sent.length - base} re-reads inside one interval`);
  await tick(400);
  assert.ok(sent.length - base <= 2);
  assert.ok(ctl.refetches <= 3);
});

test("createThrottledRunner: coalesces triggers while running and inside the interval; minMs between starts", async () => {
  let t = 0;
  const timers = [];
  let runs = 0;
  let release;
  const run = createThrottledRunner(() => { runs++; return new Promise((r) => { release = r; }); }, { minMs: 300, clock: () => t, setTimer: (f, ms) => { timers.push({ f, ms }); return timers.at(-1); }, clearTimer: () => {} });
  run(); run(); run();
  await Promise.resolve(); await Promise.resolve();
  assert.equal(runs, 1);
  release();
  await new Promise((r) => setTimeout(r, 0));
  assert.equal(runs, 1);              // the triggers that arrived while it ran are ONE trailing run, but the interval has not passed
  assert.equal(timers.length, 1);
  assert.ok(timers[0].ms > 0 && timers[0].ms <= 300);
  t += 300;
  timers.shift().f();
  await Promise.resolve(); await Promise.resolve();
  assert.equal(runs, 2);
  release();
  await new Promise((r) => setTimeout(r, 0));
  assert.equal(timers.length, 0);     // nothing more was asked
});

test("8a: a reply older than a structural change that arrived while it was in flight is followed by a re-read", async () => {
  const pending = [];
  const { ctl, sent, tick } = rig({ request: (r) => new Promise((res) => pending.push(() => res({ body: { LiveSetViewReply: view() } }))) });
  ctl.onState(state());                              // the State the request is sent under
  const p = ctl.select(ID);
  ctl.onState(mkTimbre(2));                          // a timbre edit lands while the reply is in flight
  pending[0]();
  await p;
  await tick();
  assert.equal(sent.length, 2);                      // re-read (the first reply may predate the edit)
  pending[1]();
  await tick();
  assert.equal(ctl._signature, structuralSignature(mkTimbre(2), ID));
});

test("8b: a re-read dropped by a newer select does not leave the signature advanced", async () => {
  const pending = [];
  const { ctl, tick } = rig({ request: (r) => new Promise((res) => pending.push({ id: r.body.GetLiveSetView.live_set_id, res })) });
  ctl.onState(state());
  const p0 = ctl.select(ID);
  pending[0].res({ body: { LiveSetViewReply: view() } });
  await p0;
  const sigBefore = ctl._signature;
  ctl.onState(mkTimbre(1));                          // triggers a re-read of ID
  await tick(0);
  assert.equal(pending.length, 2);
  const p1 = ctl.select(OTHER);                      // a newer select bumps the token: the re-read's reply is dropped
  pending[1].res({ body: { LiveSetViewReply: view() } });
  await tick(0);
  assert.equal(ctl._signature, sigBefore);           // not advanced by the dropped re-read
  pending[2].res({ body: { LiveSetViewReply: view({ live_set_id: OTHER }) } });
  await p1;
  assert.equal(ctl.liveSetId, OTHER);
});

test("8c: if the renderer throws, the shown model stays the previous one and the next delta re-applies", async () => {
  let fail = false;
  const log = [];
  const ctl = new LiveSetViewController({
    request: async () => ({ body: { LiveSetViewReply: view() } }),
    onModel: (m) => { if (fail) throw new Error("boom"); log.push(m.currentScene); },
    onError: (e) => log.push(e.kind),
    setTimer: () => ({}), clearTimer: () => {},
  });
  await ctl.select(ID);
  ctl.onState(state({ current_scene: 0 }));
  fail = true;
  ctl.onState(state({ current_scene: 2 }));
  assert.equal(ctl.model.currentScene, 0);           // not advanced past what is on screen
  assert.ok(log.includes("render"));
  fail = false;
  ctl.onState(state({ current_scene: 2 }));
  assert.equal(ctl.model.currentScene, 2);
});

test("8d: after reset (an outage) the State of before it is not used; the next view starts from the first push", async () => {
  const { ctl } = rig();
  await ctl.select(ID);
  ctl.onState(state({ current_scene: 2 }));
  ctl.reset();
  assert.equal(ctl.state, null);
  await ctl.select(ID);
  assert.equal(ctl.model.currentScene, -1);          // the pre-outage State is gone
  ctl.onState(state({ current_scene: 1 }));
  assert.equal(ctl.model.currentScene, 1);
});

test("a failed re-read is not retried until the signature moves again", async () => {
  let fail = false;
  const { ctl, sent, tick } = rig({ request: async (r) => (fail ? { body: { Error: { code: 14, message: "x" } } } : { body: { LiveSetViewReply: view() } }) });
  ctl.onState(state());
  await ctl.select(ID);
  await tick();
  const n = sent.length;
  fail = true;
  ctl.onState(mkTimbre(1));
  await tick();
  await tick();
  assert.equal(sent.length, n + 1);
  ctl.onState(mkTimbre(1));
  await tick();
  assert.equal(sent.length, n + 1);                  // same signature: no retry storm
  ctl.onState(mkTimbre(3, { change_count: 9 }));
  await tick();
  assert.equal(sent.length, n + 2);
});

test("a State from an older unit (no per-part fields) does not overlay the parts; the new fields do", () => {
  const base = buildModel(view());
  const old = state({ parts: Array.from({ length: 8 }, (_, i) => ({ part: i, volume: 0, keyboard_on: false, low_key: 0, high_key: 0, engine_type_id: 0, part_state: 0 })) });
  const m = applyState(base, old);
  assert.equal(m.parts[0].volumePct, 78);
  assert.equal(m.parts[0].on, true);
  assert.equal(m.parts[0].keyRange, "C1–C7");
  assert.equal(m.currentScene, 1); // scene and the flags still follow
  const fresh = applyState(base, state());
  assert.equal(fresh.parts[0].volumePct, 50);
});

test("applyState: READY clears missing / empty / unavailable; ENGINE_NOT_AVAILABLE shows as unavailable (never a normal sounding strip)", () => {
  const base = buildModel(view());
  assert.equal(base.parts[3].status, "missing");
  const s = state();
  s.parts[3] = { ...s.parts[3], part_state: 0 };
  s.parts[1] = { ...s.parts[1], part_state: 3, engine_type_id: 4 };
  const m = applyState(base, s);
  assert.equal(m.parts[3].status, "ready"); // the library came back
  assert.equal(m.parts[1].status, "unavailable");
  assert.equal(m.parts[1].addTimbre, false);
  assert.equal(m.activeParts, 3); // piano, the organ (on in this State) and the ex-missing part; the pad's engine is unavailable
  const back = applyState(base, state());
  assert.notEqual(back.parts[1].status, "unavailable");
});

test("performance: the view-model + diff of a State push on a full model costs well under the 100 ms budget (real clock)", () => {
  const base = buildModel(view({ parts: Array.from({ length: 8 }, (_, i) => part(i, { timbre_name: "A fairly long timbre name number " + i })) }));
  let prev = applyState(base, state());
  const N = 2000;
  const t0 = performance.now();
  let changed = 0;
  for (let i = 0; i < N; i++) {
    const s = state({ current_scene: i % 3, revision: i });
    s.parts[i % 8] = { ...s.parts[i % 8], volume: (i % 100) / 100 };
    const next = applyState(base, s);
    if (diffModels(prev, next).any) changed++;
    prev = next;
  }
  const perPush = (performance.now() - t0) / N;
  assert.ok(changed > 0);
  assert.ok(perPush < 10, `one push took ${perPush} ms on average`); // generous: measured in microseconds; the budget is 100 ms
});

test("controller.selectPart exposes the selection (for Story 9.9) and rejects out-of-range indexes", async () => {
  const { ctl, log } = rig();
  await ctl.select(ID);
  assert.equal(ctl.selectPart(2), true);
  assert.equal(ctl.selectedPart, 2);
  assert.deepEqual(log.parts.at(-1), [2, "off"]);
  assert.equal(ctl.selectPart(8), false);
  assert.equal(ctl.selectPart(-1), false);
  assert.equal(ctl.selectPart(1.5), false);
  assert.equal(ctl.selectedPart, 2);
});

test("controller.reset drops everything and ignores a late reply", async () => {
  let resolve;
  const { ctl, log } = rig({ request: () => new Promise((r) => { resolve = r; }) });
  const p = ctl.select(ID);
  ctl.reset();
  resolve({ body: { LiveSetViewReply: view() } });
  await p;
  assert.equal(ctl.model, null);
  assert.equal(log.models.length, 0);
  assert.equal(ctl.selectedPart, -1);
});
