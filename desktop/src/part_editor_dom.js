// Velora Studio: the Part Editor DOM (Story 9.9). Draws the model part_editor.js builds: four hairline-divided sections (no menus, no tabs), every parameter a Studio Knob
// (an ARIA slider: name, min / max / now and a value text with its unit), the insert chain, the envelope polyline and the ASSIGN map. Every string is a locale key through
// `tr` (or text that came from the unit and is set with textContent, never as markup). Colours are tokens in style.css, sizes are rem.
//
// A render with the same structure updates the existing elements in place (values, aria, read-only state), so the focus, a drag in progress and an open value entry
// survive the State pushes that follow every edit; a different structure (another part, another engine, an insert added) rebuilds the sections and puts the focus back on
// the knob with the same key.
//
// Keyboard (on a focused knob): Up / Right +1 step, Down / Left -1, Shift = 10 steps, PageUp / PageDown the large step, Home / End min / max, Space toggles a switch,
// Enter opens a value entry (Enter applies, Escape cancels; a value the knob cannot take is told in a live region and the knob keeps its value). Pointer: press and drag
// (right / up raises; Shift = fine); the mouse wheel changes the knob that has the focus (so scrolling the page over other knobs never changes a value). Tab visits the knobs in
// the order they are drawn.

import { applyDrag, applyKey, applyWheel, formatValue, groupTitleKey, paramLabel, parseEntry } from "./part_editor.js";

const SVG_NS = "http://www.w3.org/2000/svg";

function fmt(text, ...args) {
  let i = 0;
  return String(text).replace(/%[ds]/g, () => String(args[i++]));
}

export class PartEditorView {
  constructor({ doc, host, tr, controller, announce }) {
    this.doc = doc;
    this.host = host;
    this.tr = tr;
    this.controller = controller;
    this.announceFn = announce || (() => {});
    this.knobs = new Map(); // key -> { el, slider, valueEl, fill, errEl, entry, k }
    this.errors = new Map(); // key -> text: a typed error stays shown through the rebuild the revert causes, until the next edit of that knob
    this.msgText = "";
    this.signature = "";
    this.model = null;
    this.header = null;
    this.drag = null;
    this.deferred = null; // a model with another structure that arrived during a drag: drawn when the drag ends
    this.fids = new Map(); // stable id -> element, for the controls that keep the focus through a rebuild
    this.root = null;
  }

