// Velora Studio: the Setlists view model (Story 9.13). A pure ES module: no DOM, no Tauri, no locale. It keeps what the device says about setlists, tags and
// their Live Sets, turns every edit into the SAME ws.v1 control-thread command the device surface sends (CreateSetlist, RenameSetlist, ReorderSetlists,
// DeleteSetlist, AddLiveSetToSetlist, RemoveLiveSetFromSetlist, ReorderLiveSetsInSetlist, CreateLiveSet, CreateTag, RenameTag, DeleteTag, SetSetlistTags) and
// reads the library back with GetSetlists. Everything user-facing is a locale key plus arguments (the renderer owns the strings).
//
// What is deliberately NOT here: LoadSetlist / UnloadSetlist / NextLiveSet / PreviousLiveSet / Jump / Recall / Write (this view never loads or steps anything),
// DeleteLiveSet (removing a Live Set from a setlist never deletes its file) and any Live Set editing.
//
// Consistency rules (the engine is the authority, the view only predicts):
//   * Every mutation is applied optimistically (reorders, renames, removals), sent, and rolled back from a snapshot when the reply is an error. After the last queued
//     mutation the library is ALWAYS read again, so the view converges to the engine's order whatever happened.
//   * Mutations are serialised: one command in flight; a read reply that arrives while a mutation is pending is dropped (it may predate the change).
//   * Reorder commands carry the FULL permutation (ReorderSetlists: all setlist ids; ReorderLiveSetsInSetlist: the whole ordered id list), never a partial one.
//   * A cross-setlist move is two commands, AddLiveSetToSetlist(target) THEN RemoveLiveSetFromSetlist(source): the engine has no atomic move, so a failure between the
//     two leaves the Live Set in both setlists (never in neither) and the re-read shows exactly that, with a typed message.

import { allLiveSetRows, createThrottledRunner, entryRows, setlistsRequest } from "./live_set_view.js";

/** The store's limits (engine/host/include/velora/host/setlist_store.hpp, live_set_store.hpp). The engine stays the authority: these only avoid a pointless round trip. */
export const LIMITS = Object.freeze({
  maxSetlists: 100,
  maxTags: 64,
  maxLiveSetsPerSetlist: 256,
  maxTagsPerSetlist: 8,
  maxNameBytes: 64,
});

/** ws.v1 ErrorCode values used here (proto/ws_v1.proto). */
export const ERROR_CODE = Object.freeze({
  INVALID_COMMAND: 5,
  NO_SUCH_LIVE_SET: 20,
  NO_SUCH_SETLIST: 22,
  NO_SUCH_TAG: 23,
  DUPLICATE_LIVE_SET_IN_SETLIST: 24,
  INVALID_NAME: 25,
  LIMIT_REACHED: 29,
  LIVE_SET_IN_USE: 30,
  INVALID_ORDER: 31,
  LIVE_SET_NOT_IN_SETLIST: 32,
  STORAGE_FAILED: 33,
  BUSY: 13,
  STORAGE_FULL: 36,
  UPSTREAM_UNAVAILABLE: 44,
});

const ERROR_KEYS = new Map([
  [ERROR_CODE.NO_SUCH_SETLIST, "studio.setlists.err.no_such_setlist"],
  [ERROR_CODE.NO_SUCH_TAG, "studio.setlists.err.no_such_tag"],
  [ERROR_CODE.NO_SUCH_LIVE_SET, "studio.setlists.err.no_such_live_set"],
  [ERROR_CODE.DUPLICATE_LIVE_SET_IN_SETLIST, "studio.setlists.err.duplicate"],
  [ERROR_CODE.INVALID_NAME, "studio.setlists.err.invalid_name"],
  [ERROR_CODE.INVALID_ORDER, "studio.setlists.err.invalid_order"],
  [ERROR_CODE.LIVE_SET_NOT_IN_SETLIST, "studio.setlists.err.not_in_setlist"],
  [ERROR_CODE.STORAGE_FAILED, "studio.setlists.err.storage_failed"],
  [ERROR_CODE.STORAGE_FULL, "studio.setlists.err.storage_failed"],
  [ERROR_CODE.BUSY, "studio.setlists.err.busy"],
  [ERROR_CODE.UPSTREAM_UNAVAILABLE, "studio.setlists.err.upstream"],
  [ERROR_CODE.INVALID_COMMAND, "studio.setlists.err.unavailable"],
]);

// ---- names ------------------------------------------------------------------------------------------------------------------------------------------------

function wellFormed(s) {
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff) {
      const d = s.charCodeAt(i + 1);
      if (!(d >= 0xdc00 && d <= 0xdfff)) return false;
      i++;
    } else if (c >= 0xdc00 && c <= 0xdfff) {
      return false;
    }
  }
  return true;
}

