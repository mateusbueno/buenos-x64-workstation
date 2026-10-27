// Velora Studio: the Live Set view model (Story 9.8). A pure ES module: no DOM, no Tauri, no locale. It turns what the device says (a GetLiveSetView reply, the
// engine State pushes, SetlistsReply) into the plain objects app.js renders, and it applies State deltas without a reload. Everything user-facing is left as keys
// and numbers (the renderer owns the strings), so the whole module is testable with `node --test`.
//
// Data in (all are the ws.v1 messages as JSON, the shape `window.veloraDevice.request` and the `device://state` event deliver: snake_case fields, enums as numbers):
//   LiveSetViewReply  -> buildModel(view)         the static picture of one Live Set (stored copy, or the device's unsaved-edit overlay, see proto/ws_v1.proto)
//   State             -> applyState(model, state)  the sounding Live Set follows the pushes: current scene, part on/off, volume, zone, missing-library state, the unsaved flag
//   SetlistsReply     -> setlistRows(reply) / entryRows(reply) / allLiveSetRows(reply)   the sidebar
//
// READ-ONLY: nothing in this file builds a Jump, Recall, Load or Write message. The controller sends GetLiveSetView and nothing else.

export const SCENE_SLOTS = 8;
export const PART_SLOTS = 8; // the fixed Studio layout: the first 8 parts (a Pro unit's 16-part view is deferred, see `hasMoreParts`)

// core::PartState as the engine publishes it (PartState.part_state)
export const PART_STATE = Object.freeze({ READY: 0, EMPTY_TIMBRE: 1, MISSING_LIBRARY: 2, ENGINE_NOT_AVAILABLE: 3 });
// core::EngineTypeId
export const ENGINE = Object.freeze({ SAMPLE: 1, ORGAN: 2, FM: 3, VA: 4 });

// proto/category.proto ids 1..17 -> the slug that is the stem of the locale keys `category.<slug>.short` / `.full`
export const CATEGORY_SLUGS = Object.freeze([
  null, "piano", "epiano", "organ", "keys", "mallet_bells", "strings", "brass", "woodwind", "choir_vocal",
  "guitar", "bass", "synth_lead", "synth_pad", "synth_poly", "drums_perc", "world", "fx_ambient",
]);

const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

/** MIDI note to name with middle C = C4 (60): 36 -> "C2", 0 -> "C-1", 127 -> "G9". Anything that is not a note 0..127 gives "". */
export function noteName(n) {
  if (!Number.isInteger(n) || n < 0 || n > 127) return "";
  return NOTE_NAMES[n % 12] + String(Math.floor(n / 12) - 1);
}

/** "C2–G9" (an en dash, as in the mockup). A reversed or invalid range is shown as given, never repaired. */
export function keyRangeText(lo, hi) {
  const a = noteName(lo);
  const b = noteName(hi);
  return a && b ? `${a}–${b}` : "";
}

/** The locale key of a category's short label (`category.piano.short`), or null for none / unknown. */
export function categoryLabelKey(id) {
  const slug = Number.isInteger(id) ? CATEGORY_SLUGS[id] : null;
  return slug ? `category.${slug}.short` : null;
}

export function engineKind(engineTypeId) {
  switch (engineTypeId) {
    case ENGINE.ORGAN: return "organ";
    case ENGINE.FM: return "fm";
    case ENGINE.VA: return "va";
    default: return "sample"; // 1, and 0 / unknown read as a sample part like the engine's default Live Set
  }
}

function num(v, dflt) {
  return typeof v === "number" && Number.isFinite(v) ? v : dflt;
}

function clamp01(v) {
  return Math.min(1, Math.max(0, num(v, 0)));
}

/** A part's status for the renderer: missing wins over empty, empty over off. */
function partStatus(p) {
  if (p.missing) return "missing";
  if (p.unavailable) return "unavailable"; // the engine this part needs is not in this firmware (FM / VA unbound): silent, never replaced
  if (p.empty) return "empty";
  if (!p.on) return "off";
  return "ready";
}

