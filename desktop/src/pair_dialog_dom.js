// Velora Studio — Pairing Dialog View (Story 9.6, UX-DR37, UX-DR46, UX-DR77, NFR16).
// Accessible modal dialog for entering the 6-digit code shown on the Velora hardware display.

export class PairDialogView {
  /**
   * @param {Object} options
   * @param {Document} options.doc
   * @param {HTMLElement} [options.host]
   * @param {(key: string) => string} options.tr
   * @param {import("./pair_dialog.js").PairDialogController} options.controller
   */
  constructor({ doc, host, tr, controller }) {
    this.doc = doc;
    this.host = host || doc.body;
    this.tr = tr;
    this.ctl = controller;
    this._prevActive = null;
    this.elements = null;
    this._unsub = this.ctl.subscribe(() => this.render());
  }

  destroy() {
    if (this._unsub) {
      this._unsub();
      this._unsub = null;
    }
    this.close();
  }

  open() {
    this._prevActive = this.doc.activeElement;
    this._build();
    this.render();
  }

  close() {
    if (this.elements && this.elements.backdrop) {
      this.elements.backdrop.remove();
      this.elements = null;
    }
    if (this._prevActive && typeof this._prevActive.focus === "function") {
      try { this._prevActive.focus(); } catch (_) {}
      this._prevActive = null;
    }
  }

  _build() {
    if (this.elements) return;
    const doc = this.doc;
    const tr = this.tr;

    const backdrop = doc.createElement("div");
    backdrop.className = "sv-backdrop pair-dialog-backdrop";

    const box = doc.createElement("div");
    box.className = "sv-dialog pair-dialog";
    box.setAttribute("role", "dialog");
    box.setAttribute("aria-modal", "true");
    box.tabIndex = -1;

    const title = doc.createElement("h2");
    title.className = "sv-dialog-title";
    title.id = "pair-dialog-title";
    box.setAttribute("aria-labelledby", title.id);

    const prompt = doc.createElement("p");
    prompt.className = "sv-note pair-dialog-prompt";
    prompt.textContent = tr("studio.pair.prompt");

    const inputWrap = doc.createElement("div");
    inputWrap.className = "pair-input-wrap";

    const input = doc.createElement("input");
    input.type = "text";
    input.className = "pair-code-input";
    input.inputMode = "numeric";
    input.autocomplete = "off";
    input.maxLength = 6;
    input.setAttribute("aria-label", tr("studio.pair.prompt"));
    inputWrap.appendChild(input);

    const err = doc.createElement("div");
    err.className = "pair-dialog-error";
    err.setAttribute("role", "alert");

    const actions = doc.createElement("div");
    actions.className = "pair-dialog-actions";

    const cancel = doc.createElement("button");
    cancel.type = "button";
    cancel.className = "btn";
    cancel.textContent = tr("studio.pair.cancel");

    const submit = doc.createElement("button");
    submit.type = "button";
    submit.className = "btn btn-primary";
    submit.textContent = tr("studio.pair.ok");

    actions.append(cancel, submit);
    box.append(title, prompt, inputWrap, err, actions);
    backdrop.appendChild(box);

    backdrop.addEventListener("click", (e) => {
      if (e.target === backdrop) {
        this.ctl.cancel();
      }
    });

    input.addEventListener("input", () => {
      this.ctl.setCode(input.value);
    });

    box.addEventListener("keydown", (e) => this._key(e));
    cancel.addEventListener("click", () => this.ctl.cancel());
    submit.addEventListener("click", () => this.ctl.submit());

    this.host.appendChild(backdrop);
    this.elements = { backdrop, box, title, prompt, input, err, cancel, submit };

    // Autofocus input
    input.focus();
  }

  _key(e) {
    if (e.key === "Escape") {
      e.preventDefault();
      this.ctl.cancel();
      return;
    }
    if (e.key === "Enter") {
      e.preventDefault();
      this.ctl.submit();
      return;
    }
    if (e.key === "Tab") {
      if (!this.elements) return;
      const els = [this.elements.input, this.elements.cancel, this.elements.submit].filter((el) => el && !el.disabled);
      if (els.length === 0) {
        e.preventDefault();
        this.elements.box.focus();
        return;
      }
      const at = els.indexOf(e.target);
      const next = e.shiftKey ? (at <= 0 ? els.length - 1 : at - 1) : at === els.length - 1 ? 0 : at + 1;
      e.preventDefault();
      els[next].focus();
    }
  }

  render() {
    if (!this.ctl.isOpen) {
      this.close();
      return;
    }
    if (!this.elements) {
      this.open();
      return;
    }

    const { title, input, err, submit, cancel } = this.elements;
    const model = this.ctl.modelName || "Velora 73";
    title.textContent = this.tr("studio.pair.dialog_title").replace("%s", model);

    if (input.value !== this.ctl.code) {
      input.value = this.ctl.code;
    }

    err.textContent = this.ctl.error || "";
    err.hidden = !this.ctl.error;

    if (this.ctl.error) {
      input.classList.add("err");
    } else {
      input.classList.remove("err");
    }

    submit.disabled = this.ctl.code.length !== 6 || this.ctl.submitting || this.ctl.lockedOut;
    cancel.disabled = this.ctl.submitting;
  }
}
