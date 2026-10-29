// Velora Studio — Pairing Dialog Controller (Story 9.6, UX-DR37, UX-DR46, UX-DR77, NFR16).
// Coordinates pairing code entry, submission, timeout/lockout error handling, and state notification.

export class PairDialogController {
  /**
   * @param {Object} options
   * @param {(code: string, name: string) => Promise<void>} options.pairFn
   * @param {() => string} [options.getModelName]
   * @param {{ tr: (key: string) => string }} [options.localeMgr]
   * @param {() => void} [options.onPaired]
   * @param {() => void} [options.onCancel]
   */
  constructor({ pairFn, getModelName, localeMgr, onPaired, onCancel } = {}) {
    this._pairFn = pairFn;
    this._getModelName = getModelName || (() => "Velora 73");
    this._localeMgr = localeMgr;
    this._onPaired = onPaired;
    this._onCancel = onCancel;
    this._isOpen = false;
    this._code = "";
    this._error = null;
    this._submitting = false;
    this._lockedOut = false;
    this._listeners = new Set();
  }

  get isOpen() { return this._isOpen; }
  get code() { return this._code; }
  get error() { return this._error; }
  get submitting() { return this._submitting; }
  get lockedOut() { return this._lockedOut; }
  get modelName() { return this._getModelName ? this._getModelName() : "Velora 73"; }

  subscribe(fn) {
    this._listeners.add(fn);
    return () => this._listeners.delete(fn);
  }

  _notify() {
    for (const fn of this._listeners) {
      try { fn(this); } catch (_) {}
    }
  }

  open() {
    this._isOpen = true;
    this._code = "";
    this._error = null;
    this._submitting = false;
    this._lockedOut = false;
    this._notify();
  }

  close() {
    if (!this._isOpen) return;
    this._isOpen = false;
    this._notify();
    if (this._onCancel) this._onCancel();
  }

  setCode(code) {
    const clean = String(code || "").replace(/\D/g, "").slice(0, 6);
    this._code = clean;
    this._error = null;
    this._notify();
  }

  cancel() {
    this.close();
  }

  async submit() {
    if (!this._isOpen || this._submitting || this._lockedOut) return;
    if (this._code.length !== 6) return;

    this._submitting = true;
    this._error = null;
    this._notify();

    const name = (typeof navigator !== "undefined" && (navigator.platform || navigator.userAgent) || "Computer").slice(0, 60);

    try {
      if (this._pairFn) {
        await this._pairFn(this._code, name);
      }
      this._submitting = false;
      this._isOpen = false;
      this._notify();
      if (this._onPaired) this._onPaired();
    } catch (err) {
      this._submitting = false;
      const errStr = String(err || "");
      if (errStr.includes("reason=attempts") || errStr.includes("too many wrong codes")) {
        this._lockedOut = true;
        this._error = this._tr("studio.pair.locked_out");
      } else if (errStr.includes("expired") || errStr.includes("not_open")) {
        this._error = this._tr("studio.pair.expired");
      } else if (errStr.includes("PAIRING_CODE_INVALID") || errStr.includes("wrong pairing code") || errStr.includes("wrong")) {
        this._error = this._tr("studio.pair.wrong_code");
      } else {
        this._error = this._tr("studio.pair.failed").replace("%s", errStr);
      }
      this._notify();
    }
  }

  _tr(key) {
    if (this._localeMgr && typeof this._localeMgr.tr === "function") {
      return this._localeMgr.tr(key);
    }
    return key;
  }
}
