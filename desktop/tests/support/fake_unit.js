// A stand-in for the engine's edit commands for the Story 9.9 tests and for the browser check (ws.v1 JSON in and out, `unit.request(msg)` is what
// `window.veloraDevice.request` is in the page). It keeps one sounding Live Set of 8 parts with the semantics of engine/host/src/control_plane.cpp: a performance edit is
// ACKED when queued and changes the live value (read by GetPerformanceValues) but never the State revision; a structural edit (chain, sends, output, volume, ASSIGN slot)
// publishes a new State (revision + 1, pushed to subscribers); typed errors for an unknown / out-of-range / wrong-class parameter, a bad part or slot; base_revision is
// judged like engine/host/src/control_plane.cpp (Story 9.10): an edit command whose base_revision differs from State.edits.live_set_revision is STALE_REVISION (47), one without
// base_revision (and `deviceChange()`, the unit's own controls) moves that revision, Studio's own edits do not; accepted edits mark the Live Set dirty and light studio_edit_active
// until `expireStudio()`. Not a copy of the engine: the C++ tests are the authority; this is what lets the page and the model run without one.
import { REGISTRY } from "../../src/params_registry.js";

export const CODE = { NAME_IN_USE: 34, WRITE_SCOPE_UNAVAILABLE: 35, STORAGE_FULL: 36, NO_LIVE_SET: 17, INVALID_COMMAND: 5, UNKNOWN_PARAM: 6, OUT_OF_RANGE: 7, INVALID_PART: 8, WRONG_CLASS: 9, WRONG_SCOPE: 10, INVALID_SLOT: 11, BUSY: 13, FX_BUDGET: 15, NOT_ASSIGNABLE: 37, STALE_REVISION: 47 };

const BY_ID = new Map(REGISTRY.params.map((d) => [d.id, d]));
const clone = (x) => JSON.parse(JSON.stringify(x));

