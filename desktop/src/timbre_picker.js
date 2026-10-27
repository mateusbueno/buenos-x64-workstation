// Velora Studio: the "+ Add timbre" picker model and controller (Story 9.11). A pure ES module: no DOM, no Tauri, no locale. It owns what the picker knows (the timbres
// installed on the unit, the search, the active row, the state of the load and of the assignment) and what it asks of the outside world through two functions:
//
//   listTimbres()                         -> { timbres: [{library_id, library_name, instrument_key, name, category}], total, truncated }   (Rust: device_library_timbres)
//   assign({ part, libraryId, instrumentKey, liveSetId })  -> { ok: true } | { ok: false, stale: true } | { ok: false, error: { code, message, details } }
//                                                                                                                   (PartEditorController.assignTimbre)
//
// Rules this module keeps (all tested in tests/timbre_picker.test.js):
//   - READ-ONLY until the player chooses: opening the picker, searching and moving never send anything that changes the unit; `choose()` sends ONE assignment.
//   - Nothing is assigned except the exact (library_id, instrument_key) of the row the player chose: a row is never matched by name, the list is never re-sorted under a pending
//     choice, and an assignment that fails (the engine refuses, the library vanished, the revision is stale, the link dropped) leaves the part as it was.
//   - A reply that arrives after the picker was closed, re-opened for another part, or aborted is dropped (tokens), so a slow list never fills the wrong dialog.
//   - The list is searched client-side, diacritic- and case-insensitively and by words; at most MAX_VISIBLE rows are drawn (a huge catalog never becomes a huge DOM) and the cap
//     is said, not hidden.
import { categoryLabelKey } from "./live_set_view.js";

export const MAX_VISIBLE = 200; // rows drawn at once (search narrows them); 5000 timbres is the most the unit side lists (device.rs MAX_PICKER_TIMBRES)
export const MAX_QUERY = 80; // characters of the search text that count

// ---- text folding ---------------------------------------------------------------------------------------------------------------------------------------------

/**
 * The text a search compares: NFKC (full-width and compatibility forms), then NFD with the combining marks removed (so "Rhodes" finds "Rhôdes", "cafe" finds "café", "senor" finds "señor"),
 * lower-cased, whitespace collapsed. Never throws and never returns anything but a string. Characters without a decomposition (CJK, kana) are kept as they are.
 */
export function fold(text) {
  let s = String(text == null ? "" : text);
  try {
    s = s.normalize("NFKC").normalize("NFD").replace(/[\u0300-\u036f]/g, "").normalize("NFC");
  } catch (e) {
    // an engine without normalize(): compare as given
  }
  return s.toLowerCase().replace(/\s+/g, " ").trim();
}

// ---- the list ----------------------------------------------------------------------------------------------------------------------------------------------------

function str(v) {
  return typeof v === "string" ? v : "";
}

/**
 * The rows of a timbre list reply, cleaned: a row without a library id, an instrument key or a name is dropped (it could not be assigned or told apart); a name that is only
 * whitespace or control characters falls back to the key (still a row the player can choose). Rows are sorted by category (the unit's order of ids, none last), then by folded name, then
 * library name, then key: a total order, so the same catalog always draws the same list.
 */
export function buildItems(reply) {
  const raw = reply && Array.isArray(reply.timbres) ? reply.timbres : [];
  const items = [];
  for (const t of raw) {
    if (!t || typeof t !== "object") continue;
    const libraryId = str(t.library_id);
    const key = str(t.instrument_key);
    if (!libraryId || !key) continue;
    const name = str(t.name).replace(/[\u0000-\u001f\u007f]/g, " ").trim() || key;
    const libraryName = str(t.library_name).replace(/[\u0000-\u001f\u007f]/g, " ").trim();
    const category = Number.isInteger(t.category) && t.category >= 1 && t.category <= 17 ? t.category : 0;
    items.push({
      id: `${libraryId}\u0000${key}`,
      libraryId,
      instrumentKey: key,
      name,
      libraryName,
      category,
      categoryKey: categoryLabelKey(category),
      haystack: fold(`${name} ${libraryName}`),
      nameFolded: fold(name),
    });
  }
  items.sort((a, b) => {
    const ca = a.category === 0 ? 99 : a.category;
    const cb = b.category === 0 ? 99 : b.category;
    return ca - cb || (a.nameFolded < b.nameFolded ? -1 : a.nameFolded > b.nameFolded ? 1 : 0) || (a.libraryName < b.libraryName ? -1 : a.libraryName > b.libraryName ? 1 : 0) || (a.instrumentKey < b.instrumentKey ? -1 : a.instrumentKey > b.instrumentKey ? 1 : 0);
  });
  return items;
}

