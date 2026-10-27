// Story 9.10: live edit state, stale edits and disconnect mid-edit (Studio side, no DOM): the Footer scope from State.edits, base_revision = the Live Set revision the view was
// loaded at, STALE_REVISION handling and Reload, edits racing a Live Set change, a link that drops with a coalesced edit in flight and the unsaved state after reconnect.
// Against tests/support/fake_unit.js (judges base_revision like engine/host/src/control_plane.cpp).
import test from "node:test";
import assert from "node:assert/strict";

import { REGISTRY } from "../src/params_registry.js";
import { PartEditorController } from "../src/part_editor.js";
import { ERROR_STALE_REVISION, isStale, liveEditFooter, liveRevision, unsavedScope } from "../src/live_edit_state.js";
import { CATALOGS } from "../src/catalogs.js";
import { FakeUnit, CODE } from "./support/fake_unit.js";

function makeClock() {
  let t = 1000;
  let id = 0;
  const timers = new Map();
  const clock = {
    now: () => t,
    setTimer: (f, ms) => { const k = ++id; timers.set(k, { at: t + ms, f }); return k; },
    clearTimer: (k) => timers.delete(k),
    tick: async () => { for (let i = 0; i < 12; i++) await new Promise((r) => setImmediate(r)); },
    advance: async (ms) => {
      const end = t + ms;
      for (;;) {
        let next = null;
        for (const [k, v] of timers) if (v.at <= end && (!next || v.at < next[1].at)) next = [k, v];
        if (!next) break;
        timers.delete(next[0]);
        t = Math.max(t, next[1].at);
        next[1].f();
        await clock.tick();
      }
      t = end;
      await clock.tick();
    },
  };
  return clock;
}

const def = (name) => REGISTRY.params.find((d) => d.name === name);
const knobs = (m) => m.sections.flatMap((s) => s.groups.flatMap((g) => g.knobs));
const byId = (m, id) => knobs(m).find((k) => k.id === id);
const ENGINE = { 1: "sample", 2: "organ", 3: "fm", 4: "va" };

function rig(unitOptions = {}) {
  const unit = new FakeUnit(unitOptions);
  const clock = makeClock();
  const errors = [];
  const events = { stale: 0, reloaded: 0 };
  const ctl = new PartEditorController({
    request: (m) => unit.request(m), clock: clock.now, setTimer: clock.setTimer, clearTimer: clock.clearTimer,
    onChange: () => {}, onError: (e) => errors.push(e), onStale: () => { events.stale++; }, onReloaded: () => { events.reloaded++; },
  });
  unit.onState((st) => ctl.onState(st)); // the pushes the page feeds the controller with
  const open = async (part = 3) => {
    ctl.select({ liveSetId: unit.liveSetId, part, engine: ENGINE[unit.parts[part].engine_type_id] });
    ctl.onState(unit.state());
    await clock.advance(100);
  };
  return { unit, clock, ctl, errors, events, open };
}
const perfSent = (unit) => unit.log.filter((l) => l[0] === "SetPerformanceParam");

// ---- the Footer scope -------------------------------------------------------------------------------------------------------------------------------------------
test("unsaved scope: nothing, a scene, or the whole Live Set (a timbre change, or no scene to write to)", () => {
  assert.deepEqual(unsavedScope(null), { kind: "none" });
  assert.deepEqual(unsavedScope({ current_dirty: false }), { kind: "none" });
  assert.deepEqual(unsavedScope({ current_dirty: true, scene_write_available: true, current_scene_for_write: 2 }), { kind: "scene", scene: 3 }); // 1-based
  assert.deepEqual(unsavedScope({ current_dirty: true, scene_write_available: true, current_scene_for_write: 0 }), { kind: "scene", scene: 1 });
  assert.deepEqual(unsavedScope({ current_dirty: true, has_timbre_edit: true, scene_write_available: false, scene_write_blocked_reason: "timbre_change", current_scene_for_write: 1 }), { kind: "live_set" });
  assert.deepEqual(unsavedScope({ current_dirty: true, has_timbre_edit: true, scene_write_available: true, current_scene_for_write: 1 }), { kind: "live_set" }); // the timbre rule wins
  assert.deepEqual(unsavedScope({ current_dirty: true, scene_write_available: false, scene_write_blocked_reason: "no_scene", current_scene_for_write: -1 }), { kind: "live_set" });
  assert.deepEqual(unsavedScope({ current_dirty: true, scene_write_available: true, current_scene_for_write: -1 }), { kind: "live_set" });
});