function makePart(raw, index) {
  const engine = engineKind(num(raw.engine_type_id, 1));
  const missing = !!raw.missing_library;
  const volume = clamp01(raw.volume);
  const part = {
    index,
    engine,
    on: raw.on !== false, // absent = on (an older device)
    empty: !!raw.empty && !missing,
    missing,
    // A part whose library is missing never shows a name: it shows the missing-library state instead (never another timbre's name).
    timbreName: missing ? "" : String(raw.timbre_name || ""),
    categoryId: num(raw.category, 0),
    categoryKey: categoryLabelKey(num(raw.category, 0)),
    volume,
    volumePct: Math.round(volume * 100),
    keyRange: keyRangeText(num(raw.low_key, 0), num(raw.high_key, 127)),
    registration: engine === "organ" ? String(raw.organ_registration || "") : "",
    unavailable: false,
    unavailableReason: missing ? String(raw.unavailable_reason || "") : "",
  };
  part.status = partStatus(part);
  part.addTimbre = part.status === "empty";
  return part;
}

function emptyPart(index) {
  return makePart({ empty: true, on: true, engine_type_id: ENGINE.SAMPLE, low_key: 0, high_key: 127 }, index);
}

function summarize(model) {
  model.activeParts = model.parts.filter((p) => p.on && p.status === "ready").length;
  return model;
}

/**
 * The static model of one Live Set from a LiveSetViewReply (the body of the reply, not the ServerMessage). Always 8 scene tiles and 8 part strips: scenes the
 * Live Set does not have are `empty` (dashed), parts the reply does not carry are empty strips, and parts beyond the 8th (Pro) are counted in `partCount` and
 * flagged by `hasMoreParts` so the renderer can note them. Pure; the reply is not modified.
 */
export function buildModel(view) {
  const v = view || {};
  const rawScenes = Array.isArray(v.scenes) ? v.scenes : [];
  const rawParts = Array.isArray(v.parts) ? v.parts : [];
  const scenes = [];
  for (let i = 0; i < SCENE_SLOTS; i++) {
    const s = rawScenes.find((x) => x && num(x.index, -1) === i) || rawScenes[i];
    const empty = !s || !!s.empty;
    scenes.push({ index: i, empty, name: empty ? "" : String(s.name || ""), current: false });
  }
  const parts = [];
  for (let i = 0; i < PART_SLOTS; i++) {
    const raw = rawParts.find((x) => x && num(x.index, -1) === i);
    parts.push(raw ? makePart(raw, i) : emptyPart(i));
  }
  const partCount = Math.max(num(v.part_count, 0), rawParts.length);
  return summarize({
    id: String(v.live_set_id || ""),
    name: String(v.name || ""),
    dirty: !!v.dirty,
    sounding: !!v.sounding,
    factory: !!v.factory,
    partCount,
    hasMoreParts: partCount > PART_SLOTS,
    currentScene: -1,
    scenes,
    parts,
    activeParts: 0,
  });
}

/**
 * The model with the device's latest State applied. Pure: `base` (from buildModel) is never modified, so a later State that says another Live Set sounds simply
 * gives the static picture back. Rules:
 *   - the unsaved flag follows State.edits (dirty_live_set_ids, current_dirty) when the device runs the edit session;
 *   - only the Live Set that sounds (State.live_set_id == model.id) follows the rest: `current_scene` fills one tile, and each part takes on/off (keyboard_on), volume,
 *     zone, engine and the missing-library state from State.parts[i]. A part the State reports as missing never shows a timbre name; one it reports as an empty
 *     sample timbre becomes the "+ Add timbre" strip.
 */