/** UTF-8 byte length of a well-formed string (the store counts bytes, not characters). */
export function utf8Length(s) {
  let n = 0;
  for (const ch of String(s)) {
    const c = ch.codePointAt(0);
    n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
  }
  return n;
}

/**
 * A display name as the store wants it (1..64 UTF-8 bytes, valid UTF-8, no control characters), after a client-side trim.
 * Returns `{ ok: true, name }` or `{ ok: false, reason: "empty" | "too_long" | "control" | "encoding" }`.
 */
export function validateName(raw) {
  const name = String(raw === undefined || raw === null ? "" : raw).trim();
  if (!wellFormed(name)) return { ok: false, reason: "encoding" };
  if (name.length === 0) return { ok: false, reason: "empty" };
  // eslint-disable-next-line no-control-regex
  if (/[\u0000-\u001f\u007f]/.test(name)) return { ok: false, reason: "control" };
  if (utf8Length(name) > LIMITS.maxNameBytes) return { ok: false, reason: "too_long" };
  return { ok: true, name };
}

/** The locale key and args that explain a failed validateName. */
export function nameProblem(reason) {
  switch (reason) {
    case "empty": return { key: "studio.setlists.name_empty", args: [] };
    case "too_long": return { key: "studio.setlists.name_too_long", args: [LIMITS.maxNameBytes] };
    case "control": return { key: "studio.setlists.name_control", args: [] };
    default: return { key: "studio.setlists.name_encoding", args: [] };
  }
}

/** The name to show for a row: a missing Live Set has an empty name, so it shows the "missing" label and a short id. */
export function displayName(row, missingLabel) {
  if (row.name) return row.name;
  return `${missingLabel || ""} ${String(row.id || "").slice(0, 8)}`.trim();
}

// ---- ordering helpers --------------------------------------------------------------------------------------------------------------------------------------

/** A copy of `arr` with the item at `from` moved so that it ends at index `to` (clamped). A move to the same place returns an equal copy. */
export function moveItem(arr, from, to) {
  const out = arr.slice();
  if (!Number.isInteger(from) || from < 0 || from >= out.length) return out;
  const t = Math.max(0, Math.min(out.length - 1, Number.isInteger(to) ? to : from));
  const [x] = out.splice(from, 1);
  out.splice(t, 0, x);
  return out;
}

/** Alt+ArrowUp / Alt+ArrowDown: the index the item ends up at, or -1 at the edge (nothing to move) or for any other key. */
export function nudgeTarget(index, count, key) {
  if (key === "ArrowUp") return index > 0 ? index - 1 : -1;
  if (key === "ArrowDown") return index >= 0 && index < count - 1 ? index + 1 : -1;
  return -1;
}

/**
 * Drag and drop: the item at `from` is dropped on the gap before item `slot` (0..count, `count` = after the last one, in the ORIGINAL numbering). The index it ends at.
 * Dropping on the gap right before or right after itself changes nothing (returns `from`).
 */
export function slotToIndex(from, slot) {
  return slot > from ? slot - 1 : slot;
}

/** True when `next` holds exactly the ids of `prev`, each once (the only kind of reorder the store accepts). */
export function isPermutation(prev, next) {
  if (prev.length !== next.length) return false;
  const seen = new Set(prev);
  if (seen.size !== prev.length) return false;
  for (const id of next) if (!seen.delete(id)) return false;
  return seen.size === 0;
}

/**
 * The "Move to…" menu of a Live Set: positions in its own setlist and the other setlists (a Live Set the target already holds is listed disabled when that is known).
 * Items are `{ kind: "position", index }` (final index in the same setlist), `{ kind: "setlist", setlistId, where: "start" | "end", disabled }`.
 */
export function moveTargets({ setlists, selectedId, entries, liveSetId, knownMembers = new Map() }) {
  const at = entries.findIndex((e) => e.id === liveSetId);
  const items = [];
  for (let i = 0; i < entries.length; i++) if (i !== at) items.push({ kind: "position", index: i });
  if (at >= 0 && entries[at].missing) return items; // a missing Live Set has no file the other setlist could use: positions (and Remove) only
  for (const s of setlists) {
    if (s.id === selectedId) continue;
    const members = knownMembers.get(s.id);
    const has = !!(members && members.has(liveSetId));
    const full = s.count >= LIMITS.maxLiveSetsPerSetlist;
    items.push({ kind: "setlist", setlistId: s.id, where: "start", disabled: has || full });
    items.push({ kind: "setlist", setlistId: s.id, where: "end", disabled: has || full });
  }
  return items;
}

// ---- messages -----------------------------------------------------------------------------------------------------------------------------------------------