test("the Footer: LIVE EDIT ON while an edit is in memory or Studio edits just arrived, with the scope that Write would offer", () => {
  const unit = new FakeUnit();
  assert.deepEqual(liveEditFooter(unit.state()), { on: false, scope: { kind: "none" } });
  assert.deepEqual(liveEditFooter(null), { on: false, scope: { kind: "none" } });
  unit.dirty = true;
  unit.scene = 1;
  assert.deepEqual(liveEditFooter(unit.state()), { on: true, scope: { kind: "scene", scene: 2 } });
  unit.hasTimbreEdit = true;
  assert.deepEqual(liveEditFooter(unit.state()), { on: true, scope: { kind: "live_set" } });
  const idle = new FakeUnit();
  idle.studioActive = true; // Studio edits arrived but nothing is unsaved yet (the capture of the live values is a tick away)
  assert.deepEqual(liveEditFooter(idle.state()), { on: true, scope: { kind: "none" } });
  const silent = new FakeUnit();
  silent.sounding = false;
  silent.dirty = true;
  assert.deepEqual(liveEditFooter(silent.state()), { on: false, scope: { kind: "none" } }); // no Live Set sounds: nothing to report
  for (const lang of ["en", "es", "pt-BR"]) {
    for (const k of ["studio.footer.live_edit_on", "studio.footer.unsaved_scene", "studio.footer.unsaved_live_set", "studio.part_editor.stale_banner", "studio.part_editor.stale_reload"]) assert.ok(CATALOGS[lang][k], `${lang} ${k}`);
  }
  assert.equal(CATALOGS.en["studio.footer.live_edit_on"], "● LIVE EDIT ON · CHANGES PLAY ON THE VELORA");
  assert.equal(CATALOGS.en["studio.footer.unsaved_scene"].replace("%d", 3), "UNSAVED: SCENE 3");
  assert.equal(CATALOGS.en["studio.footer.unsaved_live_set"], "UNSAVED: LIVE SET");
});

test("liveRevision / isStale read the wire values; an older unit (no field) means nothing is ever sent as base_revision", () => {
  assert.equal(liveRevision({ edits: { live_set_revision: 7 } }), 7);
  assert.equal(liveRevision({ edits: {} }), 0);
  assert.equal(liveRevision({}), 0);
  assert.equal(liveRevision(null), 0);
  assert.equal(liveRevision({ edits: { live_set_revision: "9" } }), 0);
  assert.equal(ERROR_STALE_REVISION, 47);
  assert.ok(isStale({ code: 47 }) && !isStale({ code: 7 }) && !isStale(null));
});

// ---- base_revision and stale edits ---------------------------------------------------------------------------------------------------------------------------
test("edits carry the Live Set revision the view was loaded at; Studio's own edits never make it stale", async () => {
  const r = rig();
  await r.open(3);
  assert.equal(r.ctl.baseRev, 5);
  const m = r.ctl.model();
  for (const [id, v] of [["drawbar_16", 3], ["drawbar_8", 5], ["drawbar_4", 2]]) r.ctl.edit(byId(m, id).key, v);
  const vol = knobs(m).find((k) => k.command === "volume");
  assert.ok(vol, "a structural knob (part volume) is part of the sequence");
  r.ctl.edit(vol.key, 0.4);
  await r.clock.advance(400);
  assert.ok(perfSent(r.unit).length >= 3);
  assert.ok(r.unit.log.filter((l) => l[0].startsWith("Set")).every((l) => l[2] === 5), "every edit carries the loaded revision");
  assert.equal(r.unit.staleRejected, 0);
  assert.equal(r.ctl.stale, null);
  assert.equal(r.unit.liveRevision, 5, "the unit's revision did not move for Studio's edits");
  assert.ok(r.unit.revision > 10, "structural edits publish generations, which is not the Live Set revision");
  const again = r.ctl.edit(byId(r.ctl.model(), "drawbar_16").key, 4);
  assert.equal(again, 4); // still editable after its own structural edits
  await r.clock.advance(300);
  assert.equal(r.unit.staleRejected, 0);
});

test("an older unit (no live_set_revision): no base_revision is sent and nothing is judged", async () => {
  const r = rig();
  const st = r.unit.state();
  delete st.edits.live_set_revision;
  r.ctl.select({ liveSetId: r.unit.liveSetId, part: 3, engine: "organ" });
  r.ctl.onState(st);
  assert.equal(r.ctl.baseRev, 0);
  r.ctl.edit(byId(r.ctl.model(), "drawbar_16").key, 3);
  await r.clock.advance(100);
  assert.deepEqual(perfSent(r.unit).map((l) => l[2]), [0]);
});

