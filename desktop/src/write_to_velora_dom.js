// Velora Studio: the DOM side of Write to Velora (Story 9.12): the Write dialog (a modal Studio Dialog: what changed, the device's three scopes as a radio group, the name field of
// Save as new, Cancel and the Primary Button) and the Studio Banner for a failed or unconfirmed Write. Everything that comes from the unit (the Live Set name, names in an error)
// is written with textContent / setAttribute only, never as markup. `doc` is the page's document (or a shim in tests); `tr(key, ...args)` is the locale lookup. The model, the requests
// and the rules live in write_to_velora.js (WriteController).
import { SCOPE, changeLabel } from "./write_to_velora.js";

function el(doc, tag, cls, text) {
  const e = doc.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

function composing(e) {
  return !!(e && (e.isComposing || e.keyCode === 229)); // an IME is choosing: Enter / Esc / arrows belong to it
}

/** The text of a Studio Banner for a `banner` model ({ kind, device, reasonKey }). */
export function bannerText(b, tr) {
  const device = b.device || "Velora";
  switch (b.kind) {
    case "connection": return tr("studio.write.failed_connection", device);
    case "unknown": return tr("studio.write.unknown_connection", device);
    case "storage_full": return tr("studio.write.failed_storage_full", device);
    case "error": return tr("studio.write.failed_other", device, tr(b.reasonKey || "studio.write.reason.other"));
    case "not_landed": return tr("studio.write.not_landed", device);
    case "clean": return tr("studio.write.clean_after", device);
    case "restarted": return tr("studio.write.restarted", device);
    case "stale": return tr("studio.write.stale_banner");
    default: return "";
  }
}

/** What a controller announcement ({ key, args } | { parts } | { banner }) says. */
export function announceText(a, tr) {
  if (!a) return "";
  if (a.banner) return bannerText(a.banner, tr);
  if (a.parts) return a.parts.map((p) => tr(p.key, ...(p.args || []))).join(" ");
  return tr(a.key, ...(a.args || []));
}

export class WriteDialogView {
  constructor({ doc, host, tr, controller, restore }) {
    this.doc = doc;
    this.host = host;
    this.tr = tr;
    this.ctl = controller;
    this.restore = restore || (() => null); // where the focus returns when the dialog closes: an element or a function returning one
    this.dialog = null;
    this.composing = false;
    this.seq = 0;
    this._lastNameError = null;
    this._lastInline = null;
    this._wasWriting = false;
  }

  /** Draws the dialog (the controller has just opened) and moves the focus into it. Remembers the element that had it only through `restore`. */
  open() {
    this.render();
  }

  /** Called when the controller closed: removes the dialog and returns the focus (to the Write button, or the main area when that is disabled now). */
  close() {
    this._teardown();
    this.restoreFocus();
  }

  _teardown() {
    if (!this.dialog) return;
    this.host.textContent = "";
    this.dialog = null;
    this.composing = false;
    this._lastNameError = null;
    this._lastInline = null;
    this._wasWriting = false;
  }

  restoreFocus() {
    const target = typeof this.restore === "function" ? this.restore() : this.restore;
    if (target && target.focus) target.focus();
  }

  // ---- drawing -------------------------------------------------------------------------------------------------------------------------------------------------------------------------

  render() {
    const c = this.ctl;
    if (!c.isOpen) {
      this._teardown();
      return;
    }
    const fresh = !this.dialog;
    if (fresh) this._build();
    const d = this.dialog;
    const tr = this.tr;
    const v = c.view();
    const writing = v.writing;
    d.box.setAttribute("aria-busy", writing ? "true" : "false");
    d.title.textContent = tr("studio.write.title");
    d.summary.textContent = tr(v.count === 1 ? "studio.write.summary_one" : "studio.write.summary", v.liveSetName, v.count);
    this._changes(v);
    this._scopes(v, writing);
    const showName = v.scope === SCOPE.NEW;
    d.nameRow.hidden = !showName;
    if (d.input.value !== v.name && !this.composing) d.input.value = v.name;
    d.input.disabled = writing;
    const nameErr = v.nameError ? tr(v.nameError.key, ...v.nameError.args) : "";
    if (d.nameErr.textContent !== nameErr) d.nameErr.textContent = nameErr;
    d.input.setAttribute("aria-invalid", nameErr ? "true" : "false");
    const inline = v.inlineError ? tr(v.inlineError.key, ...v.inlineError.args) : "";
    if (d.inlineErr.textContent !== inline) d.inlineErr.textContent = inline;
    d.status.textContent = writing ? tr("studio.write.writing") : "";
    d.cancel.disabled = writing;
    d.confirm.disabled = writing || !v.canConfirm;
    d.confirm.setAttribute("aria-disabled", d.confirm.disabled ? "true" : "false");
    // focus: a disabled button drops the focus in a browser, so while the Write is out it rests on the dialog itself (Esc still reaches it); afterwards it goes where the refusal is
    if (fresh) {
      this._focusScope(v);
    } else if (writing) {
      if (this.doc.activeElement !== d.box) d.box.focus();
    } else if (this._wasWriting) {
      if (v.nameError && showName) d.input.focus();
      else this._focusScope(v);
    } else if (v.nameError && nameErr !== this._lastNameError && showName) {
      d.input.focus();
    }
    this._wasWriting = writing;
    this._lastNameError = nameErr;
    this._lastInline = inline;
  }

  _changes(v) {
    const d = this.dialog;
    const tr = this.tr;
    const sig = JSON.stringify([v.changes.map((i) => [i.key, i.part, i.scene, i.param_id, i.slot]), v.more]);
    if (sig === this._sigChanges) return;
    this._sigChanges = sig;
    d.list.textContent = "";
    for (const item of v.changes) d.list.appendChild(el(this.doc, "li", "wr-change", changeLabel(item, tr)));
    if (v.more > 0) d.list.appendChild(el(this.doc, "li", "wr-change wr-more", tr("studio.write.more_changes", v.more)));
    d.changesHead.hidden = v.changes.length === 0 && v.more === 0;
    d.list.hidden = d.changesHead.hidden;
  }

  _scopes(v, writing) {
    const d = this.dialog;
    const tr = this.tr;
    v.scopes.forEach((s, i) => {
      const r = d.radios[i];
      const on = s.id === v.scope;
      r.node.setAttribute("aria-checked", on ? "true" : "false");
      r.node.setAttribute("aria-disabled", !s.enabled || writing ? "true" : "false");
      r.node.tabIndex = on ? 0 : -1;
      const cls = "wr-scope" + (on ? " on" : "") + (!s.enabled ? " disabled" : "");
      if (r.node.className !== cls) r.node.className = cls;
      r.label.textContent = tr(s.key);
      r.desc.textContent = s.enabled ? tr(s.descKey, ...s.descArgs) : "";
      r.reason.textContent = s.enabled ? "" : tr(s.reasonKey);
      r.reason.hidden = s.enabled;
      r._scope = s;
    });
  }

  _focusScope(v) {
    const d = this.dialog;
    const i = v.scopes.findIndex((s) => s.id === v.scope);
    const r = d.radios[i < 0 ? 0 : i];
    if (r) r.node.focus();
  }

  _build() {
    const doc = this.doc;
    const tr = this.tr;
    this.seq++;
    this._sigChanges = null;
    const backdrop = el(doc, "div", "sv-backdrop");
    const box = el(doc, "div", "sv-dialog wr-dialog");
    box.setAttribute("role", "dialog");
    box.setAttribute("aria-modal", "true");
    box.tabIndex = -1;
    const title = el(doc, "h2", "sv-dialog-title");
    title.id = `wr-title-${this.seq}`;
    box.setAttribute("aria-labelledby", title.id);
    const summary = el(doc, "div", "sv-note wr-summary");
    summary.id = `wr-summary-${this.seq}`;
    box.setAttribute("aria-describedby", summary.id);
    const changesHead = el(doc, "h3", "wr-head", tr("studio.write.what_changed"));
    changesHead.id = `wr-changes-${this.seq}`;
    const list = el(doc, "ul", "wr-changes");
    list.setAttribute("aria-labelledby", changesHead.id);
    const scopeHead = el(doc, "h3", "wr-head", tr("studio.write.scope_head"));
    scopeHead.id = `wr-scope-${this.seq}`;
    const group = el(doc, "div", "wr-scopes");
    group.setAttribute("role", "radiogroup");
    group.setAttribute("aria-labelledby", scopeHead.id);
    const radios = [];
    for (let i = 0; i < 3; i++) {
      const wrap = el(doc, "div", "wr-scope-wrap");
      const node = el(doc, "div", "wr-scope");
      node.setAttribute("role", "radio");
      node.id = `wr-radio-${this.seq}-${i}`;
      const label = el(doc, "span", "wr-scope-label");
      const desc = el(doc, "span", "wr-scope-desc");
      node.append(label, desc);
      const reason = el(doc, "div", "sv-note wr-reason");
      reason.id = `wr-reason-${this.seq}-${i}`;
      reason.hidden = true;
      node.setAttribute("aria-describedby", reason.id);
      node.addEventListener("click", () => {
        const s = radios[i]._scope;
        if (s) this.ctl.setScope(s.id);
      });
      wrap.append(node, reason);
      group.appendChild(wrap);
      radios.push({ node, label, desc, reason, _scope: null });
    }
    const nameRow = el(doc, "label", "wr-name");
    nameRow.hidden = true;
    const nameLabel = el(doc, "span", "", tr("studio.write.name_label"));
    const input = el(doc, "input");
    input.type = "text";
    input.setAttribute("autocomplete", "off");
    input.setAttribute("autocapitalize", "off");
    input.setAttribute("spellcheck", "false");
    const nameErr = el(doc, "div", "pair-dialog-error sv-dialog-error");
    nameErr.setAttribute("role", "alert");
    nameErr.id = `wr-nameerr-${this.seq}`;
    input.setAttribute("aria-describedby", nameErr.id);
    nameRow.append(nameLabel, input);
    const inlineErr = el(doc, "div", "pair-dialog-error sv-dialog-error");
    inlineErr.setAttribute("role", "alert");
    const status = el(doc, "div", "sv-note wr-status");
    status.setAttribute("role", "status");
    status.setAttribute("aria-live", "polite");
    const actions = el(doc, "div", "pair-dialog-actions");
    const cancel = el(doc, "button", "btn", tr("studio.write.cancel"));
    cancel.type = "button";
    cancel.addEventListener("click", () => this.ctl.cancel());
    const confirm = el(doc, "button", "btn btn-primary", tr("studio.write.confirm"));
    confirm.type = "button";
    confirm.addEventListener("click", () => {
      if (confirm.disabled) return;
      this.ctl.confirm();
    });
    actions.append(cancel, confirm);
    box.append(title, summary, changesHead, list, scopeHead, group, nameRow, nameErr, inlineErr, status, actions);
    backdrop.appendChild(box);
    backdrop.addEventListener("click", (e) => {
      if (e.target === backdrop) this.ctl.cancel(); // never while a Write is out (cancel() refuses)
    });
    input.addEventListener("compositionstart", () => { this.composing = true; });
    input.addEventListener("compositionend", () => {
      this.composing = false;
      this.ctl.setName(input.value);
    });
    input.addEventListener("input", () => {
      if (!this.composing) this.ctl.setName(input.value);
    });
    box.addEventListener("keydown", (e) => this._key(e));
    this.host.appendChild(backdrop);
    this.dialog = { backdrop, box, title, summary, changesHead, list, group, radios, nameRow, input, nameErr, inlineErr, status, cancel, confirm };
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
      if (c.writing) {
        e.preventDefault(); // nothing in the dialog can be used while the Write is out: the focus rests on the dialog
        d.box.focus();
        return;
      }
      const f = [d.radios.find((r) => r.node.getAttribute("aria-checked") === "true")?.node, d.nameRow.hidden ? null : d.input, d.cancel, d.confirm].filter((n) => n && !n.disabled);
      if (f.length === 0) {
        e.preventDefault();
        d.box.focus();
        return;
      }
      const at = f.indexOf(e.target);
      const next = e.shiftKey ? (at <= 0 ? f.length - 1 : at - 1) : at === f.length - 1 ? 0 : at + 1;
      e.preventDefault();
      f[next].focus();
      return;
    }
    if (composing(e)) return;
    const radioIndex = d.radios.findIndex((r) => r.node === e.target);
    if (radioIndex >= 0 && !c.writing) {
      const enabled = d.radios.map((r, i) => (r._scope && r._scope.enabled ? i : -1)).filter((i) => i >= 0);
      const at = enabled.indexOf(radioIndex);
      let to = -1;
      if (e.key === "ArrowDown" || e.key === "ArrowRight") to = enabled[(at + 1) % enabled.length];
      else if (e.key === "ArrowUp" || e.key === "ArrowLeft") to = enabled[(at <= 0 ? enabled.length : at) - 1];
      else if (e.key === "Home") to = enabled[0];
      else if (e.key === "End") to = enabled[enabled.length - 1];
      if (to >= 0) {
        e.preventDefault();
        const s = d.radios[to]._scope;
        if (c.setScope(s.id)) d.radios[to].node.focus();
        return;
      }
      if (e.key === " " || e.key === "Spacebar" || e.key === "Enter") {
        e.preventDefault();
        const s = d.radios[radioIndex]._scope;
        if (s) c.setScope(s.id);
      }
      return;
    }
    if (e.target === d.input && e.key === "Enter") {
      e.preventDefault();
      c.confirm();
    }
  }
}