export function applyState(base, state) {
  if (!base) return null;
  const m = {
    ...base,
    scenes: base.scenes.map((s) => ({ ...s, current: false })),
    parts: base.parts.map((p) => ({ ...p })),
    currentScene: -1,
  };
  if (!state || typeof state !== "object") return summarize(m);
  const edits = state.edits;
  if (edits && edits.available) {
    const ids = Array.isArray(edits.dirty_live_set_ids) ? edits.dirty_live_set_ids : [];
    m.dirty = ids.includes(m.id) || (state.live_set_id === m.id && !!edits.current_dirty);
  }
  m.sounding = !!state.live_set_loaded && !!m.id && state.live_set_id === m.id;
  if (!m.sounding) return summarize(m);
  const cur = num(state.current_scene, -1);
  m.currentScene = cur >= 0 && cur < SCENE_SLOTS && !m.scenes[cur].empty ? cur : -1;
  if (m.currentScene >= 0) m.scenes[m.currentScene].current = true;
  const sp = Array.isArray(state.parts) ? state.parts : [];
  // Story 9.11: while the unit rebuilds the Live Set (a timbre was just assigned: the sound keeps playing from the old generation until the new one is published) its parts still
  // report the OLD generation. The picture read from the unit's edited copy is the truth about what was assigned, so the part's kind (empty / missing / ready) is not overlaid from a
  // State that is about to be replaced: the strip must not flick back to "+ Add timbre" under the player's choice. Volume, on / off and zone follow as always.
  const rebuilding = !!(state.loading && state.loading.loading);
  for (const raw of sp) {
    const i = num(raw && raw.part, -1);
    if (i < 0 || i >= PART_SLOTS) continue;
    // A State from an older unit has none of the per-part fields (JSON / serde read them as zeros: volume 0, zone 0..0, part_state READY). Only a part that says it
    // carries them (`view_fields`) is overlaid; otherwise the static view stands for it (scene, dirty flag and sounding flag still follow).
    if (raw.view_fields !== true) continue;
    const p = m.parts[i];
    if (typeof raw.keyboard_on === "boolean") p.on = raw.keyboard_on;
    p.volume = clamp01(raw.volume);
    p.volumePct = Math.round(p.volume * 100);
    if (typeof raw.low_key === "number" && typeof raw.high_key === "number") p.keyRange = keyRangeText(raw.low_key, raw.high_key);
    if (rebuilding) {
      p.status = partStatus(p);
      p.addTimbre = p.status === "empty";
      continue;
    }
    switch (raw.part_state) {
      case PART_STATE.MISSING_LIBRARY:
        p.missing = true;
        p.empty = false;
        p.unavailable = false;
        p.timbreName = "";
        break;
      case PART_STATE.EMPTY_TIMBRE:
        if (p.engine === "sample") {
          p.empty = true;
          p.missing = false;
          p.unavailable = false;
          p.timbreName = "";
        }
        break;
      case PART_STATE.ENGINE_NOT_AVAILABLE:
        p.unavailable = true;
        p.missing = false;
        p.empty = false;
        break;
      default: // READY: the part resolved and sounds: whatever the static view guessed (missing, empty, unavailable) no longer holds
        p.missing = false;
        p.unavailable = false;
        p.unavailableReason = "";
        p.empty = false;
    }
    p.status = partStatus(p);
    p.addTimbre = p.status === "empty";
  }
  return summarize(m);
}

function same(a, b) {
  return JSON.stringify(a) === JSON.stringify(b);
}

/**
 * What changed between two models, as the indexes a renderer must touch: `{ header, scenes: [..], parts: [..], note }`. A null `prev` (nothing shown yet) changes
 * everything. Tiles that did not change are not listed, so the DOM of unaffected tiles stays as it is.
 */
export function diffModels(prev, next) {
  if (!next) return { header: false, scenes: [], parts: [], note: false, any: false };
  if (!prev || prev.id !== next.id) {
    return {
      header: true,
      scenes: next.scenes.map((s) => s.index),
      parts: next.parts.map((p) => p.index),
      note: true,
      any: true,
    };
  }
  const header = prev.name !== next.name || prev.dirty !== next.dirty || prev.sounding !== next.sounding || prev.activeParts !== next.activeParts || prev.factory !== next.factory;
  const scenes = next.scenes.filter((s, i) => !same(s, prev.scenes[i])).map((s) => s.index);
  const parts = next.parts.filter((p, i) => !same(p, prev.parts[i])).map((p) => p.index);
  const note = prev.hasMoreParts !== next.hasMoreParts || prev.partCount !== next.partCount;
  return { header, scenes, parts, note, any: header || scenes.length > 0 || parts.length > 0 || note };
}

