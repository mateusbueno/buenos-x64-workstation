// Velora Studio: Write to Velora (Story 9.12), the model and controller. A pure ES module: no DOM, no Tauri, no locale catalog. It decides when the Write button is usable, what the Write
// dialog offers (the device's scopes and rules), sends the device's own Write command and keeps the truth about the result. Everything user-facing is a locale key plus arguments.
//
//   THE COMMAND. `WriteLiveSet{scope, new_name?}` is the ONE control-thread command the unit's own Write Dialog sends (ui/src/navigation_controller.cpp handle_write_confirmed ->
//   engine/host/src/edit_session.cpp do_write): there is no second path. It writes the CURRENT (sounding) Live Set only, so Studio offers Write only for the Live Set that sounds.
//   `base_revision` (Story 9.10) IS carried: the engine judges a Write like an edit (STALE_REVISION when the Live Set changed on the unit after the view was loaded, nothing written)
//   without treating it as an edit (no STUDIO, the revision does not move). A Write therefore never persists values Studio never showed.
//
//   THE RULES (the device's, read from State.edits, enforced again by the engine):
//     Scene            disabled with its reason when the edits include a timbre change ("Timbre changes are saved with the Live Set"), when no scene is current, when the Live Set has no
//                      scenes, or when the scene is not in the saved file; a factory Live Set is saved with Save as new only.
//     Whole Live Set   everything: parts, every scene, master, ASSIGN.
//     Save as new      a NEW Live Set (the current one keeps its unsaved edits: the diamond stays). The name is 1..64 bytes of UTF-8 without control characters; a name already used in the
//                      loaded setlist (every Live Set when none is loaded) is refused with the device message and a free name ("Perfect 2") is suggested. The engine stays the authority: a name
//                      that is taken by the time the Write arrives (a race) comes back as NAME_IN_USE with the engine's suggestion.
//
//   THE TRUTH ABOUT RESULTS. Success is claimed only for a WriteReply. A typed error leaves the edits on the unit (◆). A transport failure after the request was issued (the cable was
//   pulled, no reply in time) is UNKNOWN: nothing is claimed, Studio says so, and when the link is back it reads State.edits.writes_done (a counter of files the engine wrote since it
//   started) against the value it saw before sending to tell whether the Write landed, did not, or the unit restarted meanwhile.

import { REGISTRY } from "./params_registry.js";
import { paramLabel } from "./part_editor.js";

export const SCOPE = Object.freeze({ SCENE: 1, WHOLE: 2, NEW: 3 });
export const MAX_NAME_BYTES = 64; // liveset::kMaxNameBytes
export const MAX_CHANGES_SHOWN = 32; // State.edits.change_summary holds at most 32 items; change_count is the true number

/** ws.v1 ErrorCode values a Write can answer (proto/ws_v1.proto). */
export const CODE = Object.freeze({
  INVALID_COMMAND: 5,
  BUSY: 13,
  NO_LIVE_SET: 17,
  INVALID_NAME: 25,
  LIMIT_REACHED: 29,
  STORAGE_FAILED: 33,
  NAME_IN_USE: 34,
  WRITE_SCOPE_UNAVAILABLE: 35,
  STORAGE_FULL: 36,
  UPSTREAM_UNAVAILABLE: 44,
  STALE_REVISION: 47,
});

const num = (v, d = 0) => (typeof v === "number" && Number.isFinite(v) ? v : d);
const str = (v) => (typeof v === "string" ? v : "");

// ---- names (the engine's rules, edit_session.cpp trim_name / fold_name / suggest_free_name, liveset valid_display_name) ------------------------------------------------------------------

export function trimName(s) {
  return String(s == null ? "" : s).replace(/^[ \t\n\r]+|[ \t\n\r]+$/g, "");
}

/** The comparison form: trimmed and ASCII upper-cased only (as the engine does it: "é" and "É" are different names there). */
export function foldName(s) {
  return trimName(s).replace(/[a-z]/g, (c) => c.toUpperCase());
}

function utf8Bytes(s) {
  return new TextEncoder().encode(s).length;
}

