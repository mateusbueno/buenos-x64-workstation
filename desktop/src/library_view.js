// Story 9.14 / 9.15: Library View Controller & Conversion & Transfer Management
import { formatBytes } from "./export_dialog.js";

export function formatTransferLine(name, bytes, total) {
  const curGb = (Number(bytes || 0) / (1024 * 1024 * 1024)).toFixed(1);
  const totGb = (Number(total || 0) / (1024 * 1024 * 1024)).toFixed(1);
  return `Transferring ${name} · ${curGb} of ${totGb} GB`;
}

export const CANONICAL_CATEGORIES = [
  { id: 1, slug: "piano", key: "category.piano", name: "Piano" },
  { id: 2, slug: "epiano", key: "category.epiano", name: "E.Piano" },
  { id: 3, slug: "organ", key: "category.organ", name: "Organ" },
  { id: 4, slug: "keys", key: "category.keys", name: "Keys" },
  { id: 5, slug: "mallet_bells", key: "category.mallet_bells", name: "Mallet & Bells" },
  { id: 6, slug: "strings", key: "category.strings", name: "Strings" },
  { id: 7, slug: "brass", key: "category.brass", name: "Brass" },
  { id: 8, slug: "woodwind", key: "category.woodwind", name: "Woodwind" },
  { id: 9, slug: "choir_vocal", key: "category.choir_vocal", name: "Choir & Vocal" },
  { id: 10, slug: "guitar", key: "category.guitar", name: "Guitar" },
  { id: 11, slug: "bass", key: "category.bass", name: "Bass" },
  { id: 12, slug: "synth_lead", key: "category.synth_lead", name: "Synth Lead" },
  { id: 13, slug: "synth_pad", key: "category.synth_pad", name: "Synth Pad" },
  { id: 14, slug: "synth_poly", key: "category.synth_poly", name: "Synth Poly" },
  { id: 15, slug: "drums_perc", key: "category.drums_perc", name: "Drums & Perc" },
  { id: 16, slug: "world", key: "category.world", name: "World" },
  { id: 17, slug: "fx_ambient", key: "category.fx_ambient", name: "FX & Ambient" },
];

export class LibraryViewController {
  constructor({
    invoke = null,
    onAnnounce = null,
    onChange = null,
    onError = null,
    tr = (k, ...args) => k,
  } = {}) {
    this.invoke = invoke || (typeof window !== "undefined" && window.__TAURI__?.core?.invoke) || (async () => []);
    this.onAnnounce = onAnnounce;
    this.onChange = onChange;
    this.onError = onError;
    this.tr = tr;

    this.isOpen = false;
    this.activeTab = "computer"; // "computer" | "velora"
    this.onVeloraLibraries = [];
    this.onComputerLibraries = [];
    this.isLoading = false;

    // Conversion state
    this.isConvertDialogOpen = false;
    this.conversionTasks = []; // [{ path, name, category, suggestedCategory, suggestedSlug }]
    this.isConverting = false;
    this.conversionProgress = null; // { taskIndex, totalTasks, currentFile, status, reason, messages }
    this.conversionError = null;

    // Story 9.15: Library Transfer state
    this.isPaired = false;
    this.transferState = null; // { transferId, libraryId, libraryName, bytesTransferred, totalBytes, isTransferring }
    this.transferError = null;
  }

  setPaired(paired) {
    this.onConnectionChange(Boolean(paired));
  }

  onConnectionChange(paired) {
    const wasTransferring = this.transferState?.isTransferring;
    this.isPaired = Boolean(paired);
    if (!this.isPaired && wasTransferring) {
      const errMsg = this.tr("studio.library.err_connection_lost");
      this.transferState = null;
      this.transferError = errMsg;
      if (this.onError) this.onError(errMsg);
      if (this.onAnnounce) this.onAnnounce(errMsg);
    }
    if (this.onChange) this.onChange();
  }

  dismissTransferError() {
    this.transferError = null;
    if (this.onChange) this.onChange();
  }

  setDeviceLibraries(libs = []) {
    this.onVeloraLibraries = Array.isArray(libs) ? libs.map(l => ({
      library_id: l.library_id || l.id || "",
      name: l.name || l.library_name || "Sound Library",
      category: l.category || "category.piano",
      category_id: l.category_id || 1,
      size_bytes: l.size_bytes ?? l.sizeBytes ?? 0,
      instrument_count: l.instrument_count ?? l.instrumentCount ?? 1,
    })) : [];
    if (this.onChange) this.onChange();
  }

