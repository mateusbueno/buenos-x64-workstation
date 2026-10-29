// Unit tests for PairDialogController and PairDialogView (Story 9.6, UX-DR37, UX-DR46, UX-DR77, NFR16).
import test from "node:test";
import assert from "node:assert/strict";

import { CATALOGS } from "../src/catalogs.js";
import { PairDialogController } from "../src/pair_dialog.js";
import { PairDialogView } from "../src/pair_dialog_dom.js";

const tr = (k) => CATALOGS.en[k] || k;
const localeMgr = { tr };

class MockElement {
  constructor(doc, tag) {
    this.ownerDocument = doc;
    this.tagName = tag.toUpperCase();
    this.children = [];
    const classes = new Set();
    this.classList = {
      add: (c) => classes.add(c),
      remove: (c) => classes.delete(c),
      contains: (c) => classes.has(c),
      toString: () => Array.from(classes).join(" "),
      [Symbol.iterator]: () => classes.values(),
    };
    this.attrs = {};
    this.dataset = {};
    this.listeners = {};
    this.hidden = false;
    this.disabled = false;
    this.tabIndex = 0;
    this.value = "";
    this.type = "";
    this.id = "";
    this.parent = null;
    this._text = "";
  }

  get className() {
    return this.classList.toString();
  }

  set className(v) {
    for (const c of Array.from(this.classList)) this.classList.remove(c);
    if (v) v.split(/\s+/).filter(Boolean).forEach((c) => this.classList.add(c));
  }

  get textContent() {
    return this._text + this.children.map((c) => c.textContent).join("");
  }

  set textContent(v) {
    this.children.forEach((c) => { c.parent = null; });
    this.children = [];
    this._text = String(v);
  }

  appendChild(c) {
    c.parent = this;
    this.children.push(c);
    return c;
  }

  append(...items) {
    items.forEach((item) => this.appendChild(item));
  }

  remove() {
    if (this.parent) {
      const idx = this.parent.children.indexOf(this);
      if (idx >= 0) this.parent.children.splice(idx, 1);
      this.parent = null;
    }
  }

  setAttribute(k, v) {
    this.attrs[k] = String(v);
  }

  getAttribute(k) {
    return this.attrs[k] ?? null;
  }

  removeAttribute(k) {
    delete this.attrs[k];
  }

  addEventListener(type, fn) {
    (this.listeners[type] = this.listeners[type] || []).push(fn);
  }

  removeEventListener(type, fn) {
    if (!this.listeners[type]) return;
    this.listeners[type] = this.listeners[type].filter((f) => f !== fn);
  }

  dispatchEvent(evt) {
    const list = (this.listeners[evt.type] || []).slice();
    evt.target = evt.target || this;
    for (const fn of list) fn(evt);
  }

  focus() {
    this.ownerDocument.activeElement = this;
  }

  select() {}

  click() {
    this.dispatchEvent({ type: "click", target: this });
  }

  all(predicate) {
    const res = [];
    if (predicate(this)) res.push(this);
    for (const c of this.children) res.push(...c.all(predicate));
    return res;
  }
}

function makeDoc() {
  const doc = {
    createElement(tag) {
      return new MockElement(doc, tag);
    },
    activeElement: null,
  };
  doc.body = new MockElement(doc, "body");
  return doc;
}

test("PairDialogController starts in closed state and sanitizes code", () => {
  const ctl = new PairDialogController({
    pairFn: async () => {},
    getModelName: () => "Velora 73",
    localeMgr,
  });

  assert.equal(ctl.isOpen, false);
  assert.equal(ctl.code, "");
  assert.equal(ctl.error, null);
  assert.equal(ctl.submitting, false);
  assert.equal(ctl.lockedOut, false);
  assert.equal(ctl.modelName, "Velora 73");

  ctl.open();
  assert.equal(ctl.isOpen, true);

  ctl.setCode("12a34b5c678");
  assert.equal(ctl.code, "123456"); // only 6 numeric digits
});

test("PairDialogController successful pairing closes and calls onPaired", async () => {
  let pairedCalledWith = null;
  let onPairedCalled = false;

  const ctl = new PairDialogController({
    pairFn: async (code, name) => {
      pairedCalledWith = { code, name };
    },
    getModelName: () => "Velora 73",
    localeMgr,
    onPaired: () => {
      onPairedCalled = true;
    },
  });

  ctl.open();
  ctl.setCode("849201");
  await ctl.submit();

  assert.ok(pairedCalledWith);
  assert.equal(pairedCalledWith.code, "849201");
  assert.equal(ctl.isOpen, false);
  assert.equal(onPairedCalled, true);
  assert.equal(ctl.error, null);
});

