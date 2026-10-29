// Story 9.21: Device view controller and storage/diagnostics model
import { formatBytes } from "./export_dialog.js";

export const DEFAULT_STAGE_STORAGE_BYTES = 32 * 1024 * 1024 * 1024; // 32 GB
export const DEFAULT_PRO_STORAGE_BYTES = 64 * 1024 * 1024 * 1024;   // 64 GB
export const SYSTEM_OVERHEAD_BYTES = 2 * 1024 * 1024 * 1024;        // 2 GB OS & system reserve

export class DeviceViewController {
  constructor({
    request = null,
    invoke = null,
    fetchDiagnostics = null,
    saveDiagnostics = null,
    getLibraries = null,
    onAnnounce = null,
    onChange = null,
    onError = null,
    tr = (k, ...args) => k,
  } = {}) {
    this.request = request;
    this.invoke = invoke;
    this.fetchDiagnostics = fetchDiagnostics;
    this.saveDiagnostics = saveDiagnostics;
    this.getLibraries = getLibraries;
    this.onAnnounce = onAnnounce;
    this.onChange = onChange;
    this.onError = onError;
    this.tr = tr;

    this.isOpen = false;
    this.activeSection = "overview"; // overview | storage | firmware | backups | diagnostics

    this.deviceStatus = {
      status: "disconnected",
      model: "Velora 73",
      firmwareVersion: "1.4.0",
      deviceId: "velora-unknown",
      deviceVersion: "1",
      isConnected: false,
      isPaired: false,
      configured: false,
    };

    this.tier = "Stage"; // Stage | Pro
    this.imageVariant = "Production"; // Production | Development | Factory Test

    this.storage = {
      totalBytes: DEFAULT_STAGE_STORAGE_BYTES,
      usedBytes: SYSTEM_OVERHEAD_BYTES,
      freeBytes: DEFAULT_STAGE_STORAGE_BYTES - SYSTEM_OVERHEAD_BYTES,
      libraries: [],
    };

    this.firmware = {
      version: "1.4.0",
      upToDate: true,
    };

    this.backups = {
      lastBackupTime: null,
    };

    this.diagnostics = {
      exporting: false,
      error: null,
      lastExportPath: null,
      lastReport: null,
    };
  }

  setDeviceStatus(st) {
    if (!st) return;
    const isPaired = st.status === "paired";
    const isConnected = isPaired || st.status === "connected";
    const model = st.model || "Velora 73";

    this.deviceStatus = {
      status: st.status || "disconnected",
      model,
      firmwareVersion: st.firmware_version || st.firmwareVersion || "1.4.0",
      deviceId: st.device_id || st.deviceId || "velora-unknown",
      deviceVersion: st.device_version || st.deviceVersion || "1",
      isConnected,
      isPaired,
      configured: !!st.configured,
    };
    this.firmware.version = this.deviceStatus.firmwareVersion;

    if (model.toLowerCase().includes("pro") || (st.tier && (st.tier.name === "pro" || st.tier.tier === 2))) {
      this.tier = "Pro";
      this.storage.totalBytes = DEFAULT_PRO_STORAGE_BYTES;
    } else {
      this.tier = "Stage";
      this.storage.totalBytes = DEFAULT_STAGE_STORAGE_BYTES;
    }

    if (
      (model && model.includes("DEV")) ||
      (st.firmware_version && st.firmware_version.includes("-dev")) ||
      st.host === "127.0.0.1" ||
      st.variant === "development"
    ) {
      this.imageVariant = "Development";
    } else if (st.variant === "factory-test" || (model && model.includes("FACTORY"))) {
      this.imageVariant = "Factory Test";
    } else {
      this.imageVariant = "Production";
    }

    this.recomputeStorage();
    this.onChange?.();
  }

  setDeviceState(st) {
    if (!st) return;
    if (st.tier) {
      const tierName = st.tier.name || (st.tier.tier === 2 ? "pro" : "v1");
      this.tier = tierName.toLowerCase() === "pro" ? "Pro" : "Stage";
      this.storage.totalBytes = this.tier === "Pro" ? DEFAULT_PRO_STORAGE_BYTES : DEFAULT_STAGE_STORAGE_BYTES;
    }
    if (st.sound_libraries || st.libraries) {
      this.setLibraries(st.sound_libraries || st.libraries);
    } else {
      this.recomputeStorage();
    }
    this.onChange?.();
  }

  setBackupTime(timeStr) {
    this.backups.lastBackupTime = timeStr || null;
    this.onChange?.();
  }

  setLibraries(libs = []) {
    this.storage.libraries = libs.map((l) => {
      const isFactory = l.kind === 1 || String(l.kind || "").toLowerCase() === "factory";
      return {
        id: l.id || l.library_id,
        name: l.name || l.library_name || "Library",
        kind: isFactory ? "Factory" : "User",
        instrumentCount: l.instrumentCount || l.instrument_count || 1,
        sizeBytes: l.sizeBytes || l.size_bytes || 250 * 1024 * 1024,
      };
    });
    this.recomputeStorage();
    this.onChange?.();
  }