  async loadComputerLibraries() {
    this.isLoading = true;
    if (this.onChange) this.onChange();
    try {
      const result = await this.invoke("converter_list_libraries", {});
      this.onComputerLibraries = Array.isArray(result) ? result : [];
    } catch (e) {
      if (this.onError) this.onError(e.message || String(e));
    } finally {
      this.isLoading = false;
      if (this.onChange) this.onChange();
    }
  }

  async deleteComputerLibrary(libraryId) {
    if (!libraryId) return;
    try {
      await this.invoke("converter_delete_library", { libraryId });
      const deleted = this.onComputerLibraries.find(l => l.library_id === libraryId);
      this.onComputerLibraries = this.onComputerLibraries.filter(l => l.library_id !== libraryId);
      const name = deleted?.name || libraryId;
      if (this.onAnnounce) {
        this.onAnnounce(this.tr("studio.library.deleted_notice", name));
      }
      if (this.onChange) this.onChange();
    } catch (e) {
      if (this.onError) this.onError(e.message || String(e));
    }
  }

  async openConvertDialog(files = []) {
    this.isConvertDialogOpen = true;
    this.isConverting = false;
    this.conversionProgress = null;
    this.conversionError = null;

    const fileList = Array.isArray(files) ? files : [];
    const names = fileList.map(f => (typeof f === "string" ? f : f.name || f.path || ""));

    let suggestions = [];
    try {
      if (names.length > 0) {
        suggestions = await this.invoke("converter_suggest_categories", { names });
      }
    } catch (e) {
      // Fallback
    }

    this.conversionTasks = fileList.map((f, idx) => {
      const p = typeof f === "string" ? f : f.path || f.name || "";
      const basename = p.split(/[\\/]/).pop() || p;
      const sugg = (suggestions && suggestions[idx]) || { category: "category.piano", slug: "piano" };
      const defaultCategory = (sugg.category && sugg.category !== "none") ? sugg.category : "category.piano";
      return {
        path: p,
        name: basename,
        category: defaultCategory,
        suggestedCategory: sugg.category,
        suggestedSlug: sugg.slug,
      };
    });

    if (this.onChange) this.onChange();
  }

  closeConvertDialog() {
    if (this.isConverting) return;
    this.isConvertDialogOpen = false;
    this.conversionTasks = [];
    this.conversionProgress = null;
    this.conversionError = null;
    if (this.onChange) this.onChange();
  }

  setTaskCategory(index, categoryKey) {
    if (index >= 0 && index < this.conversionTasks.length) {
      this.conversionTasks[index].category = categoryKey;
      if (this.onChange) this.onChange();
    }
  }

  applyCategoryToAll(categoryKey) {
    for (const t of this.conversionTasks) {
      t.category = categoryKey;
    }
    if (this.onChange) this.onChange();
  }

  onProgressEvent(prog) {
    if (!prog) return;
    this.conversionProgress = prog;
    if (prog.status === "cancelled") {
      this.isConverting = false;
      if (this.onAnnounce) {
        this.onAnnounce(this.tr("studio.converter.cancelled"));
      }
    } else if (prog.status === "imported" && prog.task_index === prog.total_tasks) {
      this.isConverting = false;
    }
    if (this.onChange) this.onChange();
  }

  async startConversion() {
    if (this.conversionTasks.length === 0 || this.isConverting) return;
    this.isConverting = true;
    this.conversionError = null;
    if (this.onChange) this.onChange();

    const tasks = this.conversionTasks.map(t => ({
      path: t.path,
      category: t.category.replace(/^category\./, ""),
    }));

    try {
      const results = await this.invoke("converter_start_conversion", { tasks });
      let imported = 0;
      let skipped = 0;
      let failed = 0;
      if (Array.isArray(results)) {
        for (const r of results) {
          if (r.status === "imported") imported++;
          else if (r.status === "skipped") skipped++;
          else if (r.status === "failed") failed++;
        }
      }
      this.conversionProgress = {
        task_index: tasks.length,
        total_tasks: tasks.length,
        current_file: "",
        status: "completed",
        messages: [],
      };
      if (this.onAnnounce) {
        this.onAnnounce(this.tr("studio.converter.completed", imported, skipped, failed));
      }
      await this.loadComputerLibraries();
    } catch (e) {
      this.conversionError = e.message || String(e);
      if (this.onError) this.onError(this.conversionError);
    } finally {
      this.isConverting = false;
      if (this.onChange) this.onChange();
    }
  }