export const req = Object.freeze({
  get: (setlistId) => setlistsRequest(setlistId || "", true),
  createSetlist: (name) => ({ body: { CreateSetlist: { name } } }),
  renameSetlist: (id, name) => ({ body: { RenameSetlist: { id, name } } }),
  reorderSetlists: (ids) => ({ body: { ReorderSetlists: { ids: ids.slice() } } }),
  deleteSetlist: (id) => ({ body: { DeleteSetlist: { id } } }),
  addLiveSet: (setlistId, liveSetId, index) => ({
    body: { AddLiveSetToSetlist: index === undefined || index === null ? { setlist_id: setlistId, live_set_id: liveSetId } : { setlist_id: setlistId, live_set_id: liveSetId, index } },
  }),
  removeLiveSet: (setlistId, liveSetId) => ({ body: { RemoveLiveSetFromSetlist: { setlist_id: setlistId, live_set_id: liveSetId } } }),
  reorderLiveSets: (setlistId, ids) => ({ body: { ReorderLiveSetsInSetlist: { setlist_id: setlistId, live_set_ids: ids.slice() } } }),
  createLiveSet: (name) => ({ body: { CreateLiveSet: { name } } }),
  createTag: (name) => ({ body: { CreateTag: { name } } }),
  renameTag: (id, name) => ({ body: { RenameTag: { id, name } } }),
  deleteTag: (id) => ({ body: { DeleteTag: { id } } }),
  setSetlistTags: (setlistId, tagIds) => ({ body: { SetSetlistTags: { setlist_id: setlistId, tag_ids: tagIds.slice() } } }),
});

/** The names of the messages this view may send: the only ones. A test keeps this list equal to the bridge's allow-list rows it needs. */
export const SENT_MESSAGES = Object.freeze([
  "GetSetlists", "CreateSetlist", "RenameSetlist", "ReorderSetlists", "DeleteSetlist", "AddLiveSetToSetlist", "RemoveLiveSetFromSetlist",
  "ReorderLiveSetsInSetlist", "CreateLiveSet", "CreateTag", "RenameTag", "DeleteTag", "SetSetlistTags",
]);

function num(v, d) {
  return typeof v === "number" && Number.isFinite(v) ? v : d;
}

/** A ServerMessage as JSON -> `{ ok: true, kind, id, name, notDurable, body }` or `{ ok: false, error }` (typed device error, or a transport failure). */
export function parseReply(reply) {
  const body = reply && reply.body;
  const names = body && typeof body === "object" ? Object.keys(body) : [];
  if (names.length !== 1) return { ok: false, error: { kind: "device", code: 0, message: "no reply", details: {} } };
  const kind = names[0];
  const v = body[kind] || {};
  if (kind === "Error") {
    const details = v.details && typeof v.details === "object" ? { ...v.details } : {};
    return { ok: false, error: { kind: "device", code: num(v.code, 0), message: String(v.message || ""), details } };
  }
  return { ok: true, kind, id: String(v.id || ""), name: String(v.name || ""), notDurable: !!v.not_durable, body: v };
}

/** An error as `{ key, args }` for the renderer. */
export function errorMessage(err) {
  if (!err) return { key: "studio.setlists.err.generic", args: [""] };
  if (err.kind === "client") {
    if (err.reason === "limit") return { key: "studio.setlists.err.limit_" + err.limitName, args: [err.limit] };
    return nameProblem(err.reason);
  }
  if (err.partial) return { key: "studio.setlists.err.partial_move", args: [] }; // before the transport case: a Remove that timed out after the Add is still a partial move
  if (err.kind === "transport") return { key: err.read ? "studio.setlists.err.transport" : "studio.setlists.err.transport_changed", args: [err.message || ""] };
  if (err.code === ERROR_CODE.LIMIT_REACHED) {
    const ln = String((err.details && err.details.limit_name) || "");
    const limit = num(Number(err.details && err.details.limit), 0);
    if (["setlists", "tags", "live_sets_per_setlist", "tags_per_setlist"].includes(ln)) return { key: "studio.setlists.err.limit_" + ln, args: [limit] };
    return { key: "studio.setlists.err.limit_generic", args: [] };
  }
  if (err.code === ERROR_CODE.LIVE_SET_IN_USE) return { key: "studio.setlists.err.in_use", args: [] };
  const key = ERROR_KEYS.get(err.code);
  if (key) return { key, args: [] };
  return { key: "studio.setlists.err.generic", args: [err.message || ""] };
}

// ---- model ---------------------------------------------------------------------------------------------------------------------------------------------------

function parseSummaries(reply) {
  const list = reply && Array.isArray(reply.setlists) ? reply.setlists : [];
  return list.map((s) => ({
    id: String(s.id || ""),
    name: String(s.name || ""),
    tagIds: Array.isArray(s.tag_ids) ? s.tag_ids.map(String) : [],
    count: num(s.live_set_count, 0),
    missing: num(s.missing_count, 0),
    loaded: !!s.loaded,
  }));
}

function parseTags(reply) {
  const list = reply && Array.isArray(reply.tags) ? reply.tags : [];
  return list.map((t) => ({ id: String(t.id || ""), name: String(t.name || "") }));
}

