// Velora Studio: Accessible Studio Import Dialog DOM View (Story 9.19, UX-DR77, UX-DR91, UX-DR114).
// Renders the modal dialog, per-item collision actions (Keep both, Replace, Skip),
// "Apply to all" header, missing library warning banner, footer progress, and polite aria-live announcements.
// Strictly avoids innerHTML for security and compatibility with DOM shims.

import { COLLISION_ACTION } from "./import_dialog.js";

function el(doc, tag, cls, text) {
  const e = doc.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined && text !== null) e.textContent = text;
  return e;
}

export class ImportDialogView {
  constructor({ doc, host, tr, controller, restore = null }) {
    this.doc = doc;
    this.host = host;
    this.tr = tr;
    this.ctl = controller;
    this.restore = restore;

    this.dialog = null;
    this.backdrop = null;
    this.liveRegion = null;
    this.focusedRowIndex = 0;
  }

  open() {
    this.render();
  }

  close() {
    this._teardown();
    this.restoreFocus();
  }

  _teardown() {
    if (this.host) {
      this.host.textContent = "";
    }
    this.dialog = null;
    this.backdrop = null;
    this.liveRegion = null;
  }

  restoreFocus() {
    const target = typeof this.restore === "function" ? this.restore() : this.restore;
    if (target && target.focus) {
      target.focus();
    }
  }

  render() {
    const c = this.ctl;
    if (!c.isOpen) {
      this._teardown();
      return;
    }

    if (!this.dialog) {
      this._build();
    }
    this._update();
  }