test("a change on the unit after Studio loaded the Live Set: the edit is refused STALE_REVISION, nothing is applied, the knob shows what the unit holds, edits stop until Reload", async () => {
  const r = rig();
  await r.open(3);
  const k = byId(r.ctl.model(), "drawbar_16");
  const before = r.unit.perf.get(`${k.paramId}:3`);
  r.unit.deviceChange(); // the player turned something on the instrument; the State push reaches the view, which keeps its loaded base
  assert.equal(r.ctl.baseRev, 5);
  assert.equal(r.ctl.stale, null, "no banner until an edit is refused");
  assert.equal(r.ctl.edit(k.key, 7), 7); // optimistic at first
  await r.clock.advance(300);
  assert.equal(r.unit.staleRejected, 1);
  assert.equal(r.unit.perf.get(`${k.paramId}:3`), before, "the stale edit changed nothing on the unit");
  assert.equal(r.events.stale, 1);
  assert.deepEqual(r.ctl.stale, { current: 6 });
  const m = r.ctl.model();
  assert.equal(m.stale, true);
  assert.equal(m.editable, false);
  assert.equal(byId(m, "drawbar_16").value, before, "the optimistic value is gone");
  assert.equal(byId(m, "drawbar_16").pending, false);
  assert.equal(byId(m, "drawbar_16").readOnly, true);
  assert.equal(byId(m, "drawbar_16").reason, "studio.part_editor.stale_banner");
  assert.equal(r.ctl.edit(k.key, 2), null, "read-only while stale");
  assert.deepEqual(r.errors, [], "the banner replaces the per-knob error");
  const sentBefore = r.unit.log.length;
  await r.clock.advance(300);
  assert.equal(perfSent(r.unit).length, 1, "nothing else went out");
  assert.ok(r.unit.log.length >= sentBefore);
});

test("Reload applies the current snapshot: reads the State, takes the new revision as the base, and the next edit is accepted", async () => {
  const r = rig();
  await r.open(3);
  const k = byId(r.ctl.model(), "drawbar_16");
  r.unit.perf.set(`${k.paramId}:3`, 2); // what the player set on the unit
  r.unit.deviceChange();
  r.ctl.edit(k.key, 7);
  await r.clock.advance(300);
  assert.equal(r.ctl.model().stale, true);
  assert.equal(await r.ctl.reload(), true);
  assert.equal(r.events.reloaded, 1);
  assert.equal(r.ctl.stale, null);
  assert.equal(r.ctl.baseRev, 6);
  assert.ok(r.unit.log.some((l) => l[0] === "GetState"));
  await r.clock.advance(300);
  const m = r.ctl.model();
  assert.equal(m.stale, false);
  assert.equal(m.editable, true);
  assert.equal(byId(m, "drawbar_16").value, 2, "the knob shows the unit's value");
  assert.equal(r.ctl.edit(k.key, 7), 7);
  await r.clock.advance(300);
  assert.equal(r.unit.perf.get(`${k.paramId}:3`), 7);
  assert.equal(r.unit.staleRejected, 1, "no further rejection");
  assert.equal(perfSent(r.unit).at(-1)[2], 6, "the accepted edit carries the reloaded revision");
  // Reload while the unit does not answer: the view stays stale and reports it
  r.unit.deviceChange();
  r.ctl.edit(k.key, 1);
  await r.clock.advance(300);
  assert.equal(r.ctl.stale !== null, true);
  r.unit.fail = (name) => (name === "GetState" ? CODE.BUSY : 0);
  assert.equal(await r.ctl.reload(), false);
  assert.equal(r.ctl.stale !== null, true);
  assert.ok(r.errors.length >= 1);
});

test("a late STALE answer to a request sent before a Reload (another base) says nothing about the reloaded view", async () => {
  const r = rig();
  await r.open(3);
  r.unit.deviceChange();
  r.ctl.edit(byId(r.ctl.model(), "drawbar_16").key, 7);
  await r.clock.advance(300);
  await r.ctl.reload();
  assert.equal(r.ctl.stale, null);
  const stale = { body: { Error: { code: 47, message: "stale", details: { current_revision: "6" } } } };
  r.ctl._done("drawbar", stale, null, { keys: [], seqs: {} }, { base_revision: 5 }); // answered after the Reload
  assert.equal(r.ctl.stale, null);
  r.ctl._done("drawbar", stale, null, { keys: [], seqs: {} }, { base_revision: 6 }); // on the current base: a real change
  assert.deepEqual(r.ctl.stale, { current: 6 });
});

test("race: a coalesced edit waiting in the throttle when the unit's Live Set changes goes out on the old base and is refused, never applied", async () => {
  const r = rig();
  await r.open(3);
  const k = byId(r.ctl.model(), "drawbar_16");
  r.ctl.edit(k.key, 1); // goes out at once
  await r.clock.advance(5);
  r.ctl.edit(k.key, 2); // waits for the 30 ms throttle
  r.ctl.edit(k.key, 3); // replaces it (latest wins)
  r.unit.deviceChange(); // a Live Set switch / scene recall on the unit lands before the waiting edit leaves
  await r.clock.advance(300);
  assert.equal(r.unit.perf.get(`${k.paramId}:3`), 1, "only the first edit (built on the revision that was current) was applied");
  assert.equal(r.unit.staleRejected, 1);
  assert.equal(r.ctl.model().stale, true);
  assert.deepEqual(perfSent(r.unit).map((l) => l[1].value), [1, 3]);
});

