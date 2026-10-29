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
      state: "idle", // idle | inspecting | inspected | sending | verifying | ready_to_install | installing | rebooting | error
      bundlePath: null,
      bundleInfo: null,
      progress: 0,
      bytesTransferred: 0,
      totalBytes: 0,
      error: null,
      expectedReboot: false,
      activeTransferId: null,
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

    if (
      this.firmware.expectedReboot ||
      this.firmware.state === "rebooting" ||
      this.firmware.state === "installing" ||
      (this.firmware.state === "ready_to_install" &&
        this.firmware.bundleInfo &&
        this.firmware.version === this.firmware.bundleInfo.version)
    ) {
      if (isConnected && isPaired) {
        this.firmware.state = "idle";
        this.firmware.expectedReboot = false;
        this.firmware.upToDate = true;
        this.firmware.bundleInfo = null;
        this.firmware.bundlePath = null;
        this.firmware.error = null;
        this.firmware.activeTransferId = null;
        this.onAnnounce?.(this.tr("studio.device.firmware_success", this.firmware.version));
      } else if (!isConnected && (this.firmware.state === "installing" || this.firmware.state === "ready_to_install")) {
        this.firmware.state = "rebooting";
      }
    }

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

  async selectFirmwareBundle(filePath = null) {
    let targetPath = filePath;
    if (!targetPath) {
      if (typeof window !== "undefined" && window.__TAURI__?.dialog?.open) {
        try {
          const selected = await window.__TAURI__.dialog.open({
            multiple: false,
            filters: [{ name: "RAUC Bundle", extensions: ["raucb"] }],
          });
          if (!selected) return null;
          targetPath = Array.isArray(selected) ? selected[0] : selected;
        } catch (_) {
          return null;
        }
      } else {
        return null;
      }
    }

    this.firmware.state = "inspecting";
    this.firmware.error = null;
    this.onChange?.();

    try {
      let info = null;
      if (typeof this.invoke === "function") {
        info = await this.invoke("device_inspect_firmware_bundle", { bundlePath: targetPath });
      } else if (typeof window !== "undefined" && window.__TAURI__?.core?.invoke) {
        info = await window.__TAURI__.core.invoke("device_inspect_firmware_bundle", { bundlePath: targetPath });
      } else {
        info = {
          valid: true,
          version: "1.5.0",
          compatible: "Velora 73",
          description: "Velora Firmware Update",
          file_size: 150 * 1024 * 1024,
          file_name: targetPath.split("/").pop() || "velora.raucb",
          file_path: targetPath,
        };
      }

      this.firmware.bundlePath = targetPath;
      this.firmware.bundleInfo = info;
      this.firmware.totalBytes = info.file_size || 0;
      this.firmware.state = "inspected";
      this.onChange?.();
      return info;
    } catch (err) {
      const errStr = err?.message || String(err);
      this.firmware.state = "error";
      this.firmware.error = errStr;
      this.onError?.(err);
      this.onChange?.();
      throw err;
    }
  }

  async sendFirmwareUpdate() {
    if (!this.firmware.bundlePath) return;
    this.firmware.state = "sending";
    this.firmware.progress = 0;
    this.firmware.bytesTransferred = 0;
    this.firmware.error = null;
    this.onChange?.();

    try {
      const v = this.firmware.bundleInfo?.version || "1.5.0";
      const tid = `fw-update-${v}`;
      this.firmware.activeTransferId = tid;
      const args = {
        bundlePath: this.firmware.bundlePath,
        bundleVersion: this.firmware.bundleInfo?.version || null,
      };
      let res = null;
      if (typeof this.invoke === "function") {
        res = await this.invoke("device_send_firmware_update", args);
      } else if (typeof window !== "undefined" && window.__TAURI__?.core?.invoke) {
        res = await window.__TAURI__.core.invoke("device_send_firmware_update", args);
      } else {
        res = { completed: true, status: "ready_to_install" };
      }

      this.firmware.state = "ready_to_install";
      this.firmware.progress = 100;
      this.onAnnounce?.(this.tr("studio.device.firmware_ready_to_install", v));
      this.onChange?.();
      return res;
    } catch (err) {
      const errStr = err?.message || String(err);
      this.firmware.state = "error";
      if (errStr.includes("bad_signature") || errStr.includes("BAD_SIGNATURE") || errStr.includes("checksum_mismatch")) {
        this.firmware.error = this.tr("studio.device.firmware_bad_sig");
      } else if (errStr.includes("incompatible") || errStr.includes("INCOMPATIBLE")) {
        this.firmware.error = this.tr("studio.device.firmware_incompatible");
      } else if (errStr.includes("no_space") || errStr.includes("insufficient space")) {
        this.firmware.error = this.tr("studio.device.firmware_no_space");
      } else {
        this.firmware.error = errStr;
      }
      this.onError?.(err);
      this.onChange?.();
      throw err;
    }
  }

  async cancelFirmwareUpdate() {
    this.firmware.state = "idle";
    const v = this.firmware.bundleInfo?.version || "";
    this.firmware.bundlePath = null;
    this.firmware.bundleInfo = null;
    this.firmware.progress = 0;
    this.firmware.error = null;
    try {
      const tid = this.firmware.activeTransferId || `fw-update-${v}`;
      if (typeof this.invoke === "function") {
        await this.invoke("device_cancel_firmware_update", { transferId: tid });
      } else if (typeof window !== "undefined" && window.__TAURI__?.core?.invoke) {
        await window.__TAURI__.core.invoke("device_cancel_firmware_update", { transferId: tid });
      }
    } catch (_) {}
    this.firmware.activeTransferId = null;
    this.onChange?.();
  }

  handleProgress(prog) {
    if (!prog) return;
    this.firmware.bytesTransferred = prog.bytes_transferred || 0;
    this.firmware.totalBytes = prog.total_bytes || this.firmware.totalBytes || 0;
    this.firmware.progress = prog.progress != null ? prog.progress : (this.firmware.totalBytes > 0 ? Math.round((this.firmware.bytesTransferred / this.firmware.totalBytes) * 100) : 0);
    if (prog.status === "ready_to_install" || prog.completed) {
      this.firmware.state = "ready_to_install";
      this.firmware.progress = 100;
      this.onAnnounce?.(this.tr("studio.device.firmware_ready_to_install", this.firmware.bundleInfo?.version || ""));
    } else if (prog.status === "streaming" && this.firmware.state !== "ready_to_install") {
      this.firmware.state = "sending";
    }
    this.onChange?.();
  }

  handleDeviceInstallingNotice(version) {
    this.firmware.state = "installing";
    this.firmware.expectedReboot = true;
    const model = this.deviceStatus.model || "Velora 73";
    this.onAnnounce?.(this.tr("studio.device.firmware_installing", model));
    this.onChange?.();
  }

  handleDeviceDisconnectDuringReboot() {
    if (this.firmware.expectedReboot || this.firmware.state === "installing") {
      this.firmware.state = "rebooting";
      this.onChange?.();
      return true;
    }
    return false;
  }
}