/** The Studio Banner about a failed or unconfirmed Write. `el` is the banner element in the page (role="alert", hidden while empty). */
export class WriteBannerView {
  constructor({ doc, el: root, tr, controller, onReload }) {
    this.doc = doc;
    this.root = root;
    this.tr = tr;
    this.ctl = controller;
    this.onReload = onReload || (() => {});
    this.text = el(doc, "span", "wr-banner-text");
    this.action = el(doc, "button", "btn wr-banner-action");
    this.action.type = "button";
    this.action.hidden = true;
    this.dismiss = el(doc, "button", "btn wr-banner-dismiss", tr("studio.write.dismiss"));
    this.dismiss.type = "button";
    this.action.addEventListener("click", () => {
      const b = this.ctl.banner;
      if (b && b.kind === "unknown") this.ctl.recheck();
      else if (b && b.kind === "stale") this.onReload();
    });
    this.dismiss.addEventListener("click", () => this.ctl.dismissBanner());
    root.textContent = "";
    root.append(this.text, this.action, this.dismiss);
    root.hidden = true;
  }

  setLabels() {
    this.dismiss.textContent = this.tr("studio.write.dismiss");
    this.render();
  }

  render() {
    const b = this.ctl.banner;
    const tr = this.tr;
    if (!b) {
      this.root.hidden = true;
      this.text.textContent = "";
      return;
    }
    const text = bannerText(b, tr);
    if (this.text.textContent !== text) this.text.textContent = text;
    this.action.hidden = !(b.kind === "unknown" || b.kind === "stale");
    this.action.textContent = tr(b.kind === "unknown" ? "studio.write.check_now" : "studio.part_editor.stale_reload");
    this.root.hidden = false;
  }
}