/** "" when `name` (already trimmed) is a valid Live Set name, else "empty" | "too_long" | "invalid" (control characters or a lone surrogate, which is not valid UTF-8). */
export function nameProblem(name) {
  if (!name) return "empty";
  if (/[\u0000-\u001f\u007f]/.test(name)) return "invalid";
  for (let i = 0; i < name.length; i++) {
    const c = name.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff) {
      const d = name.charCodeAt(i + 1);
      if (!(d >= 0xdc00 && d <= 0xdfff)) return "invalid"; // a lone surrogate is not UTF-8
      i++;
    } else if (c >= 0xdc00 && c <= 0xdfff) {
      return "invalid";
    }
  }
  if (utf8Bytes(name) > MAX_NAME_BYTES) return "too_long";
  return "";
}

function cutUtf8(s, maxBytes) {
  let out = "";
  for (const ch of s) {
    if (utf8Bytes(out + ch) > maxBytes) break;
    out += ch;
  }
  return out;
}

/** engine suggest_free_name: strip a trailing " N", then the first "<stem> N" (N from 2) whose folded form is not in `takenFolded`; never cuts inside a character, stays within 64 bytes. */
export function suggestFreeName(wanted, takenFolded) {
  let stem = trimName(wanted);
  const m = /^(.*) (\d{1,9})$/.exec(stem);
  if (m && trimName(m[1]) !== "") stem = trimName(m[1]);
  if (!stem) stem = "Live Set";
  const taken = takenFolded instanceof Set ? takenFolded : new Set(takenFolded || []);
  for (let n = 2; ; n++) {
    const suffix = ` ${n}`;
    let s = stem;
    if (utf8Bytes(s) + suffix.length > MAX_NAME_BYTES) s = trimName(cutUtf8(s, MAX_NAME_BYTES - suffix.length));
    const candidate = s + suffix;
    if (!taken.has(foldName(candidate))) return candidate;
  }
}

// ---- what changed ------------------------------------------------------------------------------------------------------------------------------------------------------------------------

const WHAT = new Set(["timbre", "engine", "zone", "octave", "transpose", "keyboard", "volume", "output", "inserts", "send_reverb", "send_delay", "arp"]);

/**
 * The line a ChangeItem (State.edits.change_summary) is shown as, localised from its stable `key` (never from the English `text`, which is only the fallback for a key this Studio
 * does not know). `tr(key, ...args)` is the page's locale lookup. Part and scene numbers are 1-based, ASSIGN slots are K1..K8.
 */
export function changeLabel(item, tr) {
  const key = str(item && item.key);
  const hasPart = Number.isInteger(item && item.part) && item.part >= 0;
  const hasScene = Number.isInteger(item && item.scene) && item.scene >= 0;
  const part = hasPart ? item.part + 1 : 0;
  const scene = hasScene ? item.scene + 1 : 0;
  const param = () => {
    const def = REGISTRY.params.find((p) => p.id === (item && item.param_id));
    return def ? paramLabel(def, (k) => tr(k)) : str(item && item.text) || key;
  };
  const what = (w) => tr(`studio.write.what.${w}`);
  const fallback = () => str(item && item.text) || key;
  switch (key) {
    case "tempo": return tr("studio.write.change.tempo");
    case "parts.list": return tr("studio.write.change.parts_list");
    case "scenes.list": return tr("studio.write.change.scenes_list");
    case "assign.list": return tr("studio.write.change.assign_list");
    case "assign.slot": return Number.isInteger(item.slot) ? tr("studio.write.change.assign_slot", item.slot + 1) : fallback();
    case "master.param": return tr("studio.write.change.master_param", param());
    case "scene.name": return hasScene ? tr("studio.write.change.scene_item", scene, what("name")) : fallback();
    case "scene.master.param": return hasScene ? tr("studio.write.change.scene_master_param", scene, param()) : fallback();
    default: break;
  }
  if (key.startsWith("scene.part.")) {
    const f = key.slice("scene.part.".length);
    if (!hasScene || !hasPart || !(WHAT.has(f) || f === "param")) return fallback();
    return tr("studio.write.change.scene_part_item", scene, part, f === "param" ? param() : what(f));
  }
  if (key.startsWith("part.")) {
    const f = key.slice("part.".length);
    if (!hasPart || !(WHAT.has(f) || f === "param")) return fallback();
    return tr("studio.write.change.part_item", part, f === "param" ? param() : what(f));
  }
  return fallback();
}