/**
 * Keeps the setlists view up to date. DOM-free: `request` (the device request helper: JSON ClientMessage in, JSON ServerMessage out), the callbacks and the timers are the
 * only collaborators.
 *
 *   refresh()            a throttled, single-flight read (GetSetlists with the selected setlist's entries and All Live Sets); `refreshNow()` reads at once.
 *   select(id)           shows a setlist's Live Sets (a read; a reply for an older selection is dropped).
 *   onState(state)       a State push: reads again (throttled) when the library revision or the loaded setlist moved, never while a mutation is pending.
 *   <mutations>          createSetlist, renameSetlist, deleteSetlist, moveSetlist / nudgeSetlist, addLiveSet, removeLiveSet, moveLiveSet / nudgeLiveSet,
 *                        moveLiveSetToSetlist, createLiveSet, createTag, renameTag, deleteTag, setSetlistTags. Each resolves to `{ ok: true, ... }` or `{ ok: false, error }`.
 * `onChange()` is called after every change of what is shown; `onError(error | null)` with the inline error (null when cleared); `onAnnounce({ key, args })` with what
 * the live region should say; `onNotice({ key, args })` with a non-error message (the change is applied but not durable yet).
 */
export class SetlistsController {
  constructor({ request, onChange, onError, onAnnounce, onNotice, missingLabel, refreshMinMs = 300, clock, setTimer, clearTimer } = {}) {
    this.request = request;
    this.missingLabel = missingLabel || (() => "");
    this._unmodeled = 0; // queued mutations whose result the local model cannot predict (creates, adds): a reorder waits for them
    const guard = (f) => (...a) => { try { if (f) f(...a); } catch (e) { /* a throwing renderer never breaks the controller */ } };
    this.onChange = guard(onChange);
    this.onError = guard(onError);
    this.onAnnounce = guard(onAnnounce);
    this.onNotice = guard(onNotice);
    this.setlists = [];
    this.tags = [];
    this.all = [];
    this.entries = [];
    this.selectedId = "";
    this.loadedId = "";
    this.libraryRevision = -1;
    this.hasData = false;
    this.error = null;
    this.pending = 0;
    this.readCount = 0;
    this.membership = new Map(); // setlist id -> Set of Live Set ids, for the setlists whose entries were read (the selected one, and ones moved into)
    this._queue = Promise.resolve();
    this._readToken = 0;
    this._lastKey = null;
    this._runner = createThrottledRunner(() => this.refreshNow(), { minMs: refreshMinMs, clock, setTimer, clearTimer });
  }

  get selected() { return this.setlists.find((s) => s.id === this.selectedId) || null; }

  // A throwing callback (a renderer bug) must never freeze the controller: every callback goes through here.
  _safe(fn, ...args) {
    try { fn(...args); } catch (e) { /* the view's problem, not the queue's */ }
  }

  _emit() { this._safe(this.onChange); }

  _setError(err) {
    this.error = err;
    this._safe(this.onError, err);
  }

  _nm(row) { return displayName(row, this.missingLabel()); }

  // Reorder `list` (objects with .id) back to `ids`; anything not in `ids` (added meanwhile) keeps its place after them, and ids that are gone are skipped.
  _reorderBack(list, ids) {
    const byId = new Map(list.map((x) => [x.id, x]));
    const out = ids.filter((i) => byId.has(i)).map((i) => byId.get(i));
    list.forEach((x) => { if (!ids.includes(x.id)) out.push(x); });
    return out;
  }

  // ---- reads ----

  refresh() { this._runner(); }

  async refreshNow() {
    for (let attempt = 0; attempt < 3; attempt++) {
      const token = ++this._readToken;
      const want = this.selectedId;
      let reply;
      try {
        reply = await this.request(req.get(want));
      } catch (e) {
        if (token === this._readToken && this.pending === 0) this._setError({ kind: "transport", message: String(e && e.message ? e.message : e), read: true });
        return false;
      }
      if (token !== this._readToken || this.pending > 0) return false; // superseded, or a mutation is in flight (its own read follows)
      const r = parseReply(reply);
      if (!r.ok) {
        if (r.error.code === ERROR_CODE.NO_SUCH_SETLIST && want) {
          this.selectedId = ""; // the selected setlist is gone (deleted on the unit): read again for the list alone, then pick one
          continue;
        }
        this._setError({ ...r.error, read: true }); // previous data stays
        return false;
      }
      if (r.kind !== "SetlistsReply") {
        this._setError({ kind: "device", code: 0, message: r.kind, details: {}, read: true });
        return false;
      }
      this.readCount++;
      const body = r.body;
      this.setlists = parseSummaries(body);
      this.tags = parseTags(body);
      this.all = allLiveSetRows(body);
      const status = body.status || {};
      this.loadedId = String(status.loaded_setlist_id || "");
      this.libraryRevision = num(status.library_revision, 0);
      this._lastKey = `${this.libraryRevision}/${this.loadedId}`;
      this.hasData = true;
      const ids = new Set(this.setlists.map((s) => s.id));
      if (this.selectedId && !ids.has(this.selectedId)) this.selectedId = "";
      if (!this.selectedId && this.setlists.length > 0) {
        this.selectedId = this.setlists[0].id; // keep a selection whenever there is a setlist
        continue; // read its entries
      }
      const e = entryRows(body);
      this.entries = e.setlistId === this.selectedId ? e.rows : [];
      if (this.selectedId && e.setlistId === this.selectedId) this.membership.set(this.selectedId, new Set(e.rows.map((x) => x.id)));
      for (const id of [...this.membership.keys()]) if (!ids.has(id)) this.membership.delete(id);
      if (this.error && this.error.read) this._setError(null); // a good read clears a read error only: the error of a refused command stays until the next change succeeds
      this._emit();
      return true;
    }
    return false;
  }