  recomputeStorage() {
    const libsSum = this.storage.libraries.reduce((acc, l) => acc + (l.sizeBytes || 0), 0);
    this.storage.usedBytes = libsSum + SYSTEM_OVERHEAD_BYTES;
    this.storage.freeBytes = Math.max(0, this.storage.totalBytes - this.storage.usedBytes);
  }

  async loadStorage() {
    if (!this.deviceStatus.isPaired) {
      // Default fallback storage breakdown
      this.setLibraries([
        { id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8", name: "Concert Grand", kind: "Factory", instrumentCount: 3, sizeBytes: 2400 * 1024 * 1024 },
        { id: "f1234567-89ab-cdef-0123-456789abcdef", name: "Studio Strings & Winds", kind: "Factory", instrumentCount: 12, sizeBytes: 1850 * 1024 * 1024 },
        { id: "e9f01234-5678-9abc-def0-123456789abc", name: "Vintage Keys", kind: "Factory", instrumentCount: 8, sizeBytes: 950 * 1024 * 1024 },
      ]);
      return;
    }

    try {
      if (typeof this.getLibraries === "function") {
        const libs = await this.getLibraries();
        if (Array.isArray(libs)) {
          this.setLibraries(libs);
          return;
        }
      }
      // If window.veloraDevice has libraryTimbres
      if (typeof window !== "undefined" && window.veloraDevice?.libraryTimbres) {
        const res = await window.veloraDevice.libraryTimbres();
        if (res && Array.isArray(res.timbres)) {
          const map = new Map();
          for (const t of res.timbres) {
            if (!map.has(t.library_id)) {
              map.set(t.library_id, {
                id: t.library_id,
                name: t.library_name || t.library_id,
                kind: t.library_id.startsWith("lib-factory") ? "Factory" : "User",
                instrumentCount: 0,
                sizeBytes: 120 * 1024 * 1024,
              });
            }
            const item = map.get(t.library_id);
            item.instrumentCount++;
            item.sizeBytes += 45 * 1024 * 1024;
          }
          this.setLibraries(Array.from(map.values()));
          return;
        }
      }
    } catch (e) {
      // fallback
    }

    // Default library set if none loaded
    this.setLibraries([
      { id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8", name: "Concert Grand", kind: "Factory", instrumentCount: 3, sizeBytes: 2400 * 1024 * 1024 },
      { id: "f1234567-89ab-cdef-0123-456789abcdef", name: "Studio Strings & Winds", kind: "Factory", instrumentCount: 12, sizeBytes: 1850 * 1024 * 1024 },
    ]);
  }

  open(section = "overview") {
    this.isOpen = true;
    this.activeSection = section;
    this.diagnostics.error = null;
    this.loadStorage();
    this.onChange?.();
  }

  close() {
    this.isOpen = false;
    this.onChange?.();
  }

  async exportDiagnostics(filePath = null) {
    if (this.diagnostics.exporting) return;
    this.diagnostics.exporting = true;
    this.diagnostics.error = null;
    this.onChange?.();

    let targetPath = filePath;
    if (!targetPath) {
      const defaultFilename = `velora_diagnostics_${new Date().toISOString().slice(0, 10)}.json`;
      if (typeof window !== "undefined" && window.__TAURI__?.dialog?.save) {
        try {
          const selected = await window.__TAURI__.dialog.save({
            defaultPath: defaultFilename,
            filters: [{ name: "JSON", extensions: ["json"] }],
          });
          if (!selected) {
            this.diagnostics.exporting = false;
            this.onChange?.();
            return { cancelled: true };
          }
          targetPath = selected;
        } catch (_) {
          targetPath = defaultFilename;
        }
      } else {
        targetPath = defaultFilename;
      }
    }

    try {
      let savedPath = targetPath;
      if (typeof this.saveDiagnostics === "function") {
        savedPath = await this.saveDiagnostics(targetPath);
      } else if (typeof this.invoke === "function") {
        const res = await this.invoke("save_diagnostics_export", { filePath: targetPath });
        savedPath = res?.file_path || res?.path || (typeof res === "string" ? res : targetPath);
      } else if (typeof window !== "undefined" && window.__TAURI__?.core?.invoke) {
        const res = await window.__TAURI__.core.invoke("save_diagnostics_export", { filePath: targetPath });
        savedPath = res?.file_path || res?.path || (typeof res === "string" ? res : targetPath);
      }

      this.diagnostics.exporting = false;
      this.diagnostics.lastExportPath = savedPath;
      const successMsg = this.tr("studio.device.diagnostics_success", savedPath);
      this.onAnnounce?.(successMsg);
      this.onChange?.();
      return { ok: true, file_path: savedPath, path: savedPath };
    } catch (err) {
      const errStr = err?.message || String(err);
      this.diagnostics.exporting = false;
      this.diagnostics.error = errStr;
      this.onError?.(err);
      this.onAnnounce?.(this.tr("studio.device.diagnostics_failed", errStr));
      this.onChange?.();
      throw err;
    }
  }
}
