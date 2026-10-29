// Velora Studio: Export dialog controller (Story 9.18, AD-10, AD-14, FR122, AR29, AR30).
// Manages export scopes (Live Sets, Timbres, Bundle), bundle dependency locking,
// live size estimation, 2 GB warnings, disk free space validation, and aria-live announcements.

export const EXPORT_SCOPE = Object.freeze({
  SETS: "sets",
  TIMBRES: "timbres",
  BUNDLE: "bundle",
});

export const TWO_GIGABYTES = 2 * 1024 * 1024 * 1024;
export const DEFAULT_USER_TIMBRE_SIZE = 25 * 1024 * 1024; // ~25 MB default for user-imported timbres if unknown
export const ESTIMATED_LIVE_SET_SIZE = 1024; // ~1 KB per Live Set protobuf message

export function formatBytes(bytes) {
  const b = typeof bytes === "number" && Number.isFinite(bytes) && bytes > 0 ? bytes : 0;
  if (b >= 1024 * 1024 * 1024) {
    return `${(b / (1024 * 1024 * 1024)).toFixed(1)} GB`;
  }
  if (b >= 1024 * 1024) {
    return `${(b / (1024 * 1024)).toFixed(1)} MB`;
  }
  if (b >= 1024) {
    return `${(b / 1024).toFixed(1)} KB`;
  }
  return `${b} B`;
}

export function isFactoryTimbre(libraryId) {
  if (!libraryId) return false;
  const lower = String(libraryId).toLowerCase();
  return (
    lower === "c82b9b7e-9669-424a-9ef8-e0eef0437aa8" ||
    lower.includes("factory") ||
    lower.includes("partner")
  );
}

export class ExportController {
  constructor({
    liveSets = [],
    timbres = [],
    freeSpaceBytes = null,
    onExport = null,
    onCancel = null,
    onAnnounce = null,
    onChange = null,
  } = {}) {
    this.liveSets = Array.isArray(liveSets) ? liveSets : [];
    this.timbres = Array.isArray(timbres) ? timbres : [];
    this.freeSpaceBytes = freeSpaceBytes;
    this.onExport = onExport || (() => {});
    this.onCancel = onCancel || (() => {});
    this.onAnnounce = onAnnounce || (() => {});
    this.onChange = onChange || (() => {});

    this.isOpen = false;
    this.scope = EXPORT_SCOPE.BUNDLE;
    this.selectedLiveSetIds = new Set();
    this.selectedTimbreKeys = new Set();
    this.lockedTimbreKeys = new Set();
    this.isExporting = false;
    this.progress = null; // { currentBytes, totalBytes }
    this.targetPath = "";
  }

  setAvailableData({ liveSets, timbres }) {
    if (liveSets) this.liveSets = Array.isArray(liveSets) ? liveSets : [];
    if (timbres) this.timbres = Array.isArray(timbres) ? timbres : [];
    this._recomputeDependencies();
    this._notify();
  }

  open({ scope = EXPORT_SCOPE.BUNDLE, preselectLiveSetId = null, preselectTimbreKey = null, targetPath = "" } = {}) {
    this.isOpen = true;
    this.scope = scope;
    this.isExporting = false;
    this.progress = null;
    this.targetPath = targetPath;
    this.selectedLiveSetIds.clear();
    this.selectedTimbreKeys.clear();

    if (preselectLiveSetId) {
      this.selectedLiveSetIds.add(preselectLiveSetId);
    } else if (this.scope === EXPORT_SCOPE.SETS || this.scope === EXPORT_SCOPE.BUNDLE) {
      // By default select all live sets if opening generally
      for (const ls of this.liveSets) {
        this.selectedLiveSetIds.add(ls.id);
      }
    }

    if (preselectTimbreKey) {
      const parts = preselectTimbreKey.split(":");
      const libId = parts[0];
      if (!isFactoryTimbre(libId) || this.scope === EXPORT_SCOPE.BUNDLE) {
        this.selectedTimbreKeys.add(preselectTimbreKey);
      }
    }

    this._recomputeDependencies();
    this._notify();
  }

  close() {
    if (!this.isOpen) return;
    if (this.isExporting) {
      this.cancelExport();
    }
    this.isOpen = false;
    this._notify();
  }

  setScope(scope) {
    if (this.scope === scope) return;
    this.scope = scope;

    if (this.scope === EXPORT_SCOPE.TIMBRES) {
      // In Timbres scope, remove factory timbres from selected
      for (const key of Array.from(this.selectedTimbreKeys)) {
        const libId = key.split(":")[0];
        if (isFactoryTimbre(libId)) {
          this.selectedTimbreKeys.delete(key);
        }
      }
    }

    this._recomputeDependencies();
    this._notify();
  }