  async select(id) {
    if (!id || id === this.selectedId) return true;
    this.selectedId = id;
    this.entries = [];
    this._readToken++; // a read that was already on its way answers for the old selection
    this._emit();
    return this.refreshNow();
  }

  onState(state) {
    if (!state || this.pending > 0) return;
    const sl = state.setlists || {};
    const key = `${num(sl.library_revision, 0)}/${sl.loaded_setlist_id || ""}`;
    if (key !== this._lastKey) this._runner();
  }

  reset() {
    this._readToken++;
    this._runner.cancel();
    this.setlists = [];
    this.tags = [];
    this.all = [];
    this.entries = [];
    this.selectedId = "";
    this.loadedId = "";
    this.libraryRevision = -1;
    this.hasData = false;
    this.error = null;
    this.membership.clear();
    this._lastKey = null;
    this._setError(null);
    this._emit();
  }

  // ---- mutations ----

  async _send(message) {
    let reply;
    try {
      reply = await this.request(message);
    } catch (e) {
      return { ok: false, error: { kind: "transport", message: String(e && e.message ? e.message : e) } };
    }
    return parseReply(reply);
  }

  /**
   * Runs one mutation: `apply()` is the optimistic change (synchronous) and returns an `undo()` that puts back ONLY what it changed (so a failure never yanks a selection the
   * user changed meanwhile), `run(send)` sends the command(s) and returns `{ ok: true, ... }` or `{ ok: false, error }`, `onOk(result)` runs after a success.
   * `unmodeled`: the local model does not show this change before the reply (create, add): reorders called meanwhile wait for it, so they are built from the engine's list.
   * After the last queued mutation the library is read again whatever happened (also when a callback throws).
   */
  _exec({ apply, run, onOk, unmodeled }) {
    this.pending++;
    if (unmodeled) this._unmodeled++;
    let undo = null;
    if (apply) {
      try { undo = apply(); } catch (e) { undo = null; }
      this._emit();
    }
    const job = this._queue.then(async () => {
      let result;
      try {
        try {
          result = await run((m) => this._send(m));
        } catch (e) {
          result = { ok: false, error: { kind: "transport", message: String(e && e.message ? e.message : e) } };
        }
        if (result.ok) {
          this._setError(null);
          if (result.notDurable) this._safe(this.onNotice, { key: "studio.setlists.not_durable", args: [] });
          if (onOk) this._safe(onOk, result);
        } else {
          if (undo) this._safe(undo);
          this._setError(result.error);
          this._emit();
        }
      } finally {
        this.pending--;
        if (unmodeled) this._unmodeled--;
        if (this.pending === 0) await this.refreshNow().catch(() => {}); // converge to the engine's order
      }
      return result;
    });
    this._queue = job.catch(() => {});
    return job;
  }

  /** Reorders built from the model must wait for queued creates / adds (their ids are not in the model yet). */
  _afterBarrier(fn) {
    return (async () => {
      while (this._unmodeled > 0) await this._queue;
      return fn();
    })();
  }

  _clientError(error) {
    this._setError(error);
    return Promise.resolve({ ok: false, error });
  }

  _checkName(raw) {
    const v = validateName(raw);
    return v.ok ? { ok: true, name: v.name } : { ok: false, error: { kind: "client", reason: v.reason } };
  }

  createSetlist(raw) {
    const n = this._checkName(raw);
    if (!n.ok) return this._clientError(n.error);
    if (this.setlists.length >= LIMITS.maxSetlists) return this._clientError({ kind: "client", reason: "limit", limitName: "setlists", limit: LIMITS.maxSetlists });
    return this._exec({
      unmodeled: true,
      run: async (send) => {
        const r = await send(req.createSetlist(n.name));
        return r.ok && r.kind === "CreatedReply" ? { ok: true, id: r.id, name: r.name || n.name, notDurable: r.notDurable } : r.ok ? { ok: false, error: { kind: "device", code: 0, message: r.kind, details: {} } } : r;
      },
      onOk: (r) => {
        this.selectedId = r.id; // the new setlist is selected (the re-read after this fills its entries)
        this.entries = [];
        this.onAnnounce({ key: "studio.setlists.announce_setlist_created", args: [r.name] });
      },
    });
  }