  el(tag, cls, text) {
    const e = this.doc.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  // ---- public ----------------------------------------------------------------------------------------------------------------------------------------------

  /** The small line above the sections: `{ number, name, category }` of the selected part (text from the unit). */
  setHeader(h) {
    this.header = h;
    if (this.headerEl) this.paintHeader();
  }

  render(model) {
    this.model = model;
    const sig = this.signatureOf(model);
    if (this.drag && this.root && sig !== this.signature) {
      this.deferred = model; // never rebuild under a drag in progress (the slider being dragged must stay the same element)
      return;
    }
    if (sig !== this.signature || !this.root) this.rebuild(model, sig);
    else this.update(model);
  }

  clear() {
    this.signature = "";
    this.knobs.clear();
    this.fids.clear();
    this.root = null;
    this.deferred = null;
    this.host.textContent = "";
  }

  /** A typed error from the unit for `key` (a knob key, "chain" or "assign:n"): shown next to the knob (or at the top) and announced. */
  showError(err) {
    const text = err.notice
      ? this.tr(err.notice === "not_applied" ? "studio.part_editor.err_not_applied" : "studio.part_editor.err_unconfirmed")
      : fmt(this.tr("studio.part_editor.err_rejected"), err.message || String(err.code || ""));
    if (this.knobs.has(err.key)) this.errors.set(err.key, text);
    else this.msgText = text;
    this.paintErrors();
    this.say(text); // once: through the editor's own polite region
  }

  /** The polite live region of the editor (set after a tick so the same text twice is announced twice). Safe before the first render. */
  say(text) {
    const live = this.live;
    if (!live) return;
    live.textContent = "";
    queueMicrotask(() => { live.textContent = text; });
  }

  clearError(key) {
    if (key) this.errors.delete(key);
    else this.msgText = "";
    this.paintErrors();
  }

  paintErrors() {
    for (const [key, k] of this.knobs) {
      const text = this.errors.get(key);
      if (text) {
        k.errEl.textContent = text;
        k.errEl.hidden = false;
        k.slider.setAttribute("aria-invalid", "true");
      } else if (!k.entry || k.entry.getAttribute("aria-invalid") !== "true") {
        k.errEl.textContent = "";
        k.errEl.hidden = true;
        k.slider.removeAttribute("aria-invalid");
      }
    }
    if (this.msgEl) {
      this.msgEl.textContent = this.msgText;
      this.msgEl.hidden = !this.msgText;
    }
  }

  // ---- structure ---------------------------------------------------------------------------------------------------------------------------------------------

  signatureOf(m) {
    const parts = [m.partIndex, m.engine];
    for (const s of m.sections) {
      parts.push("S" + s.id);
      for (const g of s.groups) parts.push("G" + g.id + ":" + g.knobs.map((k) => k.key).join(","));
      if (s.inserts) parts.push("I" + s.inserts.map((i) => `${i.slot}:${i.type.value === 0 ? 0 : 1}:${i.params.length}`).join(","));
      if (s.assign) parts.push("A" + s.assign.options.length); // the selects show their value through paintAssign: a changed slot is no structural change
      if (s.envelope !== undefined) parts.push("E" + (s.envelope ? 1 : 0));
    }
    return parts.join("|");
  }

  rebuild(m, sig) {
    const focusKey = this.focusedKey();
    this.signature = sig;
    this.knobs.clear();
    this.fids.clear();
    this.host.textContent = "";
    const root = this.el("div", "pe");
    this.root = root;
    this.live = this.el("div", "sr-only");
    this.live.setAttribute("role", "status");
    this.live.setAttribute("aria-live", "polite");
    this.headerEl = this.el("div", "pe-head");
    this.paintHeader();
    this.bannerEl = this.el("div", "pe-banner");
    this.bannerEl.setAttribute("role", "status");
    this.msgEl = this.el("div", "pe-msg");
    this.msgEl.hidden = true;
    root.appendChild(this.live);
    root.appendChild(this.headerEl);
    root.appendChild(this.bannerEl);
    root.appendChild(this.msgEl);
    this.labelCounts = new Map();
    for (const sec of m.sections) for (const g of sec.groups) for (const k of g.knobs) this.labelCounts.set(this.labelOf(k), (this.labelCounts.get(this.labelOf(k)) || 0) + 1);
    const grid = this.el("div", "pe-sections");
    root.appendChild(grid);
    for (const s of m.sections) grid.appendChild(this.buildSection(s, m));
    this.host.appendChild(root);
    this.update(m);
    if (focusKey) {
      const el = this.fids.get(focusKey) || (focusKey.startsWith("ins:") ? this.addBtn : null);
      if (el && el.focus) el.focus();
    }
  }

  /** The stable id (`data-fid`) of the control that has the focus, "" when none of ours: knobs by key, the insert buttons, "+ Add insert", the ASSIGN selects. */
  focusedKey() {
    const a = this.doc.activeElement;
    if (!a) return "";
    for (const [key, k] of this.knobs) if (k.entry === a) return key;
    for (const [fid, el] of this.fids) if (el === a) return fid;
    return "";
  }

  fid(el, id) {
    el.setAttribute("data-fid", id);
    this.fids.set(id, el);
    return el;
  }

  paintHeader() {
    const h = this.header;
    this.headerEl.textContent = "";
    if (!h) return;
    this.headerEl.appendChild(this.el("small", "", fmt(this.tr("studio.live_set.part"), h.number)));
    this.headerEl.appendChild(this.el("b", "", h.name || ""));
    if (h.category) this.headerEl.appendChild(this.el("small", "pe-cat", h.category));
  }

  buildSection(s, m) {
    const sec = this.el("section", "pe-sec");
    sec.setAttribute("data-section", s.id);
    const id = `pe-h-${s.id}`;
    const h = this.el("h3", "", this.tr(s.titleKey));
    h.id = id;
    sec.setAttribute("aria-labelledby", id);
    sec.appendChild(h);
    if (s.inert) sec.appendChild(this.el("div", "pe-note", this.tr("studio.part_editor.inert_section")));
    // labels that appear more than once in the editor get their group title in the accessible name (so no two sliders share a name)
    const counts = this.labelCounts;
    if (s.envelope) sec.appendChild(this.buildEnvelope(s.envelope));
    for (const g of s.groups) {
      const grp = this.el("div", "pe-group");
      grp.setAttribute("role", "group");
      const title = this.groupTitle(g.id);
      const gh = this.el("h4", "", title);
      gh.id = `pe-g-${g.id.replace(/\./g, "-")}`;
      grp.setAttribute("aria-labelledby", gh.id);
      grp.appendChild(gh);
      const wrap = this.el("div", "pe-knobs");
      for (const k of g.knobs) {
        const dup = counts.get(this.labelOf(k)) > 1;
        wrap.appendChild(this.buildKnob(k, dup ? `${title} · ${this.labelOf(k)}` : this.labelOf(k)));
      }
      grp.appendChild(wrap);
      sec.appendChild(grp);
    }
    if (s.inserts) sec.appendChild(this.buildInserts(s));
    if (s.assign) sec.appendChild(this.buildAssign(s.assign));
    return sec;
  }

  groupTitle(id) {
    const key = groupTitleKey(id);
    const t = this.tr(key);
    return t && t !== key ? t : id.split(".").pop().replace(/_/g, " ");
  }

  labelOf(k) {
    if (k.kind === "builtin") {
      const t = this.tr(k.labelKey);
      return t && t !== k.labelKey ? t : k.id;
    }
    return paramLabel({ name: k.id, localeKey: k.localeKey }, this.tr);
  }

  buildEnvelope(env) {
    const wrap = this.el("div", "pe-env");
    const svg = this.doc.createElementNS ? this.doc.createElementNS(SVG_NS, "svg") : this.el("svg");
    svg.setAttribute("viewBox", "0 0 220 70");
    svg.setAttribute("preserveAspectRatio", "xMidYMid meet"); // dots stay round
    svg.setAttribute("role", "img");
    svg.setAttribute("class", "pe-env-svg");
    this.envSvg = svg;
    wrap.appendChild(svg);
    this.envWrap = wrap;
    this.paintEnvelope(env);
    return wrap;
  }

  paintEnvelope(env) {
    const svg = this.envSvg;
    if (!svg || !env) return;
    svg.textContent = "";
    const mk = (tag) => (this.doc.createElementNS ? this.doc.createElementNS(SVG_NS, tag) : this.el(tag));
    const line = mk("polyline");
    line.setAttribute("points", env.pointsAttr);
    line.setAttribute("class", "pe-env-line");
    line.setAttribute("fill", "none");
    svg.appendChild(line);
    env.points.slice(1, -1).forEach((p) => {
      const c = mk("circle");
      c.setAttribute("cx", String(p.x));
      c.setAttribute("cy", String(p.y));
      c.setAttribute("r", "3");
      c.setAttribute("class", "pe-env-dot");
      svg.appendChild(c);
    });
    const bits = [`${this.tr("studio.part_editor.env_attack")} ${Math.round(env.attackMs)} ms`];
    if (env.decayMs) bits.push(`${this.tr("studio.part_editor.env_decay")} ${Math.round(env.decayMs)} ms`);
    if (env.sustain !== null && env.sustain !== undefined && env.sustain < 1) bits.push(`${this.tr("studio.part_editor.env_sustain")} ${Math.round(env.sustain * 100)} %`);
    bits.push(`${this.tr("studio.part_editor.env_release")} ${Math.round(env.releaseMs)} ms`);
    svg.setAttribute("aria-label", fmt(this.tr("studio.part_editor.envelope_aria"), bits.join(", ")));
  }

  // ---- one Studio Knob ---------------------------------------------------------------------------------------------------------------------------------------

  buildKnob(k, name) {
    const box = this.el("div", "pe-knob");
    box.setAttribute("data-key", k.key);
    const label = this.el("span", "pe-knob-label", this.labelOf(k));
    label.setAttribute("aria-hidden", "true"); // the slider's aria-label carries the name
    const labelId = `pe-l-${k.key.replace(/[^a-z0-9]/gi, "-")}`;
    label.id = labelId;
    const slider = this.el("div", "pe-slider");
    this.fid(slider, k.key);
    slider.setAttribute("role", "slider");
    slider.setAttribute("tabindex", "0");
    slider.setAttribute("aria-label", name);
    slider.setAttribute("aria-valuemin", String(k.min));
    slider.setAttribute("aria-valuemax", String(k.max));
    const track = this.el("div", "pe-track");
    const fill = this.el("div", "pe-fill");
    track.appendChild(fill);
    slider.appendChild(track);
    const valueEl = this.el("span", "pe-value"); // not an <output> (a live region): aria-valuetext carries the value
    valueEl.setAttribute("aria-hidden", "true");
    const errEl = this.el("div", "pe-err"); // announced once, by the editor's own polite region (say)
    errEl.hidden = true;
    box.appendChild(label);
    box.appendChild(slider);
    box.appendChild(valueEl);
    box.appendChild(errEl);
    const rec = { box, el: box, slider, valueEl, fill, errEl, entry: null, k, name };
    this.knobs.set(k.key, rec);
    this.wireKnob(rec);
    return box;
  }

  wireKnob(rec) {
    const { slider } = rec;
    slider.addEventListener("keydown", (ev) => {
      const k = rec.k;
      if (k.readOnly || ev.ctrlKey || ev.metaKey || ev.altKey) return; // browser / OS shortcuts are not ours
      const r = applyKey(k, k.value, ev);
      if (!r.handled) return;
      ev.preventDefault();
      if (r.entry) this.openEntry(rec);
      else this.change(rec, r.value);
    });
    slider.addEventListener("pointerdown", (ev) => {
      const k = rec.k;
      if (k.readOnly || (ev.button !== undefined && ev.button !== 0)) return;
      slider.focus();
      if (slider.setPointerCapture && ev.pointerId !== undefined) {
        try { slider.setPointerCapture(ev.pointerId); } catch (e) { /* a synthetic pointer */ }
      }
      this.drag = { rec, x: ev.clientX || 0, y: ev.clientY || 0, start: Number.isFinite(k.value) ? k.value : k.min, last: null, shift: !!ev.shiftKey };
      slider.setAttribute("data-dragging", "true");
    });
    slider.addEventListener("pointermove", (ev) => {
      const d = this.drag;
      if (!d || d.rec !== rec) return;
      if (!!ev.shiftKey !== d.shift) {
        // Shift pressed or released mid-drag: carry on from here at the other sensitivity (the value must not jump)
        d.shift = !!ev.shiftKey;
        d.start = Number.isFinite(rec.k.value) ? rec.k.value : d.start;
        d.x = ev.clientX || 0;
        d.y = ev.clientY || 0;
      }
      const v = applyDrag(rec.k, d.start, (ev.clientX || 0) - d.x, (ev.clientY || 0) - d.y, d.shift);
      if (v !== d.last && v !== rec.k.value) {
        d.last = v;
        this.change(rec, v);
      }
    });
    const end = () => {
      if (this.drag && this.drag.rec === rec) {
        this.drag = null;
        slider.removeAttribute("data-dragging");
        if (this.deferred) {
          const m = this.deferred;
          this.deferred = null;
          this.render(m);
        }
      }
    };
    slider.addEventListener("pointerup", end);
    slider.addEventListener("pointercancel", end);
    slider.addEventListener("lostpointercapture", end);
    slider.addEventListener("wheel", (ev) => {
      const k = rec.k;
      if (k.readOnly || this.doc.activeElement !== slider) return; // the page scrolls over knobs that do not have the focus
      ev.preventDefault();
      // a notch is about 100 px (a trackpad sends many small deltas, with momentum): accumulate, and step once per notch
      const unit = ev.deltaMode === 1 ? 3 : ev.deltaMode === 2 ? 800 : 100;
      const dy = (ev.deltaY || 0) / unit;
      if (rec.wheelAcc && Math.sign(rec.wheelAcc) !== Math.sign(dy)) rec.wheelAcc = 0;
      rec.wheelAcc = (rec.wheelAcc || 0) + dy;
      const steps = Math.trunc(Math.abs(rec.wheelAcc));
      if (!steps) return;
      rec.wheelAcc -= Math.sign(rec.wheelAcc) * steps;
      let v = k.value;
      for (let i = 0; i < Math.min(steps, 3); i++) v = applyWheel(k, v, { deltaY: dy < 0 ? -1 : 1, shiftKey: ev.shiftKey });
      this.change(rec, v);
    });
  }

  change(rec, value) {
    if (value === rec.k.value) return;
    this.clearError(rec.k.key);
    const shown = this.controller.edit(rec.k.key, value);
    if (shown !== null && shown !== undefined) this.announceKnob(rec, shown);
  }

  announceKnob(rec, shown) {
    // the slider itself is announced by the screen reader from aria-valuetext; nothing extra here
    void rec;
    void shown;
  }

  openEntry(rec) {
    if (rec.entry) {
      rec.entry.focus();
      return;
    }
    const input = this.el("input", "pe-entry");
    input.setAttribute("type", "text");
    input.setAttribute("inputmode", "decimal");
    input.setAttribute("aria-label", fmt(this.tr("studio.part_editor.entry_label"), rec.name));
    input.value = formatValue(rec.k, rec.k.value, this.tr) === "—" ? "" : formatValue(rec.k, rec.k.value, this.tr);
    rec.entry = input;
    rec.box.appendChild(input);
    rec.valueEl.hidden = true;
    const close = (refocus) => {
      if (!rec.entry) return;
      const e = rec.entry;
      rec.entry = null;
      e.remove();
      rec.valueEl.hidden = false;
      this.paintErrors(); // an entry error goes away with the entry
      if (refocus) rec.slider.focus();
    };
    input.addEventListener("keydown", (ev) => {
      if (ev.key === "Escape") {
        ev.preventDefault();
        close(true);
      } else if (ev.key === "Enter") {
        ev.preventDefault();
        const r = parseEntry(rec.k, input.value, this.tr);
        if (r.ok) {
          close(true);
          this.change(rec, r.value);
        } else {
          const text = r.reason === "range"
            ? fmt(this.tr("studio.part_editor.entry_range"), rec.name, formatValue(rec.k, rec.k.min, this.tr), formatValue(rec.k, rec.k.max, this.tr))
            : fmt(this.tr("studio.part_editor.entry_invalid"), rec.name);
          input.setAttribute("aria-invalid", "true");
          rec.errEl.textContent = text;
          rec.errEl.hidden = false;
          this.say(text);
        }
      }
    });
    input.addEventListener("blur", () => close(false));
    input.focus();
    if (input.select) input.select();
  }

  // ---- in-place update -----------------------------------------------------------------------------------------------------------------------------------------

  update(m) {
    const all = [];
    for (const s of m.sections) {
      for (const g of s.groups) all.push(...g.knobs);
      for (const i of s.inserts || []) all.push(i.type, i.bypass, ...i.params);
    }
    for (const k of all) this.paintKnob(k);
    this.paintErrors();
    this.bannerEl.hidden = m.editable;
    this.bannerEl.textContent = m.editable ? "" : this.tr(m.reason);
    for (const s of m.sections) {
      if (s.envelope) this.paintEnvelope(s.envelope);
      if (s.inserts) this.paintInserts(s);
      if (s.assign) this.paintAssign(s.assign);
    }
  }

  paintKnob(k) {
    const rec = this.knobs.get(k.key);
    if (!rec) return;
    rec.k = k;
    const s = rec.slider;
    const text = formatValue(k, k.value, this.tr);
    s.setAttribute("aria-valuemin", String(k.min));
    s.setAttribute("aria-valuemax", String(k.max));
    if (k.valueNow === null || k.valueNow === undefined) s.removeAttribute("aria-valuenow");
    else s.setAttribute("aria-valuenow", String(k.valueNow));
    s.setAttribute("aria-valuetext", text);
    if (k.readOnly) s.setAttribute("aria-readonly", "true");
    else s.removeAttribute("aria-readonly");
    // the hint: why it cannot be edited, else that the unit stores the value but does not play it yet
    const why = k.readOnly && k.reason ? this.tr(k.reason) : k.inert ? this.tr("studio.part_editor.inert_hint") : "";
    s.title = why;
    if (why) s.setAttribute("aria-description", why);
    else s.removeAttribute("aria-description");
    if (k.inert) s.setAttribute("data-inert", "true");
    else s.removeAttribute("data-inert");
    if (k.pending) {
      s.setAttribute("data-pending", "true");
      s.setAttribute("aria-busy", "true");
    } else {
      s.removeAttribute("data-pending");
      s.removeAttribute("aria-busy");
    }
    const span = k.max - k.min;
    const pct = k.value === null || k.value === undefined || !(span > 0) ? 0 : Math.min(1, Math.max(0, (k.value - k.min) / span));
    rec.fill.style.width = `${Math.round(pct * 1000) / 10}%`;
    if (rec.valueEl.textContent !== text) rec.valueEl.textContent = text;
  }

  // ---- insert chain ------------------------------------------------------------------------------------------------------------------------------------------------

  buildInserts(s) {
    const wrap = this.el("div", "pe-group pe-inserts");
    wrap.setAttribute("role", "group");
    const h = this.el("h4", "", this.groupTitle("fx.insert"));
    h.id = "pe-g-fx-insert";
    wrap.setAttribute("aria-labelledby", h.id);
    wrap.appendChild(h);
    this.insertList = this.el("div", "pe-chain");
    wrap.appendChild(this.insertList);
    this.insertRows = [];
    s.inserts.forEach((ins) => {
      const row = this.el("div", "pe-insert");
      row.setAttribute("role", "group");
      row.setAttribute("aria-label", fmt(this.tr("studio.part_editor.insert_n"), ins.slot + 1));
      row.appendChild(this.el("span", "pe-insert-n", String(ins.slot + 1)));
      const knobsEl = this.el("div", "pe-knobs");
      for (const k of [ins.type, ins.bypass, ...ins.params]) {
        const base = this.labelOf(k);
        const nm = fmt(this.tr("studio.part_editor.insert_knob"), ins.slot + 1, base);
        knobsEl.appendChild(this.buildKnob(k, nm));
      }
      row.appendChild(knobsEl);
      const btns = this.el("div", "pe-insert-btns");
      const mk = (cls, text, aria, fn) => {
        const b = this.el("button", "btn " + cls, text);
        b.setAttribute("type", "button");
        b.setAttribute("aria-label", aria);
        this.fid(b, `ins:${ins.slot}:${cls.slice(3)}`); // ins:0:up | down | remove
        b.title = aria;
        b.addEventListener("click", () => { this.clearError(""); fn(); });
        btns.appendChild(b);
        return b;
      };
      row.btnUp = mk("pe-up", "▲", fmt(this.tr("studio.part_editor.insert_up"), ins.slot + 1), () => this.controller.moveInsert(ins.slot, -1));
      row.btnDown = mk("pe-down", "▼", fmt(this.tr("studio.part_editor.insert_down"), ins.slot + 1), () => this.controller.moveInsert(ins.slot, 1));
      row.btnRemove = mk("pe-remove", "✕", fmt(this.tr("studio.part_editor.insert_remove"), ins.slot + 1), () => this.controller.removeInsert(ins.slot));
      row.appendChild(btns);
      row.slot = ins.slot;
      this.insertList.appendChild(row);
      this.insertRows.push(row);
    });
    this.addBtn = this.el("button", "btn pe-add", this.tr("studio.part_editor.add_insert"));
    this.addBtn.setAttribute("type", "button");
    this.fid(this.addBtn, "add");
    this.addBtn.addEventListener("click", () => { this.clearError(""); this.controller.addInsert(); });
    wrap.appendChild(this.addBtn);
    return wrap;
  }

  paintInserts(s) {
    const rows = this.insertRows || [];
    s.inserts.forEach((ins, i) => {
      const row = rows[i];
      if (!row) return;
      row.btnUp.disabled = !ins.canMoveUp || ins.type.readOnly;
      row.btnDown.disabled = !ins.canMoveDown || ins.type.readOnly;
      row.btnRemove.disabled = ins.type.readOnly;
    });
    this.addBtn.disabled = !s.canAdd;
    const why = !s.canAdd && s.addReason ? this.tr(s.addReason) : "";
    this.addBtn.title = why;
    if (why) this.addBtn.setAttribute("aria-description", why);
    else this.addBtn.removeAttribute("aria-description");
  }

  // ---- ASSIGN · KNOBS ----------------------------------------------------------------------------------------------------------------------------------------

  buildAssign(a) {
    const wrap = this.el("div", "pe-group pe-assign");
    wrap.setAttribute("role", "group");
    const h = this.el("h4", "", this.tr("studio.part_editor.assign_title"));
    h.id = "pe-g-assign";
    wrap.setAttribute("aria-labelledby", h.id);
    wrap.appendChild(h);
    this.assignSelects = [];
    const reg = this.controller.registry;
    const byId = new Map(reg.params.map((d) => [d.id, d]));
    const label = (opt) => paramLabel({ name: opt.name, localeKey: opt.localeKey }, this.tr);
    a.slots.forEach((slot) => {
      const row = this.el("label", "pe-assign-slot");
      row.appendChild(this.el("span", "pe-assign-k", fmt(this.tr("studio.part_editor.assign_k"), slot.slot)));
      const sel = this.el("select", "pe-assign-select");
      sel.setAttribute("aria-label", fmt(this.tr("studio.part_editor.assign_aria"), slot.slot));
      this.fid(sel, `assign:${slot.slot}`);
      const add = (value, text, parent) => {
        const o = this.el("option", "", text);
        o.value = String(value);
        o.setAttribute("value", String(value));
        (parent || sel).appendChild(o);
        return o;
      };
      add(0, this.tr("studio.part_editor.assign_empty"));
      const groups = new Map();
      for (const opt of a.options) {
        if (!groups.has(opt.group)) {
          const og = this.el("optgroup", "");
          og.setAttribute("label", opt.scope === "master" ? this.tr("studio.part_editor.assign_global") : this.groupTitle(opt.group));
          groups.set(opt.group, og);
          sel.appendChild(og);
        }
        add(opt.paramId, label(opt), groups.get(opt.group));
      }
      if (slot.paramId && !a.options.some((o) => o.paramId === slot.paramId)) {
        const d = byId.get(slot.paramId);
        add(slot.paramId, d ? label({ name: d.name, localeKey: d.localeKey }) : `#${slot.paramId}`);
      }
      sel.addEventListener("change", () => { this.clearError(""); this.controller.assign(slot.slot, Number(sel.value)); });
      row.appendChild(sel);
      row.select = sel;
      this.assignSelects.push(sel);
      wrap.appendChild(row);
    });
    return wrap;
  }

  paintAssign(a) {
    (this.assignSelects || []).forEach((sel, i) => {
      const slot = a.slots[i];
      if (!slot) return;
      sel.value = String(slot.paramId);
      sel.disabled = slot.readOnly;
      sel.title = slot.readOnly && slot.reason ? this.tr(slot.reason) : "";
      if (slot.pending) sel.setAttribute("data-pending", "true");
      else sel.removeAttribute("data-pending");
    });
  }
}