  async cancelConversion() {
    if (!this.isConverting) return;
    try {
      await this.invoke("converter_cancel_conversion", {});
      this.isConverting = false;
      if (this.onAnnounce) {
        this.onAnnounce(this.tr("studio.converter.cancelled"));
      }
      await this.loadComputerLibraries();
    } catch (e) {
      if (this.onError) this.onError(e.message || String(e));
    } finally {
      if (this.onChange) this.onChange();
    }
  }

  // --- Story 9.15: Library Transfer to the unit ---

  async sendToVelora(library) {
    if (!library || this.transferState?.isTransferring) return;
    this.transferError = null;

    if (!this.isPaired) {
      const errMsg = this.tr("studio.library.err_connection_lost");
      this.transferError = errMsg;
      if (this.onError) this.onError(errMsg);
      if (this.onAnnounce) this.onAnnounce(errMsg);
      if (this.onChange) this.onChange();
      return;
    }

    const libName = library.name || library.library_id || "Sound Library";
    const totalBytes = Number(library.size_bytes ?? library.sizeBytes ?? 0);

    // 1. Space check preflight via ws.v1
    try {
      const space = await this.invoke("device_check_library_space", { requiredBytes: totalBytes });
      if (space && !space.fits) {
        const reqStr = formatBytes((space.required_bytes || totalBytes) + (space.reserve_bytes || 268435456));
        const freeStr = formatBytes(space.free_bytes || 0);
        const errMsg = this.tr("studio.library.err_insufficient_space", reqStr, freeStr);
        this.transferError = errMsg;
        if (this.onError) this.onError(errMsg);
        if (this.onAnnounce) this.onAnnounce(errMsg);
        if (this.onChange) this.onChange();
        return;
      }
    } catch (e) {
      const isConnLost = !this.isPaired || String(e).toLowerCase().includes("connection") || String(e).toLowerCase().includes("disconnected");
      const errMsg = isConnLost ? this.tr("studio.library.err_connection_lost") : (e.message || String(e));
      this.transferError = errMsg;
      if (this.onError) this.onError(errMsg);
      if (this.onAnnounce) this.onAnnounce(errMsg);
      if (this.onChange) this.onChange();
      return;
    }

    // 2. Start transfer state
    const transferId = `xfer-${library.library_id}`;
    this.transferState = {
      transferId,
      libraryId: library.library_id,
      libraryName: libName,
      bytesTransferred: 0,
      totalBytes,
      isTransferring: true,
    };
    if (this.onAnnounce) {
      this.onAnnounce(formatTransferLine(libName, 0, totalBytes));
    }
    if (this.onChange) this.onChange();

    // 3. Execute transfer streaming
    try {
      const res = await this.invoke("device_start_library_transfer", {
        libraryId: library.library_id,
        libraryName: libName,
        bundlePath: library.file_path,
        categoryId: library.category_id || 1,
      });

      this.transferState = null;
      const readyMsg = this.tr("studio.library.ready_notice", libName);
      if (this.onAnnounce) {
        this.onAnnounce(readyMsg);
      }
    } catch (e) {
      const isConnLost = !this.isPaired || String(e).toLowerCase().includes("connection") || String(e).toLowerCase().includes("disconnected");
      const errMsg = isConnLost ? this.tr("studio.library.err_connection_lost") : (e.message || String(e));
      this.transferState = null;
      this.transferError = errMsg;
      if (this.onError) this.onError(errMsg);
      if (this.onAnnounce) this.onAnnounce(errMsg);
    } finally {
      if (this.onChange) this.onChange();
    }
  }

  onTransferProgress(prog) {
    if (!prog || !this.transferState) return;
    if (prog.transfer_id && this.transferState.transferId && prog.transfer_id !== this.transferState.transferId) return;

    this.transferState.bytesTransferred = Number(prog.bytes_transferred || 0);
    if (prog.total_bytes) this.transferState.totalBytes = Number(prog.total_bytes);

    if (prog.completed || prog.status === "completed") {
      const name = this.transferState.libraryName;
      this.transferState = null;
      if (this.onAnnounce) {
        this.onAnnounce(this.tr("studio.library.ready_notice", name));
      }
    }
    if (this.onChange) this.onChange();
  }

  async cancelTransfer() {
    if (!this.transferState) return;
    const tid = this.transferState.transferId;
    this.transferState = null;
    try {
      await this.invoke("device_cancel_library_transfer", { transferId: tid });
    } catch (e) {
      // ignore
    } finally {
      if (this.onChange) this.onChange();
    }
  }
}