test("race: the Live Set that sounds changes (a switch on the unit): the knobs go read-only; coming back to the first Live Set needs a Reload", async () => {
  const r = rig();
  await r.open(3);
  r.unit.liveSetId = "ls-2"; // the unit now plays another Live Set; the revision moved with the switch
  r.unit.deviceChange();
  assert.equal(r.ctl.model().editable, false);
  assert.equal(r.ctl.edit(byId(r.ctl.model(), "drawbar_16").key, 4), null);
  r.unit.liveSetId = "ls-1"; // and back
  r.unit.deviceChange();
  assert.equal(r.ctl.model().editable, true);
  r.ctl.edit(byId(r.ctl.model(), "drawbar_16").key, 4);
  await r.clock.advance(300);
  assert.equal(r.ctl.model().stale, true, "the view was loaded before both switches");
  await r.ctl.reload();
  assert.equal(r.ctl.baseRev, r.unit.liveRevision);
});

// ---- the cable ---------------------------------------------------------------------------------------------------------------------------------------------
test("cable pulled with a coalesced edit in flight: nothing leaks into the next link, the unit keeps what it applied (unsaved), and the view reloads clean", async () => {
  const r = rig();
  await r.open(3);
  const k = byId(r.ctl.model(), "drawbar_16");
  r.ctl.edit(k.key, 1);
  await r.clock.advance(5); // applied on the unit: the edit is in its memory, dirty
  assert.equal(r.unit.dirty, true);
  r.unit.delay = 6; // the next answer is slow
  r.ctl.edit(k.key, 2); // in flight (or waiting) when the cable goes
  r.ctl.edit(k.key, 3);
  r.ctl.reset(); // the link went down: the page drops the State and every optimistic value (applyDeviceStatus)
  r.unit.delay = 0;
  await r.clock.advance(400);
  assert.equal(r.ctl.stale, null, "a late answer from the dead link never raises the banner");
  assert.equal(r.ctl.baseRev, 0);
  assert.equal(r.ctl.pending.size, 0);
  assert.equal(r.errors.length, 0);
  // the unit kept what it applied: unsaved, with the scope the Footer shows after the reconnect
  const sent = perfSent(r.unit).map((l) => l[1].value);
  assert.ok(!sent.includes(3) || r.unit.perf.get(`${k.paramId}:3`) !== undefined);
  const st = r.unit.state();
  assert.equal(st.edits.current_dirty, true);
  assert.deepEqual(liveEditFooter(st), { on: true, scope: { kind: "scene", scene: 1 } });
  assert.ok(st.edits.dirty_live_set_ids.includes("ls-1"), "the Live Set carries the diamond");
  // after reconnect: the first State and the selection give a fresh base; the edit works and nothing is stale
  r.ctl.onState(st);
  r.ctl.select({ liveSetId: "ls-1", part: 3, engine: "organ" });
  assert.equal(r.ctl.baseRev, 5);
  assert.equal(r.ctl.model().editable, true);
  assert.equal(r.ctl.edit(k.key, 6), 6);
  await r.clock.advance(300);
  assert.equal(r.unit.perf.get(`${k.paramId}:3`), 6);
  assert.equal(r.unit.staleRejected, 0);
});

test("reconnect after the unit changed while the link was down: the view is loaded afresh at the new revision (no banner)", async () => {
  const r = rig();
  await r.open(3);
  r.ctl.reset();
  r.unit.deviceChange(); // changed on the unit during the outage
  r.ctl.onState(r.unit.state());
  r.ctl.select({ liveSetId: "ls-1", part: 3, engine: "organ" });
  assert.equal(r.ctl.baseRev, 6);
  r.ctl.edit(byId(r.ctl.model(), "drawbar_16").key, 4);
  await r.clock.advance(300);
  assert.equal(r.unit.staleRejected, 0);
  assert.equal(r.ctl.model().stale, false);
});

test("race: the link drops while a Reload is in flight: its late answer neither revives the view nor sets a base", async () => {
  const r = rig();
  await r.open(3);
  r.unit.deviceChange();
  r.ctl.edit(byId(r.ctl.model(), "drawbar_16").key, 7);
  await r.clock.advance(300);
  assert.equal(r.ctl.model().stale, true);
  r.unit.delay = 5;
  const pending = r.ctl.reload();
  r.ctl.reset(); // the cable goes out during the Reload
  r.unit.delay = 0;
  assert.equal(await pending, false);
  assert.equal(r.ctl.state, null);
  assert.equal(r.ctl.baseRev, 0);
  assert.equal(r.events.reloaded, 0);
});