  _build() {
    const d = this.doc;
    const tr = this.tr;

    // Backdrop
    const backdrop = el(d, "div", "modal-backdrop active");
    backdrop.addEventListener("click", () => {
      if (!this.ctl.isImporting) {
        this.ctl.close();
        this.close();
      }
    });
    this.backdrop = backdrop;
    this.host.appendChild(backdrop);

    // Modal dialog
    const modal = el(d, "dialog", "modal import-modal");
    modal.setAttribute("role", "dialog");
    modal.setAttribute("aria-modal", "true");
    modal.setAttribute("aria-labelledby", "import-dialog-title");
    modal.tabIndex = -1;

    // Keydown listener for Esc and Tab focus trap
    modal.addEventListener("keydown", (e) => {
      if (e.key === "Escape") {
        e.preventDefault();
        if (!this.ctl.isImporting) {
          this.ctl.close();
          this.close();
        }
      } else if (e.key === "Tab") {
        const focusables = Array.from(modal.querySelectorAll(
          'button:not([disabled]):not([hidden]), [tabindex="0"]:not([disabled]):not([hidden]), input:not([disabled]):not([hidden])'
        ));
        if (focusables.length === 0) return;
        const first = focusables[0];
        const last = focusables[focusables.length - 1];
        if (e.shiftKey) {
          if (document.activeElement === first || document.activeElement === modal) {
            e.preventDefault();
            last.focus();
          }
        } else {
          if (document.activeElement === last) {
            e.preventDefault();
            first.focus();
          }
        }
      }
    });

    // Header
    const header = el(d, "div", "modal-header");
    const title = el(d, "h2", "modal-title", tr("studio.import.title"));
    title.id = "import-dialog-title";
    const closeBtn = el(d, "button", "btn", tr("studio.import.btn_cancel"));
    closeBtn.setAttribute("type", "button");
    closeBtn.setAttribute("aria-label", tr("studio.import.btn_cancel"));
    closeBtn.addEventListener("click", () => {
      if (!this.ctl.isImporting) {
        this.ctl.close();
        this.close();
      }
    });
    header.appendChild(title);
    header.appendChild(closeBtn);
    modal.appendChild(header);

    // Body
    const body = el(d, "div", "modal-body import-modal-body");

    // Error banner (for newer format version or fatal error)
    const errorBanner = el(d, "div", "import-banner banner-error", "");
    errorBanner.id = "import-error-banner";
    errorBanner.setAttribute("role", "alert");
    errorBanner.hidden = true;
    body.appendChild(errorBanner);

    // Missing library warning banner
    const missingLibBanner = el(d, "div", "import-banner banner-warning", "");
    missingLibBanner.id = "import-missing-libs-banner";
    missingLibBanner.hidden = true;
    body.appendChild(missingLibBanner);

    // Item Table Container
    const tableContainer = el(d, "div", "import-table-container");
    const table = el(d, "div", "import-table");
    table.setAttribute("role", "table");
    table.setAttribute("aria-label", "Import Items");

    // Table Header
    const thead = el(d, "div", "import-thead");
    thead.setAttribute("role", "row");

    const thName = el(d, "div", "import-th col-name", tr("studio.import.col_item"));
    thName.setAttribute("role", "columnheader");
    const thCat = el(d, "div", "import-th col-cat", tr("studio.import.col_category"));
    thCat.setAttribute("role", "columnheader");
    const thCol = el(d, "div", "import-th col-collision", tr("studio.import.col_collision"));
    thCol.setAttribute("role", "columnheader");

    const thAction = el(d, "div", "import-th col-action");
    thAction.setAttribute("role", "columnheader");

    const actionHeaderWrap = el(d, "div", "action-header-wrap");
    const actionTitle = el(d, "span", "action-header-title", tr("studio.import.col_action"));
    actionHeaderWrap.appendChild(actionTitle);

    // Apply to all dropdown / buttons
    const applyAllGroup = el(d, "div", "apply-all-group");
    const applyAllLabel = el(d, "span", "apply-all-label", tr("studio.import.apply_all"));
    applyAllGroup.appendChild(applyAllLabel);

    const applyKeepBoth = el(d, "button", "btn btn-sm btn-apply-all", tr("studio.import.action_keep_both"));
    applyKeepBoth.setAttribute("type", "button");
    applyKeepBoth.addEventListener("click", () => {
      this.ctl.applyAllAction(COLLISION_ACTION.KEEP_BOTH);
      this._update();
    });

    const applyReplace = el(d, "button", "btn btn-sm btn-apply-all", tr("studio.import.action_replace"));
    applyReplace.setAttribute("type", "button");
    applyReplace.addEventListener("click", () => {
      this.ctl.applyAllAction(COLLISION_ACTION.REPLACE);
      this._update();
    });

    const applySkip = el(d, "button", "btn btn-sm btn-apply-all", tr("studio.import.action_skip"));
    applySkip.setAttribute("type", "button");
    applySkip.addEventListener("click", () => {
      this.ctl.applyAllAction(COLLISION_ACTION.SKIP);
      this._update();
    });

    applyAllGroup.appendChild(applyKeepBoth);
    applyAllGroup.appendChild(applyReplace);
    applyAllGroup.appendChild(applySkip);
    actionHeaderWrap.appendChild(applyAllGroup);
    thAction.appendChild(actionHeaderWrap);

    thead.appendChild(thName);
    thead.appendChild(thCat);
    thead.appendChild(thCol);
    thead.appendChild(thAction);
    table.appendChild(thead);

    // Table Body / Rows
    const tbody = el(d, "div", "import-tbody");
    table.appendChild(tbody);
    tableContainer.appendChild(table);
    body.appendChild(tableContainer);

    modal.appendChild(body);

    // Footer
    const footer = el(d, "div", "modal-footer import-footer");

    // Footer progress line (for running state)
    const progressEl = el(d, "div", "import-progress-text", "");
    progressEl.id = "import-progress-status";
    progressEl.hidden = true;
    footer.appendChild(progressEl);

    const cancelBtn = el(d, "button", "btn btn-secondary", tr("studio.import.btn_cancel"));
    cancelBtn.id = "btn-cancel-import";
    cancelBtn.setAttribute("type", "button");
    cancelBtn.addEventListener("click", () => {
      if (this.ctl.isImporting) {
        this.ctl.cancelImport();
      } else {
        this.ctl.close();
      }
      this.close();
    });

    const importBtn = el(d, "button", "btn btn-primary", tr("studio.import.btn_import"));
    importBtn.id = "btn-confirm-import";
    importBtn.setAttribute("type", "button");
    importBtn.addEventListener("click", () => {
      this.ctl.startImport();
      this._update();
    });

    footer.appendChild(cancelBtn);
    footer.appendChild(importBtn);
    modal.appendChild(footer);

    // Polite live region for accessibility announcements
    const live = el(d, "div", "sr-only");
    live.setAttribute("aria-live", "polite");
    live.setAttribute("aria-atomic", "true");
    modal.appendChild(live);
    this.liveRegion = live;

    this.dialog = {
      modal,
      errorBanner,
      missingLibBanner,
      tbody,
      applyAllGroup,
      progressEl,
      cancelBtn,
      importBtn,
    };

    this.host.appendChild(modal);
    modal.focus();
  }

