// A stand-in for the engine's edit commands for the Story 9.9 tests and for the browser check (ws.v1 JSON in and out, `unit.request(msg)` is what
// `window.veloraDevice.request` is in the page). It keeps one sounding Live Set of 8 parts with the semantics of engine/host/src/control_plane.cpp: a performance edit is
// ACKED when queued and changes the live value (read by GetPerformanceValues) but never the State revision; a structural edit (chain, sends, output, volume, ASSIGN slot)
// publishes a new State (revision + 1, pushed to subscribers); typed errors for an unknown / out-of-range / wrong-class parameter, a bad part or slot; base_revision is
// judged like engine/host/src/control_plane.cpp (Story 9.10): an edit command whose base_revision differs from State.edits.live_set_revision is STALE_REVISION (47), one without
// base_revision (and `deviceChange()`, the unit's own controls) moves that revision, Studio's own edits do not; accepted edits mark the Live Set dirty and light studio_edit_active
// until `expireStudio()`. Not a copy of the engine: the C++ tests are the authority; this is what lets the page and the model run without one.
import { REGISTRY } from "../../src/params_registry.js";

export const CODE = { INVALID_COMMAND: 5, UNKNOWN_PARAM: 6, OUT_OF_RANGE: 7, INVALID_PART: 8, WRONG_CLASS: 9, WRONG_SCOPE: 10, INVALID_SLOT: 11, BUSY: 13, FX_BUDGET: 15, NOT_ASSIGNABLE: 37, STALE_REVISION: 47 };

const BY_ID = new Map(REGISTRY.params.map((d) => [d.id, d]));
const clone = (x) => JSON.parse(JSON.stringify(x));

export class FakeUnit {
  constructor({ liveSetId = "ls-1", engines = [1, 3, 4, 2, 1, 1, 1, 1], fxMax = 5, rtDelay = 0, noRender = false } = {}) {
    this.rtDelay = rtDelay; // macrotask turns between an acked performance edit and the RT plane applying it (Ack means QUEUED)
    this.noRender = noRender; // --no-audio: no render thread, a queued edit is never applied
    this.audibleLog = []; // [id, part, value] of every applied edit an engine CONSUMES (the sound changed); the others are stored only
    this.liveSetId = liveSetId;
    this.fxMax = fxMax;
    this.revision = 10;
    this.liveRevision = 5; // State.edits.live_set_revision (Story 9.10)
    this.dirty = false;
    this.studioActive = false;
    this.hasTimbreEdit = false;
    this.staleRejected = 0;
    this.log = []; // [name, body, base_revision] of every request
    this.fail = null; // (name, body) => error code | null
    this.delay = 0; // macrotask turns before an answer
    this.listeners = new Set();
    this.perf = new Map();
    for (const d of REGISTRY.params) {
      if (d.class !== "performance") continue;
      if (d.scope === "part") for (let p = 0; p < 8; p++) this.perf.set(`${d.id}:${p}`, d.default);
      else this.perf.set(`${d.id}:0`, d.default);
    }
    this.parts = engines.map((e, i) => ({
      part: i, timbre_ref: 100 + i, inserts: [], output: 0, send_reverb: 0.25, send_delay: 0.1, volume: 0.7, keyboard_on: true, low_key: 36, high_key: 96,
      engine_type_id: e, part_state: 0, view_fields: true,
    }));
    for (let p = 0; p < 8; p++) {
      this.perf.set(`400:${p}`, this.parts[p].send_reverb); // the live send levels start at the snapshot's initial ones
      this.perf.set(`401:${p}`, this.parts[p].send_delay);
    }
    this.assignMap = [0, 0, 0, 0, 0, 0, 0, 0];
    this.sounding = true;
    this.scene = 0;
  }

  state() {
    return clone({
      revision: this.revision, live_set_loaded: this.sounding, live_set_id: this.sounding ? this.liveSetId : "", current_scene: this.scene, scene_count: 2,
      parts: this.parts, master: [], performance_init: [...this.perf].map(([k, value]) => {
        const [id, part] = k.split(":").map(Number);
        return { param_id: id, part, value };
      }),
      encoders: { mode: 3, selected_part: 0, assign: this.assignMap },
      edits: {
        available: true, dirty_live_set_ids: this.dirty ? [this.liveSetId] : [], current_dirty: this.dirty, change_summary: [], change_count: this.dirty ? 1 : 0, writes_done: 0,
        has_timbre_edit: this.hasTimbreEdit, scene_write_available: !this.hasTimbreEdit, scene_write_blocked_reason: this.hasTimbreEdit ? "timbre_change" : "",
        live_set_revision: this.liveRevision, studio_edit_active: this.studioActive,
      },
      setlists: { library_revision: 1 },
    });
  }