  renameSetlist(id, raw) {
    const n = this._checkName(raw);
    if (!n.ok) return this._clientError(n.error);
    const sl = this.setlists.find((s) => s.id === id);
    const old = sl ? sl.name : "";
    return this._exec({
      apply: () => {
        if (sl) sl.name = n.name;
        return () => { if (sl && sl.name === n.name) sl.name = old; };
      },
      run: (send) => send(req.renameSetlist(id, n.name)),
      onOk: () => this.onAnnounce({ key: "studio.setlists.announce_setlist_renamed", args: [n.name] }),
    });
  }

  deleteSetlist(id) {
    const sl = this.setlists.find((s) => s.id === id);
    const name = sl ? sl.name : "";
    return this._exec({
      apply: () => {
        const at = this.setlists.findIndex((s) => s.id === id);
        if (at < 0) return null;
        const removed = this.setlists.splice(at, 1)[0];
        let moved = null;
        const oldEntries = this.entries;
        if (this.selectedId === id) {
          const next = this.setlists[Math.min(at, this.setlists.length - 1)];
          this.selectedId = next ? next.id : "";
          this.entries = [];
          moved = next ? next.id : "";
        }
        return () => {
          if (!this.setlists.some((s) => s.id === id)) this.setlists.splice(Math.min(at, this.setlists.length), 0, removed);
          if (moved !== null && this.selectedId === moved) { this.selectedId = id; this.entries = oldEntries; } // only if the user did not choose another setlist meanwhile
        };
      },
      run: (send) => send(req.deleteSetlist(id)),
      onOk: () => { this.membership.delete(id); this.onAnnounce({ key: "studio.setlists.announce_setlist_deleted", args: [name] }); },
    });
  }

  /** Moves a setlist to `toIndex` (its final index): the FULL permutation of all setlist ids is sent. */
  moveSetlist(id, toIndex) {
    if (this._unmodeled > 0) return this._afterBarrier(() => this.moveSetlist(id, toIndex));
    const from = this.setlists.findIndex((s) => s.id === id);
    if (from < 0) return Promise.resolve({ ok: true, noop: true });
    const ids = this.setlists.map((s) => s.id);
    const next = moveItem(ids, from, toIndex);
    const to = next.indexOf(id);
    if (to === from) return Promise.resolve({ ok: true, noop: true });
    const name = this.setlists[from].name;
    return this._exec({
      apply: () => {
        this.setlists = this._reorderBack(this.setlists, next);
        return () => { this.setlists = this._reorderBack(this.setlists, ids); };
      },
      run: (send) => send(req.reorderSetlists(next)),
      onOk: () => this.onAnnounce({ key: "studio.setlists.announce_setlist_moved", args: [name, to + 1, next.length] }),
    });
  }

  nudgeSetlist(id, key) {
    if (this._unmodeled > 0) return this._afterBarrier(() => this.nudgeSetlist(id, key));
    const at = this.setlists.findIndex((s) => s.id === id);
    const to = nudgeTarget(at, this.setlists.length, key);
    if (to < 0) {
      this.onAnnounce({ key: key === "ArrowUp" ? "studio.setlists.announce_at_start" : "studio.setlists.announce_at_end", args: [] });
      return Promise.resolve({ ok: true, noop: true });
    }
    return this.moveSetlist(id, to);
  }

  /** Adds a Live Set to a setlist (at `index`, absent = the end). Modeled optimistically only through the re-read (the engine decides duplicates and limits). */
  addLiveSet(setlistId, liveSetId, index) {
    const sl = this.setlists.find((s) => s.id === setlistId);
    if (sl && sl.count >= LIMITS.maxLiveSetsPerSetlist) return this._clientError({ kind: "client", reason: "limit", limitName: "live_sets_per_setlist", limit: LIMITS.maxLiveSetsPerSetlist });
    const row = this.all.find((x) => x.id === liveSetId);
    return this._exec({
      unmodeled: true,
      run: (send) => send(req.addLiveSet(setlistId, liveSetId, index)),
      onOk: () => this.onAnnounce({ key: "studio.setlists.announce_added", args: [row ? this._nm(row) : "", sl ? sl.name : ""] }),
    });
  }