test("PairDialogController handles wrong code error message", async () => {
  const ctl = new PairDialogController({
    pairFn: async () => {
      throw new Error("PAIRING_CODE_INVALID (attempts_left=4)");
    },
    getModelName: () => "Velora 73",
    localeMgr,
  });

  ctl.open();
  ctl.setCode("111111");
  await ctl.submit();

  assert.equal(ctl.isOpen, true);
  assert.equal(ctl.lockedOut, false);
  assert.equal(ctl.error, "That code doesn't match. Check the Velora screen.");
});

test("PairDialogController handles 5 wrong attempts lockout", async () => {
  const ctl = new PairDialogController({
    pairFn: async () => {
      throw new Error("PAIRING_CLOSED (reason=attempts)");
    },
    getModelName: () => "Velora 73",
    localeMgr,
  });

  ctl.open();
  ctl.setCode("999999");
  await ctl.submit();

  assert.equal(ctl.isOpen, true);
  assert.equal(ctl.lockedOut, true);
  assert.equal(
    ctl.error,
    "Pairing window closed after too many wrong codes. Start again on your Velora."
  );
});

test("PairDialogController handles window expired error", async () => {
  const ctl = new PairDialogController({
    pairFn: async () => {
      throw new Error("PAIRING_CLOSED (reason=expired)");
    },
    getModelName: () => "Velora 73",
    localeMgr,
  });

  ctl.open();
  ctl.setCode("123456");
  await ctl.submit();

  assert.equal(ctl.isOpen, true);
  assert.equal(ctl.lockedOut, false);
  assert.equal(ctl.error, "Pairing window expired. Start again on your Velora.");
});

test("PairDialogView builds modal dialog, autofocuses input and restores focus on close", () => {
  const doc = makeDoc();
  const prevButton = doc.createElement("button");
  doc.body.appendChild(prevButton);
  prevButton.focus();
  assert.equal(doc.activeElement, prevButton);

  const ctl = new PairDialogController({
    pairFn: async () => {},
    getModelName: () => "Velora 73",
    localeMgr,
  });

  const view = new PairDialogView({
    doc,
    host: doc.body,
    tr,
    controller: ctl,
  });

  ctl.open();
  // View automatically opened
  assert.ok(view.elements);
  assert.equal(
    view.elements.title.textContent,
    "Enter the code shown on Velora 73"
  );
  assert.equal(doc.activeElement, view.elements.input);
  assert.equal(view.elements.input.maxLength, 6);
  assert.equal(view.elements.input.inputMode, "numeric");
  assert.equal(view.elements.submit.disabled, true); // < 6 digits

  ctl.setCode("123456");
  assert.equal(view.elements.submit.disabled, false);

  ctl.close();
  assert.equal(view.elements, null);
  assert.equal(doc.activeElement, prevButton); // focus restored
});

test("PairDialogView focus trap and keyboard navigation (Tab, Shift+Tab, Esc, Enter)", async () => {
  const doc = makeDoc();
  let submitted = false;

  const ctl = new PairDialogController({
    pairFn: async () => {
      submitted = true;
    },
    getModelName: () => "Velora 73",
    localeMgr,
  });

  const view = new PairDialogView({
    doc,
    host: doc.body,
    tr,
    controller: ctl,
  });

  ctl.open();
  ctl.setCode("654321");

  const { box, input, cancel, submit } = view.elements;
  assert.equal(doc.activeElement, input);

  // Tab: input -> cancel
  box.dispatchEvent({ type: "keydown", key: "Tab", target: input, preventDefault() {} });
  assert.equal(doc.activeElement, cancel);

  // Tab: cancel -> submit
  box.dispatchEvent({ type: "keydown", key: "Tab", target: cancel, preventDefault() {} });
  assert.equal(doc.activeElement, submit);

  // Tab: submit -> input (cycle wrap)
  box.dispatchEvent({ type: "keydown", key: "Tab", target: submit, preventDefault() {} });
  assert.equal(doc.activeElement, input);

  // Shift+Tab: input -> submit (reverse cycle wrap)
  box.dispatchEvent({ type: "keydown", key: "Tab", shiftKey: true, target: input, preventDefault() {} });
  assert.equal(doc.activeElement, submit);

  // Enter triggers submit
  box.dispatchEvent({ type: "keydown", key: "Enter", target: input, preventDefault() {} });
  await Promise.resolve(); // wait tick
  assert.equal(submitted, true);
});