  setFreeSpace(bytes) {
    this.freeSpaceBytes = bytes;
    this._notify();
  }

  setTargetPath(path) {
    this.targetPath = path;
    this._notify();
  }

  toggleLiveSet(id) {
    if (this.selectedLiveSetIds.has(id)) {
      this.selectedLiveSetIds.delete(id);
    } else {
      this.selectedLiveSetIds.add(id);
    }
    this._recomputeDependencies();
    this._notify();
  }

  toggleTimbre(key) {
    if (this.scope === EXPORT_SCOPE.BUNDLE) {
      // In bundle mode, timbres are locked to Live Set dependencies
      return;
    }
    const libId = key.split(":")[0];
    if (isFactoryTimbre(libId)) {
      // Factory timbres cannot be selected on their own
      return;
    }

    if (this.selectedTimbreKeys.has(key)) {
      this.selectedTimbreKeys.delete(key);
    } else {
      this.selectedTimbreKeys.add(key);
    }
    this._notify();
  }

  toggleSelectAll(checked) {
    if (this.scope === EXPORT_SCOPE.SETS || this.scope === EXPORT_SCOPE.BUNDLE) {
      if (checked) {
        for (const ls of this.liveSets) {
          this.selectedLiveSetIds.add(ls.id);
        }
      } else {
        this.selectedLiveSetIds.clear();
      }
      this._recomputeDependencies();
    } else if (this.scope === EXPORT_SCOPE.TIMBRES) {
      if (checked) {
        for (const t of this.timbres) {
          if (!isFactoryTimbre(t.library_id)) {
            const key = `${t.library_id}:${t.instrument_key}`;
            this.selectedTimbreKeys.add(key);
          }
        }
      } else {
        this.selectedTimbreKeys.clear();
      }
    }
    this._notify();
  }

  _recomputeDependencies() {
    this.lockedTimbreKeys.clear();
    if (this.scope === EXPORT_SCOPE.BUNDLE) {
      for (const ls of this.liveSets) {
        if (this.selectedLiveSetIds.has(ls.id) && Array.isArray(ls.parts)) {
          for (const part of ls.parts) {
            if (part && part.timbre && part.timbre.library_id && part.timbre.instrument_key) {
              const key = `${part.timbre.library_id.toLowerCase()}:${part.timbre.instrument_key}`;
              this.lockedTimbreKeys.add(key);
            }
          }
        }
      }
    }
  }

  getTimbreByKey(key) {
    const parts = key.split(":");
    const libId = parts[0].toLowerCase();
    const instKey = parts[1] || "";
    const found = this.timbres.find(
      (t) => (t.library_id || "").toLowerCase() === libId && (t.instrument_key || "") === instKey
    );
    if (found) return found;

    return {
      library_id: libId,
      instrument_key: instKey,
      name: instKey,
      category: "Timbre",
      size_bytes: isFactoryTimbre(libId) ? 0 : DEFAULT_USER_TIMBRE_SIZE,
      is_factory: isFactoryTimbre(libId),
    };
  }