// ---- the State as the Write sees it ----------------------------------------------------------------------------------------------------------------------------------------

function sceneReason(edits, scene) {
  if (edits.has_timbre_edit || edits.scene_write_blocked_reason === "timbre_change") return "timbre_change";
  if (edits.scene_write_blocked_reason) return str(edits.scene_write_blocked_reason);
  if (!edits.scene_write_available || scene < 0) return "no_scene";
  return "";
}

/** The reason keys the dialog shows under a disabled scope (the device's wording for the first, a plain sentence for the others). */
export const REASON_KEYS = Object.freeze({
  timbre_change: "studio.write.scene_disabled_timbre",
  no_scene: "studio.write.scene_disabled_no_scene",
  no_scenes: "studio.write.scene_disabled_no_scenes",
  scene_not_in_saved: "studio.write.scene_disabled_not_saved",
  factory_read_only: "studio.write.scope_disabled_factory",
});

/**
 * What the unit holds, for `liveSetId` (the Live Set shown in Studio): `sounding` (the Write command is about the Live Set that sounds), `dirty`, the change list, and the Scene scope's
 * availability with its reason. `state` null (no State yet) says nothing is known.
 */
export function writeSnapshot(state, liveSetId) {
  const st = state && typeof state === "object" ? state : null;
  const edits = (st && st.edits) || {};
  const sounding = !!st && !!st.live_set_loaded && !!liveSetId && st.live_set_id === liveSetId;
  const dirty = !!st && edits.available !== false && !!edits.current_dirty;
  const scene = st ? num(st.current_scene, -1) : -1;
  const items = Array.isArray(edits.change_summary) ? edits.change_summary.filter((i) => i && typeof i === "object") : [];
  const count = Math.max(items.length, num(edits.change_count, items.length));
  return {
    known: !!st,
    sounding,
    dirty,
    anyDirty: !!st && edits.available !== false && !!edits.current_dirty,
    scene,
    sceneReason: sceneReason(edits, scene),
    items,
    count,
    more: Math.max(0, count - items.length),
    hasTimbre: !!edits.has_timbre_edit,
    writesDone: num(edits.writes_done, 0),
    revision: num(edits.live_set_revision, 0),
    name: st ? str(st.live_set_name) : "",
  };
}

/**
 * Is the title-bar "Write to Velora" button usable for the Live Set shown? `{ enabled, reason }`; reason (when disabled) is a locale key for the tooltip: "Nothing to write" when the
 * Live Set has no unsaved edits, the explanation when the edits are on another Live Set (the one that sounds), or while a Write is out / unconfirmed.
 */
export function writeAvailability(state, liveSetId, { writing = false, unconfirmed = false } = {}) {
  if (writing) return { enabled: false, reason: "studio.write.tooltip_writing" };
  if (unconfirmed) return { enabled: false, reason: "studio.write.tooltip_unconfirmed" };
  const s = writeSnapshot(state, liveSetId);
  if (!s.known) return { enabled: false, reason: "studio.write.tooltip_nothing" };
  if (!s.sounding) return { enabled: false, reason: s.anyDirty ? "studio.write.tooltip_not_sounding" : "studio.write.tooltip_nothing" };
  if (!s.dirty) return { enabled: false, reason: "studio.write.tooltip_nothing" };
  return { enabled: true, reason: "" };
}

// ---- errors ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

function errorOf(reply) {
  const b = reply && reply.body;
  if (b && b.Error) return { code: num(b.Error.code, 0), message: str(b.Error.message), details: b.Error.details && typeof b.Error.details === "object" ? b.Error.details : {} };
  return null;
}