/**
 * A signature of what a State push says about the Live Set that the static view cannot follow by itself (timbre / engine / part-list / scene-name edits, the
 * unsaved flag flipping, a Write, a jump to or away from it). When it changes, the controller re-reads the view: the picture is then taken from the device again.
 * Volume, on/off and scene changes are NOT in it (they are applied in place).
 */
export function structuralSignature(state, liveSetId) {
  if (!state || typeof state !== "object") return "";
  const e = state.edits || {};
  const sounding = !!state.live_set_loaded && state.live_set_id === liveSetId;
  const dirtyIds = Array.isArray(e.dirty_live_set_ids) ? e.dirty_live_set_ids : [];
  const dirty = dirtyIds.includes(liveSetId) || (sounding && !!e.current_dirty);
  const keys = sounding && Array.isArray(e.change_summary)
    ? e.change_summary
      .filter((i) => i && /^(part\.(timbre|engine)|parts\.list|scene\.name|scenes\.list)/.test(String(i.key)))
      .map((i) => `${i.key}:${i.part ?? ""}:${i.scene ?? ""}`)
      .sort()
      .join("|")
    : "";
  // change_summary lists at most 32 items, so the true counts and the timbre flag are folded in too; the Live Set's name (a rename) and the library revision (a library
  // or setlist change) make the view read again as well.
  const name = sounding ? String(state.live_set_name || "") : "";
  const lib = num(state.setlists && state.setlists.library_revision, 0);
  return `${sounding ? 1 : 0}/${dirty ? 1 : 0}/${num(e.writes_done, 0)}/${e.has_timbre_edit ? 1 : 0}/${num(e.change_count, 0)}/${lib}/${name}/${keys}`;
}

/**
 * The next index of a one-row grid (8 scene tiles, 8 part strips) for an arrow key. Wrap rule: Left from the first goes to the last and Right from the last goes to
 * the first; Home / End jump to the ends; Up and Down do not move (a one-row grid has no other row, the focus stays). Any other key returns null (not handled).
 */
export function moveFocus(index, key, count) {
  if (!(count > 0)) return null;
  const i = Number.isInteger(index) && index >= 0 && index < count ? index : 0;
  switch (key) {
    case "ArrowRight": return (i + 1) % count;
    case "ArrowLeft": return (i - 1 + count) % count;
    case "Home": return 0;
    case "End": return count - 1;
    case "ArrowUp":
    case "ArrowDown": return i;
    default: return null;
  }
}

// ---- sidebar rows ------------------------------------------------------------------------------------------------------------------------------------------

/** The setlists of a SetlistsReply as sidebar rows: `{ id, name, count, missing, loaded }`. */
export function setlistRows(reply) {
  const list = reply && Array.isArray(reply.setlists) ? reply.setlists : [];
  return list.map((s) => ({
    id: String(s.id || ""),
    name: String(s.name || ""),
    count: num(s.live_set_count, 0),
    missing: num(s.missing_count, 0),
    loaded: !!s.loaded,
  }));
}

function liveSetRow(e) {
  return { id: String(e.live_set_id || ""), name: String(e.name || ""), missing: !!e.missing };
}

/** The Live Sets of the setlist a SetlistsReply carries entries for, in setlist order; `setlistId` is the one it answers for. */
export function entryRows(reply) {
  const list = reply && Array.isArray(reply.entries) ? reply.entries : [];
  return { setlistId: String((reply && reply.detail_setlist_id) || ""), rows: list.map(liveSetRow) };
}

/** "All Live Sets" (only when the request asked for `include_all_live_sets`). */
export function allLiveSetRows(reply) {
  const list = reply && Array.isArray(reply.all_live_sets) ? reply.all_live_sets : [];
  return list.map(liveSetRow);
}

/** The body of a ServerMessage as JSON, e.g. `{ LiveSetViewReply: {...} }`; returns [name, value] or [null, null]. */
function bodyOf(message) {
  const body = message && message.body;
  if (!body || typeof body !== "object") return [null, null];
  const names = Object.keys(body);
  return names.length === 1 ? [names[0], body[names[0]]] : [null, null];
}

