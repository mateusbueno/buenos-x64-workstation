// Velora Studio — Connection Banner Controller & View (Story 9.7, FR99, UX-DR47, UX-DR77, UX-DR91, AR25).
// Non-modal notification strip at top of main area for disconnects and version mismatches.

export class ConnectionBannerController {
  /**
   * @param {Object} options
   * @param {() => Promise<void> | void} [options.reconnectFn]
   * @param {() => void} [options.updateFirmwareFn]
   * @param {() => string} [options.getModelName]
   * @param {{ tr: (key: string) => string }} options.localeMgr
   * @param {(msg: string, isWarning?: boolean) => void} [options.announceFn]
   */
  constructor({ reconnectFn, updateFirmwareFn, getModelName, localeMgr, announceFn } = {}) {
    this._reconnectFn = reconnectFn;
    this._updateFirmwareFn = updateFirmwareFn;
    this._getModelName = getModelName || (() => "Velora 73");
    this._localeMgr = localeMgr;
    this._announceFn = announceFn;
    this._status = "disconnected";
    this._configured = false;
    this._wasPaired = false;
    this._wasDisconnected = false;
    this._model = "";
    this._firmwareVersion = "";
    this._requiredVersion = "1.3";
    this._listeners = new Set();
  }

  get status() { return this._status; }
  get configured() { return this._configured; }
  get modelName() {
    return (this._getModelName ? this._getModelName() : "") || this._model || "Velora 73";
  }
  get isBannerVisible() {
    if (!this._configured) return false;
    if (this._status === "disconnected" || this._status === "version-mismatch") return true;
    if (this._status === "connecting" && (this._wasDisconnected || this._wasPaired)) return true;
    return false;
  }

  subscribe(fn) {
    this._listeners.add(fn);
    return () => this._listeners.delete(fn);
  }

  _notify() {
    for (const fn of this._listeners) {
      try { fn(this); } catch (_) {}
    }
  }

  setStatus(st) {
    if (!st) return;
    const prevStatus = this._status;
    const prevPaired = this._wasPaired;
    this._configured = !!st.configured;
    this._status = st.status || "disconnected";
    if (st.model) {
      this._model = st.model;
    }
    if (st.firmware_version) {
      this._firmwareVersion = st.firmware_version;
    } else if (st.device_version) {
      this._firmwareVersion = st.device_version;
    }

    if (this._status === "paired") {
      this._wasPaired = true;
      this._wasDisconnected = false;
      if (prevStatus === "disconnected" || prevStatus === "connecting") {
        if (this._announceFn) {
          this._announceFn(this._tr("studio.conn.connected"), false);
        }
      }
    } else if (this._status === "disconnected") {
      this._wasDisconnected = true;
      if (prevPaired && prevStatus !== "disconnected") {
        const msg = this._tr("studio.conn.disconnected").replace("%s", this.modelName);
        if (this._announceFn) {
          this._announceFn(msg, true);
        }
      }
    }

    this._notify();
  }

  async reconnect() {
    if (this._reconnectFn) {
      try {
        await this._reconnectFn();
      } catch (_) {}
    }
  }

  updateFirmware() {
    if (this._updateFirmwareFn) {
      try {
        this._updateFirmwareFn();
      } catch (_) {}
    }
  }

  _tr(key) {
    if (this._localeMgr && typeof this._localeMgr.tr === "function") {
      return this._localeMgr.tr(key);
    }
    return key;
  }
}

export class ConnectionBannerView {
  /**
   * @param {Object} options
   * @param {Document} options.doc
   * @param {HTMLElement} options.el
   * @param {ConnectionBannerController} options.controller
   * @param {(key: string) => string} options.tr
   */
  constructor({ doc, el, controller, tr }) {
    this.doc = doc;
    this.el = el;
    this.ctl = controller;
    this.tr = tr;
    this.elements = null;
    this._unsub = this.ctl.subscribe(() => this.render());
    this._build();
    this.render();
  }

  destroy() {
    if (this._unsub) {
      this._unsub();
      this._unsub = null;
    }
    if (this.el) {
      this.el.hidden = true;
      this.el.replaceChildren();
    }
  }

  _build() {
    if (!this.el) return;
    this.el.replaceChildren();

    const msgBox = this.doc.createElement("div");
    msgBox.className = "conn-banner-msg";

    const icon = this.doc.createElement("span");
    icon.className = "conn-banner-icon";
    icon.setAttribute("aria-hidden", "true");
    icon.textContent = "⚠";

    const text = this.doc.createElement("span");
    text.className = "conn-banner-text";

    msgBox.append(icon, text);

    const action = this.doc.createElement("button");
    action.type = "button";
    action.className = "btn conn-banner-action";

    action.addEventListener("click", () => {
      if (this.ctl.status === "disconnected") {
        action.disabled = true;
        action.textContent = this.tr("studio.conn.connecting");
        this.ctl.reconnect().finally(() => {
          action.disabled = false;
          action.textContent = this.tr("studio.conn.reconnect");
        });
      } else if (this.ctl.status === "version-mismatch") {
        this.ctl.updateFirmware();
      }
    });

    this.el.append(msgBox, action);
    this.elements = { msgBox, icon, text, action };
  }

  render() {
    if (!this.el || !this.elements) return;

    if (!this.ctl.isBannerVisible) {
      this.el.hidden = true;
      return;
    }

    const { text, action } = this.elements;
    const model = this.ctl.modelName || "Velora 73";

    if (this.ctl.status === "disconnected" || this.ctl.status === "connecting") {
      this.el.hidden = false;
      const msg = this.tr("studio.banner.disconnect_msg").replace("%s", model);
      text.textContent = msg;
      action.hidden = false;
      if (this.ctl.status === "connecting") {
        action.disabled = true;
        action.textContent = this.tr("studio.conn.connecting");
      } else {
        action.disabled = false;
        action.textContent = this.tr("studio.conn.reconnect");
      }
    } else if (this.ctl.status === "version-mismatch") {
      this.el.hidden = false;
      const devVer = this.ctl._firmwareVersion || "1.2";
      const appVer = this.ctl._requiredVersion || "1.3";
      const msg = this.tr("studio.banner.version_mismatch_msg")
        .replace("%s", model)
        .replace("%s", devVer)
        .replace("%s", appVer);
      text.textContent = msg;
      action.hidden = false;
      action.disabled = false;
      action.textContent = this.tr("studio.banner.update_firmware");
    } else {
      this.el.hidden = true;
    }
  }
}