  onState(cb) {
    this.listeners.add(cb);
    return () => this.listeners.delete(cb);
  }

  push() {
    const st = this.state();
    this.listeners.forEach((cb) => cb(st));
  }

  bump() {
    this.revision++;
    this.push();
  }

  /** A change from the unit's own controls (panel, encoder, fader, scene recall, device UI): moves the Live Set revision, marks it dirty, pushes the State. */
  deviceChange({ dirty = true } = {}) {
    this.liveRevision++;
    if (dirty) this.dirty = true;
    this.push();
  }

  /** The 3 s idle window passed: STUDIO goes out and the State is pushed. */
  expireStudio() {
    this.studioActive = false;
    this.push();
  }

  err(code, message = "") {
    return { request_id: 0, revision: this.revision, body: { Error: { code, message: message || `error ${code}`, details: {} } } };
  }

  ack() {
    return { request_id: 0, revision: this.revision, body: { Ack: { not_durable: false } } };
  }

  async request(msg) {
    for (let i = 0; i < this.delay; i++) await new Promise((r) => setImmediate(r)); // `delay`: macrotask turns the unit takes to answer (the tests drive a fake clock)
    const name = Object.keys(msg.body)[0];
    const b = msg.body[name];
    this.log.push([name, clone(b), msg.base_revision || 0]);
    if (this.fail) {
      const c = this.fail(name, b);
      if (c) return this.err(c, "injected");
    }
    const edit = ["SetInsertChain", "SetSendLevels", "SetPartOutput", "SetMasterParams", "SetPerformanceParam", "SetPartTimbre", "SetPartVolume", "SetAssignSlot", "ClearAssignSlot"].includes(name);
    const base = msg.base_revision || 0;
    if (edit && base !== 0 && base !== this.liveRevision) {
      this.staleRejected++;
      return { request_id: 0, revision: this.revision, body: { Error: { code: CODE.STALE_REVISION, message: "stale", details: { base_revision: String(base), current_revision: String(this.liveRevision) } } } };
    }
    const reply = await this.handle(name, b);
    if (edit && reply.body && !reply.body.Error) {
      const wasActive = this.studioActive;
      if (base === 0) this.liveRevision++; // an edit that does not speak for a Studio view changed the Live Set under every view
      else this.studioActive = true;
      const was = this.dirty;
      this.dirty = true;
      if (!was || (base !== 0 && !wasActive) || base === 0) this.push(); // like the engine: a State push when something visible flipped
    }
    return reply;
  }

  /** What the engines consume of the registry's part parameters (engine/core: only OrganEngine overrides set_part_param; the sends belong to the mixer). */
  consumes(part, id) {
    const d = BY_ID.get(id);
    if (!d) return false;
    if (d.name === "send_reverb_level" || d.name === "send_delay_level") return true;
    return this.parts[part].engine_type_id === 2 && /^(drawbar_|percussion_|vibrato_chorus_|key_click|leakage|rotary_)/.test(d.name);
  }

  /** The RT plane applies a queued performance edit: after `rtDelay` turns, or never without a render thread. */
  applyPerf(id, part, value) {
    if (this.noRender) return;
    const apply = () => {
      this.perf.set(`${id}:${part}`, value);
      if (this.consumes(part, id)) this.audibleLog.push([id, part, value]);
    };
    if (this.rtDelay <= 0) return apply();
    let n = this.rtDelay;
    const tick = () => (--n <= 0 ? apply() : setImmediate(tick));
    setImmediate(tick);
  }

  partOk(p) {
    return Number.isInteger(p) && p >= 0 && p < this.parts.length;
  }