  /** Removes a Live Set from a setlist. The Live Set itself (its file) is never touched. */
  removeLiveSet(setlistId, liveSetId) {
    const inSel = setlistId === this.selectedId;
    const at = inSel ? this.entries.findIndex((e) => e.id === liveSetId) : -1;
    const row = at >= 0 ? this.entries[at] : null;
    const sl = this.setlists.find((s) => s.id === setlistId);
    return this._exec({
      apply: () => {
        if (inSel) this.entries = this.entries.filter((e) => e.id !== liveSetId);
        if (sl) { sl.count = Math.max(0, sl.count - 1); if (row && row.missing) sl.missing = Math.max(0, sl.missing - 1); }
        return () => {
          if (row && this.selectedId === setlistId && !this.entries.some((e) => e.id === liveSetId)) this.entries.splice(Math.min(at, this.entries.length), 0, row);
          if (sl) { sl.count += 1; if (row && row.missing) sl.missing += 1; }
        };
      },
      run: (send) => send(req.removeLiveSet(setlistId, liveSetId)),
      onOk: () => {
        const m = this.membership.get(setlistId);
        if (m) m.delete(liveSetId);
        this.onAnnounce({ key: "studio.setlists.announce_removed", args: [row ? this._nm(row) : "", sl ? sl.name : ""] });
      },
    });
  }

  /** Moves a Live Set inside the selected setlist to `toIndex` (its final index): the FULL ordered id list is sent. */
  moveLiveSet(liveSetId, toIndex) {
    if (this._unmodeled > 0) return this._afterBarrier(() => this.moveLiveSet(liveSetId, toIndex));
    const setlistId = this.selectedId;
    const from = this.entries.findIndex((e) => e.id === liveSetId);
    if (!setlistId || from < 0) return Promise.resolve({ ok: true, noop: true });
    const ids = this.entries.map((e) => e.id);
    const next = moveItem(ids, from, toIndex);
    const to = next.indexOf(liveSetId);
    if (to === from) return Promise.resolve({ ok: true, noop: true });
    const name = this._nm(this.entries[from]);
    return this._exec({
      apply: () => {
        this.entries = this._reorderBack(this.entries, next);
        return () => { if (this.selectedId === setlistId) this.entries = this._reorderBack(this.entries, ids); };
      },
      run: (send) => send(req.reorderLiveSets(setlistId, next)),
      onOk: () => this.onAnnounce({ key: "studio.setlists.announce_moved", args: [name, to + 1, next.length] }),
    });
  }

  nudgeLiveSet(liveSetId, key) {
    if (this._unmodeled > 0) return this._afterBarrier(() => this.nudgeLiveSet(liveSetId, key));
    const at = this.entries.findIndex((e) => e.id === liveSetId);
    const to = nudgeTarget(at, this.entries.length, key);
    if (to < 0) {
      this.onAnnounce({ key: key === "ArrowUp" ? "studio.setlists.announce_at_start" : "studio.setlists.announce_at_end", args: [] });
      return Promise.resolve({ ok: true, noop: true });
    }
    return this.moveLiveSet(liveSetId, to);
  }

  /** Moves a Live Set from the selected setlist into another one (`where`: "start" | "end"): Add to the target, then Remove from the source (see the header). */
  moveLiveSetToSetlist(liveSetId, targetId, where = "end") {
    if (this._unmodeled > 0) return this._afterBarrier(() => this.moveLiveSetToSetlist(liveSetId, targetId, where));
    const sourceId = this.selectedId;
    const target = this.setlists.find((s) => s.id === targetId);
    const source = this.setlists.find((s) => s.id === sourceId);
    const at = this.entries.findIndex((e) => e.id === liveSetId);
    const row = at >= 0 ? this.entries[at] : null;
    if (!source || !target || !row || targetId === sourceId) return Promise.resolve({ ok: true, noop: true });
    if (row.missing) return Promise.resolve({ ok: true, noop: true }); // a missing Live Set can only be reordered or removed
    if (target.count >= LIMITS.maxLiveSetsPerSetlist) return this._clientError({ kind: "client", reason: "limit", limitName: "live_sets_per_setlist", limit: LIMITS.maxLiveSetsPerSetlist });
    return this._exec({
      apply: () => {
        this.entries = this.entries.filter((e) => e.id !== liveSetId);
        source.count = Math.max(0, source.count - 1);
        target.count += 1;
        return () => {
          if (this.selectedId === sourceId && !this.entries.some((e) => e.id === liveSetId)) this.entries.splice(Math.min(at, this.entries.length), 0, row);
          source.count += 1;
          target.count = Math.max(0, target.count - 1);
        };
      },
      run: async (send) => {
        const added = await send(req.addLiveSet(targetId, liveSetId, where === "start" ? 0 : undefined));
        if (!added.ok) return added;
        const removed = await send(req.removeLiveSet(sourceId, liveSetId));
        if (!removed.ok) return { ok: false, error: { ...removed.error, partial: true } }; // it is now in both: the re-read shows that
        return { ok: true, notDurable: added.notDurable || removed.notDurable };
      },
      onOk: () => {
        const m = this.membership.get(sourceId);
        if (m) m.delete(liveSetId);
        const t = this.membership.get(targetId);
        if (t) t.add(liveSetId);
        this.onAnnounce({ key: "studio.setlists.announce_moved_to_setlist", args: [this._nm(row), target.name] });
      },
    });
  }