  _update() {
    if (!this.dialog) return;

    const d = this.doc;
    const tr = this.tr;
    const summary = this.ctl.computeSummary();

    // Error banner
    if (summary.errorBanner) {
      this.dialog.errorBanner.textContent = summary.errorBanner;
      this.dialog.errorBanner.hidden = false;
    } else {
      this.dialog.errorBanner.textContent = "";
      this.dialog.errorBanner.hidden = true;
    }

    // Missing libraries banner
    if (summary.missingLibraries && summary.missingLibraries.length > 0) {
      const liveSetsCount = summary.items ? summary.items.filter((it) => it.missing_libraries && it.missing_libraries.length > 0).length : 1;
      const libsStr = summary.missingLibraries.join(", ");
      const tpl = tr("studio.import.missing_libraries_summary") || "%d Live Sets need libraries: %s (factory)";
      this.dialog.missingLibBanner.textContent = tpl.replace("%d", liveSetsCount).replace("%s", libsStr);
      this.dialog.missingLibBanner.hidden = false;
    } else if (summary.missingLibrariesReport) {
      this.dialog.missingLibBanner.textContent = summary.missingLibrariesReport;
      this.dialog.missingLibBanner.hidden = false;
    } else {
      this.dialog.missingLibBanner.textContent = "";
      this.dialog.missingLibBanner.hidden = true;
    }

    // Table rows
    this.dialog.tbody.textContent = "";
    const items = summary.items || [];

    items.forEach((item, index) => {
      const row = el(d, "div", "import-row");
      row.setAttribute("role", "row");
      row.tabIndex = index === this.focusedRowIndex ? 0 : -1;
      row.dataset.itemId = item.id;

      // Keyboard navigation (roving tabindex)
      row.addEventListener("keydown", (e) => {
        if (e.key === "ArrowDown") {
          e.preventDefault();
          this._focusRow(Math.min(index + 1, items.length - 1));
        } else if (e.key === "ArrowUp") {
          e.preventDefault();
          this._focusRow(Math.max(index - 1, 0));
        }
      });

      // Name cell
      const cellName = el(d, "div", "import-cell col-name");
      cellName.setAttribute("role", "cell");
      const nameText = el(d, "span", "item-name", item.name);
      cellName.appendChild(nameText);

      if (item.missing_libraries && item.missing_libraries.length > 0) {
        const missBadge = el(d, "span", "badge badge-warn", tr("studio.import.missing_library_tag"));
        cellName.appendChild(missBadge);
      }

      // Category cell
      const cellCat = el(d, "div", "import-cell col-cat", item.category);
      cellCat.setAttribute("role", "cell");

      // Collision cell
      const cellCol = el(d, "div", "import-cell col-collision");
      cellCol.setAttribute("role", "cell");
      if (item.has_collision) {
        const colBadge = el(d, "span", "badge badge-collision", tr("studio.import.badge_collision"));
        cellCol.appendChild(colBadge);
      }

      // Action cell
      const cellAction = el(d, "div", "import-cell col-action");
      cellAction.setAttribute("role", "cell");

      if (item.has_collision) {
        const radioGroup = el(d, "div", "collision-actions-group");
        radioGroup.setAttribute("role", "radiogroup");
        radioGroup.setAttribute("aria-label", `Merge action for ${item.name}`);

        const actions = [
          { key: COLLISION_ACTION.KEEP_BOTH, label: tr("studio.import.action_keep_both") },
          { key: COLLISION_ACTION.REPLACE, label: tr("studio.import.action_replace") },
          { key: COLLISION_ACTION.SKIP, label: tr("studio.import.action_skip") },
        ];

        for (const act of actions) {
          const actBtn = el(d, "button", "action-choice-btn", act.label);
          actBtn.setAttribute("type", "button");
          actBtn.setAttribute("role", "radio");
          const isSelected = item.collision_action === act.key;
          actBtn.setAttribute("aria-checked", isSelected ? "true" : "false");
          if (isSelected) {
            actBtn.classList.add("selected");
          }
          actBtn.disabled = this.ctl.isImporting;
          actBtn.addEventListener("click", () => {
            this.ctl.setItemAction(item.id, act.key);
            this._update();
          });
          radioGroup.appendChild(actBtn);
        }
        cellAction.appendChild(radioGroup);
      }

      row.appendChild(cellName);
      row.appendChild(cellCat);
      row.appendChild(cellCol);
      row.appendChild(cellAction);

      this.dialog.tbody.appendChild(row);
    });

    // Buttons and Progress
    this.dialog.importBtn.disabled = !summary.canImport;
    this.dialog.cancelBtn.disabled = false;

    if (summary.isImporting && summary.progress) {
      const p = summary.progress;
      let runningStr = tr("studio.import.running_footer");
      runningStr = runningStr
        .replace("%s", p.deviceName || "VELORA 73")
        .replace("%d", String(p.current || 0))
        .replace("%d", String(p.total || items.length));
      this.dialog.progressEl.textContent = runningStr;
      this.dialog.progressEl.hidden = false;
      this.dialog.importBtn.disabled = true;
    } else {
      this.dialog.progressEl.hidden = true;
    }
  }

  _focusRow(index) {
    if (!this.dialog) return;
    const rows = this.dialog.tbody.children;
    if (rows && rows[index]) {
      this.focusedRowIndex = index;
      for (let i = 0; i < rows.length; i++) {
        rows[i].tabIndex = i === index ? 0 : -1;
      }
      rows[index].focus();
    }
  }
}
