// Velora Studio: Import dialog controller (Story 9.19, FR122, FR92, AR30, UX-DR77, UX-DR91, UX-DR114).
// Manages import manifest inspection, per-item collision merge decisions
// (Skip, Replace, Keep both), "Apply to all", missing library detection, and progress reporting.

export const COLLISION_ACTION = Object.freeze({
  KEEP_BOTH: "keep_both",
  REPLACE: "replace",
  SKIP: "skip",
});

export class ImportController {
  constructor({
    onImport = null,
    onCancel = null,
    onAnnounce = null,
    onChange = null,
    deviceName = "Velora 73",
    tr = (key) => key,
  } = {}) {
    this.onImport = onImport || (() => {});
    this.onCancel = onCancel || (() => {});
    this.onAnnounce = onAnnounce || (() => {});
    this.onChange = onChange || (() => {});
    this.deviceName = deviceName;
    this.tr = tr;

    this.isOpen = false;
    this.manifest = null;
    this.items = [];
    this.isImporting = false;
    this.progress = null; // { current, total, deviceName }
    this.errorBanner = null;
    this.missingLibrariesReport = null;
  }

  loadManifest(manifestResult) {
    if (!manifestResult) return;
    this.manifest = manifestResult;
    this.isOpen = true;
    this.isImporting = false;
    this.progress = null;
    this.errorBanner = null;

    if (manifestResult.format_version > 1) {
      const fname = manifestResult.file_name || "file";
      this.errorBanner = `Couldn't import ${fname}: file made by a newer Velora Studio. Update the app.`;
      this.items = [];
      this.missingLibrariesReport = null;
      this._notify();
      return;
    }

    this.items = (manifestResult.items || []).map((item) => ({
      id: item.id,
      name: item.name || item.id,
      category: item.category || "Item",
      item_type: item.item_type || "live_set",
      is_reference_only: !!item.is_reference_only,
      has_collision: !!item.has_collision,
      collision_type: item.collision_type || null,
      collision_action: item.has_collision ? COLLISION_ACTION.KEEP_BOTH : COLLISION_ACTION.KEEP_BOTH,
      library_id: item.library_id || "",
      instrument_key: item.instrument_key || "",
      missing_libraries: Array.isArray(item.missing_libraries) ? item.missing_libraries : [],
    }));

    this.missingLibrariesReport = manifestResult.missing_libraries_report || null;
    this._notify();
  }

  open(manifestResult) {
    this.loadManifest(manifestResult);
  }

  close() {
    if (!this.isOpen) return;
    if (this.isImporting) {
      this.cancelImport();
    }
    this.isOpen = false;
    this._notify();
  }

  setItemAction(itemId, action) {
    const item = this.items.find((i) => i.id === itemId);
    if (!item) return;
    if (Object.values(COLLISION_ACTION).includes(action)) {
      item.collision_action = action;
      this._notify();
    }
  }

  applyAllAction(action) {
    if (!Object.values(COLLISION_ACTION).includes(action)) return;
    for (const item of this.items) {
      if (item.has_collision) {
        item.collision_action = action;
      }
    }
    this._notify();
  }

  computeSummary() {
    const totalItems = this.items.length;
    const collisionItems = this.items.filter((i) => i.has_collision);
    const collisionCount = collisionItems.length;

    let keepBothCount = 0;
    let replaceCount = 0;
    let skipCount = 0;

    for (const item of this.items) {
      if (item.collision_action === COLLISION_ACTION.KEEP_BOTH) keepBothCount += 1;
      else if (item.collision_action === COLLISION_ACTION.REPLACE) replaceCount += 1;
      else if (item.collision_action === COLLISION_ACTION.SKIP) skipCount += 1;
    }

    const missingLibraries = this.manifest?.missing_libraries || [];
    const canImport = totalItems > 0 && !this.isImporting && !this.errorBanner;

    return {
      fileName: this.manifest?.file_name || "",
      totalItems,
      collisionCount,
      keepBothCount,
      replaceCount,
      skipCount,
      missingLibraries,
      missingLibrariesReport: this.missingLibrariesReport,
      errorBanner: this.errorBanner,
      canImport,
      isImporting: this.isImporting,
      progress: this.progress,
      items: this.items,
    };
  }

  startImport() {
    const summary = this.computeSummary();
    if (!summary.canImport) return false;

    this.isImporting = true;
    this.progress = { current: 0, total: this.items.length, deviceName: this.deviceName };
    this._notify();

    const decisions = this.items.map((i) => ({
      id: i.id,
      action: i.collision_action,
    }));

    try {
      this.onImport({
        filePath: this.manifest?.file_path || "",
        decisions,
      });
      return true;
    } catch (e) {
      this.isImporting = false;
      this.progress = null;
      this._notify();
      throw e;
    }
  }

  updateProgress(current, total) {
    if (!this.isImporting) return;
    this.progress = { current, total, deviceName: this.deviceName };
    this._notify();
  }

  cancelImport() {
    if (!this.isImporting && !this.isOpen) return;
    this.isImporting = false;
    this.progress = null;
    this.onCancel();
    const t = this.tr("studio.import.announced_cancelled");
    const msg = (t && t !== "studio.import.announced_cancelled") ? t : "Import cancelled";
    this.onAnnounce(msg);
    this._notify();
  }

  finishImport({ importedCount = 0, skippedCount = 0 } = {}) {
    this.isImporting = false;
    this.progress = null;
    this.isOpen = false;
    const t = this.tr("studio.import.announced_success");
    const tpl = (t && t !== "studio.import.announced_success") ? t : "Imported %d items · %d skipped";
    const msg = tpl.replace("%d", importedCount).replace("%d", skippedCount);
    this.onAnnounce(msg);
    this._notify();
  }

  handleConnectionLost({ importedCount = 0, totalCount = 0, rawMessage = null } = {}) {
    this.isImporting = false;
    this.progress = null;
    let msg = rawMessage;
    if (!msg) {
      const t = this.tr("studio.import.connection_lost");
      const tpl = (t && t !== "studio.import.connection_lost") ? t : "connection lost · %d of %d imported. Reconnect and import again; finished items are kept.";
      msg = tpl.replace("%d", importedCount).replace("%d", totalCount);
    }
    this.errorBanner = msg;
    this.onAnnounce(msg);
    this._notify();
  }

  _notify() {
    try {
      this.onChange(this.computeSummary());
    } catch (e) {
      // ignore
    }
  }
}