  /** Creates a Live Set (8 empty parts, one scene) and, when `addToSetlistId` is given, adds it to the end of that setlist. */
  createLiveSet(raw, addToSetlistId) {
    const n = this._checkName(raw);
    if (!n.ok) return this._clientError(n.error);
    const target = addToSetlistId ? this.setlists.find((s) => s.id === addToSetlistId) : null;
    if (target && target.count >= LIMITS.maxLiveSetsPerSetlist) return this._clientError({ kind: "client", reason: "limit", limitName: "live_sets_per_setlist", limit: LIMITS.maxLiveSetsPerSetlist });
    return this._exec({
      unmodeled: true,
      run: async (send) => {
        const made = await send(req.createLiveSet(n.name));
        if (!made.ok) return made;
        if (made.kind !== "CreatedReply") return { ok: false, error: { kind: "device", code: 0, message: made.kind, details: {} } };
        if (target) {
          const added = await send(req.addLiveSet(target.id, made.id));
          if (!added.ok) return { ok: false, error: added.error }; // the Live Set exists; the re-read lists it under All Live Sets
        }
        return { ok: true, id: made.id, name: made.name || n.name, notDurable: made.notDurable };
      },
      onOk: (r) => this.onAnnounce({ key: target ? "studio.setlists.announce_live_set_created_in" : "studio.setlists.announce_live_set_created", args: target ? [r.name, target.name] : [r.name] }),
    });
  }

  createTag(raw) {
    const n = this._checkName(raw);
    if (!n.ok) return this._clientError(n.error);
    if (this.tags.length >= LIMITS.maxTags) return this._clientError({ kind: "client", reason: "limit", limitName: "tags", limit: LIMITS.maxTags });
    return this._exec({
      run: async (send) => {
        const r = await send(req.createTag(n.name));
        return r.ok && r.kind === "CreatedReply" ? { ok: true, id: r.id, name: r.name || n.name, notDurable: r.notDurable } : r.ok ? { ok: false, error: { kind: "device", code: 0, message: r.kind, details: {} } } : r;
      },
      onOk: (r) => this.onAnnounce({ key: "studio.setlists.announce_tag_created", args: [r.name] }),
    });
  }

  renameTag(id, raw) {
    const n = this._checkName(raw);
    if (!n.ok) return this._clientError(n.error);
    const tag = this.tags.find((t) => t.id === id);
    const old = tag ? tag.name : "";
    return this._exec({
      apply: () => {
        if (tag) tag.name = n.name;
        return () => { if (tag && tag.name === n.name) tag.name = old; };
      },
      run: (send) => send(req.renameTag(id, n.name)),
      onOk: () => this.onAnnounce({ key: "studio.setlists.announce_tag_renamed", args: [n.name] }),
    });
  }

  deleteTag(id) {
    const tag = this.tags.find((t) => t.id === id);
    const tagAt = this.tags.findIndex((t) => t.id === id);
    return this._exec({
      apply: () => {
        const had = this.setlists.filter((s) => s.tagIds.includes(id));
        this.tags = this.tags.filter((t) => t.id !== id);
        had.forEach((s) => { s.tagIds = s.tagIds.filter((x) => x !== id); });
        return () => {
          if (tag && !this.tags.some((t) => t.id === id)) this.tags.splice(Math.min(tagAt, this.tags.length), 0, tag);
          had.forEach((s) => { if (!s.tagIds.includes(id)) s.tagIds.push(id); });
        };
      },
      run: (send) => send(req.deleteTag(id)),
      onOk: () => this.onAnnounce({ key: "studio.setlists.announce_tag_deleted", args: [tag ? tag.name : ""] }),
    });
  }

  /** Replaces a setlist's tags (at most 8). */
  setSetlistTags(setlistId, tagIds) {
    const unique = [...new Set(tagIds)];
    if (unique.length > LIMITS.maxTagsPerSetlist) return this._clientError({ kind: "client", reason: "limit", limitName: "tags_per_setlist", limit: LIMITS.maxTagsPerSetlist });
    const sl = this.setlists.find((s) => s.id === setlistId);
    const old = sl ? sl.tagIds.slice() : [];
    return this._exec({
      apply: () => {
        if (sl) sl.tagIds = unique.slice();
        return () => { if (sl) sl.tagIds = old; };
      },
      run: (send) => send(req.setSetlistTags(setlistId, unique)),
      onOk: () => this.onAnnounce({ key: unique.length === 1 ? "studio.setlists.announce_tags_set_one" : "studio.setlists.announce_tags_set", args: unique.length === 1 ? [sl ? sl.name : ""] : [sl ? sl.name : "", unique.length] }),
    });
  }
}