/**
 * Items matching `query`: every word of the query (split on spaces) must occur in the folded name or library name, or in the category's label when `categoryLabel(item)` is given. An
 * empty query matches everything. The category label is passed in because this module has no locale.
 */
export function filterItems(items, query, categoryLabel) {
  const q = fold(String(query || "").slice(0, MAX_QUERY));
  if (!q) return items;
  const words = q.split(" ").filter(Boolean);
  return items.filter((it) => {
    const label = categoryLabel ? fold(categoryLabel(it)) : "";
    return words.every((w) => it.haystack.includes(w) || (label !== "" && label.includes(w)));
  });
}

/**
 * The visible rows as `{ groups: [{ category, categoryKey, items: [...] }], shown, matched, capped }`: grouped by category in list order, at most MAX_VISIBLE items in all. `matched` is the
 * number of items that match, `shown` the number drawn.
 */
export function visibleGroups(items) {
  const groups = [];
  let shown = 0;
  for (const it of items) {
    if (shown >= MAX_VISIBLE) break;
    let g = groups[groups.length - 1];
    if (!g || g.category !== it.category) {
      g = { category: it.category, categoryKey: it.categoryKey, items: [] };
      groups.push(g);
    }
    g.items.push(it);
    shown++;
  }
  return { groups, shown, matched: items.length, capped: items.length > shown };
}

/** The next active index in a list of `count` rows for a navigation key (-1 = none yet). Down from the last stays on the last (a list, not a grid: no wrap); PageUp / PageDown move 10. */
export function moveActive(index, key, count) {
  if (!(count > 0)) return -1;
  const i = Number.isInteger(index) ? index : -1;
  switch (key) {
    case "ArrowDown": return Math.min(count - 1, i + 1);
    case "ArrowUp": return i <= 0 ? 0 : i - 1;
    case "Home": return 0;
    case "End": return count - 1;
    case "PageDown": return Math.min(count - 1, Math.max(0, i) + 10);
    case "PageUp": return Math.max(0, (i < 0 ? 0 : i) - 10);
    default: return i;
  }
}

// ---- the controller --------------------------------------------------------------------------------------------------------------------------------------------

/**
 * One picker at a time. `status`: "closed" | "loading" | "ready" | "empty" | "error" | "assigning". `onChange()` is called whenever anything the view draws changed;
 * `onClose(reason, detail)` when the picker closes itself: reason "assigned" (the unit took the timbre), "stale" (the Live Set changed on the unit: the editor shows its banner), "aborted"
 * (the link dropped, or the part / Live Set changed under it: nothing was changed), and "cancelled" (Esc / Cancel: nothing was changed).
 */
export class TimbrePickerController {
  constructor({ listTimbres, assign, categoryLabel, onChange, onClose } = {}) {
    this.listTimbres = listTimbres;
    this.assign = assign;
    this.categoryLabel = categoryLabel || (() => "");
    this.onChange = onChange || (() => {});
    this.onClose = onClose || (() => {});
    this._token = 0;
    this._reset();
  }

  _reset() {
    this.status = "closed";
    this.context = null; // { liveSetId, part }
    this.items = [];
    this.truncated = false;
    this.query = "";
    this.filtered = [];
    this.view = { groups: [], shown: 0, matched: 0, capped: false };
    this.active = -1; // index into the drawn rows (flat), -1 = none
    this.error = null; // { kind: "load" | "assign", message }
    this.chosen = null; // the item an assignment is in flight for
  }

  get isOpen() { return this.status !== "closed"; }

  /** The drawn rows in order (flat), the same order `view.groups` draws. */
  get rows() {
    const out = [];
    for (const g of this.view.groups) for (const it of g.items) out.push(it);
    return out;
  }

  get activeItem() {
    return this.active >= 0 ? this.rows[this.active] || null : null;
  }

