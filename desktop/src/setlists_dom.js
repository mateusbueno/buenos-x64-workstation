// Velora Studio: the DOM side of the Setlists view (Story 9.13): the two lists (setlists, and the Live Sets of the selected one), their keyboard and mouse reordering,
// the "Move to…" and "Add Live Set…" menus, the dialogs (name, confirm, tags) and the live-region announcements. Everything that comes from the device (names) is
// written with textContent / setAttribute only, never as markup. `doc` is the page's document (or a shim in tests); `tr(key, ...args)` is the locale lookup;
// `announce(text)` writes to the polite live region. The model, the commands and the rollback live in setlists_view.js (SetlistsController).
import { LIMITS, displayName, errorMessage, moveTargets, nameProblem, slotToIndex, validateName } from "./setlists_view.js";

function el(doc, tag, cls, text) {
  const e = doc.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

function button(doc, label, onClick, cls) {
  const b = el(doc, "button", cls ? `btn ${cls}` : "btn", label);
  b.type = "button";
  b.addEventListener("click", onClick);
  return b;
}

function setClass(node, c, on) {
  const parts = String(node.className || "").split(/\s+/).filter(Boolean).filter((x) => x !== c);
  if (on) parts.push(c);
  node.className = parts.join(" ");
}

function closestOption(target) {
  return target && target.closest ? target.closest('[role="option"]') : null;
}

// The enabled buttons and inputs inside a node, in document order (a real DOM, or the test shim's `all`).
function focusables(node) {
  const all = node.querySelectorAll ? Array.from(node.querySelectorAll("button, input")) : node.all((n) => n.tagName === "BUTTON" || n.tagName === "INPUT");
  return all.filter((n) => !n.disabled);
}

export class SetlistsView {
  constructor({ doc, host, tr, controller, announce, onClose }) {
    this.doc = doc;
    this.host = host;
    this.tr = tr;
    this.ctl = controller;
    this.announce = announce || (() => {});
    this.onClose = onClose || (() => {});
    this.focusSetlist = ""; // roving tabindex targets (ids)
    this.activeEntry = ""; // the Live Set "Move to…" and "Remove" act on
    this.drag = null; // { kind: "setlist" | "liveset", id }
    this.drop = null; // { slot } | { into }
    this.deferred = false;
    this.dialog = null; // { backdrop, restore }
    this.built = false;
    this.disabled = false;
  }

  // ---- skeleton -------------------------------------------------------------------------------------------------------------------------------------------

  build() {
    if (this.built) return;
    this.built = true;
    const d = this.doc;
    this.host.textContent = "";
    const head = el(d, "div", "sv-head");
    this.title = el(d, "h1", "editor-header-title");
    this.title.tabIndex = -1;
    const actions = el(d, "div", "actions-section");
    this.btnNewSetlist = button(d, "", () => this.openNewSetlist(), "");
    this.btnNewLiveSet = button(d, "", () => this.openNewLiveSet(), "");
    this.btnClose = button(d, "", () => this.onClose(), "");
    actions.append(this.btnNewSetlist, this.btnNewLiveSet, this.btnClose);
    head.append(this.title, actions);

    this.errorEl = el(d, "div", "live-error sv-error");
    this.errorEl.setAttribute("role", "alert");
    this.errorEl.hidden = true;
    this.noticeEl = el(d, "div", "live-note sv-notice");
    this.noticeEl.setAttribute("role", "status");
    this.noticeEl.hidden = true;

    const cols = el(d, "div", "sv-cols");
    // setlists column
    const colA = el(d, "section", "sv-col");
    this.headingA = el(d, "h2", "sidebar-heading");
    const barA = el(d, "div", "sv-bar");
    this.btnRename = button(d, "", () => this.openRenameSetlist(), "");
    this.btnTags = button(d, "", () => this.openTags(), "");
    this.btnDelete = button(d, "", () => this.openDeleteSetlist(), "");
    barA.append(this.btnRename, this.btnTags, this.btnDelete);
    this.listA = el(d, "div", "sv-list");
    this.listA.setAttribute("role", "listbox");
    this.listA.setAttribute("aria-orientation", "vertical");
    this.emptyA = el(d, "div", "sv-empty");
    colA.append(this.headingA, barA, this.listA, this.emptyA);
    // Live Sets column
    const colB = el(d, "section", "sv-col");
    this.headingB = el(d, "h2", "sidebar-heading");
    const barB = el(d, "div", "sv-bar");
    this.btnAdd = button(d, "", () => this.openAddLiveSet(), "");
    this.btnMove = button(d, "", () => this.openMoveTo(), "");
    this.btnRemove = button(d, "", () => this.openRemoveLiveSet(), "");
    barB.append(this.btnAdd, this.btnMove, this.btnRemove);
    this.listB = el(d, "div", "sv-list");
    this.listB.setAttribute("role", "listbox");
    this.listB.setAttribute("aria-orientation", "vertical");
    this.emptyB = el(d, "div", "sv-empty");
    colB.append(this.headingB, barB, this.listB, this.emptyB);
    cols.append(colA, colB);

    this.dialogHost = el(d, "div", "sv-dialog-host");
    this.host.append(head, this.errorEl, this.noticeEl, cols, this.dialogHost);

    this._wireList(this.listA, "setlist");
    this._wireList(this.listB, "liveset");
    this.setLabels();
  }

  /** Static texts (re-applied on a language change). */
  setLabels() {
    const tr = this.tr;
    this.title.textContent = tr("studio.setlists.title");
    this.btnNewSetlist.textContent = tr("studio.setlists.new_setlist");
    this.btnNewLiveSet.textContent = tr("studio.setlists.new_live_set");
    this.btnClose.textContent = tr("studio.setlists.close");
    this.headingA.textContent = tr("studio.setlists.setlists_heading");
    this.btnRename.textContent = tr("studio.setlists.rename");
    this.btnTags.textContent = tr("studio.setlists.tags");
    this.btnDelete.textContent = tr("studio.setlists.delete");
    this.btnAdd.textContent = tr("studio.setlists.add_live_set");
    this.btnMove.textContent = tr("studio.setlists.move_to");
    this.btnRemove.textContent = tr("studio.setlists.remove");
    this.listA.setAttribute("aria-label", tr("studio.setlists.list_setlists_label"));
    this.listB.setAttribute("aria-label", tr("studio.setlists.list_entries_label"));
  }

  show() {
    this.build();
    this.host.hidden = false;
    this.render();
  }

  hide() {
    this.closeDialog(false);
    this.host.hidden = true;
  }

  setDisabled(on) {
    this.disabled = !!on;
  }

  _nm(row) { return displayName(row, this.tr("studio.sidebar.entry_missing")); }

  _countText(n) { return n === 1 ? this.tr("studio.setlists.count_one") : this.tr("studio.setlists.count", n); }

  /** The row of `list` with this key, else the one at the nearest index (a row that was removed or moved away), else null. */
  _rowOrNearest(list, key, index) {
    const rows = this._rows(list);
    return rows.find((r) => r.dataset.key === key) || rows[Math.max(0, Math.min(rows.length - 1, index))] || null;
  }

  // ---- rendering ------------------------------------------------------------------------------------------------------------------------------------------

  showError(err) {
    this.build();
    if (!err) {
      this.errorEl.hidden = true;
      this.errorEl.textContent = "";
      return;
    }
    const m = errorMessage(err);
    this.errorEl.textContent = err.read ? this.tr("studio.setlists.error_read", this.tr(m.key, ...m.args)) : this.tr(m.key, ...m.args);
    this.errorEl.hidden = false;
  }

  showNotice(n) {
    this.build();
    this.noticeEl.hidden = !n;
    this.noticeEl.textContent = n ? this.tr(n.key, ...n.args) : "";
  }

  _tagNames(sl) {
    const byId = new Map(this.ctl.tags.map((t) => [t.id, t.name]));
    return sl.tagIds.map((id) => byId.get(id)).filter((x) => x !== undefined);
  }

  _focusedKey() {
    const a = this.doc.activeElement;
    if (a && a.dataset && a.dataset.key && a.dataset.list) {
      const list = a.dataset.list === "setlist" ? this.listA : this.listB;
      return { list: a.dataset.list, id: a.dataset.key, index: this._rows(list).indexOf(a) };
    }
    return null;
  }

  render() {
    if (!this.built || this.host.hidden) return;
    if (this.drag) {
      this.deferred = true; // never rebuild under a drag in progress
      return;
    }
    this.deferred = false;
    const tr = this.tr;
    const keep = this._focusedKey();
    const c = this.ctl;
    const sels = c.setlists;
    if (!sels.some((s) => s.id === this.focusSetlist)) this.focusSetlist = c.selectedId || (sels[0] ? sels[0].id : "");
    if (!c.entries.some((e) => e.id === this.activeEntry)) this.activeEntry = c.entries[0] ? c.entries[0].id : "";

    this.listA.textContent = "";
    sels.forEach((s, i) => this.listA.appendChild(this._setlistRow(s, i, sels.length)));
    this.emptyA.textContent = sels.length === 0 && c.hasData ? tr("studio.setlists.empty") : "";
    this.emptyA.hidden = !(sels.length === 0 && c.hasData);

    const sel = c.selected;
    this.headingB.textContent = sel ? tr("studio.setlists.live_sets_heading", sel.name) : tr("studio.setlists.live_sets_none");
    this.listB.textContent = "";
    c.entries.forEach((e, i) => this.listB.appendChild(this._entryRow(e, i, c.entries.length)));
    this.emptyB.textContent = sel && c.entries.length === 0 ? tr("studio.setlists.empty_entries") : "";
    this.emptyB.hidden = !(sel && c.entries.length === 0);

    const off = this.disabled;
    this.btnNewSetlist.disabled = off;
    this.btnNewLiveSet.disabled = off;
    this.btnRename.disabled = off || !sel;
    this.btnTags.disabled = off || !sel;
    this.btnDelete.disabled = off || !sel;
    this.btnAdd.disabled = off || !sel;
    this.btnMove.disabled = off || !sel || !this.activeEntry;
    this.btnRemove.disabled = off || !sel || !this.activeEntry;

    if (keep) {
      // The rows are rebuilt, so the focus is put back: on the same row, or, when it is gone (removed, deleted, moved to another setlist), on the row now at its place,
      // or on the list's button when the list is empty.
      const list = keep.list === "setlist" ? this.listA : this.listB;
      const again = this._rowOrNearest(list, keep.id, keep.index);
      (again || (keep.list === "setlist" ? this.btnNewSetlist : this.btnAdd)).focus();
    }
  }

  _rows(list) {
    return Array.from(list.children).filter((r) => r.dataset && r.dataset.key !== undefined);
  }

  _handle(label) {
    const h = el(this.doc, "span", "sv-handle", "⠿");
    h.setAttribute("role", "img");
    h.setAttribute("aria-label", label);
    return h;
  }

  _setlistRow(s, index, count) {
    const d = this.doc;
    const tr = this.tr;
    const row = el(d, "div", "sv-row");
    row.setAttribute("role", "option");
    row.dataset.key = s.id;
    row.dataset.list = "setlist";
    row.draggable = true;
    row.setAttribute("draggable", "true");
    row.tabIndex = s.id === this.focusSetlist ? 0 : -1;
    row.setAttribute("aria-selected", s.id === this.ctl.selectedId ? "true" : "false");
    setClass(row, "on", s.id === this.ctl.selectedId);
    const tags = this._tagNames(s);
    let aria = tr("studio.setlists.setlist_aria", s.name, this._countText(s.count));
    if (s.missing > 0) aria += ", " + tr("studio.setlists.count_missing", s.missing);
    if (tags.length > 0) aria += ", " + tr("studio.setlists.tags_aria", tags.join(", "));
    if (s.loaded) aria += ", " + tr("studio.setlists.loaded_aria");
    row.setAttribute("aria-label", aria);
    row.appendChild(this._handle(tr("studio.setlists.handle", s.name)));
    row.appendChild(el(d, "span", "sv-name", s.name));
    const chips = el(d, "span", "sv-chips");
    tags.forEach((t) => chips.appendChild(el(d, "span", "sv-chip", t)));
    row.appendChild(chips);
    let meta = this._countText(s.count);
    if (s.missing > 0) meta += " · " + tr("studio.setlists.count_missing", s.missing);
    row.appendChild(el(d, "span", "sv-meta", meta));
    if (s.loaded) row.appendChild(el(d, "span", "sv-loaded", tr("studio.setlists.loaded")));
    void index;
    void count;
    return row;
  }

  _entryRow(e, index, count) {
    const d = this.doc;
    const tr = this.tr;
    const row = el(d, "div", "sv-row");
    row.setAttribute("role", "option");
    row.dataset.key = e.id;
    row.dataset.list = "liveset";
    row.draggable = true;
    row.setAttribute("draggable", "true");
    row.tabIndex = e.id === this.activeEntry ? 0 : -1;
    row.setAttribute("aria-selected", e.id === this.activeEntry ? "true" : "false");
    setClass(row, "on", e.id === this.activeEntry);
    setClass(row, "missing", e.missing);
    const shown = this._nm(e);
    let aria = tr("studio.setlists.entry_aria", shown, index + 1, count);
    if (e.missing) aria += ", " + tr("studio.sidebar.entry_missing");
    row.setAttribute("aria-label", aria);
    row.appendChild(this._handle(tr("studio.setlists.handle", shown)));
    row.appendChild(el(d, "span", "sv-pos", String(index + 1)));
    row.appendChild(el(d, "span", "sv-name", shown));
    if (e.missing) row.appendChild(el(d, "span", "sl-miss", tr("studio.sidebar.entry_missing")));
    return row;
  }

  // ---- keyboard and mouse ---------------------------------------------------------------------------------------------------------------------------------

  _wireList(list, kind) {
    const isSetlist = kind === "setlist";
    list.addEventListener("focusin", (e) => {
      const row = closestOption(e.target);
      if (!row) return;
      if (isSetlist) this.focusSetlist = row.dataset.key;
      else this.activeEntry = row.dataset.key;
      this._rows(list).forEach((r) => { r.tabIndex = r === row ? 0 : -1; });
    });
    list.addEventListener("click", (e) => {
      const row = closestOption(e.target);
      if (row) this._activate(kind, row.dataset.key);
    });
    list.addEventListener("keydown", (e) => {
      const row = closestOption(e.target);
      if (!row) return;
      const id = row.dataset.key;
      const rows = this._rows(list);
      const at = rows.indexOf(row);
      if (e.altKey && (e.key === "ArrowUp" || e.key === "ArrowDown")) {
        e.preventDefault();
        if (this.disabled) return;
        if (isSetlist) this.ctl.nudgeSetlist(id, e.key);
        else this.ctl.nudgeLiveSet(id, e.key);
        return;
      }
      if (e.altKey) {
        if (!isSetlist && e.code === "KeyM") {
          e.preventDefault();
          this.openMoveTo(row);
        }
        return;
      }
      let to = -1;
      if (e.key === "ArrowUp") to = Math.max(0, at - 1);
      else if (e.key === "ArrowDown") to = Math.min(rows.length - 1, at + 1);
      else if (e.key === "Home") to = 0;
      else if (e.key === "End") to = rows.length - 1;
      if (to >= 0) {
        e.preventDefault();
        if (rows[to]) rows[to].focus();
        return;
      }
      if (e.key === "Enter" || e.key === " ") {
        e.preventDefault();
        this._activate(kind, id);
      } else if (isSetlist && e.key === "F2") {
        e.preventDefault();
        this._activate(kind, id).then(() => this.openRenameSetlist());
      } else if (e.key === "Delete" || e.key === "Backspace") { // Backspace is the Delete key of a Mac's main keyboard
        e.preventDefault();
        if (isSetlist) this._activate(kind, id).then(() => this.openDeleteSetlist());
        else { this.activeEntry = id; this.openRemoveLiveSet(); }
      } else if (!isSetlist && (e.key === "ContextMenu" || (e.shiftKey && e.key === "F10"))) {
        e.preventDefault();
        this.openMoveTo(row);
      }
    });
    // Drag and drop (HTML5 drag events), with a visible drop indicator.
    list.addEventListener("dragstart", (e) => {
      const row = closestOption(e.target);
      if (!row || this.disabled) return;
      this.drag = { kind, id: row.dataset.key };
      setClass(row, "dragging", true);
      if (e.dataTransfer) {
        e.dataTransfer.effectAllowed = "move";
        try { e.dataTransfer.setData("text/plain", row.dataset.key); } catch (err) { /* some webviews refuse; the id is kept in this.drag */ }
      }
    });
    list.addEventListener("dragover", (e) => this._dragOver(e, list, kind));
    list.addEventListener("dragleave", (e) => {
      if (!e.relatedTarget || !(list.contains ? list.contains(e.relatedTarget) : true)) this._clearIndicators();
    });
    list.addEventListener("drop", (e) => this._drop(e, list, kind));
    list.addEventListener("dragend", () => this._endDrag());
  }

  async _activate(kind, id) {
    if (kind === "setlist") {
      this.focusSetlist = id;
      if (id !== this.ctl.selectedId) {
        await this.ctl.select(id);
        const sl = this.ctl.selected;
        if (sl) this.announce(this.tr("studio.setlists.announce_selected", sl.name, this._countText(sl.count)));
      }
    } else {
      this.activeEntry = id;
      this.render();
    }
  }

  _rowGeometry(list, e) {
    // The insertion slot for a pointer position: before / after the row under it (the lists' rows are in order).
    const rows = this._rows(list);
    const row = closestOption(e.target);
    if (!row) return { slot: rows.length, row: null, rows };
    const i = rows.indexOf(row);
    const r = row.getBoundingClientRect ? row.getBoundingClientRect() : null;
    const before = r && typeof e.clientY === "number" ? e.clientY < r.top + r.height / 2 : true;
    return { slot: before ? i : i + 1, row, rows, before, index: i };
  }

  _clearIndicators() {
    [this.listA, this.listB].forEach((l) => this._rows(l).forEach((r) => {
      setClass(r, "drop-before", false);
      setClass(r, "drop-after", false);
      setClass(r, "drop-into", false);
    }));
    this.drop = null;
  }

  _dragOver(e, list, kind) {
    if (!this.drag || this.disabled) return;
    const g = this._rowGeometry(list, e);
    if (this.drag.kind === kind) {
      e.preventDefault();
      this._clearIndicators();
      if (g.row) setClass(g.row, g.before ? "drop-before" : "drop-after", true);
      else if (g.rows.length > 0) setClass(g.rows[g.rows.length - 1], "drop-after", true);
      this.drop = { slot: g.slot };
    } else if (this.drag.kind === "liveset" && kind === "setlist" && g.row && g.row.dataset.key !== this.ctl.selectedId) {
      e.preventDefault(); // a Live Set dropped on another setlist moves it there (to its end)
      this._clearIndicators();
      setClass(g.row, "drop-into", true);
      this.drop = { into: g.row.dataset.key };
    }
  }

  _drop(e, list, kind) {
    if (!this.drag) return;
    e.preventDefault();
    const drag = this.drag;
    const drop = this.drop || (this.drag.kind === kind ? { slot: this._rowGeometry(list, e).slot } : null);
    this._endDrag();
    if (!drop) return;
    if (drop.into !== undefined) {
      this.ctl.moveLiveSetToSetlist(drag.id, drop.into, "end");
      return;
    }
    const ids = (kind === "setlist" ? this.ctl.setlists : this.ctl.entries).map((x) => x.id);
    const from = ids.indexOf(drag.id);
    if (from < 0) return;
    const to = slotToIndex(from, Math.max(0, Math.min(ids.length, drop.slot)));
    if (kind === "setlist") this.ctl.moveSetlist(drag.id, to);
    else this.ctl.moveLiveSet(drag.id, to);
  }

  _endDrag() {
    this.drag = null;
    this._clearIndicators();
    [this.listA, this.listB].forEach((l) => this._rows(l).forEach((r) => setClass(r, "dragging", false)));
    if (this.deferred) this.render();
  }

  // ---- menus -----------------------------------------------------------------------------------------------------------------------------------------------

  /** A labelled `role="menu"`: Up / Down / Home / End move, Enter / Space choose, Escape closes and returns the focus. Items: `{ label, disabled, separator, onPick }`. */
  openMenu({ label, items, restore }) {
    this.closeDialog(false);
    const d = this.doc;
    const backdrop = el(d, "div", "sv-backdrop");
    const menu = el(d, "div", "sv-menu");
    menu.setAttribute("role", "menu");
    menu.setAttribute("aria-label", label);
    const entries = [];
    items.forEach((it) => {
      if (it.separator) {
        const sep = el(d, "div", "sv-menu-sep");
        sep.setAttribute("role", "separator");
        menu.appendChild(sep);
        return;
      }
      const mi = el(d, "div", "sv-menu-item", it.label);
      mi.setAttribute("role", "menuitem");
      mi.tabIndex = -1;
      if (it.disabled) mi.setAttribute("aria-disabled", "true");
      mi._item = it;
      entries.push(mi);
      menu.appendChild(mi);
    });
    const live = entries.filter((m) => !m._item.disabled);
    const focusAt = (i) => {
      if (live.length === 0) return;
      const t = Math.max(0, Math.min(live.length - 1, i));
      live.forEach((m, k) => { m.tabIndex = k === t ? 0 : -1; });
      live[t].focus();
    };
    const choose = (mi) => {
      if (!mi || mi._item.disabled) return;
      this.closeDialog(true);
      mi._item.onPick();
    };
    menu.addEventListener("keydown", (e) => {
      const at = live.indexOf(e.target);
      if (e.key === "ArrowDown") { e.preventDefault(); focusAt(at + 1); }
      else if (e.key === "ArrowUp") { e.preventDefault(); focusAt(at - 1); }
      else if (e.key === "Home") { e.preventDefault(); focusAt(0); }
      else if (e.key === "End") { e.preventDefault(); focusAt(live.length - 1); }
      else if (e.key === "Enter" || e.key === " ") { e.preventDefault(); choose(e.target); }
      else if (e.key === "Escape") { e.preventDefault(); this.closeDialog(true); }
      else if (e.key === "Tab") e.preventDefault();
    });
    menu.addEventListener("click", (e) => {
      for (let n = e.target; n; n = n.parent || n.parentNode) if (n._item) { choose(n); return; }
    });
    backdrop.addEventListener("click", (e) => { if (e.target === backdrop) this.closeDialog(true); });
    backdrop.appendChild(menu);
    this.dialogHost.appendChild(backdrop);
    this.dialog = { backdrop, restore: restore || this.doc.activeElement };
    focusAt(0);
    return menu;
  }

  openMoveTo(rowEl) {
    const c = this.ctl;
    const id = this.activeEntry;
    const entry = c.entries.find((e) => e.id === id);
    if (!entry || !c.selected || this.disabled) return;
    const entryIndex = c.entries.indexOf(entry);
    const tr = this.tr;
    const targets = moveTargets({ setlists: c.setlists, selectedId: c.selectedId, entries: c.entries, liveSetId: id, knownMembers: c.membership });
    const byId = new Map(c.setlists.map((s) => [s.id, s]));
    const items = [];
    let sepDone = false;
    for (const t of targets) {
      if (t.kind === "position") {
        items.push({ label: tr("studio.setlists.menu_position", t.index + 1), onPick: () => c.moveLiveSet(id, t.index) });
      } else {
        if (!sepDone) { items.push({ separator: true }); sepDone = true; }
        const name = byId.get(t.setlistId).name;
        items.push({
          label: tr(t.where === "start" ? "studio.setlists.menu_setlist_start" : "studio.setlists.menu_setlist_end", name),
          disabled: t.disabled,
          onPick: () => c.moveLiveSetToSetlist(id, t.setlistId, t.where),
        });
      }
    }
    // The focus goes back to the row by key when the menu closes (the row may have been rebuilt, moved or removed meanwhile), else to the row now at its place.
    this.openMenu({ label: tr("studio.setlists.menu_move_title", this._nm(entry)), items, restore: () => this._rowOrNearest(this.listB, id, entryIndex) || this.btnMove });
    void rowEl;
  }

  openAddLiveSet() {
    const c = this.ctl;
    const sel = c.selected;
    if (!sel || this.disabled) return;
    const tr = this.tr;
    const have = new Set(c.entries.map((e) => e.id));
    const items = c.all.filter((l) => !have.has(l.id)).map((l) => ({ label: l.name, onPick: () => c.addLiveSet(sel.id, l.id) }));
    if (items.length === 0) items.push({ label: tr("studio.setlists.menu_add_none"), disabled: true, onPick: () => {} });
    this.openMenu({ label: tr("studio.setlists.menu_add_title", sel.name), items, restore: this.btnAdd });
  }

  // ---- dialogs ---------------------------------------------------------------------------------------------------------------------------------------------

  closeDialog(restoreFocus) {
    if (!this.dialog) return;
    const { restore } = this.dialog;
    this.dialogHost.textContent = "";
    this.dialog = null;
    const target = typeof restore === "function" ? restore() : restore;
    if (restoreFocus && target && target.focus) target.focus();
  }

  _modal(title, restore) {
    this.closeDialog(false);
    const d = this.doc;
    const backdrop = el(d, "div", "sv-backdrop");
    const box = el(d, "div", "sv-dialog");
    box.setAttribute("role", "dialog");
    box.setAttribute("aria-modal", "true");
    const h = el(d, "h2", "sv-dialog-title", title);
    h.id = "sv-dialog-title";
    box.setAttribute("aria-labelledby", "sv-dialog-title");
    box.appendChild(h);
    const body = el(d, "div", "sv-dialog-body");
    const err = el(d, "div", "pair-dialog-error sv-dialog-error");
    err.setAttribute("role", "alert");
    const actions = el(d, "div", "pair-dialog-actions");
    box.append(body, err, actions);
    backdrop.appendChild(box);
    box.addEventListener("keydown", (e) => {
      if (e.key === "Escape") { e.preventDefault(); this.closeDialog(true); return; }
      if (e.key === "Tab") {
        const f = focusables(box);
        if (f.length === 0) return;
        const at = f.indexOf(e.target);
        const next = e.shiftKey ? (at <= 0 ? f.length - 1 : at - 1) : at === f.length - 1 ? 0 : at + 1;
        e.preventDefault();
        f[next].focus();
      }
    });
    this.dialogHost.appendChild(backdrop);
    this.dialog = { backdrop, restore: restore || this.doc.activeElement };
    return { box, body, err, actions };
  }

  _setDialogError(parts, problem) {
    parts.err.textContent = problem ? this.tr(problem.key, ...problem.args) : "";
  }

  /** A name dialog: validates (trim, 1..64 bytes, no control characters) before sending; an engine error stays inside the dialog. `submit(name, extra)` -> result. */
  _nameDialog({ title, initial, confirmLabel, restore, extra, submit }) {
    const tr = this.tr;
    const d = this.doc;
    const parts = this._modal(title, restore);
    const label = el(d, "label", "", tr("studio.setlists.name_label"));
    const input = el(d, "input");
    input.type = "text";
    input.value = initial || "";
    input.setAttribute("autocomplete", "off");
    label.appendChild(input);
    parts.body.appendChild(label);
    let extraBox = null;
    if (extra) {
      const l2 = el(d, "label", "sv-check");
      extraBox = el(d, "input");
      extraBox.type = "checkbox";
      extraBox.checked = extra.checked;
      l2.appendChild(extraBox);
      l2.appendChild(el(d, "span", "", extra.label));
      parts.body.appendChild(l2);
    }
    const cancel = button(d, tr("studio.setlists.dlg_cancel"), () => this.closeDialog(true), "");
    const ok = button(d, confirmLabel, () => go(), "btn-primary");
    parts.actions.append(cancel, ok);
    let busy = false; // a second Enter (or click) while the first command is in flight is ignored
    const go = async () => {
      if (busy) return;
      const v = validateName(input.value);
      if (!v.ok) { this._setDialogError(parts, nameProblem(v.reason)); return; }
      busy = true;
      ok.disabled = true;
      try {
        const r = await submit(v.name, extraBox ? extraBox.checked : false);
        if (r && r.ok) this.closeDialog(true);
        else this._setDialogError(parts, errorMessage(r && r.error));
      } finally {
        busy = false;
        ok.disabled = false;
      }
    };
    input.addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); go(); } });
    input.focus();
    return parts;
  }

  openNewSetlist() {
    if (this.disabled) return;
    this._nameDialog({
      title: this.tr("studio.setlists.dlg_new_setlist"), initial: "", confirmLabel: this.tr("studio.setlists.dlg_create"), restore: this.btnNewSetlist,
      submit: (name) => this.ctl.createSetlist(name),
    });
  }

  openRenameSetlist() {
    const sel = this.ctl.selected;
    if (!sel || this.disabled) return;
    this._nameDialog({
      title: this.tr("studio.setlists.dlg_rename_setlist"), initial: sel.name, confirmLabel: this.tr("studio.setlists.dlg_save"), restore: this.btnRename,
      submit: (name) => this.ctl.renameSetlist(sel.id, name),
    });
  }

  openNewLiveSet() {
    if (this.disabled) return;
    const sel = this.ctl.selected;
    this._nameDialog({
      title: this.tr("studio.setlists.dlg_new_live_set"), initial: "", confirmLabel: this.tr("studio.setlists.dlg_create"), restore: this.btnNewLiveSet,
      extra: sel ? { checked: true, label: this.tr("studio.setlists.add_to_setlist", sel.name) } : null,
      submit: (name, add) => this.ctl.createLiveSet(name, add && sel ? sel.id : ""),
    });
  }

  _confirm({ title, message, confirmLabel, restore, run }) {
    const d = this.doc;
    const parts = this._modal(title, restore);
    parts.body.appendChild(el(d, "p", "", message));
    const cancel = button(d, this.tr("studio.setlists.dlg_cancel"), () => this.closeDialog(true), "");
    let busy = false;
    const ok = button(d, confirmLabel, async () => {
      if (busy) return;
      busy = true;
      ok.disabled = true;
      try {
        const r = await run();
        if (r && r.ok) this.closeDialog(true);
        else this._setDialogError(parts, errorMessage(r && r.error));
      } finally {
        busy = false;
        ok.disabled = false;
      }
    }, "btn-primary");
    parts.actions.append(cancel, ok);
    cancel.focus(); // the safe choice has the focus
    return parts;
  }

  openDeleteSetlist() {
    const sel = this.ctl.selected;
    if (!sel || this.disabled) return;
    const setlistIndex = this.ctl.setlists.findIndex((x) => x.id === sel.id);
    this._confirm({
      title: this.tr("studio.setlists.dlg_delete_setlist"), message: this.tr("studio.setlists.dlg_delete_setlist_msg", sel.name),
      confirmLabel: this.tr("studio.setlists.delete"), restore: () => this._rowOrNearest(this.listA, sel.id, setlistIndex) || this.btnNewSetlist, run: () => this.ctl.deleteSetlist(sel.id),
    });
  }

  openRemoveLiveSet() {
    const c = this.ctl;
    const sel = c.selected;
    const e = c.entries.find((x) => x.id === this.activeEntry);
    if (!sel || !e || this.disabled) return;
    const entryIndex = c.entries.indexOf(e);
    this._confirm({
      title: this.tr("studio.setlists.dlg_remove"), message: this.tr("studio.setlists.dlg_remove_msg", this._nm(e), sel.name),
      confirmLabel: this.tr("studio.setlists.remove_confirm"), restore: () => this._rowOrNearest(this.listB, e.id, entryIndex) || this.btnAdd, run: () => c.removeLiveSet(sel.id, e.id),
    });
  }

  /** The tag editor of the selected setlist: tick the tags it carries (at most 8), create / rename / delete tags. */
  openTags() {
    const c = this.ctl;
    const sel = c.selected;
    if (!sel || this.disabled) return;
    const tr = this.tr;
    const d = this.doc;
    const parts = this._modal(tr("studio.setlists.tags_title", sel.name), this.btnTags);
    let chosen = new Set(sel.tagIds);
    const note = el(d, "p", "sv-note", tr("studio.setlists.tags_limit_note", LIMITS.maxTagsPerSetlist));
    const list = el(d, "div", "sv-tags");
    list.setAttribute("role", "group");
    list.setAttribute("aria-label", tr("studio.setlists.tags_group"));
    const addRow = el(d, "div", "sv-tag-add");
    const addLabel = el(d, "label", "", tr("studio.setlists.tag_new_label"));
    const addInput = el(d, "input");
    addInput.type = "text";
    addInput.setAttribute("autocomplete", "off");
    addLabel.appendChild(addInput);
    const addBtn = button(d, tr("studio.setlists.tag_add"), () => addTag(), "");
    addRow.append(addLabel, addBtn);
    parts.body.append(note, list, addRow);
    let doneBusy = false;
    const done = button(d, tr("studio.setlists.dlg_done"), async () => {
      if (doneBusy) return;
      const before = new Set(sel.tagIds);
      const same = before.size === chosen.size && [...chosen].every((x) => before.has(x));
      if (same) { this.closeDialog(true); return; }
      doneBusy = true;
      done.disabled = true;
      let r;
      try { r = await c.setSetlistTags(sel.id, c.tags.map((t) => t.id).filter((id) => chosen.has(id))); } finally { doneBusy = false; done.disabled = false; }
      if (r && r.ok) this.closeDialog(true);
      else this._setDialogError(parts, errorMessage(r && r.error));
    }, "btn-primary");
    const cancel = button(d, tr("studio.setlists.dlg_cancel"), () => this.closeDialog(true), "");
    parts.actions.append(cancel, done);

    let tagBusy = false; // one tag command at a time inside the editor (add / rename)
    const addTag = async () => {
      if (tagBusy) return;
      const v = validateName(addInput.value);
      if (!v.ok) { this._setDialogError(parts, nameProblem(v.reason)); return; }
      tagBusy = true;
      let r;
      try { r = await c.createTag(v.name); } finally { tagBusy = false; }
      if (r && r.ok) { addInput.value = ""; this._setDialogError(parts, null); renderTags(); addInput.focus(); }
      else this._setDialogError(parts, errorMessage(r && r.error));
    };
    addInput.addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); addTag(); } });

    const renderTags = (focusTag) => {
      list.textContent = "";
      const full = chosen.size >= LIMITS.maxTagsPerSetlist;
      if (c.tags.length === 0) list.appendChild(el(d, "div", "sv-empty", tr("studio.setlists.tags_none")));
      c.tags.forEach((t) => {
        const row = el(d, "div", "sv-tag-row");
        const lab = el(d, "label", "sv-check");
        const cb = el(d, "input");
        cb.type = "checkbox";
        cb.checked = chosen.has(t.id);
        cb.disabled = full && !chosen.has(t.id);
        cb.addEventListener("change", () => { if (cb.checked) chosen.add(t.id); else chosen.delete(t.id); renderTags(t.id); });
        lab.appendChild(cb);
        lab.appendChild(el(d, "span", "", t.name));
        const ren = button(d, tr("studio.setlists.tag_rename"), () => editRow(row, t), "");
        ren.setAttribute("aria-label", tr("studio.setlists.tag_rename_aria", t.name));
        const del = button(d, tr("studio.setlists.tag_delete"), async () => {
          if (del.dataset.armed !== "1") {
            del.dataset.armed = "1"; // the destructive action asks twice: the button turns into "Confirm delete" and disarms itself after 4 s (a re-render disarms it too)
            del.textContent = tr("studio.setlists.tag_delete_confirm");
            this.announce(tr("studio.setlists.tag_delete_armed", t.name));
            const timer = setTimeout(() => { del.dataset.armed = "0"; del.textContent = tr("studio.setlists.tag_delete"); }, 4000);
            if (timer && timer.unref) timer.unref();
            return;
          }
          const r = await c.deleteTag(t.id);
          if (r && r.ok) { chosen.delete(t.id); this._setDialogError(parts, null); renderTags(); }
          else this._setDialogError(parts, errorMessage(r && r.error));
        }, "");
        del.setAttribute("aria-label", tr("studio.setlists.tag_delete_aria", t.name));
        row.append(lab, ren, del);
        list.appendChild(row);
        if (focusTag === t.id) cb.focus();
      });
    };
    const editRow = (row, t) => {
      row.textContent = "";
      const input = el(d, "input");
      input.type = "text";
      input.value = t.name;
      input.setAttribute("aria-label", tr("studio.setlists.tag_name_label"));
      const saveRename = async () => {
        if (tagBusy) return;
        const v = validateName(input.value);
        if (!v.ok) { this._setDialogError(parts, nameProblem(v.reason)); return; }
        tagBusy = true;
        let r;
        try { r = await c.renameTag(t.id, v.name); } finally { tagBusy = false; }
        if (r && r.ok) { this._setDialogError(parts, null); renderTags(t.id); }
        else this._setDialogError(parts, errorMessage(r && r.error));
      };
      const save = button(d, tr("studio.setlists.dlg_save"), saveRename, "btn-primary");
      const cancelEdit = button(d, tr("studio.setlists.dlg_cancel"), () => renderTags(t.id), "");
      input.addEventListener("keydown", (e) => {
        if (e.key === "Enter") { e.preventDefault(); saveRename(); }
        if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); renderTags(t.id); }
      });
      row.append(input, save, cancelEdit);
      input.focus();
    };
    renderTags();
    (focusables(list)[0] || addInput).focus();
  }
}