export class FakeUnit {
  constructor({ liveSetId = "ls-1", engines = [1, 3, 4, 2, 1, 1, 1, 1], fxMax = 5, rtDelay = 0, noRender = false, empties = [] } = {}) {
    this.rtDelay = rtDelay; // macrotask turns between an acked performance edit and the RT plane applying it (Ack means QUEUED)
    this.noRender = noRender; // --no-audio: no render thread, a queued edit is never applied
    this.audibleLog = []; // [id, part, value] of every applied edit an engine CONSUMES (the sound changed); the others are stored only
    this.liveSetId = liveSetId;
    this.fxMax = fxMax;
    this.revision = 10;
    this.liveRevision = 5; // State.edits.live_set_revision (Story 9.10)
    this.dirty = false;
    // Story 9.12: Write. `changes` is State.edits.change_summary (one ChangeItem per kind of edit made since the last Write), `writesDone` the engine's counter of files written;
    // `factory` makes the Live Set read-only (Save as new only), `storageFull` makes every Write answer STORAGE_FULL (the store's inject_write_errno), `takenNames` are the folded names
    // already used in the loaded setlist, `writeHook(body)` may answer instead (a refusal, or a Promise that never settles to hold the Write out), `writeLog` every WriteLiveSet body
    this.changes = [];
    this.writesDone = 0;
    this.factory = false;
    this.storageFull = false;
    this.takenNames = new Set();
    this.writeHook = null;
    this.writeLog = [];
    this.liveSetName = "Perfect";
    this.loadedSetlist = "";
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
      engine_type_id: e, part_state: empties.includes(i) ? 1 : 0, view_fields: true, empty: empties.includes(i), timbre: null,
    }));
    // Story 9.11: the timbres installed on the unit (what Studio's picker lists through the Library API) and the libraries that exist (a timbre whose library is not in `installed` is
    // accepted like the engine accepts it, and its part then reads MISSING_LIBRARY with no name)
    this.catalog = [
      { library_id: "lib-a", library_name: "Studio Collection", instrument_key: "grand", name: "Concert Grand", category: 1 },
      { library_id: "lib-a", library_name: "Studio Collection", instrument_key: "rhodes", name: "Rhôdes Mk I", category: 2 },
      { library_id: "lib-b", library_name: "Strings Vol 1", instrument_key: "ensemble", name: "String Ensemble", category: 6 },
      { library_id: "lib-b", library_name: "Strings Vol 1", instrument_key: "grand", name: "Soft Grand", category: 1 },
    ];
    this.installed = new Set(["lib-a", "lib-b"]);
    for (let p = 0; p < 8; p++) {
      this.perf.set(`400:${p}`, this.parts[p].send_reverb); // the live send levels start at the snapshot's initial ones
      this.perf.set(`401:${p}`, this.parts[p].send_delay);
    }
    this.assignMap = [0, 0, 0, 0, 0, 0, 0, 0];
    this.sounding = true;
    this.scene = 0;
  }

  /** What `window.veloraDevice.libraryTimbres()` resolves to (device.rs list_timbres): every installed timbre, never a device path. */
  async listTimbres() {
    for (let i = 0; i < this.delay; i++) await new Promise((r) => setImmediate(r));
    if (this.listFail) throw new Error(this.listFail);
    const timbres = clone(this.catalog.filter((t) => this.installed.has(t.library_id)));
    return { timbres, libraries: this.installed.size, total: timbres.length, truncated: false };
  }

  state() {
    return clone({
      revision: this.revision, live_set_loaded: this.sounding, live_set_id: this.sounding ? this.liveSetId : "", live_set_name: this.sounding ? this.liveSetName : "", current_scene: this.scene, scene_count: 2,
      parts: this.parts, master: [], performance_init: [...this.perf].map(([k, value]) => {
        const [id, part] = k.split(":").map(Number);
        return { param_id: id, part, value };
      }),
      encoders: { mode: 3, selected_part: 0, assign: this.assignMap },
      edits: {
        available: true, dirty_live_set_ids: this.dirty ? [this.liveSetId] : [], current_dirty: this.dirty, change_summary: this.changes, change_count: this.changes.length || (this.dirty ? 1 : 0), writes_done: this.writesDone,
        has_timbre_edit: this.hasTimbreEdit, scene_write_available: !this.hasTimbreEdit, scene_write_blocked_reason: this.hasTimbreEdit ? "timbre_change" : "",
        live_set_revision: this.liveRevision, studio_edit_active: this.studioActive,
      },
      setlists: { library_revision: 1, loaded_setlist_id: this.loadedSetlist },
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
    const writeJudged = name === "WriteLiveSet" && (msg.base_revision || 0) !== 0; // Story 9.12: a Write is judged like an edit, but is not one
    const edit = ["SetInsertChain", "SetSendLevels", "SetPartOutput", "SetMasterParams", "SetPerformanceParam", "SetPartTimbre", "SetPartVolume", "SetAssignSlot", "ClearAssignSlot"].includes(name);
    const base = msg.base_revision || 0;
    if ((edit || writeJudged) && base !== 0 && base !== this.liveRevision) {
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
      if (this.noteChange(name, b)) this.push(); // like the engine: every structural change publishes a State carrying the new change list
      if (!was || (base !== 0 && !wasActive) || base === 0) this.push(); // like the engine: a State push when something visible flipped
    }
    return reply;
  }

  /** Records the ChangeItem of an accepted edit (State.edits.change_summary), once per kind. */
  noteChange(name, b) {
    const item = { SetPartVolume: { key: "part.volume", part: b.part }, SetSendLevels: { key: "part.send_reverb", part: b.part }, SetPartOutput: { key: "part.output", part: b.part },
      SetInsertChain: { key: "part.inserts", part: b.part }, SetPartTimbre: { key: "part.timbre", part: b.part }, SetPerformanceParam: { key: "part.param", part: b.part, param_id: b.param_id },
      SetMasterParams: { key: "master.param", param_id: b.values && b.values[0] && b.values[0].param_id }, SetAssignSlot: { key: "assign.slot", slot: b.slot - 1 }, ClearAssignSlot: { key: "assign.slot", slot: b.slot - 1 } }[name];
    if (!item) return false;
    const same = (x) => x.key === item.key && x.part === item.part && x.param_id === item.param_id && x.slot === item.slot;
    if (this.changes.some(same)) return false;
    this.changes.push({ part: null, param_id: null, scene: null, slot: null, text: "", ...item });
    return true;
  }

  /** WriteLiveSet with engine/host/src/edit_session.cpp do_write's rules (the C++ tests are the authority). */
  async write(b) {
    this.writeLog.push(clone(b));
    if (this.writeHook) {
      const r = await this.writeHook(b);
      if (r) return r;
    }
    const e = (code, message, details = {}) => ({ request_id: 0, revision: this.revision, body: { Error: { code, message, details } } });
    if (![1, 2, 3].includes(b.scope)) return e(CODE.INVALID_COMMAND, "needs a scope");
    if (!this.sounding) return e(CODE.NO_LIVE_SET, "no Live Set is loaded");
    if (b.scope !== 3 && this.factory) return e(CODE.WRITE_SCOPE_UNAVAILABLE, "factory", { reason: "factory_read_only" });
    let name = this.liveSetName;
    if (b.scope === 3) {
      if (b.new_name != null) {
        name = String(b.new_name).trim();
        if (!name || /[\u0000-\u001f]/.test(name) || new TextEncoder().encode(name).length > 64) return e(25, "invalid name");
        if (this.takenNames.has(name.replace(/[a-z]/g, (c) => c.toUpperCase()))) {
          let n = 2;
          while (this.takenNames.has(`${name.replace(/ \d+$/, "").replace(/[a-z]/g, (c) => c.toUpperCase())} ${n}`)) n++;
          return e(CODE.NAME_IN_USE, "name in use", { name, suggested_name: `${name.replace(/ \d+$/, "")} ${n}` });
        }
      } else name = `${this.liveSetName} 2`;
    }
    if (b.scope === 1) {
      if (this.hasTimbreEdit) return e(CODE.WRITE_SCOPE_UNAVAILABLE, "Timbre changes are saved with the Live Set", { reason: "timbre_change" });
    }
    if (this.storageFull && this.dirty) return e(CODE.STORAGE_FULL, "the storage is full; nothing was written and the edits stay in memory");
    const mine = b.scope === 1 ? this.changes.filter((c) => c.scene === this.scene) : this.changes;
    const nothing = b.scope !== 3 && mine.length === 0 && !(b.scope === 2 && this.dirty);
    if (!nothing) {
      this.writesDone++;
      if (b.scope === 2) { this.changes = []; this.dirty = false; this.hasTimbreEdit = false; }
      else if (b.scope === 1) { this.changes = this.changes.filter((c) => c.scene !== this.scene); this.dirty = this.changes.length > 0; }
      this.revision++; // the Write moves the generic revision, never live_set_revision
      this.push();
    }
    return { request_id: 0, revision: this.revision, body: { WriteReply: { scope: b.scope, live_set_id: this.liveSetId, written_live_set_id: b.scope === 3 ? "ls-new" : this.liveSetId, written_name: name, scene: b.scope === 1 ? this.scene : -1,
      written: mine, written_count: nothing ? 0 : mine.length, nothing_to_write: nothing, current_dirty: this.dirty, remaining_changes: this.changes.length, not_durable: false, setlist_id: "", setlist_index: -1 } } };
  }

  /** What the engines consume of the registry's part parameters (engine/core: OrganEngine, FmEngine (fm_*), VaEngine (va_*, not va_unison_spread: mono unison sum), SampleEngine (part_attack /
   * part_release only), and the Mixer's per-part tone stage for part_brightness / part_eq_* on every engine; the sends belong to the mixer). Engine type ids: 1 sample, 2 organ, 3 FM, 4 VA. */
  consumes(part, id) {
    const d = BY_ID.get(id);
    if (!d) return false;
    if (d.name === "send_reverb_level" || d.name === "send_delay_level") return true;
    if (/^part_(brightness|eq_)/.test(d.name)) return true;
    const type = this.parts[part].engine_type_id;
    if (type === 2) return /^(drawbar_|percussion_|vibrato_chorus_|key_click|leakage|rotary_)/.test(d.name);
    if (type === 3) return /^fm_/.test(d.name);
    if (type === 4) return /^va_/.test(d.name) && d.name !== "va_unison_spread";
    if (type === 1) return /^part_(attack|release)$/.test(d.name);
    return false;
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
          parts: this.parts.map((p) => {
            const t = p.timbre && this.catalog.find((c) => c.library_id === p.timbre.library_id && c.instrument_key === p.timbre.instrument_key);
            const missing = !!p.timbre && p.part_state === 2;
            return { index: p.part, empty: p.empty && !p.timbre, missing_library: missing, unavailable_reason: missing ? "missing_library" : "", on: p.keyboard_on, engine_type_id: p.engine_type_id,
              timbre_name: missing || (p.empty && !p.timbre) ? "" : t ? t.name : p.engine_type_id === 1 ? "Concert Grand" : "", category: t ? t.category : p.engine_type_id === 2 ? 3 : p.engine_type_id === 1 ? 1 : 13,
              volume: p.volume, low_key: p.low_key, high_key: p.high_key, organ_registration: p.engine_type_id === 2 ? "88 8000 000" : "" };
          }),
          part_count: this.parts.length, scene_count: 2, factory: false, dirty: this.dirty } } };
      case "GetSetlists":
        return { revision: this.revision, body: { SetlistsReply: { setlists: [], tags: [], status: { available: true, library_revision: 1 }, detail_setlist_id: "", entries: [], all_live_sets: [{ live_set_id: this.liveSetId, name: "Perfect", missing: false }] } } };
      case "GetState": return { revision: this.revision, body: { State: this.state() } };
      case "WriteLiveSet": return this.write(b);
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
      case "SetPartTimbre": {
        if (!this.partOk(b.part)) return this.err(CODE.INVALID_PART);
        if (!b.library_id || !b.instrument_key) return this.err(CODE.INVALID_COMMAND, "needs a library and an instrument");
        if (this.busyRebuild) return { request_id: 0, revision: this.revision, body: { Error: { code: CODE.BUSY, message: "a Live Set load is in progress", details: { reason: "reload_in_progress" } } } };
        const p = this.parts[b.part];
        p.timbre = { library_id: b.library_id, instrument_key: b.instrument_key };
        p.part_state = this.installed.has(b.library_id) && this.catalog.some((c) => c.library_id === b.library_id && c.instrument_key === b.instrument_key) ? 0 : 2; // the engine takes the reference; the part reads MISSING_LIBRARY when it cannot resolve
        this.hasTimbreEdit = true;
        this.bump();
        return this.ack();
      }
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