  computeEstimate() {
    let totalSizeBytes = 0;
    let liveSetCount = 0;
    let timbreCount = 0;
    let referenceCount = 0;
    let userTimbreCount = 0;
    const items = [];

    if (this.scope === EXPORT_SCOPE.SETS || this.scope === EXPORT_SCOPE.BUNDLE) {
      for (const ls of this.liveSets) {
        if (this.selectedLiveSetIds.has(ls.id)) {
          liveSetCount += 1;
          const lsSize = ESTIMATED_LIVE_SET_SIZE;
          totalSizeBytes += lsSize;
          items.push({
            id: ls.id,
            name: ls.name || "Live Set",
            category: "Live Set",
            sizeBytes: lsSize,
            isReferenceOnly: false,
            isLocked: false,
            itemType: "live_set",
          });
        }
      }
    }

    if (this.scope === EXPORT_SCOPE.BUNDLE) {
      for (const key of this.lockedTimbreKeys) {
        timbreCount += 1;
        const timbre = this.getTimbreByKey(key);
        const isFactory = isFactoryTimbre(timbre.library_id);
        if (isFactory) {
          referenceCount += 1;
          items.push({
            id: key,
            name: timbre.name || timbre.instrument_key,
            category: timbre.category || "Factory",
            sizeBytes: 0,
            isReferenceOnly: true,
            isLocked: true,
            itemType: "timbre",
          });
        } else {
          userTimbreCount += 1;
          const userSize = timbre.size_bytes || DEFAULT_USER_TIMBRE_SIZE;
          totalSizeBytes += userSize;
          items.push({
            id: key,
            name: timbre.name || timbre.instrument_key,
            category: timbre.category || "User Timbre",
            sizeBytes: userSize,
            isReferenceOnly: false,
            isLocked: true,
            itemType: "timbre",
          });
        }
      }
    } else if (this.scope === EXPORT_SCOPE.TIMBRES) {
      for (const key of this.selectedTimbreKeys) {
        const timbre = this.getTimbreByKey(key);
        const isFactory = isFactoryTimbre(timbre.library_id);
        if (!isFactory) {
          timbreCount += 1;
          userTimbreCount += 1;
          const userSize = timbre.size_bytes || DEFAULT_USER_TIMBRE_SIZE;
          totalSizeBytes += userSize;
          items.push({
            id: key,
            name: timbre.name || timbre.instrument_key,
            category: timbre.category || "User Timbre",
            sizeBytes: userSize,
            isReferenceOnly: false,
            isLocked: false,
            itemType: "timbre",
          });
        }
      }
    }

    const exceeds2GB = totalSizeBytes > TWO_GIGABYTES;
    const insufficientFreeSpace =
      typeof this.freeSpaceBytes === "number" && this.freeSpaceBytes > 0 && totalSizeBytes > this.freeSpaceBytes;

    const formattedSize = formatBytes(totalSizeBytes);
    const summaryText = `About ${formattedSize} · ${liveSetCount} Live Sets, ${timbreCount} timbres (${referenceCount} by reference)`;

    let canExport = true;
    if (this.scope === EXPORT_SCOPE.SETS && liveSetCount === 0) canExport = false;
    if (this.scope === EXPORT_SCOPE.BUNDLE && liveSetCount === 0) canExport = false;
    if (this.scope === EXPORT_SCOPE.TIMBRES && timbreCount === 0) canExport = false;
    if (insufficientFreeSpace) canExport = false;
    if (this.isExporting) canExport = false;

    let allChecked = false;
    if (this.scope === EXPORT_SCOPE.SETS || this.scope === EXPORT_SCOPE.BUNDLE) {
      allChecked = this.liveSets.length > 0 && this.selectedLiveSetIds.size === this.liveSets.length;
    } else if (this.scope === EXPORT_SCOPE.TIMBRES) {
      const exportable = this.timbres.filter((t) => !isFactoryTimbre(t.library_id));
      allChecked = exportable.length > 0 && this.selectedTimbreKeys.size >= exportable.length;
    }

    return {
      scope: this.scope,
      totalSizeBytes,
      formattedSize,
      liveSetCount,
      timbreCount,
      referenceCount,
      userTimbreCount,
      exceeds2GB,
      insufficientFreeSpace,
      summaryText,
      canExport,
      allChecked,
      items,
    };
  }

  startExport() {
    const est = this.computeEstimate();
    if (!est.canExport) return false;

    this.isExporting = true;
    this.progress = { currentBytes: 0, totalBytes: est.totalSizeBytes };
    this._notify();

    const params = {
      scope: this.scope,
      live_set_ids: Array.from(this.selectedLiveSetIds),
      timbre_keys:
        this.scope === EXPORT_SCOPE.BUNDLE
          ? Array.from(this.lockedTimbreKeys)
          : Array.from(this.selectedTimbreKeys),
      target_path: this.targetPath,
    };

    try {
      this.onExport(params);
      return true;
    } catch (e) {
      this.isExporting = false;
      this._notify();
      throw e;
    }
  }

  updateProgress(currentBytes, totalBytes) {
    if (!this.isExporting) return;
    this.progress = { currentBytes, totalBytes };
    this._notify();
  }

  cancelExport() {
    if (!this.isExporting && !this.isOpen) return;
    this.isExporting = false;
    this.progress = null;
    this.onCancel();
    this.onAnnounce("Export cancelled");
    this._notify();
  }

  finishExport({ fileName = "export.velorasets", liveSetCount = 0, timbreCount = 0 } = {}) {
    this.isExporting = false;
    this.progress = null;
    this.isOpen = false;
    const msg = `Exported ${liveSetCount} Live Sets and ${timbreCount} timbres to ${fileName}`;
    this.onAnnounce(msg);
    this._notify();
  }

  _notify() {
    try {
      this.onChange(this.computeEstimate());
    } catch (e) {
      // ignore
    }
  }
}