  handle(name, b) {
    switch (name) {
      case "Subscribe": return this.ack();
      case "GetLiveSetView": // what Studio's Live Set view needs (Story 9.8), for the browser harness
        if (b.live_set_id !== this.liveSetId) return this.err(20, "no such Live Set");
        return { revision: this.revision, body: { LiveSetViewReply: {
          live_set_id: this.liveSetId, name: "Perfect", dirty: false, sounding: this.sounding,
          scenes: Array.from({ length: 8 }, (_, i) => ({ index: i, name: i < 2 ? ["Verse", "Chorus"][i] : "", empty: i >= 2 })),
          parts: this.parts.map((p) => ({ index: p.part, empty: false, on: p.keyboard_on, engine_type_id: p.engine_type_id, timbre_name: p.engine_type_id === 1 ? "Concert Grand" : "", category: p.engine_type_id === 2 ? 3 : p.engine_type_id === 1 ? 1 : 13, volume: p.volume, low_key: p.low_key, high_key: p.high_key, organ_registration: p.engine_type_id === 2 ? "88 8000 000" : "" })),
          part_count: this.parts.length, scene_count: 2, factory: false } } };
      case "GetSetlists":
        return { revision: this.revision, body: { SetlistsReply: { setlists: [], tags: [], status: { available: true, library_revision: 1 }, detail_setlist_id: "", entries: [], all_live_sets: [{ live_set_id: this.liveSetId, name: "Perfect", missing: false }] } } };
      case "GetState": return { revision: this.revision, body: { State: this.state() } };
      case "GetPerformanceValues":
        return { revision: this.revision, body: { PerformanceValues: { values: [...this.perf].map(([k, value]) => { const [id, part] = k.split(":").map(Number); return { param_id: id, part, value }; }), current_scene: this.scene } } };
      case "SetPerformanceParam": {
        const d = BY_ID.get(b.param_id);
        if (!d) return this.err(CODE.UNKNOWN_PARAM);
        if (d.class !== "performance") return this.err(CODE.WRONG_CLASS);
        if (d.scope === "part" && !this.partOk(b.part)) return this.err(CODE.INVALID_PART);
        if (!Number.isFinite(b.value) || b.value < d.min || b.value > d.max) return this.err(CODE.OUT_OF_RANGE);
        if (d.type !== "float" && !Number.isInteger(b.value)) return this.err(CODE.OUT_OF_RANGE);
        this.applyPerf(d.id, d.scope === "part" ? b.part : 0, b.value);
        return this.ack(); // queued for RT: no new revision, no State push
      }
      case "SetSendLevels":
        if (!this.partOk(b.part)) return this.err(CODE.INVALID_PART);
        if (!(b.reverb >= 0 && b.reverb <= 1 && b.delay >= 0 && b.delay <= 1)) return this.err(CODE.OUT_OF_RANGE);
        this.parts[b.part].send_reverb = b.reverb;
        this.parts[b.part].send_delay = b.delay;
        // the RT plane takes the new initial level at the generation change only when it differs from the previous one; the live value follows
        this.applyPerf(400, b.part, b.reverb);
        this.applyPerf(401, b.part, b.delay);
        this.bump();
        return this.ack();
      case "SetPartOutput":
        if (!this.partOk(b.part)) return this.err(CODE.INVALID_PART);
        if (![0, 1].includes(b.output)) return this.err(12, "unknown enum value");
        this.parts[b.part].output = b.output;
        this.bump();
        return this.ack();
      case "SetPartVolume":
        if (!this.partOk(b.part)) return this.err(CODE.INVALID_PART);
        if (!(b.volume >= 0 && b.volume <= 1)) return this.err(CODE.OUT_OF_RANGE);
        this.parts[b.part].volume = b.volume;
        this.bump();
        return this.ack();
      case "SetInsertChain": {
        if (!this.partOk(b.part)) return this.err(CODE.INVALID_PART);
        if (b.slots.length > REGISTRY.insertSlots) return this.err(CODE.INVALID_SLOT);
        if (b.slots.length > this.fxMax) return this.err(CODE.FX_BUDGET, "fx budget");
        for (const s of b.slots) if (!(s.type >= 0 && s.type < REGISTRY.enums.insert_type.length)) return this.err(12);
        this.parts[b.part].inserts = clone(b.slots);
        this.bump();
        return this.ack();
      }
      case "SetAssignSlot": {
        if (!(b.slot >= 1 && b.slot <= 8)) return this.err(CODE.INVALID_SLOT);
        const d = BY_ID.get(b.param_id);
        if (!d) return this.err(CODE.UNKNOWN_PARAM);
        if (!d.assignable) return this.err(CODE.NOT_ASSIGNABLE);
        this.assignMap[b.slot - 1] = b.param_id;
        this.bump();
        return this.ack();
      }
      case "ClearAssignSlot":
        if (!(b.slot >= 1 && b.slot <= 8)) return this.err(CODE.INVALID_SLOT);
        this.assignMap[b.slot - 1] = 0;
        this.bump();
        return this.ack();
      default: return this.err(CODE.INVALID_COMMAND, name);
    }
  }
}