  /** Opens the picker for `part` of `liveSetId` and reads the list. Opening again (another part) replaces the previous picker; its late replies are dropped. */
  async open({ liveSetId, part }) {
    const token = ++this._token;
    this._reset();
    this.context = { liveSetId: String(liveSetId || ""), part };
    this.status = "loading";
    this.onChange();
    return this._load(token);
  }

  async _load(token) {
    let reply;
    try {
      reply = await this.listTimbres();
    } catch (e) {
      if (token !== this._token) return false;
      this.status = "error";
      this.error = { kind: "load", message: String(e && e.message ? e.message : e) };
      this.onChange();
      return false;
    }
    if (token !== this._token) return false; // closed, aborted or re-opened meanwhile
    this.items = buildItems(reply);
    this.truncated = !!(reply && reply.truncated);
    this.error = null;
    this.status = this.items.length === 0 ? "empty" : "ready";
    this._refilter();
    this.active = this.rows.length > 0 ? 0 : -1;
    this.onChange();
    return true;
  }

  /** Reads the list again after a failed load. */
  async retry() {
    if (this.status !== "error" || !this.context || this.error?.kind !== "load") return false;
    const token = ++this._token;
    this.status = "loading";
    this.error = null;
    this.onChange();
    return this._load(token);
  }

  _refilter() {
    this.filtered = filterItems(this.items, this.query, this.categoryLabel);
    this.view = visibleGroups(this.filtered);
  }

  /** The search text changed. The active row follows the first match; the list keeps its order. */
  setQuery(text) {
    if (this.status === "closed" || this.status === "assigning") return;
    const q = String(text || "").slice(0, MAX_QUERY);
    if (q === this.query) return;
    this.query = q; // while the list loads the text is kept and applied when it arrives
    this._refilter();
    this.active = this.rows.length > 0 ? 0 : -1;
    this.onChange();
  }

  /** A navigation key moved the active row; returns the new index (unchanged when the key is not a movement or nothing is drawn). */
  move(key) {
    if (this.status !== "ready") return this.active;
    const next = moveActive(this.active, key, this.rows.length);
    if (next !== this.active) {
      this.active = next;
      this.onChange();
    }
    return this.active;
  }

  setActive(index) {
    if (this.status !== "ready" || !Number.isInteger(index) || index < 0 || index >= this.rows.length || index === this.active) return;
    this.active = index;
    this.onChange();
  }

  /** The player chose `item` (the row's own object, identified by library and instrument key, never by name). Sends the one assignment. Resolves true when the unit took it. */
  async choose(item) {
    if (this.status !== "ready" || !item || !this.context) return false;
    const row = this.rows.find((r) => r.id === item.id);
    if (!row) return false;
    const token = this._token;
    const ctx = this.context;
    this.chosen = row;
    this.status = "assigning";
    this.error = null;
    this.onChange();
    let res;
    try {
      res = await this.assign({ part: ctx.part, libraryId: row.libraryId, instrumentKey: row.instrumentKey, liveSetId: ctx.liveSetId });
    } catch (e) {
      res = { ok: false, error: { code: 0, message: String(e && e.message ? e.message : e), details: {} } };
    }
    if (token !== this._token || this.status !== "assigning") return false; // aborted (link dropped) or closed while the request was out: the abort already reported
    if (res && res.ok) {
      const done = row;
      this._finish("assigned", done);
      return true;
    }
    if (res && res.stale) {
      this._finish("stale", null);
      return false;
    }
    this.chosen = null;
    this.status = "ready"; // the picker stays open on the same list: the player can try again or pick another
    this.error = { kind: "assign", message: String(res && res.error && res.error.message ? res.error.message : "") || String(res && res.error && res.error.code ? res.error.code : "") };
    this.onChange();
    return false;
  }

  /** Esc, the Cancel button, a click outside: closes without sending anything. During an assignment the request is already out: the picker cannot take it back and says so by staying open. */
  cancel() {
    if (this.status === "closed" || this.status === "assigning") return false;
    this._finish("cancelled", null);
    return true;
  }

  /** The link dropped, the Live Set or the part changed under the picker: it closes, nothing is assigned by it from now on (a request already out is judged by the unit). */
  abort(why) {
    if (this.status === "closed") return false;
    this._finish("aborted", why || "");
    return true;
  }

  _finish(reason, detail) {
    const ctx = this.context;
    this._token++;
    this._reset();
    this.onChange();
    this.onClose(reason, detail, ctx);
  }
}