/** The sentence part of a failure the engine or the bridge typed (the banner adds "Couldn't write to <device>:" and "Your edits are still on the Velora (◆)."). */
function reasonKeyOf(err) {
  switch (err.code) {
    case CODE.BUSY: return "studio.write.reason.busy";
    case CODE.NO_LIVE_SET: return "studio.write.reason.no_live_set";
    case CODE.STORAGE_FAILED: return "studio.write.reason.storage_failed";
    case CODE.LIMIT_REACHED: return "studio.write.reason.limit_reached";
    case CODE.WRITE_SCOPE_UNAVAILABLE: return "studio.write.reason.scope";
    case CODE.INVALID_COMMAND: return "studio.write.reason.unavailable";
    case CODE.UPSTREAM_UNAVAILABLE: return "studio.write.reason.upstream";
    default: return "studio.write.reason.other";
  }
}

// ---- the controller --------------------------------------------------------------------------------------------------------------------------------------------------------------------

/**
 * The Write to Velora flow. DOM-free; everything outside goes through the options:
 *   request(msg)             the device request helper (ClientMessage JSON in, ServerMessage JSON out; rejects on a transport failure)
 *   flush()                  resolves true when every edit already made has been answered by the unit (PartEditorController.flush), false when it did not in time
 *   writeContext(liveSetId)  { base, known, stale, token }: the Live Set revision the view was loaded at (PartEditorController.writeContext)
 *   noteStale(err, ctx)      raises the Reload banner and locks the knobs (PartEditorController.noteStale); returns whether it did
 *   state()                  the latest State of the unit (JSON) or null
 *   isPaired()               the link is paired right now
 *   takenNames()             Set of folded names a new Live Set must not take (the loaded setlist's, else every Live Set's); may be empty (the engine decides)
 *   deviceName()             "Velora 73" (HelloAck.model), "Velora" when unknown
 *   onChange()               something visible changed
 *   onClose(reason)          the dialog closed: "cancelled" | "written" | "failed" | "stale" | "changed" | "nothing"
 *   onAnnounce({key, args})  a polite live-region message
 *   onWritten(result)        a Write was accepted: re-read the unit (the Live Set view, the State)
 *   onStale(err)             the unit refused the Write as stale (the page decides where the banner shows)
 */
export class WriteController {
  constructor(o = {}) {
    this.request = o.request;
    this.flush = o.flush || (() => Promise.resolve(true));
    this.writeContext = o.writeContext || (() => ({ base: 0, known: true, stale: false, token: 0 }));
    this.noteStale = o.noteStale || (() => false);
    this.getState = o.state || (() => null);
    this.isPaired = o.isPaired || (() => true);
    this.takenNames = o.takenNames || (() => new Set());
    this.deviceName = o.deviceName || (() => "Velora");
    this.onChange = o.onChange || (() => {});
    this.onClose = o.onClose || (() => {});
    this.onAnnounce = o.onAnnounce || (() => {});
    this.onWritten = o.onWritten || (() => {});
    this.onStale = o.onStale || (() => {});
    this.status = "closed"; // closed | open | writing
    this.scope = SCOPE.WHOLE;
    this.name = "";
    this.nameError = null; // { key, args } under the name field
    this.inlineError = null; // { key, args } a refusal that keeps the dialog open
    this.blocked = { scene: "", whole: "" }; // reasons learnt from the engine's refusals
    this.ctx = null; // { liveSetId, liveSetName, factory }
    this.banner = null; // { kind, ... } the Studio Banner about the last failed / unconfirmed Write
    this.unknown = null; // { before: { writesDone, liveSetId }, linkLost }
    this.sent = 0; // Write commands sent (a measurement for the tests)
    this._token = 0;
    this._epoch = 0; // counts the times the link was seen to drop: a Write that fails across a drop is judged by the next State, not before
  }

  // -- derived --------------------------------------------------------------------------------------------------------------------------------------------------------------------------

  get isOpen() { return this.status === "open" || this.status === "writing"; }
  /** The Footer reads WRITING TO VELORA… and the Primary Buttons are disabled: from the confirmation until the answer. */
  get writing() { return this.status === "writing"; }
  /** A Write whose result is not known is waiting to be judged: no second Write meanwhile. */
  get unconfirmed() { return !!this.unknown; }

