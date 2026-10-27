// Shared by the Write to Velora tests (Story 9.12): a fake clock, and a WriteController wired to a fake unit and a REAL Part Editor controller (so flush, base_revision and the stale banner
// are the real ones).
import { PartEditorController } from "../../src/part_editor.js";
import { WriteController } from "../../src/write_to_velora.js";
import { FakeUnit } from "./fake_unit.js";

export function makeClock() {
  let t = 1000;
  let id = 0;
  const timers = new Map();
  const clock = {
    now: () => t,
    setTimer: (f, ms) => { const k = ++id; timers.set(k, { at: t + ms, f }); return k; },
    clearTimer: (k) => timers.delete(k),
    async tick() { for (let i = 0; i < 12; i++) await new Promise((r) => setImmediate(r)); },
    async advance(ms) {
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

export const settle = async () => { for (let i = 0; i < 10; i++) await new Promise((r) => setImmediate(r)); };

/** A Write controller wired to a fake unit and a REAL Part Editor controller (so flush, base_revision and the stale banner are the real ones). */
export async function rig({ unitOptions, wire } = {}) {
  const unit = new FakeUnit({ liveSetId: "ls-1", ...unitOptions });
  const clock = makeClock();
  let state = unit.state();
  let paired = true;
  const ev = { closes: [], announces: [], written: [], stale: [], changes: 0, peErrors: [] };
  const pe = new PartEditorController({ request: (m) => unit.request(m), clock: clock.now, setTimer: clock.setTimer, clearTimer: clock.clearTimer, onError: (e) => ev.peErrors.push(e) });
  pe.select({ liveSetId: "ls-1", part: 3, engine: "organ" });
  pe.onState(state);
  const request = (m) => (wire ? wire(m, unit) : unit.request(m));
  const wc = new WriteController({
    request,
    flush: () => pe.flush(200),
    writeContext: (id) => pe.writeContext(id),
    noteStale: (e, c) => pe.noteStale(e, c),
    state: () => state,
    isPaired: () => paired,
    takenNames: () => unit.takenNames,
    deviceName: () => "Velora 73",
    onChange: () => { ev.changes++; },
    onClose: (r) => ev.closes.push(r),
    onAnnounce: (a) => ev.announces.push(a),
    onWritten: (r) => ev.written.push(r),
    onStale: (e) => ev.stale.push(e),
  });
  unit.onState((st) => { state = st; pe.onState(st); wc.onState(st); });
  await clock.advance(100);
  const studioEdit = (name, body) => unit.request({ base_revision: unit.liveRevision, body: { [name]: body } });
  const dirtyIt = async () => { await studioEdit("SetPartVolume", { part: 0, volume: 0.4 }); await settle(); };
  return { unit, clock, pe, wc, ev, dirtyIt, studioEdit, get state() { return state; }, setPaired: (v) => { paired = v; } };
}

