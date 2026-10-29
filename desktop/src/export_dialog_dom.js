// Velora Studio: Accessible Studio Export Dialog DOM View (Story 9.18, UX-DR45, UX-DR46, UX-DR71, UX-DR91, UX-DR114).
// Renders the modal dialog, scope switch, item selection table with roving tabindex,
// locked dependency indicators, live size estimation, warnings, and polite aria-live announcements.
// Strictly avoids innerHTML for security and compatibility with DOM shims.

import { EXPORT_SCOPE, formatBytes, isFactoryTimbre } from "./export_dialog.js";

function el(doc, tag, cls, text) {
  const e = doc.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

export class ExportDialogView {
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
      if (!this.ctl.isExporting) {
        this.ctl.close();
        this.close();
      }
    });
    this.backdrop = backdrop;
    this.host.appendChild(backdrop);

    // Modal dialog
    const modal = el(d, "dialog", "modal export-modal");
    modal.setAttribute("role", "dialog");
    modal.setAttribute("aria-modal", "true");
    modal.setAttribute("aria-labelledby", "export-dialog-title");
    modal.tabIndex = -1;

    // Keydown listener for Esc and Space
    modal.addEventListener("keydown", (e) => {
      if (e.key === "Escape") {
        e.preventDefault();
        if (!this.ctl.isExporting) {
          this.ctl.close();
          this.close();
        }
      }
    });

    // Header
    const header = el(d, "div", "modal-header");
    const title = el(d, "h2", "modal-title", tr("studio.export.title"));
    title.id = "export-dialog-title";
    const closeBtn = el(d, "button", "btn", tr("studio.export.btn_cancel"));
    closeBtn.setAttribute("type", "button");
    closeBtn.setAttribute("aria-label", tr("studio.export.btn_cancel"));
    closeBtn.addEventListener("click", () => {
      if (!this.ctl.isExporting) {
        this.ctl.close();
        this.close();
      }
    });
    header.appendChild(title);
    header.appendChild(closeBtn);
    modal.appendChild(header);

    // Body
    const body = el(d, "div", "modal-body export-modal-body");

    // Scope selection (Tabs / RadioGroup)
    const scopeGroup = el(d, "div", "export-scopes", undefined);
    scopeGroup.setAttribute("role", "radiogroup");
    scopeGroup.setAttribute("aria-label", "Export Scope");

    const scopes = [
      { id: EXPORT_SCOPE.BUNDLE, label: tr("studio.export.scope_bundle") },
      { id: EXPORT_SCOPE.SETS, label: tr("studio.export.scope_sets") },
      { id: EXPORT_SCOPE.TIMBRES, label: tr("studio.export.scope_timbres") },
    ];

    for (const sc of scopes) {
      const btn = el(d, "button", "scope-button", sc.label);
      btn.setAttribute("type", "button");
      btn.setAttribute("role", "radio");
      btn.dataset.scope = sc.id;
      btn.addEventListener("click", () => {
        if (!this.ctl.isExporting) {
          this.ctl.setScope(sc.id);
          this._update();
        }
      });
      scopeGroup.appendChild(btn);
    }
    body.appendChild(scopeGroup);

    // Item Table Container
    const tableContainer = el(d, "div", "export-table-container");
    const table = el(d, "div", "export-table");
    table.setAttribute("role", "table");
    table.setAttribute("aria-label", "Export Items");

    // Table Header
    const thead = el(d, "div", "export-thead");
    thead.setAttribute("role", "row");

    const thSelect = el(d, "div", "export-th col-select");
    thSelect.setAttribute("role", "columnheader");
    const selectAllCheckbox = el(d, "input", "export-select-all");
    selectAllCheckbox.setAttribute("type", "checkbox");
    selectAllCheckbox.setAttribute("aria-label", tr("studio.export.select_all"));
    selectAllCheckbox.addEventListener("change", (e) => {
      this.ctl.toggleSelectAll(e.target.checked);
      this._update();
    });
    thSelect.appendChild(selectAllCheckbox);

    const thName = el(d, "div", "export-th col-name", tr("studio.export.col_name"));
    thName.setAttribute("role", "columnheader");
    const thCat = el(d, "div", "export-th col-cat", tr("studio.export.col_category"));
    thCat.setAttribute("role", "columnheader");
    const thSize = el(d, "div", "export-th col-size", tr("studio.export.col_size"));
    thSize.setAttribute("role", "columnheader");

    thead.appendChild(thSelect);
    thead.appendChild(thName);
    thead.appendChild(thCat);
    thead.appendChild(thSize);
    table.appendChild(thead);

    // Table Body / Rows
    const tbody = el(d, "div", "export-tbody");
    table.appendChild(tbody);
    tableContainer.appendChild(table);
    body.appendChild(tableContainer);

    // Live Estimation Summary Line
    const estimateLine = el(d, "div", "export-estimate-line", "");
    estimateLine.id = "export-estimate-summary";
    body.appendChild(estimateLine);

    // Warning Lines
    const warn2gb = el(d, "div", "export-warning warn-2gb", "");
    warn2gb.hidden = true;
    body.appendChild(warn2gb);

    const warnSpace = el(d, "div", "export-warning warn-space", "");
    warnSpace.hidden = true;
    body.appendChild(warnSpace);

    modal.appendChild(body);

    // Footer
    const footer = el(d, "div", "modal-footer export-footer");

    // Footer progress line (for running state)
    const progressEl = el(d, "div", "export-progress-text", "");
    progressEl.hidden = true;
    footer.appendChild(progressEl);

    const cancelBtn = el(d, "button", "btn btn-secondary", tr("studio.export.btn_cancel"));
    cancelBtn.id = "btn-cancel-export";
    cancelBtn.setAttribute("type", "button");
    cancelBtn.addEventListener("click", () => {
      if (this.ctl.isExporting) {
        this.ctl.cancelExport();
      } else {
        this.ctl.close();
      }
      this.close();
    });

    const exportBtn = el(d, "button", "btn btn-primary", tr("studio.export.btn_export"));
    exportBtn.id = "btn-confirm-export";
    exportBtn.setAttribute("type", "button");
    exportBtn.addEventListener("click", () => {
      this.ctl.startExport();
      this._update();
    });

    footer.appendChild(cancelBtn);
    footer.appendChild(exportBtn);
    modal.appendChild(footer);

    // Polite live region for accessibility announcements
    const live = el(d, "div", "sr-only");
    live.setAttribute("aria-live", "polite");
    live.setAttribute("aria-atomic", "true");
    modal.appendChild(live);
    this.liveRegion = live;

    this.dialog = {
      modal,
      scopeGroup,
      selectAllCheckbox,
      tbody,
      estimateLine,
      warn2gb,
      warnSpace,
      progressEl,
      cancelBtn,
      exportBtn,
      closeBtn,
    };

    this.host.appendChild(modal);

    // Focus initial element
    modal.focus();
  }

  _update() {
    if (!this.dialog) return;

    const d = this.doc;
    const tr = this.tr;
    const ctl = this.ctl;
    const est = ctl.computeEstimate();
    const dg = this.dialog;

    // Set aria-busy
    dg.modal.setAttribute("aria-busy", ctl.isExporting ? "true" : "false");

    // Scope buttons active state
    const scopeBtns = dg.scopeGroup.children || [];
    for (const btn of scopeBtns) {
      const active = btn.dataset && btn.dataset.scope === ctl.scope;
      btn.className = active ? "scope-button active" : "scope-button";
      btn.setAttribute("aria-checked", active ? "true" : "false");
    }

    // Select-all checkbox state
    dg.selectAllCheckbox.checked = est.allChecked;
    dg.selectAllCheckbox.disabled = ctl.isExporting;

    // Render Table Rows
    const tbody = dg.tbody;
    tbody.textContent = "";

    const items = this._getItemsToRender();
    items.forEach((item, index) => {
      const row = el(d, "div", "export-row");
      row.setAttribute("role", "row");
      row.tabIndex = index === this.focusedRowIndex ? 0 : -1;
      row.dataset.index = String(index);

      // Select / Checkbox column
      const tdSelect = el(d, "div", "export-td col-select");
      tdSelect.setAttribute("role", "cell");
      const cb = el(d, "input", "export-row-checkbox");
      cb.setAttribute("type", "checkbox");
      cb.checked = item.isChecked;
      cb.disabled = item.isLocked || ctl.isExporting;

      cb.addEventListener("change", () => {
        if (!item.isLocked && !ctl.isExporting) {
          if (item.type === "live_set") {
            ctl.toggleLiveSet(item.id);
          } else {
            ctl.toggleTimbre(item.id);
          }
          this._update();
        }
      });
      tdSelect.appendChild(cb);

      // Name column
      const tdName = el(d, "div", "export-td col-name");
      tdName.setAttribute("role", "cell");
      const nameText = el(d, "span", "export-item-name", item.name);
      tdName.appendChild(nameText);

      if (item.isLocked) {
        const lockBadge = el(d, "span", "badge badge-locked", " " + tr("studio.export.reference_only"));
        tdName.appendChild(lockBadge);
      }

      // Category column
      const tdCat = el(d, "div", "export-td col-cat", item.category);
      tdCat.setAttribute("role", "cell");

      // Size column
      const sizeStr = item.isReferenceOnly ? "0 B" : formatBytes(item.sizeBytes);
      const tdSize = el(d, "div", "export-td col-size", sizeStr);
      tdSize.setAttribute("role", "cell");

      row.appendChild(tdSelect);
      row.appendChild(tdName);
      row.appendChild(tdCat);
      row.appendChild(tdSize);

      // Keyboard navigation for roving tabindex & Space selection
      row.addEventListener("keydown", (e) => {
        if (ctl.isExporting) return;
        if (e.key === " " || e.key === "Spacebar") {
          e.preventDefault();
          if (!item.isLocked) {
            if (item.type === "live_set") {
              ctl.toggleLiveSet(item.id);
            } else {
              ctl.toggleTimbre(item.id);
            }
            this._update();
          }
        } else if (e.key === "ArrowDown") {
          e.preventDefault();
          this._moveFocus(1);
        } else if (e.key === "ArrowUp") {
          e.preventDefault();
          this._moveFocus(-1);
        }
      });

      row.addEventListener("focus", () => {
        this.focusedRowIndex = index;
      });

      tbody.appendChild(row);
    });

    // Live Estimate Summary
    dg.estimateLine.textContent = est.summaryText;

    // Warnings
    if (est.exceeds2GB) {
      dg.warn2gb.hidden = false;
      dg.warn2gb.textContent = tr("studio.export.warn_size_2gb", est.formattedSize);
    } else {
      dg.warn2gb.hidden = true;
    }

    if (est.insufficientFreeSpace) {
      dg.warnSpace.hidden = false;
      const freeStr = formatBytes(ctl.freeSpaceBytes || 0);
      dg.warnSpace.textContent = tr("studio.export.warn_insufficient_space", freeStr, est.formattedSize);
    } else {
      dg.warnSpace.hidden = true;
    }

    // Export button state
    dg.exportBtn.disabled = !est.canExport;
    dg.cancelBtn.disabled = false;

    // Running Progress state
    if (ctl.isExporting) {
      dg.progressEl.hidden = false;
      if (ctl.progress) {
        const cur = formatBytes(ctl.progress.currentBytes);
        const tot = formatBytes(ctl.progress.totalBytes);
        dg.progressEl.textContent = tr("studio.export.running_footer", cur, tot);
      } else {
        dg.progressEl.textContent = tr("studio.export.running_footer", "0 B", est.formattedSize);
      }
    } else {
      dg.progressEl.hidden = true;
    }
  }

  _getItemsToRender() {
    const ctl = this.ctl;
    const items = [];

    if (ctl.scope === EXPORT_SCOPE.SETS || ctl.scope === EXPORT_SCOPE.BUNDLE) {
      for (const ls of ctl.liveSets) {
        const checked = ctl.selectedLiveSetIds.has(ls.id);
        items.push({
          id: ls.id,
          name: ls.name || "Live Set",
          category: "Live Set",
          sizeBytes: 1024,
          isChecked: checked,
          isLocked: false,
          isReferenceOnly: false,
          type: "live_set",
        });
      }
    }

    if (ctl.scope === EXPORT_SCOPE.BUNDLE) {
      for (const key of ctl.lockedTimbreKeys) {
        const t = ctl.getTimbreByKey(key);
        const isFact = isFactoryTimbre(t.library_id);
        items.push({
          id: key,
          name: t.name || t.instrument_key,
          category: isFact ? "Factory" : "User Timbre",
          sizeBytes: isFact ? 0 : t.size_bytes || 25 * 1024 * 1024,
          isChecked: true,
          isLocked: true,
          isReferenceOnly: isFact,
          type: "timbre",
        });
      }
    } else if (ctl.scope === EXPORT_SCOPE.TIMBRES) {
      for (const t of ctl.timbres) {
        if (!isFactoryTimbre(t.library_id)) {
          const key = `${t.library_id}:${t.instrument_key}`;
          const checked = ctl.selectedTimbreKeys.has(key);
          items.push({
            id: key,
            name: t.name || t.instrument_key,
            category: t.category || "User Timbre",
            sizeBytes: t.size_bytes || 25 * 1024 * 1024,
            isChecked: checked,
            isLocked: false,
            isReferenceOnly: false,
            type: "timbre",
          });
        }
      }
    }

    return items;
  }

  _moveFocus(delta) {
    const rows = this.dialog && this.dialog.tbody ? this.dialog.tbody.children : [];
    if (!rows || rows.length === 0) return;

    this.focusedRowIndex = (this.focusedRowIndex + delta + rows.length) % rows.length;
    for (let i = 0; i < rows.length; i++) {
      rows[i].tabIndex = i === this.focusedRowIndex ? 0 : -1;
    }
    if (rows[this.focusedRowIndex] && rows[this.focusedRowIndex].focus) {
      rows[this.focusedRowIndex].focus();
    }
  }

  announce(text) {
    if (this.liveRegion) {
      this.liveRegion.textContent = text;
    }
  }
}