  availability(liveSetId) {
    return writeAvailability(this.getState(), liveSetId, { writing: this.writing, unconfirmed: this.unconfirmed });
  }

  /** The dialog's content, from the latest State (it follows State pushes while open). */
  view() {
    const ctx = this.ctx || { liveSetId: "", liveSetName: "", factory: false };
    const snap = writeSnapshot(this.getState(), ctx.liveSetId);
    const sceneBlocked = this.blocked.scene || (ctx.factory ? "factory_read_only" : "") || snap.sceneReason;
    const wholeBlocked = this.blocked.whole || (ctx.factory ? "factory_read_only" : "");
    const scopes = [
      { id: SCOPE.SCENE, key: "studio.write.scope.scene", descKey: "studio.write.scope.scene_desc", descArgs: [snap.scene + 1], enabled: !sceneBlocked, reasonKey: sceneBlocked ? REASON_KEYS[sceneBlocked] || "studio.write.scene_disabled_no_scene" : "" },
      { id: SCOPE.WHOLE, key: "studio.write.scope.whole", descKey: "studio.write.scope.whole_desc", descArgs: [], enabled: !wholeBlocked, reasonKey: wholeBlocked ? REASON_KEYS[wholeBlocked] : "" },
      { id: SCOPE.NEW, key: "studio.write.scope.new", descKey: "studio.write.scope.new_desc", descArgs: [], enabled: true, reasonKey: "" },
    ];
    const chosen = scopes.find((s) => s.id === this.scope);
    return {
      liveSetName: snap.name || ctx.liveSetName,
      changes: snap.items,
      more: snap.more,
      count: snap.count,
      scopes,
      scope: this.scope,
      name: this.name,
      nameError: this.nameError,
      inlineError: this.inlineError,
      writing: this.writing,
      canConfirm: this.status === "open" && !!chosen && chosen.enabled && snap.sounding && snap.dirty,
    };
  }

  // -- opening ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------

  /** Opens the dialog for the Live Set shown. Refused (false, nothing opens) when the button would be disabled: nothing to write, not the sounding Live Set, a Write out or unconfirmed. */
  open({ liveSetId, liveSetName = "", factory = false } = {}) {
    if (this.isOpen || !liveSetId) return false;
    if (!this.availability(liveSetId).enabled) return false;
    this.ctx = { liveSetId, liveSetName, factory: !!factory };
    this.blocked = { scene: "", whole: "" };
    this.inlineError = null;
    this.nameError = null;
    this.status = "open";
    const snap = writeSnapshot(this.getState(), liveSetId);
    this.name = suggestFreeName(snap.name || liveSetName, this.takenNames());
    // the device opens on Whole Live Set; a factory Live Set can only be saved as new
    this.scope = factory ? SCOPE.NEW : SCOPE.WHOLE;
    this._token++;
    this.onChange();
    return true;
  }

  setScope(id) {
    if (this.status !== "open") return false;
    const s = this.view().scopes.find((x) => x.id === id);
    if (!s || !s.enabled) return false; // a disabled scope (Scene with a timbre change) cannot be chosen
    this.scope = id;
    this.inlineError = null;
    this.nameError = null;
    this.onChange();
    return true;
  }

  setName(text) {
    if (this.status !== "open") return;
    this.name = String(text == null ? "" : text);
    this.nameError = null;
    this.inlineError = null;
    this.onChange();
  }

  cancel() {
    if (this.status !== "open") return false; // a Write that is out is not taken back
    this._close("cancelled");
    return true;
  }

  _close(reason) {
    this.status = "closed";
    this._token++;
    this.onChange();
    this.onClose(reason);
  }

  // -- the State -------------------------------------------------------------------------------------------------------------------------------------------------------------------------