export function viewRequest(liveSetId) {
  return { body: { GetLiveSetView: { live_set_id: liveSetId } } };
}
export function setlistsRequest(setlistId, includeAll) {
  return { body: { GetSetlists: { setlist_id: setlistId || "", include_all_live_sets: !!includeAll } } };
}

// ---- throttled single-flight runner -----------------------------------------------------------------------------------------------------------------------

/**
 * Runs `task` at most once at a time and at most once per `minMs` (a trailing run: a trigger that arrives while the task runs or inside the interval is coalesced into
 * ONE later run), so a stream of triggers (alternating change keys, a library revision that keeps moving) never becomes a continuous request stream.
 * `clock` and `setTimer` are injectable. Returns the trigger function (`trigger.cancel()` drops a scheduled run).
 */
export function createThrottledRunner(task, { minMs = 300, clock, setTimer, clearTimer } = {}) {
  const now = clock || (() => (typeof performance !== "undefined" ? performance.now() : Date.now()));
  const set = setTimer || ((f, ms) => setTimeout(f, ms));
  const clear = clearTimer || ((t) => clearTimeout(t));
  let inflight = false;
  let again = false;
  let timer = null;
  let lastStart = -Infinity;
  const trigger = () => {
    if (inflight) {
      again = true;
      return;
    }
    if (timer !== null) return;
    const wait = lastStart + minMs - now();
    if (wait > 0) {
      timer = set(() => {
        timer = null;
        trigger();
      }, wait);
      return;
    }
    inflight = true;
    again = false;
    lastStart = now();
    Promise.resolve()
      .then(task)
      .catch(() => {})
      .finally(() => {
        inflight = false;
        if (again) {
          again = false;
          trigger();
        }
      });
  };
  trigger.cancel = () => {
    if (timer !== null) clear(timer);
    timer = null;
    again = false;
  };
  return trigger;
}

// ---- the controller --------------------------------------------------------------------------------------------------------------------------------------

/**
 * Keeps one Live Set view up to date. DOM-free: `request` (the device request helper: JSON ClientMessage in, JSON ServerMessage out), `clock` (milliseconds, injectable)
 * and the callbacks are the only collaborators.
 *
 *   select(id)        asks the device for the Live Set's view (GetLiveSetView, read-only: never a Jump / Recall / Load). A failure keeps the previous view and calls
 *                     `onError`. Replies that arrive after a newer select() are dropped, so a slow reply never replaces the Live Set the player chose last.
 *   onState(state)    a State push (or reply): applied at once, synchronously, to the part of the picture it changes; `onModel(model, changes)` is called with only the
 *                     tiles that changed. `lastDeltaMs` is the time from the call to the end of `onModel` (a measurement of this module and the callback, not of the
 *                     device or the webview's paint).
 *   selectPart(i)     the selected part strip (Enter in the part grid); exposed for the Part Editor (Story 9.9) through `selectedPart` and `onSelectPart`.
 *
 * Re-reads (what the in-place update cannot follow) go through a throttled single-flight runner (`refetchMinMs`, default 300 ms). `_signature` is the structural
 * signature of the State that was current when the request that produced the shown view was SENT, so a reply older than a structural change is followed by a re-read.
 */
export class LiveSetViewController {
  constructor({ request, clock, onModel, onError, onSelectPart, onLoading, onFetched, refetchMinMs = 300, setTimer, clearTimer } = {}) {
    this.request = request;
    this.clock = clock || (() => (typeof performance !== "undefined" ? performance.now() : Date.now()));
    this.onModel = onModel || (() => {});
    this.onError = onError || (() => {});
    this.onSelectPart = onSelectPart || (() => {});
    this.onLoading = onLoading || (() => {});
    this.onFetched = onFetched || (() => {});
    this.model = null;
    this.base = null;
    this.state = null;
    this.selectedId = "";
    this.selectedPart = -1;
    this.lastDeltaMs = 0;
    this.maxDeltaMs = 0;
    this.refetches = 0;
    this.pendingSelect = "";
    this._token = 0;
    this._signature = null;
    this._failedSig = null;
    this._runner = createThrottledRunner(() => this._refetchTask(), { minMs: refetchMinMs, clock: this.clock, setTimer, clearTimer });
  }

