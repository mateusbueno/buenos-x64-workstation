// Velora Studio: the DOM side of the "+ Add timbre" picker (Story 9.11): a modal dialog with a search field (combobox pattern: the focus stays in the field, Up / Down / PageUp /
// PageDown move the active option, Enter adds it, Esc closes without change), the timbres grouped by category, a status line (polite live region) and the Cancel / Try again buttons.
// Everything that comes from the unit (names) is written with textContent / setAttribute only, never as markup. `doc` is the page's document (or a shim in tests); `tr(key, ...args)` is
// the locale lookup; `announce(text)` writes to the polite live region. The model, the requests and the rules live in timbre_picker.js (TimbrePickerController).
import { MAX_VISIBLE } from "./timbre_picker.js";

function el(doc, tag, cls, text) {
  const e = doc.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

function composing(e) {
  return !!(e && (e.isComposing || e.keyCode === 229)); // an IME is choosing: Enter / Esc / arrows belong to it
}

export class TimbrePickerView {
  constructor({ doc, host, tr, controller, announce, categoryLabel }) {
    this.doc = doc;
    this.host = host;
    this.tr = tr;
    this.ctl = controller;
    this.announce = announce || (() => {});
    this.categoryLabel = categoryLabel || (() => "");
    this.dialog = null; // the built parts while the picker is open
    this.restore = null; // where the focus goes when the picker closes: an element or a function returning one
    this.composing = false;
    this.seq = 0;
  }

  /** Remembers where the focus returns to, then draws the opened picker (the controller's open() has already been called). */
  open(restore) {
    this.restore = restore || this.doc.activeElement || null;
    this.render();
  }

  /** Closes the dialog and returns the focus (the controller has already closed itself). `restoreFocus` false leaves the focus alone (the page is going elsewhere). */
  close(restoreFocus = true) {
    this._teardown();
    if (restoreFocus) this.restoreFocus();
  }

  _teardown() {
    if (!this.dialog) return;
    this.host.textContent = "";
    this.dialog = null;
    this.composing = false;
  }

  /** Returns the focus to where the picker was opened from (the part strip). Called once, after the picker closed. */
  restoreFocus() {
    const target = typeof this.restore === "function" ? this.restore() : this.restore;
    this.restore = null;
    if (target && target.focus) target.focus();
  }

  // ---- drawing -------------------------------------------------------------------------------------------------------------------------------------------

  render() {
    const c = this.ctl;
    if (!c.isOpen) {
      this._teardown(); // the focus is returned by restoreFocus(), which the closing path calls (a closed controller never steals it here)
      return;
    }
    if (!this.dialog) this._build();
    const d = this.dialog;
    const tr = this.tr;
    const busy = c.status === "assigning";
    d.title.textContent = tr("studio.timbre_picker.title", c.context.part + 1);
    d.input.disabled = false;
    d.input.readOnly = busy; // typing is allowed while the list loads: the text filters the list when it arrives
    d.box.setAttribute("aria-busy", c.status === "loading" || busy ? "true" : "false");
    d.input.setAttribute("aria-expanded", c.status === "ready" && c.rows.length > 0 ? "true" : "false");
    d.cancel.disabled = busy;
    d.retry.hidden = !(c.status === "error" && c.error && c.error.kind === "load");
    d.retry.disabled = false;
    this._status();
    this._list();
    this._error();
  }

  _status() {
    const c = this.ctl;
    const tr = this.tr;
    let text = "";
    if (c.status === "loading") text = tr("studio.timbre_picker.loading");
    else if (c.status === "assigning") text = tr("studio.timbre_picker.assigning");
    else if (c.status === "empty") text = tr("studio.timbre_picker.empty");
    else if (c.status === "ready") {
      if (c.view.matched === 0) text = tr("studio.timbre_picker.no_match", c.query);
      else {
        const parts = [c.query ? tr("studio.timbre_picker.count_filtered", c.view.matched, c.items.length) : tr("studio.timbre_picker.count", c.items.length)];
        if (c.view.capped) parts.push(tr("studio.timbre_picker.capped", MAX_VISIBLE));
        if (c.truncated) parts.push(tr("studio.timbre_picker.truncated"));
        text = parts.join(" · ");
      }
    }
    if (this.dialog.status.textContent !== text) this.dialog.status.textContent = text;
  }

  _list() {
    const d = this.dialog;
    const c = this.ctl;
    const tr = this.tr;
    const list = d.list;
    this._sigRows = this._sigRows || "";
    const rows = c.status === "ready" || c.status === "assigning" ? c.rows : [];
    const sig = rows.map((r) => r.id).join("|") + "#" + (c.status === "ready" || c.status === "assigning" ? "1" : "0");
    if (sig !== this._sigRows) {
      this._sigRows = sig;
      list.textContent = "";
      d.options = [];
      if (rows.length > 0) {
        let index = 0;
        for (const g of c.view.groups) {
          const grp = el(this.doc, "div", "tp-group");
          grp.setAttribute("role", "group");
          const hid = `tp-g-${this.seq}-${index}`;
          const head = el(this.doc, "div", "tp-group-head", g.categoryKey ? tr(g.categoryKey) : tr("studio.timbre_picker.uncategorised"));
          head.id = hid;
          head.setAttribute("aria-hidden", "true"); // the group's name is read through aria-label (an id reference would not survive a rebuild mid-announcement)
          grp.setAttribute("aria-label", g.categoryKey ? tr(g.categoryKey) : tr("studio.timbre_picker.uncategorised"));
          grp.appendChild(head);
          for (const it of g.items) {
            const o = el(this.doc, "div", "tp-option");
            o.setAttribute("role", "option");
            o.id = `tp-o-${this.seq}-${index}`;
            o.dataset.idx = String(index);
            o.setAttribute("aria-label", tr("studio.timbre_picker.option_aria", it.name, it.libraryName || "?"));
            const nm = el(this.doc, "span", "tp-name", it.name);
            const lib = el(this.doc, "span", "tp-lib", it.libraryName);
            o.append(nm, lib);
            o._item = it;
            o.addEventListener("click", () => {
              c.setActive(Number(o.dataset.idx));
              c.choose(it);
            });
            grp.appendChild(o);
            d.options.push(o);
            index++;
          }
          list.appendChild(grp);
        }
      }
    }
    // the active option: aria-selected, the combobox's aria-activedescendant, kept in view
    d.options.forEach((o, i) => {
      const on = i === c.active;
      o.setAttribute("aria-selected", on ? "true" : "false");
      const cls = "tp-option" + (on ? " active" : "");
      if (o.className !== cls) o.className = cls;
      if (on) {
        d.input.setAttribute("aria-activedescendant", o.id);
        if (o.scrollIntoView) o.scrollIntoView({ block: "nearest" });
      }
    });
    if (c.active < 0 || d.options.length === 0) d.input.removeAttribute("aria-activedescendant");
    list.hidden = d.options.length === 0;
  }

  _error() {
    const c = this.ctl;
    const tr = this.tr;
    let text = "";
    if (c.error) text = tr(c.error.kind === "load" ? "studio.timbre_picker.error_load" : "studio.timbre_picker.error_assign", c.error.message || "");
    if (this.dialog.err.textContent !== text) this.dialog.err.textContent = text; // role=alert: announced when it appears
  }

  _build() {
    const d = this.doc;
    const tr = this.tr;
    this.seq++;
    this._sigRows = "";
    const backdrop = el(d, "div", "sv-backdrop");
    const box = el(d, "div", "sv-dialog tp-dialog");
    box.setAttribute("role", "dialog");
    box.setAttribute("aria-modal", "true");
    const title = el(d, "h2", "sv-dialog-title");
    title.id = `tp-title-${this.seq}`;
    box.setAttribute("aria-labelledby", title.id);
    const hint = el(d, "div", "sv-note tp-hint", tr("studio.timbre_picker.search_hint"));
    hint.id = `tp-hint-${this.seq}`;
    const label = el(d, "label", "tp-search");
    const labelText = el(d, "span", "", tr("studio.timbre_picker.search_label"));
    const input = el(d, "input");
    input.type = "text";
    input.setAttribute("autocomplete", "off");
    input.setAttribute("autocapitalize", "off");
    input.setAttribute("spellcheck", "false");
    input.setAttribute("role", "combobox");
    input.setAttribute("aria-autocomplete", "list");
    input.setAttribute("aria-haspopup", "listbox");
    input.setAttribute("aria-expanded", "false");
    input.setAttribute("aria-describedby", hint.id);
    label.append(labelText, input);
    const list = el(d, "div", "tp-list");
    list.id = `tp-list-${this.seq}`;
    list.setAttribute("role", "listbox");
    list.setAttribute("aria-label", tr("studio.timbre_picker.list_label"));
    list.hidden = true;
    input.setAttribute("aria-controls", list.id);
    const status = el(d, "div", "sv-note tp-status");
    status.setAttribute("role", "status");
    status.setAttribute("aria-live", "polite");
    const err = el(d, "div", "pair-dialog-error sv-dialog-error");
    err.setAttribute("role", "alert");
    const actions = el(d, "div", "pair-dialog-actions");
    const retry = el(d, "button", "btn", tr("studio.timbre_picker.retry"));
    retry.type = "button";
    retry.hidden = true;
    retry.addEventListener("click", () => this.ctl.retry());
    const cancel = el(d, "button", "btn", tr("studio.timbre_picker.cancel"));
    cancel.type = "button";
    cancel.addEventListener("click", () => this.ctl.cancel());
    actions.append(retry, cancel);
    box.append(title, hint, label, list, status, err, actions);
    backdrop.appendChild(box);
    backdrop.addEventListener("click", (e) => {
      if (e.target === backdrop) this.ctl.cancel(); // never while an assignment is out (cancel() refuses)
    });
    input.addEventListener("compositionstart", () => { this.composing = true; });
    input.addEventListener("compositionend", () => {
      this.composing = false;
      this.ctl.setQuery(input.value);
    });
    input.addEventListener("input", () => {
      if (!this.composing) this.ctl.setQuery(input.value); // while an IME composes, the list waits for the committed text
    });
    box.addEventListener("keydown", (e) => this._key(e));
    this.host.appendChild(backdrop);
    this.dialog = { backdrop, box, title, input, list, status, err, retry, cancel, options: [] };
    // the focus goes to the search field at once (also while the list loads)
    input.focus();
  }

  _key(e) {
    const c = this.ctl;
    const d = this.dialog;
    if (!d) return;
    if (e.key === "Escape") {
      if (composing(e)) return; // Esc ends the IME composition only
      e.preventDefault();
      c.cancel();
      return;
    }
    if (e.key === "Tab") {
      const f = [d.input, d.retry, d.cancel].filter((n) => !n.disabled && !n.hidden);
      if (f.length === 0) return;
      const at = f.indexOf(e.target);
      const next = e.shiftKey ? (at <= 0 ? f.length - 1 : at - 1) : at === f.length - 1 ? 0 : at + 1;
      e.preventDefault();
      f[next].focus();
      return;
    }
    if (composing(e) || e.target !== d.input) return;
    if (e.key === "ArrowDown" || e.key === "ArrowUp" || e.key === "PageDown" || e.key === "PageUp") {
      e.preventDefault();
      c.move(e.key);
    } else if (e.key === "Enter") {
      e.preventDefault();
      const it = c.activeItem;
      if (it) c.choose(it);
    }
  }
}