  /** Every State push: the open dialog follows it (and closes when what it was about is gone); an unconfirmed Write is judged once the link has come back. */
  onState(st) {
    if (this.unknown && this.unknown.linkLost && st && typeof st === "object") this._judgeUnknown(st);
    if (this.status === "open") {
      const snap = writeSnapshot(st, this.ctx && this.ctx.liveSetId);
      if (!snap.sounding) {
        this._close("changed");
        this.onAnnounce({ key: "studio.write.closed_changed", args: [] });
      } else if (!snap.dirty) {
        this._close("nothing"); // the edits were written (or put back) on the unit while the dialog was open
        this.onAnnounce({ key: "studio.write.closed_nothing", args: [] });
      } else {
        this.onChange();
      }
    }
  }

  /** The link dropped. An open dialog closes (nothing is sent); a Write that is out becomes UNKNOWN (see confirm()). */
  onLinkLost() {
    this._epoch++;
    if (this.unknown) this.unknown.linkLost = true;
    if (this.status === "open") {
      this._close("changed");
      this.onAnnounce({ key: "studio.write.closed_disconnected", args: [] });
    }
    this.onChange();
  }

  // -- confirm ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------

  /**
   * Sends the Write. Resolves to what happened: "ignored" (not open, or a Write already out: a double click sends nothing more), "invalid" (the name or scope is refused here, nothing
   * sent), "written", "nothing" (the unit had nothing to write for the scope), "inline" (the unit refused it and the dialog stays open: name in use, scope unavailable), "stale",
   * "failed" (a typed refusal: banner), "unknown" (the answer never came: banner, nothing claimed).
   */
  async confirm() {
    if (this.status !== "open") return "ignored";
    const ctx = this.ctx;
    const view = this.view();
    const scope = this.scope;
    const chosen = view.scopes.find((s) => s.id === scope);
    if (!chosen || !chosen.enabled) {
      this.inlineError = chosen && chosen.reasonKey ? { key: chosen.reasonKey, args: [] } : null;
      this.onChange();
      return "invalid";
    }
    let name = "";
    if (scope === SCOPE.NEW) {
      name = trimName(this.name);
      const problem = nameProblem(name);
      if (problem) {
        this.nameError = { key: `studio.write.err.name_${problem}`, args: [] };
        this.onChange();
        return "invalid";
      }
      const taken = this.takenNames();
      if (taken.has(foldName(name))) {
        this.nameError = { key: "studio.write.err.name_in_use", args: [name] };
        this.name = suggestFreeName(name, taken);
        this.onChange();
        return "invalid";
      }
    }
    this.status = "writing";
    this.inlineError = null;
    this.nameError = null;
    const token = ++this._token;
    this.onChange();
    this.onAnnounce({ key: "studio.write.writing", args: [] });

    // Every edit made so far goes first (a knob still being dragged has its last value waiting for its turn).
    let flushed = false;
    try {
      flushed = await this.flush();
    } catch (e) {
      flushed = false;
    }
    if (token !== this._token || this.status !== "writing") return "ignored";
    if (!flushed) return this._backToOpen({ key: "studio.write.err.flush", args: [] });
    const st = this.getState();
    const snap = writeSnapshot(st, ctx.liveSetId);
    if (!snap.sounding) {
      this.status = "closed";
      this._token++;
      this.onChange();
      this.onClose("changed");
      this.onAnnounce({ key: "studio.write.closed_changed", args: [] });
      return "invalid";
    }
    if (!snap.dirty) {
      this.status = "closed";
      this._token++;
      this.onChange();
      this.onClose("nothing");
      this.onAnnounce({ key: "studio.write.closed_nothing", args: [] });
      return "nothing";
    }
    if (scope === SCOPE.SCENE && snap.sceneReason) {
      // a timbre change arrived while the dialog was open: the Scene scope is gone, nothing is sent
      this.blocked.scene = snap.sceneReason;
      this.scope = SCOPE.WHOLE;
      return this._backToOpen({ key: REASON_KEYS[snap.sceneReason] || "studio.write.scene_disabled_no_scene", args: [] });
    }
    const wctx = this.writeContext(ctx.liveSetId);
    if (wctx.stale) return this._finishStale(null, wctx);
    if (snap.revision > 0 && !wctx.known) return this._backToOpen({ key: "studio.write.err.reading", args: [] });
    const msg = { body: { WriteLiveSet: scope === SCOPE.NEW ? { scope, new_name: name } : { scope } } };
    if (wctx.base > 0) msg.base_revision = wctx.base;
    const before = { writesDone: snap.writesDone, liveSetId: ctx.liveSetId };
    const pairedAtSend = this.isPaired();
    const epoch = this._epoch;
    this.sent++;
    let reply;
    try {
      reply = await this.request(msg);
    } catch (e) {
      if (token !== this._token) return "ignored";
      return this._transportFailure(e, before, pairedAtSend, epoch);
    }
    if (token !== this._token || this.status !== "writing") return "ignored";
    const err = errorOf(reply);
    if (err) return this._typedFailure(err, scope, wctx, name);
    const wr = reply && reply.body && reply.body.WriteReply;
    if (!wr) {
      // an Ack or nothing at all is not a Write result: never claim success for it
      return this._failBanner({ kind: "error", reasonKey: "studio.write.reason.no_answer" });
    }
    return this._success(wr, scope);
  }