  /** The Live Set id whose view is shown (a failed select() leaves the previous one). */
  get liveSetId() { return this.selectedId; }
  /** The Live Set id a select() is reading right now ("" when none). */
  get pendingId() { return this.pendingSelect; }

  async select(id) {
    if (!id) return false;
    this.pendingSelect = id;
    this.onLoading(true, id);
    const ok = await this._fetch(id, true);
    this.onLoading(false, id);
    return ok;
  }

  async _fetch(id, isSelect) {
    const token = ++this._token;
    const sentSig = this.state ? structuralSignature(this.state, id) : ""; // "" (never a real signature): a first State after the reply is checked against it
    let reply;
    try {
      reply = await this.request(viewRequest(id));
    } catch (e) {
      if (token === this._token) this._failed(isSelect, sentSig, { kind: "transport", message: String(e && e.message ? e.message : e), id });
      return false;
    }
    if (token !== this._token) return false; // superseded by a newer select()
    const [name, value] = bodyOf(reply);
    if (name !== "LiveSetViewReply") {
      const detail = name === "Error" ? value : {};
      this._failed(isSelect, sentSig, { kind: "device", code: num(detail.code, 0), message: String(detail.message || name || "no reply"), id });
      return false;
    }
    const base = buildModel(value);
    if (base.id !== id) {
      this._failed(isSelect, sentSig, { kind: "mismatch", message: "the reply is for another Live Set", id });
      return false;
    }
    const arrival = this.clock();
    if (isSelect) {
      this.selectedId = id;
      this.pendingSelect = "";
      if (this.selectedPart < 0) this.selectedPart = 0;
    }
    this.base = base;
    this._signature = sentSig; // what the shown view is known to reflect
    this._failedSig = null;
    this._recompute(arrival);
    this.onFetched(id);
    if (this._needsRefetch()) this._runner(); // the reply was older than a structural change that arrived meanwhile
    return true;
  }

  _failed(isSelect, sentSig, error) {
    if (isSelect) this.pendingSelect = "";
    else this._failedSig = this.state && this.base ? structuralSignature(this.state, this.base.id) : null; // no retry until the signature moves again
    this.onError(error);
  }

  _needsRefetch() {
    if (!this.base || this.selectedId !== this.base.id || !this.state || this._signature === null) return false;
    const sig = structuralSignature(this.state, this.base.id);
    return sig !== this._signature && sig !== this._failedSig;
  }

  _recompute(arrival) {
    if (!this.base) return;
    const next = applyState(this.base, this.state);
    const changes = diffModels(this.model, next);
    if (changes.any) {
      try {
        this.onModel(next, changes);
      } catch (e) {
        // the renderer failed: the shown model stays the previous one, so the next delta diffs against what is really on screen
        this.onError({ kind: "render", message: String(e && e.message ? e.message : e), id: next.id });
        return;
      }
    }
    this.model = next;
    this.lastDeltaMs = this.clock() - arrival;
    if (this.lastDeltaMs > this.maxDeltaMs) this.maxDeltaMs = this.lastDeltaMs;
  }

  onState(state) {
    const arrival = this.clock();
    this.state = state;
    if (!this.base) return;
    this._recompute(arrival); // the in-place part first: scene, on/off and volume never wait for a re-read
    if (this._needsRefetch()) this._runner();
  }

  async _refetchTask() {
    if (!this.base || this.selectedId !== this.base.id) return; // a newer select() is the one in charge
    this.refetches++;
    await this._fetch(this.base.id, false);
  }

  selectPart(index) {
    if (!Number.isInteger(index) || index < 0 || index >= PART_SLOTS) return false;
    this.selectedPart = index;
    this.onSelectPart(index, this.model ? this.model.parts[index] : null);
    return true;
  }

  /** The device went away (or was never there): nothing is shown, nothing is kept (the State of before the outage is dropped: the first push after the reconnect is the next one used). */
  reset() {
    this._token++;
    this._runner.cancel();
    this.model = null;
    this.base = null;
    this.state = null;
    this.selectedId = "";
    this.pendingSelect = "";
    this.selectedPart = -1;
    this._signature = null;
    this._failedSig = null;
  }
}