  _backToOpen(inline) {
    this.status = "open";
    this.inlineError = inline;
    this._token++;
    this.onChange();
    return "inline";
  }

  _success(wr, scope) {
    this.banner = null;
    this.unknown = null;
    const result = {
      scope: num(wr.scope, scope),
      nothingToWrite: !!wr.nothing_to_write,
      writtenName: str(wr.written_name),
      currentDirty: !!wr.current_dirty,
      remaining: num(wr.remaining_changes, 0),
      notDurable: !!wr.not_durable,
      setlistId: str(wr.setlist_id),
    };
    this.status = "closed";
    this._token++;
    this.lastResult = result;
    this.onChange();
    this.onClose(result.nothingToWrite ? "nothing" : "written");
    if (result.nothingToWrite) {
      this.onAnnounce({ key: "studio.write.nothing_in_scope", args: [] });
    } else {
      const parts = [];
      parts.push({ key: "studio.write.written", args: [] });
      if (result.scope === SCOPE.NEW) parts.push({ key: "studio.write.saved_as_new", args: [result.writtenName] });
      if (result.currentDirty) parts.push({ key: result.scope === SCOPE.NEW ? "studio.write.still_unsaved_new" : "studio.write.still_unsaved", args: [] });
      if (result.notDurable) parts.push({ key: "studio.write.not_durable", args: [] });
      this.onAnnounce({ parts });
    }
    this.onWritten(result);
    return result.nothingToWrite ? "nothing" : "written";
  }

  _typedFailure(err, scope, wctx, name) {
    switch (err.code) {
      case CODE.NAME_IN_USE: {
        // the race: the name was free when Studio looked and is taken now; the engine's suggestion goes into the field
        const suggested = str(err.details.suggested_name);
        const taken = str(err.details.name) || name;
        this.status = "open";
        this.nameError = { key: "studio.write.err.name_in_use", args: [taken] };
        if (suggested && nameProblem(suggested) === "") this.name = suggested;
        this._token++;
        this.onChange();
        return "inline";
      }
      case CODE.INVALID_NAME:
        this.status = "open";
        this.nameError = { key: "studio.write.err.name_invalid", args: [] };
        this._token++;
        this.onChange();
        return "inline";
      case CODE.WRITE_SCOPE_UNAVAILABLE: {
        const reason = str(err.details.reason);
        if (reason === "factory_read_only") {
          this.blocked.scene = "factory_read_only";
          this.blocked.whole = "factory_read_only";
          this.scope = SCOPE.NEW;
        } else {
          this.blocked.scene = reason || "no_scene";
          if (this.scope === SCOPE.SCENE) this.scope = SCOPE.WHOLE;
        }
        return this._backToOpen({ key: REASON_KEYS[reason] || "studio.write.scene_disabled_no_scene", args: [] });
      }
      case CODE.STALE_REVISION:
        return this._finishStale(err, wctx);
      case CODE.STORAGE_FULL:
        return this._failBanner({ kind: "storage_full" });
      case CODE.UPSTREAM_UNAVAILABLE:
        // "connect_failed": the bridge never forwarded it (definitely not written). "timeout": the engine never answered (it may have written: unknown). Anything else: refused.
        if (str(err.details.reason) === "timeout") return this._unknownFailure({ writesDone: this._writesDone(), liveSetId: this.ctx.liveSetId }, false);
        return this._failBanner({ kind: "connection" });
      default:
        return this._failBanner({ kind: "error", reasonKey: reasonKeyOf(err) });
    }
  }

  _writesDone() {
    return writeSnapshot(this.getState(), this.ctx && this.ctx.liveSetId).writesDone;
  }

  _finishStale(err, wctx) {
    this.status = "closed";
    this._token++;
    this.onChange();
    this.onClose("stale");
    if (err) this.noteStale(err, wctx); // (a view already known to be stale has its banner up)
    this.onStale(err);
    this.onAnnounce({ key: "studio.write.stale", args: [] });
    return "stale";
  }

  _failBanner(banner) {
    this.status = "closed";
    this.banner = { ...banner, device: this.deviceName() };
    this._token++;
    this.onChange();
    this.onClose("failed");
    this.onAnnounce({ banner: this.banner });
    return "failed";
  }

  _transportFailure(e, before, pairedAtSend, epoch) {
    const msg = String(e && e.message ? e.message : e);
    // "not connected" with the link down at the moment of sending: the request never left this computer, so nothing was written
    if (!pairedAtSend && /not connected/i.test(msg)) return this._failBanner({ kind: "connection" });
    return this._unknownFailure(before, !this.isPaired() || this._epoch !== epoch);
  }

  _unknownFailure(before, linkAlreadyLost) {
    this.unknown = { before, linkLost: !!linkAlreadyLost };
    this.status = "closed";
    this.banner = { kind: "unknown", device: this.deviceName() };
    this._token++;
    this.onChange();
    this.onClose("failed");
    this.onAnnounce({ banner: this.banner });
    return "unknown";
  }

  // -- unknown results -------------------------------------------------------------------------------------------------------------------------------------------------------------

  _judgeUnknown(st) {
    const u = this.unknown;
    if (!u) return;
    const edits = st.edits || {};
    const wd = num(edits.writes_done, 0);
    this.unknown = null;
    if (wd > u.before.writesDone) {
      this.banner = null;
      this.lastResult = { recovered: true, currentDirty: !!edits.current_dirty };
      this.onAnnounce({ key: "studio.write.confirmed_after" });
      this.onWritten(this.lastResult);
    } else if (wd < u.before.writesDone) {
      this.banner = { kind: "restarted", device: this.deviceName() };
      this.onAnnounce({ banner: this.banner });
    } else if (edits.current_dirty) {
      this.banner = { kind: "not_landed", device: this.deviceName() };
      this.onAnnounce({ banner: this.banner });
    } else {
      this.banner = { kind: "clean", device: this.deviceName() };
      this.onAnnounce({ banner: this.banner });
    }
    this.onChange();
  }

  /** "Check now" on the unconfirmed banner: reads the unit and judges. Resolves whether the unit answered. */
  async recheck() {
    if (!this.unknown) return true;
    let reply;
    try {
      reply = await this.request({ body: { GetState: {} } });
    } catch (e) {
      return false;
    }
    const st = reply && reply.body && reply.body.State;
    if (!st) return false;
    this._judgeUnknown(st);
    return true;
  }

  showBanner(kind) {
    this.banner = { kind, device: this.deviceName() };
    this.onChange();
  }

  dismissBanner() {
    if (!this.banner) return;
    this.banner = null;
    this.onChange();
  }

  /** The link is gone for good (unpaired / another device): everything about the Write is forgotten. */
  reset() {
    this._token++;
    this.status = "closed";
    this.ctx = null;
    this.banner = null;
    this.unknown = null;
    this.inlineError = null;
    this.nameError = null;
    this.onChange();
  }
}
