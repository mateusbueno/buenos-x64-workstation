// Velora Studio App Shell, Part Editor for FM & VA, Backup & Restore (Stories 9.5, 9.16, 9.17, 12.13), Live Set view (Story 9.8), Part Editor with Studio Knobs (Story 9.9), Write to Velora (Story 9.12)

import { CATALOGS } from "./catalogs.js";
import {
  buildGrids,
  categoryLabel,
  partName,
  setGridLabels,
  updatePartCell,
  updateSceneCell,
} from "./live_set_dom.js";
import {
  LiveSetViewController,
  allLiveSetRows,
  createThrottledRunner,
  entryRows,
  setlistRows,
  setlistsRequest,
} from "./live_set_view.js";
import { PartEditorController } from "./part_editor.js";
import { liveEditFooter } from "./live_edit_state.js";
import { PartEditorView } from "./part_editor_dom.js";
import { SetlistsController } from "./setlists_view.js";
import { SetlistsView } from "./setlists_dom.js";
import { TimbrePickerController } from "./timbre_picker.js";
import { TimbrePickerView } from "./timbre_picker_dom.js";
import { WriteController, foldName } from "./write_to_velora.js";
import { WriteBannerView, WriteDialogView, announceText } from "./write_to_velora_dom.js";
import { ExportController, EXPORT_SCOPE } from "./export_dialog.js";
import { ExportDialogView } from "./export_dialog_dom.js";


// FM 32 Algorithms metadata per Yamaha/DX standard & engine/core/fm_tables.hpp
const FM_ALGORITHMS = [
  null,
  { id: 1, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 6, desc: "Algorithm 1: OP 2 to OP 1, OP 6 to OP 5 to OP 4 to OP 3, feedback on OP 6" },
  { id: 2, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 2, desc: "Algorithm 2: OP 2 to OP 1, OP 6 to OP 5 to OP 4 to OP 3, feedback on OP 2" },
  { id: 3, carriers: [1, 4], modulators: [2, 3, 5, 6], feedbackOp: 6, desc: "Algorithm 3: OP 3 to OP 2 to OP 1, OP 6 to OP 5 to OP 4, feedback on OP 6" },
  { id: 4, carriers: [1, 4], modulators: [2, 3, 5, 6], feedbackOp: 4, desc: "Algorithm 4: OP 3 to OP 2 to OP 1, OP 6 to OP 5 to OP 4, feedback on OP 4" },
  { id: 5, carriers: [1, 3, 5], modulators: [2, 4, 6], feedbackOp: 6, desc: "Algorithm 5: OP 2 to OP 1, OP 4 to OP 3, OP 6 to OP 5, feedback on OP 6" },
  { id: 6, carriers: [1, 3, 5], modulators: [2, 4, 6], feedbackOp: 5, desc: "Algorithm 6: OP 2 to OP 1, OP 4 to OP 3, OP 6 to OP 5, feedback on OP 5" },
  { id: 7, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 6, desc: "Algorithm 7: OP 2 to OP 1, OP 6 to OP 5 to OP 3, OP 4 to OP 3, feedback on OP 6" },
  { id: 8, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 4, desc: "Algorithm 8: OP 2 to OP 1, OP 6 to OP 5 to OP 3, OP 4 to OP 3, feedback on OP 4" },
  { id: 9, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 2, desc: "Algorithm 9: OP 2 to OP 1, OP 6 to OP 5 to OP 3, OP 4 to OP 3, feedback on OP 2" },
  { id: 10, carriers: [1, 4], modulators: [2, 3, 5, 6], feedbackOp: 3, desc: "Algorithm 10: OP 3 to OP 2 to OP 1, OP 5 to OP 4, OP 6 to OP 4, feedback on OP 3" },
  { id: 11, carriers: [1, 4], modulators: [2, 3, 5, 6], feedbackOp: 6, desc: "Algorithm 11: OP 3 to OP 2 to OP 1, OP 5 to OP 4, OP 6 to OP 4, feedback on OP 6" },
  { id: 12, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 2, desc: "Algorithm 12: OP 2 to OP 1, OP 4 to OP 3, OP 5 to OP 3, OP 6 to OP 3, feedback on OP 2" },
  { id: 13, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 6, desc: "Algorithm 13: OP 2 to OP 1, OP 4 to OP 3, OP 5 to OP 3, OP 6 to OP 3, feedback on OP 6" },
  { id: 14, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 6, desc: "Algorithm 14: OP 2 to OP 1, OP 5 to OP 4 to OP 3, OP 6 to OP 4, feedback on OP 6" },
  { id: 15, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 2, desc: "Algorithm 15: OP 2 to OP 1, OP 5 to OP 4 to OP 3, OP 6 to OP 4, feedback on OP 2" },
  { id: 16, carriers: [1], modulators: [2, 3, 4, 5, 6], feedbackOp: 6, desc: "Algorithm 16: OP 2 to OP 1, OP 4 to OP 3 to OP 1, OP 6 to OP 5 to OP 1, feedback on OP 6" },
  { id: 17, carriers: [1], modulators: [2, 3, 4, 5, 6], feedbackOp: 2, desc: "Algorithm 17: OP 2 to OP 1, OP 4 to OP 3 to OP 1, OP 6 to OP 5 to OP 1, feedback on OP 2" },
  { id: 18, carriers: [1], modulators: [2, 3, 4, 5, 6], feedbackOp: 3, desc: "Algorithm 18: OP 2 to OP 1, OP 3 to OP 1, OP 6 to OP 5 to OP 4 to OP 1, feedback on OP 3" },
  { id: 19, carriers: [1, 4, 5], modulators: [2, 3, 6], feedbackOp: 6, desc: "Algorithm 19: OP 3 to OP 2 to OP 1, OP 6 to OP 4 and OP 5, feedback on OP 6" },
  { id: 20, carriers: [1, 2, 4], modulators: [3, 5, 6], feedbackOp: 3, desc: "Algorithm 20: OP 3 to OP 1 and OP 2, OP 5 to OP 4, OP 6 to OP 4, feedback on OP 3" },
  { id: 21, carriers: [1, 2, 4, 5], modulators: [3, 6], feedbackOp: 3, desc: "Algorithm 21: OP 3 to OP 1 and OP 2, OP 6 to OP 4 and OP 5, feedback on OP 3" },
  { id: 22, carriers: [1, 3, 4, 5], modulators: [2, 6], feedbackOp: 6, desc: "Algorithm 22: OP 2 to OP 1, OP 6 to OP 3, OP 4, OP 5, feedback on OP 6" },
  { id: 23, carriers: [1, 2, 4, 5], modulators: [3, 6], feedbackOp: 6, desc: "Algorithm 23: OP 3 to OP 2, OP 6 to OP 4 and OP 5, feedback on OP 6" },
  { id: 24, carriers: [1, 2, 3, 4, 5], modulators: [6], feedbackOp: 6, desc: "Algorithm 24: OP 6 to OP 3, OP 4, OP 5, feedback on OP 6" },
  { id: 25, carriers: [1, 2, 3, 4, 5], modulators: [6], feedbackOp: 6, desc: "Algorithm 25: OP 6 to OP 4 and OP 5, feedback on OP 6" },
  { id: 26, carriers: [1, 2, 4], modulators: [3, 5, 6], feedbackOp: 6, desc: "Algorithm 26: OP 3 to OP 2, OP 5 to OP 4, OP 6 to OP 4, feedback on OP 6" },
  { id: 27, carriers: [1, 2, 4], modulators: [3, 5, 6], feedbackOp: 3, desc: "Algorithm 27: OP 3 to OP 2, OP 5 to OP 4, OP 6 to OP 4, feedback on OP 3" },
  { id: 28, carriers: [1, 3, 6], modulators: [2, 4, 5], feedbackOp: 5, desc: "Algorithm 28: OP 2 to OP 1, OP 5 to OP 4 to OP 3, feedback on OP 5" },
  { id: 29, carriers: [1, 2, 3, 5], modulators: [4, 6], feedbackOp: 6, desc: "Algorithm 29: OP 4 to OP 3, OP 6 to OP 5, feedback on OP 6" },
  { id: 30, carriers: [1, 2, 3, 6], modulators: [4, 5], feedbackOp: 5, desc: "Algorithm 30: OP 5 to OP 4 to OP 3, feedback on OP 5" },
  { id: 31, carriers: [1, 2, 3, 4, 5], modulators: [6], feedbackOp: 6, desc: "Algorithm 31: OP 6 to OP 5, feedback on OP 6" },
  { id: 32, carriers: [1, 2, 3, 4, 5, 6], modulators: [], feedbackOp: 6, desc: "Algorithm 32: all operators to output, feedback on OP 6" }
];

class LocaleManager {
  constructor() {
    this.override = localStorage.getItem("velora_studio_lang") || "system";
  }

  detectOSLocale() {
    const raw = (navigator.language || (navigator.languages && navigator.languages[0]) || "en").toLowerCase();
    if (raw.startsWith("pt")) return "pt-BR";
    if (raw.startsWith("es")) return "es";
    return "en";
  }

  getActiveLocale() {
    if (this.override && this.override !== "system" && CATALOGS[this.override]) {
      return this.override;
    }
    return this.detectOSLocale();
  }

  setOverride(locale) {
    this.override = locale;
    localStorage.setItem("velora_studio_lang", locale);
    this.apply();
  }

  tr(key) {
    const loc = this.getActiveLocale();
    const catalog = CATALOGS[loc] || CATALOGS.en;
    if (catalog[key] !== undefined) return catalog[key];
    if (CATALOGS.en[key] !== undefined) return CATALOGS.en[key];
    return key;
  }

  apply() {
    document.querySelectorAll("[data-i18n]").forEach(el => {
      const key = el.getAttribute("data-i18n");
      const text = this.tr(key);
      if (el.tagName === "INPUT" || el.tagName === "TEXTAREA") {
        el.setAttribute("placeholder", text);
      } else {
        el.textContent = text;
      }
    });
  }
}

class ThemeManager {
  constructor() {
    this.override = localStorage.getItem("velora_studio_theme") || "system";
    this.mediaQuery = window.matchMedia("(prefers-color-scheme: light)");
    this.mediaQuery.addEventListener("change", () => {
      if (this.override === "system") {
        this.apply();
      }
    });
  }

  setOverride(theme) {
    this.override = theme;
    localStorage.setItem("velora_studio_theme", theme);
    this.apply();
  }

  apply() {
    if (this.override === "stage") {
      document.documentElement.setAttribute("data-theme", "stage");
    } else if (this.override === "studio") {
      document.documentElement.setAttribute("data-theme", "studio");
    } else {
      document.documentElement.removeAttribute("data-theme");
    }
  }
}

class FocusManager {
  constructor() {
    this.regions = [
      document.getElementById("sidebar-region"),
      document.getElementById("main-region"),
      document.getElementById("footer-region")
    ].filter(Boolean);

    this.currentRegionIndex = 0;
    this.init();
  }

  init() {
    window.addEventListener("keydown", (e) => {
      if (e.key === "F6") {
        e.preventDefault();
        this.cycleRegion(e.shiftKey ? -1 : 1);
      }
    });
  }

  cycleRegion(direction) {
    if (this.regions.length === 0) return;
    this.currentRegionIndex = (this.currentRegionIndex + direction + this.regions.length) % this.regions.length;
    const target = this.regions[this.currentRegionIndex];
    if (target) {
      target.focus();
    }
  }
}

function announce(message, isWarning = false) {
  const liveRegion = document.getElementById("live-region");
  if (liveRegion) {
    liveRegion.textContent = message;
  }

  const toast = document.getElementById("toast-notice");
  if (toast) {
    toast.textContent = message;
    toast.classList.toggle("warn", isWarning);
    toast.classList.add("active");
    setTimeout(() => {
      toast.classList.remove("active");
    }, 4000);
  }
}

// App Initialization
window.addEventListener("DOMContentLoaded", () => {
  const localeMgr = new LocaleManager();
  const themeMgr = new ThemeManager();
  new FocusManager();

  localeMgr.apply();
  themeMgr.apply();

  // Active state
  let currentDeviceState = null;
  let activeBackup = null;
  let activeBackupPath = "Velora_Backup_Offline.velorabackup";
  let activeLiveSetIndex = 0;
  let selectedPartIndex = 1; // Default to Part 2 (FM) per Story 12.10
  let isConnected = true;
  let deviceLinkActive = false; // true once a real device address is configured: the connection UI then follows the real link
  let deviceLinkPaired = false; // the real link is paired: the real Live Set view (Story 9.8) replaces the simulated Live Sets
  let deviceHasFmVa = true;
  let deviceFirmwareVersion = "1.4.0";
  let isDirty = false;
  let restoreMode = "merge";
  let restoreCandidateBackup = null;

  // Restore cached last backup time
  const currentDeviceId = "Velora 73";
  const cachedBackupTime = localStorage.getItem("velora_last_backup_" + currentDeviceId);
  if (cachedBackupTime) {
    const backupEl = document.getElementById("footer-backup-status");
    if (backupEl) {
      const tpl = localeMgr.tr("studio.footer.last_backup");
      backupEl.textContent = tpl.replace("%s", cachedBackupTime);
    }
  }

  // Populate FM Algorithm Select Options (1 to 32)
  const algoSelect = document.getElementById("fm-algo-select");
  if (algoSelect) {
    algoSelect.innerHTML = "";
    for (let i = 1; i <= 32; i++) {
      const opt = document.createElement("option");
      opt.value = i;
      opt.textContent = i;
      if (i === 5) opt.selected = true;
      algoSelect.appendChild(opt);
    }
  }

  // Settings Modal Controls
  const modal = document.getElementById("settings-modal");
  const restoreModal = document.getElementById("restore-modal");
  const backdrop = document.getElementById("modal-backdrop");
  const themeSelect = document.getElementById("theme-select");
  const langSelect = document.getElementById("lang-select");

  if (themeSelect) {
    themeSelect.value = themeMgr.override;
    themeSelect.addEventListener("change", (e) => {
      themeMgr.setOverride(e.target.value);
    });
  }

  if (langSelect) {
    langSelect.value = localeMgr.override;
    langSelect.addEventListener("change", (e) => {
      localeMgr.setOverride(e.target.value);
      renderEditor();
      updateManageButton();
      if (setlistsUi.view) { setlistsUi.view.setLabels(); setlistsUi.view.render(); } // Story 9.13: the Setlists view follows the language
      if (writeUi.banner) writeUi.banner.setLabels(); // Story 9.12
    });
  }

  const openSettings = () => {
    if (!modal) return;
    modal.classList.add("active");
    if (backdrop) backdrop.classList.add("active");
    if (themeSelect) themeSelect.focus();
  };

  const closeModals = () => {
    if (modal) modal.classList.remove("active");
    if (restoreModal) restoreModal.classList.remove("active");
    if (backdrop) backdrop.classList.remove("active");
  };

  document.getElementById("btn-open-settings")?.addEventListener("click", openSettings);
  document.getElementById("side-settings")?.addEventListener("click", openSettings);
  document.getElementById("btn-offline-settings")?.addEventListener("click", openSettings);
  document.getElementById("btn-close-settings")?.addEventListener("click", closeModals);
  document.getElementById("btn-close-settings-x")?.addEventListener("click", closeModals);
  document.getElementById("btn-close-restore-x")?.addEventListener("click", closeModals);
  document.getElementById("btn-cancel-restore")?.addEventListener("click", closeModals);
  backdrop?.addEventListener("click", closeModals);

  window.addEventListener("keydown", (e) => {
    if (e.key === "Escape") {
      closeModals();
    }
  });

  // Story 9.10: the Footer follows State.edits of the unit (live_edit_state.js): "● LIVE EDIT ON · CHANGES PLAY ON THE VELORA" while an edit is in the unit's memory or Studio edits
  // arrived a moment ago, plus "UNSAVED: SCENE n" / "UNSAVED: LIVE SET" (the scope Write would offer). Only the Live Set that sounds has edits to report; another shown Live Set
  // falls back to the diamond text.
  function paintLiveEditFooter() {
    const footerStatus = document.getElementById("footer-live-edit-status");
    const unsaved = document.getElementById("footer-unsaved-scope");
    if (!footerStatus) return;
    const st = deviceStore.state;
    const sounding = !!st && !!liveCtl.liveSetId && st.live_set_id === liveCtl.liveSetId;
    const f = liveEditFooter(sounding ? st : null);
    footerStatus.classList.toggle("foot-writing", writeUi.ctl.writing);
    if (writeUi.ctl.writing) footerStatus.textContent = localeMgr.tr("studio.footer.writing"); // Story 9.12: from the confirmation until the unit answers
    else if (f.on) footerStatus.textContent = localeMgr.tr("studio.footer.live_edit_on");
    else footerStatus.textContent = localeMgr.tr(liveCtl.model && liveCtl.model.dirty ? "studio.part_editor.live_edit_edited" : "studio.part_editor.live_edit_clean");
    if (unsaved) {
      unsaved.hidden = f.scope.kind === "none";
      unsaved.textContent = f.scope.kind === "scene" ? tr("studio.footer.unsaved_scene", f.scope.scene) : f.scope.kind === "live_set" ? tr("studio.footer.unsaved_live_set") : "";
    }
  }

  // Mark live edit dirty
  function markLiveEdit(dirty = true) {
    isDirty = dirty;
    const badge = document.getElementById("live-edit-badge");
    const writeBtn = document.getElementById("btn-write");
    const footerStatus = document.getElementById("footer-live-edit-status");

    const real = pairedReal();
    const unsavedEl = document.getElementById("footer-unsaved-scope");
    if (unsavedEl && !real) unsavedEl.hidden = true;
    if (badge) badge.style.display = isDirty && !real ? "inline-block" : "none"; // paired: the real marker is the diamond after the Live Set name
    if (writeBtn && real) {
      paintWriteButton(); // Story 9.12: the real Write to Velora (the Live Set that sounds, its unsaved edits)
    } else if (writeBtn && isConnected && deviceLinkActive) {
      writeBtn.disabled = true; // a real unit is connected but the editor still shows simulated Live Sets (until the paired view is up): never write them
      writeBtn.title = localeMgr.tr("studio.write.simulated");
    } else if (writeBtn && isConnected) {
      writeBtn.disabled = !isDirty;
    }
    if (footerStatus) {
      if (real) {
        paintLiveEditFooter();
      } else {
        footerStatus.textContent = isDirty
          ? localeMgr.tr("studio.part_editor.live_edit_edited")
          : localeMgr.tr("studio.part_editor.live_edit_clean");
      }
    }
    if (real) return; // the real strips are drawn by the Live Set view (never edited from here)

    // Update active part strip indicator
    const currentStrip = document.querySelector(`#editor-part-strips .st[data-part-idx="${selectedPartIndex}"]`);
    if (currentStrip) {
      const nameEl = currentStrip.querySelector(".n");
      if (nameEl) {
        const baseName = nameEl.getAttribute("data-basename") || nameEl.textContent.replace(" ◆", "");
        nameEl.setAttribute("data-basename", baseName);
        nameEl.textContent = isDirty ? `${baseName} ◆` : baseName;
      }
    }
  }

  // Draw Dynamic FM Algorithm Diagram SVG (with accessible role="img" & aria-label)
  function renderFmAlgorithmDiagram(algoNum) {
    const meta = FM_ALGORITHMS[algoNum] || FM_ALGORITHMS[5];
    const svg = document.getElementById("fm-algo-svg");
    if (!svg) return;

    svg.setAttribute("aria-label", meta.desc);

    const carriers = new Set(meta.carriers);
    const modulators = new Set(meta.modulators);
    const feedbackOp = meta.feedbackOp;

    // Coordinate layout: 3 columns x 2 rows
    // Col 0: OP 1 & 2; Col 1: OP 3 & 4; Col 2: OP 5 & 6
    const opPos = {
      2: { x: 24, y: 22 },
      1: { x: 24, y: 100 },
      4: { x: 146, y: 22 },
      3: { x: 146, y: 100 },
      6: { x: 268, y: 22 },
      5: { x: 268, y: 100 }
    };

    let elements = "";

    // Operator Boxes (OP 1 to 6)
    for (let op = 1; op <= 6; op++) {
      const pos = opPos[op] || { x: 24 + ((op - 1) % 3) * 122, y: op <= 3 ? 100 : 22 };
      const isCar = carriers.has(op);
      const isMovedByBright = modulators.has(op);
      const strokeDash = isCar ? "" : 'stroke-dasharray="5 3"';
      const brightGlyph = isMovedByBright ? `<text x="${pos.x + 50}" y="${pos.y - 5}" text-anchor="middle" style="fill:var(--accent);font-size:15px">◐</text>` : "";

      elements += `
        <rect x="${pos.x}" y="${pos.y}" width="56" height="38" rx="6" style="fill:var(--raised);stroke:var(--accent)" stroke-width="2" ${strokeDash} />
        <text x="${pos.x + 28}" y="${pos.y + 25}" text-anchor="middle" style="fill:var(--text);font-family:inherit;font-size:16px">OP ${op}</text>
        ${brightGlyph}
      `;
    }

    // Vertical connections between modulators and carriers
    elements += `
      <line x1="52" y1="60" x2="52" y2="100" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="174" y1="60" x2="174" y2="100" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="296" y1="60" x2="296" y2="100" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="52" y1="138" x2="52" y2="176" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="174" y1="138" x2="174" y2="176" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="296" y1="138" x2="296" y2="176" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="52" y1="176" x2="296" y2="176" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="174" y1="176" x2="174" y2="196" style="stroke:var(--text)" stroke-width="2"/>
      <text x="174" y="216" text-anchor="middle" style="fill:var(--muted);font-size:14px;letter-spacing:2px">OUT</text>
    `;

    // Feedback loop
    const fbPos = opPos[feedbackOp] || { x: 268, y: 22 };
    elements += `
      <path d="M${fbPos.x + 56} ${fbPos.y + 8} h14 v26 h-14" fill="none" style="stroke:var(--text)" stroke-width="2"/>
      <text x="${fbPos.x + 70}" y="${fbPos.y + 54}" text-anchor="end" style="fill:var(--muted);font-size:12px">FB</text>
    `;

    svg.innerHTML = elements;

    // Update Operator Cards titles (Carrier vs Modulator)
    for (let op = 1; op <= 6; op++) {
      const cardTitle = document.getElementById(`op${op}-h`);
      if (cardTitle) {
        const typeStr = carriers.has(op) ? "Carrier" : "Modulator";
        cardTitle.textContent = `OP ${op} · ${typeStr}`;
      }
    }

    // Update Brightness Note text
    const noteEl = document.getElementById("brightness-note-text");
    if (noteEl) {
      if (modulators.size > 0) {
        const modsList = Array.from(modulators).sort().map(m => `OP ${m}`).join(" · ");
        const tpl = localeMgr.tr("studio.part_editor.brightness_fm_note");
        noteEl.textContent = tpl.replace("%s", modsList);
      } else {
        noteEl.textContent = "(no modulators)";
      }
    }

    // Update Feedback Label
    const fbLabel = document.getElementById("fm-feedback-label");
    if (fbLabel) {
      fbLabel.textContent = `Feedback (OP ${feedbackOp})`;
    }
  }

  // Populate Dynamic OP 1 to OP 6 Cards
  function createOperatorCards() {
    const container = document.getElementById("fm-operators-cards-container");
    if (!container || container.children.length > 0) return;

    const defaultRatios = [1.00, 2.00, 1.00, 3.50, 0.50, 7.00];
    const defaultLevels = [99, 78, 99, 62, 90, 41];
    const polylines = [
      "0,55 20,8 80,24 200,55",
      "0,55 10,6 60,40 200,52",
      "0,55 30,10 110,30 200,55",
      "0,55 8,4 40,44 200,54",
      "0,55 20,8 120,20 200,55",
      "0,55 6,10 50,46 200,55"
    ];

    for (let op = 1; op <= 6; op++) {
      const sec = document.createElement("section");
      sec.className = "card";
      sec.id = `op${op}`;
      sec.tabIndex = -1;
      sec.setAttribute("aria-labelledby", `op${op}-h`);

      sec.innerHTML = `
        <h4 id="op${op}-h">OP ${op}</h4>
        <label class="f">
          <span>Ratio</span>
          <input type="number" id="op${op}-ratio" value="${defaultRatios[op - 1].toFixed(2)}" step="0.01" min="0.5" max="32.0" aria-label="OP ${op} Ratio">
        </label>
        <label class="f">
          <span>Level</span>
          <span class="rg">
            <input type="range" id="op${op}-level" min="0" max="99" value="${defaultLevels[op - 1]}" role="slider" aria-label="OP ${op} Level" aria-valuemin="0" aria-valuemax="99" aria-valuenow="${defaultLevels[op - 1]}" aria-valuetext="${defaultLevels[op - 1]} of 99">
            <output id="op${op}-level-out">${defaultLevels[op - 1]}</output>
          </span>
        </label>
        <div class="env">
          <svg viewBox="0 0 200 60" preserveAspectRatio="none" aria-hidden="true">
            <polyline points="${polylines[op - 1]}" fill="none" style="stroke:var(--accent)" stroke-width="2" vector-effect="non-scaling-stroke"/>
          </svg>
        </div>
        <button class="btn" type="button" data-i18n="studio.part_editor.edit_envelope">Edit envelope</button>
      `;

      container.appendChild(sec);
    }
  }

  createOperatorCards();

  // Range Slider ARIA & Input Event Wiring
  function wireRangeInputs() {
    document.querySelectorAll("input[type=range]").forEach((slider) => {
      slider.addEventListener("input", (e) => {
        const val = e.target.value;
        const out = e.target.parentNode.querySelector("output");
        if (out) out.textContent = val;
        e.target.setAttribute("aria-valuenow", val);
        e.target.setAttribute("aria-valuetext", `${val} of ${e.target.max}`);

        markLiveEdit(true);

        // Brightness coordination
        if (slider.id === "va-flt-cutoff") {
          const brightVal = document.getElementById("brightness-val-text");
          if (brightVal) brightVal.textContent = val;
        }

        // Send Tauri live update if connected
        if (window.__TAURI__?.core?.invoke && currentDeviceState) {
          const curLs = currentDeviceState.live_sets?.[activeLiveSetIndex];
          if (curLs) {
            window.__TAURI__.core.invoke("update_part_param", {
              liveSetId: curLs.id,
              partIndex: selectedPartIndex,
              paramId: 1000,
              value: parseFloat(val)
            }).catch(() => {});
          }
        }
      });

      // Keyboard navigation enhancements for ARIA slider
      slider.addEventListener("keydown", (e) => {
        let step = 1;
        if (e.key === "PageUp") {
          step = 10;
        } else if (e.key === "PageDown") {
          step = -10;
        } else if (e.key === "Home") {
          slider.value = slider.min;
          slider.dispatchEvent(new Event("input"));
          e.preventDefault();
          return;
        } else if (e.key === "End") {
          slider.value = slider.max;
          slider.dispatchEvent(new Event("input"));
          e.preventDefault();
          return;
        }

        if (step !== 1) {
          slider.value = Math.max(Number(slider.min), Math.min(Number(slider.max), Number(slider.value) + step));
          slider.dispatchEvent(new Event("input"));
          e.preventDefault();
        }
      });
    });

    document.querySelectorAll("input[type=number], select, input[type=checkbox]").forEach((inp) => {
      inp.addEventListener("change", () => {
        markLiveEdit(true);
      });
    });

    // Algorithm Select change
    document.getElementById("fm-algo-select")?.addEventListener("change", (e) => {
      const algo = parseInt(e.target.value, 10);
      renderFmAlgorithmDiagram(algo);
      markLiveEdit(true);
    });
  }

  wireRangeInputs();

  // Jump navigation links generator
  function updateJumpNavigation(engineTypeId) {
    const nav = document.getElementById("sound-jump-nav");
    if (!nav) return;
    nav.innerHTML = "";

    let links = [];
    if (engineTypeId === 3) { // FM
      links = [
        { href: "#alg", text: "Algorithm" },
        { href: "#op1", text: "OP 1" },
        { href: "#op2", text: "OP 2" },
        { href: "#op3", text: "OP 3" },
        { href: "#op4", text: "OP 4" },
        { href: "#op5", text: "OP 5" },
        { href: "#op6", text: "OP 6" },
        { href: "#pe", text: "Pitch env" },
        { href: "#lfo", text: "LFO" }
      ];
    } else if (engineTypeId === 4) { // VA
      links = [
        { href: "#osc1", text: "Osc 1" },
        { href: "#osc2", text: "Osc 2" },
        { href: "#mix", text: "Mixer" },
        { href: "#flt", text: "Filter" },
        { href: "#aenv", text: "Amp env" },
        { href: "#fenv", text: "Filter env" },
        { href: "#vlfo", text: "LFO" },
        { href: "#voice", text: "Voice" }
      ];
    }

    links.forEach((l) => {
      const a = document.createElement("a");
      a.href = l.href;
      a.textContent = l.text;
      a.addEventListener("click", (e) => {
        e.preventDefault();
        const targetId = l.href.substring(1);
        const card = document.getElementById(targetId);
        if (card) {
          card.focus();
          card.classList.add("is-focus");
          setTimeout(() => card.classList.remove("is-focus"), 1200);
        }
      });
      nav.appendChild(a);
    });
  }

  // Render Full Studio Part Editor
  function renderEditor() {
    if (pairedReal()) {
      renderPairedEditor(); // Story 9.8 / 9.9: the real Live Set view and the real Part Editor (the simulated editor stays for offline)
      return;
    }
    hideRealView();
    const dataSource = currentDeviceState || activeBackup;
    if (!dataSource || !dataSource.live_sets || dataSource.live_sets.length === 0) {
      document.getElementById("offline-empty-card").style.display = "flex";
      document.getElementById("editor-container").style.display = "none";
      return;
    }

    document.getElementById("offline-empty-card").style.display = "none";
    const editor = document.getElementById("editor-container");
    editor.style.display = "grid";

    // Setlists & Live Sets in sidebar
    const setlistsListEl = document.getElementById("sidebar-setlists-list");
    setlistsListEl.innerHTML = "";
    const setlists = dataSource.setlists?.setlists || [];
    if (setlists.length === 0) {
      const hint = document.createElement("div");
      hint.className = "sidebar-item empty-hint";
      hint.textContent = localeMgr.tr("studio.sidebar.empty_setlists");
      setlistsListEl.appendChild(hint);
    } else {
      setlists.forEach((sl) => {
        const item = document.createElement("button");
        item.type = "button";
        item.className = "sidebar-item";
        item.textContent = sl.name;
        setlistsListEl.appendChild(item);
      });
    }

    const lsListEl = document.getElementById("sidebar-livesets-list");
    lsListEl.innerHTML = "";
    dataSource.live_sets.forEach((ls, idx) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "sidebar-item" + (idx === activeLiveSetIndex ? " on" : "");
      btn.textContent = ls.name;
      btn.addEventListener("click", () => {
        activeLiveSetIndex = idx;
        renderEditor();
      });
      lsListEl.appendChild(btn);
    });

    const curLs = dataSource.live_sets[activeLiveSetIndex] || dataSource.live_sets[0];
    const titleEl = document.getElementById("editor-title");
    titleEl.textContent = curLs.name;

    // Render Part Strips (1-8)
    const stripsNav = document.getElementById("editor-part-strips");
    stripsNav.innerHTML = "";

    const partsList = curLs.parts || [];
    for (let pIdx = 0; pIdx < 8; pIdx++) {
      const p = partsList[pIdx] || {};
      const st = document.createElement("button");
      st.type = "button";
      st.className = "st" + (pIdx === selectedPartIndex ? " sel" : "");
      st.setAttribute("data-part-idx", pIdx);

      const engineTypeId = p.engine?.engine_type_id || (pIdx === 1 ? 3 : pIdx === 3 ? 4 : 1);
      const isFmOrVa = engineTypeId === 3 || engineTypeId === 4;
      const isMismatch = !deviceHasFmVa && isFmOrVa;

      let catName = "SAMPLE";
      let timbreName = "Empty";
      if (pIdx === 0) { catName = "PIANO"; timbreName = "Concert Grand"; }
      else if (pIdx === 1) { catName = "E.PIANO"; timbreName = "Glass Keys"; }
      else if (pIdx === 2) { catName = "PIANO"; timbreName = "Grand Piano"; }
      else if (pIdx === 3) { catName = "SYNTH PAD"; timbreName = "Warm Saw Pad"; }
      else if (pIdx === 4) { catName = "STRINGS"; timbreName = "Strings"; }
      else if (pIdx === 5) { catName = "BASS"; timbreName = "Soft Bass"; }
      else if (pIdx === 6) { catName = "BRASS"; timbreName = "Brass Stab"; }
      else if (pIdx === 7) { catName = "VOICE"; timbreName = "Choir"; }

      const volVal = Math.round((p.volume || 0.7) * 100);

      st.innerHTML = `
        <span class="c">${catName}</span>
        <span class="n" data-basename="${timbreName}">${timbreName}</span>
        ${isMismatch
          ? `<span class="w">! Needs update 1.4</span>`
          : `<span class="m">${volVal}</span>`}
      `;

      st.addEventListener("click", () => {
        selectedPartIndex = pIdx;
        renderEditor();
      });

      stripsNav.appendChild(st);
    }

    // Active Part Details Header
    const activePart = partsList[selectedPartIndex] || {};
    const engineTypeId = activePart.engine?.engine_type_id || (selectedPartIndex === 1 ? 3 : selectedPartIndex === 3 ? 4 : 1);
    const isFmOrVa = engineTypeId === 3 || engineTypeId === 4;
    const isMismatch = !deviceHasFmVa && isFmOrVa;

    let catName = "PIANO";
    let timbreName = "Concert Grand";
    if (selectedPartIndex === 1) { catName = "E.PIANO"; timbreName = "Glass Keys"; }
    else if (selectedPartIndex === 2) { catName = "PIANO"; timbreName = "Grand Piano"; }
    else if (selectedPartIndex === 3) { catName = "SYNTH PAD"; timbreName = "Warm Saw Pad"; }
    else if (selectedPartIndex === 4) { catName = "STRINGS"; timbreName = "Strings"; }
    else if (selectedPartIndex === 5) { catName = "BASS"; timbreName = "Soft Bass"; }
    else if (selectedPartIndex === 6) { catName = "BRASS"; timbreName = "Brass Stab"; }
    else if (selectedPartIndex === 7) { catName = "VOICE"; timbreName = "Choir"; }

    renderPartDetails(catName, timbreName, engineTypeId);
    markLiveEdit(isDirty);
  }

  // The Part Editor header and the FM / VA cards for the selected part (simulated data until Story 9.9; the real view passes the real engine and names).
  function renderPartDetails(catName, timbreName, engineTypeId) {
    const isFmOrVa = engineTypeId === 3 || engineTypeId === 4;
    const isMismatch = !deviceHasFmVa && isFmOrVa;

    document.getElementById("part-num-label").textContent = `PART ${selectedPartIndex + 1}`;
    document.getElementById("part-name-display").textContent = timbreName;
    document.getElementById("part-category-display").textContent = catName;

    const banner = document.getElementById("firmware-mismatch-banner");
    const fmCards = document.getElementById("fm-sound-cards");
    const vaCards = document.getElementById("va-sound-cards");
    const brightEl = document.getElementById("brightness-readout");

    if (isMismatch) {
      // Version mismatch: hide FM/VA editor cards, show banner (FR99)
      banner.style.display = "grid";
      fmCards.style.display = "none";
      vaCards.style.display = "none";
      brightEl.style.display = "none";
      updateJumpNavigation(0);
    } else {
      banner.style.display = "none";
      brightEl.style.display = "flex";

      if (engineTypeId === 3) { // FM
        fmCards.style.display = "grid";
        vaCards.style.display = "none";
        updateJumpNavigation(3);
        renderFmAlgorithmDiagram(5);
      } else if (engineTypeId === 4) { // VA
        vaCards.style.display = "grid";
        fmCards.style.display = "none";
        updateJumpNavigation(4);
        const noteEl = document.getElementById("brightness-note-text");
        if (noteEl) noteEl.textContent = localeMgr.tr("studio.part_editor.brightness_va_note");
      } else {
        fmCards.style.display = "none";
        vaCards.style.display = "none";
        brightEl.style.display = "none";
        updateJumpNavigation(0);
      }
    }
  }

  // Write to Velora persistence handler
  const writeBtn = document.getElementById("btn-write");
  writeBtn?.addEventListener("click", () => {
    if (pairedReal()) { openWriteDialog(); return; } // Story 9.12: the real Write dialog; the branches below are the simulated device
    if (window.__TAURI__?.core?.invoke && currentDeviceState) {
      const curLs = currentDeviceState.live_sets?.[activeLiveSetIndex];
      if (curLs) {
        window.__TAURI__.core.invoke("write_to_device", { liveSetId: curLs.id })
          .then(() => {
            markLiveEdit(false);
            announce(localeMgr.tr("studio.part_editor.written_to_velora"));
          })
          .catch((err) => {
            announce("Write failed: " + err, true);
          });
      }
    } else {
      markLiveEdit(false);
      announce(localeMgr.tr("studio.part_editor.written_to_velora"));
    }
  });

  // Mismatch Banner "Update firmware" button (FR99)
  document.getElementById("btn-mismatch-update-fw")?.addEventListener("click", () => {
    deviceHasFmVa = true;
    deviceFirmwareVersion = "1.4.0";
    if (window.__TAURI__?.core?.invoke) {
      window.__TAURI__.core.invoke("set_firmware_version", { version: "1.4.0" })
        .then(() => {
          renderEditor();
          announce("Firmware updated to 1.4.0. FM and VA sound engines are now available.");
        })
        .catch(() => {
          renderEditor();
        });
    } else {
      renderEditor();
      announce("Firmware updated to 1.4.0. FM and VA sound engines are now available.");
    }
  });

  // Mismatch Banner "Change timbre" button
  document.getElementById("btn-mismatch-change-timbre")?.addEventListener("click", () => {
    // Switch to concert grand
    selectedPartIndex = 0;
    renderEditor();
  });

  // ---- Live Set view (Story 9.8) ---------------------------------------------------------------------------------------------------------------------
  // While paired, the main area shows the real Live Set the player chose in the sidebar: header (name, unsaved diamond), 8 scene tiles, 8 part strips. The data is
  // read from the unit with GetLiveSetView (read-only: selecting never sends Jump / Recall / Load) and the sounding Live Set follows the State pushes in place
  // (live_set_view.js owns the model and the diffs, live_set_dom.js the cells; this section wires them to the page). The Part Editor below the strips stays
  // simulated until Story 9.9, and Write stays disabled for real data (markLiveEdit). Unpaired / offline: the simulated Live Sets and offline card behave as before.
  const fmt = (tpl, ...args) => {
    let i = 0;
    return tpl.replace(/%[ds]/g, () => String(args[i++]));
  };
  const tr = (key, ...args) => fmt(localeMgr.tr(key), ...args);
  const liveEls = {
    container: document.getElementById("editor-container"),
    title: document.getElementById("editor-title"),
    subtitle: document.getElementById("editor-subtitle"),
    dirty: document.getElementById("live-set-dirty"),
    meta: document.getElementById("live-set-meta"),
    error: document.getElementById("live-view-error"),
    scenes: document.getElementById("live-scenes"),
    note: document.getElementById("live-view-note"),
    parts: document.getElementById("live-part-grid"),
    simStrips: document.getElementById("editor-part-strips"),
  };
  const liveUi = { grids: null, partListeners: new Set(), picker: null };
  const sidebar = { setlists: [], all: [], entries: new Map(), expanded: new Set(), key: null, error: "", firstLoad: true, playingId: "", autoTried: "" };

  function buildLiveGrids() {
    if (liveUi.grids) return;
    liveUi.grids = buildGrids(document, liveEls.scenes, liveEls.parts, { onSelectPart: (i) => liveCtl.selectPart(i), onAddTimbre: (i) => openTimbrePicker(i) });
    setGridLabels(liveEls.scenes, liveEls.parts, tr);
  }

  function renderLiveHeader(m) {
    liveEls.title.textContent = m.name;
    liveEls.subtitle.textContent = tr("studio.live_set.eyebrow");
    liveEls.dirty.hidden = !m.dirty;
    liveEls.dirty.setAttribute("aria-label", tr("studio.live_set.unsaved"));
    liveEls.dirty.title = tr("studio.live_set.unsaved_tooltip");
    const meta = [tr("studio.live_set.parts_active", m.activeParts)];
    if (m.sounding) meta.push(tr("studio.live_set.sounding"));
    liveEls.meta.textContent = meta.join(" · ");
    liveEls.meta.hidden = false;
    liveEls.note.hidden = !m.hasMoreParts;
    if (m.hasMoreParts) liveEls.note.textContent = tr("studio.live_set.parts_16_note", m.partCount);
  }

  // Draws a model. `changes` names what changed ({ header, scenes: [..], parts: [..] }): only those tiles are touched, so a delta never rebuilds the others, and
  // the sidebar and the part editor are not redrawn for a header-only change (a dirty / sounding flip): markers move in place.
  function renderLiveModel(m, changes) {
    buildLiveGrids();
    if (!pairedReal()) return;
    liveEls.scenes.hidden = false;
    liveEls.parts.hidden = false;
    if (changes.header || changes.note) renderLiveHeader(m);
    const g = liveUi.grids;
    changes.scenes.forEach((i) => updateSceneCell(g.sceneCells[i], m.scenes[i], tr));
    changes.parts.forEach((i) => updatePartCell(g.partCells[i], m.parts[i], { tr, selected: liveCtl.selectedPart === i }));
    if (changes.header) markLiveEdit(isDirty); // the footer follows the real unsaved flag
    if (changes.parts.includes(liveCtl.selectedPart)) renderRealPartDetails(m); // the controller assigns its model after this callback: use the one passed in
    if (changes.header) updateSidebarMarkers();
    // Story 9.11: an open picker is about an EMPTY part of the Live Set it was opened on; when the unit changed that under it (another Live Set, the part filled elsewhere) it closes
    // without assigning anything. (While its own assignment is in flight the part legitimately stops being empty: the controller judges that result.)
    const tp = liveUi.picker;
    if (tp && tp.ctl.isOpen && tp.ctl.status !== "assigning") {
      const ctx = tp.ctl.context;
      if (liveCtl.liveSetId !== ctx.liveSetId || !m.parts[ctx.part] || m.parts[ctx.part].status !== "empty") tp.ctl.abort("changed");
    }
  }

  const ALL_CHANGES = { header: true, note: true, scenes: [0, 1, 2, 3, 4, 5, 6, 7], parts: [0, 1, 2, 3, 4, 5, 6, 7] };

  function showLiveError(e) {
    liveEls.error.hidden = false;
    liveEls.error.textContent = tr("studio.live_set.error_load", e.message || "");
  }

  function pairedReal() {
    return deviceLinkActive && deviceLinkPaired;
  }

  function hideRealView() {
    liveEls.container.classList.remove("live-real");
    [liveEls.error, liveEls.scenes, liveEls.note, liveEls.parts, liveEls.dirty, liveEls.meta].forEach((el) => { el.hidden = true; });
    liveEls.simStrips.style.display = "";
    liveEls.subtitle.textContent = "VELORA STUDIO";
    peEls.host.hidden = true; // Story 9.9: the real Part Editor is paired-only; the simulated editor below the strips comes back
    peEls.sim.style.display = "";
  }

  function renderPairedEditor() {
    if (setlistsUi.open) return; // the Setlists view (Story 9.13) owns the main area until a Live Set is chosen or it is closed
    buildLiveGrids();
    setGridLabels(liveEls.scenes, liveEls.parts, tr); // follows a language change
    document.getElementById("offline-empty-card").style.display = "none";
    liveEls.container.style.display = "grid";
    liveEls.container.classList.add("live-real");
    liveEls.simStrips.style.display = "none";
    renderSidebarReal();
    if (liveCtl.model) {
      renderLiveModel(liveCtl.model, ALL_CHANGES);
      renderRealPartDetails();
    } else {
      liveEls.title.textContent = liveCtl.pendingId ? tr("studio.live_set.loading") : "";
      liveEls.subtitle.textContent = tr("studio.live_set.eyebrow");
      liveEls.dirty.hidden = true;
      liveEls.meta.hidden = true;
      liveEls.note.hidden = true;
      liveEls.scenes.hidden = true;
      liveEls.parts.hidden = true;
      peEls.host.hidden = true;
      peEls.sim.style.display = "none";
    }
    markLiveEdit(isDirty);
  }

  // ---- Part Editor (Story 9.9) ---------------------------------------------------------------------------------------------------------------------
  // The selected real part is edited with Studio Knobs built from the generated registry (part_editor.js is the model and the controller, part_editor_dom.js the DOM). Each
  // change is a ws.v1 edit with base_revision (Story 9.10: the Live Set revision the view was loaded at; the unit refuses a stale one and the editor shows the Reload banner); the unit plays it at once and the knob shows what the unit confirms.
  const peEls = { host: document.getElementById("real-part-editor"), sim: document.getElementById("studio-part-editor") };
  const pe = { view: null, lang: "" };

  function ensurePartEditorView() {
    const lang = localeMgr.getActiveLocale();
    if (pe.view && pe.lang !== lang) {
      pe.view.clear(); // another language: the labels are rebuilt
      pe.view = null;
    }
    if (!pe.view) {
      pe.lang = lang;
      peEls.host.setAttribute("aria-label", tr("studio.part_editor.region"));
      pe.view = new PartEditorView({ doc: document, host: peEls.host, tr: (key) => localeMgr.tr(key), controller: partEditorCtl, announce: quietAnnounce }); // the view fills the %d / %s itself
    }
    return pe.view;
  }

  function renderPartEditor() {
    if (!pairedReal() || setlistsUi.open || peEls.host.hidden || partEditorCtl.partIndex < 0) return;
    ensurePartEditorView().render(partEditorCtl.model());
  }

  const partEditorCtl = new PartEditorController({
    request: (r) => window.veloraDevice.request(r),
    onChange: renderPartEditor,
    onError: (e) => { if (!e.render && pe.view) pe.view.showError(e); },
    // Story 9.10: Reload applied the current snapshot of the unit: the Live Set view is read again too (the ◆, scenes and strips), and the footer follows the State
    onReloaded: (st) => {
      if (liveCtl.liveSetId) liveCtl.select(liveCtl.liveSetId);
      if (writeUi.ctl.banner && writeUi.ctl.banner.kind === "stale") writeUi.ctl.dismissBanner(); // Story 9.12: a Reload answers the stale Write banner too
      paintLiveEditFooter();
    },
  });

  // The Part Editor for the real selection: the engine and the names are the real ones, the parameters come from the registry.
  function renderRealPartDetails(m = liveCtl.model) {
    const i = liveCtl.selectedPart;
    if (!m || i < 0) return;
    const p = m.parts[i];
    selectedPartIndex = i;
    peEls.sim.style.display = "none";
    peEls.host.hidden = false;
    const view = ensurePartEditorView();
    view.setHeader({ number: i + 1, name: partName(p, tr), category: categoryLabel(p, tr) });
    partEditorCtl.select({ liveSetId: liveCtl.liveSetId, part: i, engine: p.engine, available: p.status !== "unavailable" });
  }

  // The polite live region: cleared first and set on a microtask, so the same text twice in a row is announced twice.
  function quietAnnounce(message) {
    const region = document.getElementById("live-region");
    if (!region) return;
    region.textContent = "";
    queueMicrotask(() => { region.textContent = message; });
  }

  function onRealPartSelected(i, p) {
    const g = liveUi.grids;
    if (g && liveCtl.model) g.partCells.forEach((c, k) => updatePartCell(c, liveCtl.model.parts[k], { tr, selected: k === i }));
    renderRealPartDetails();
    if (p) {
      const label = partName(p, tr);
      const bare = p.status === "empty" || p.status === "missing" || p.status === "unavailable";
      const cat = bare ? "" : categoryLabel(p, tr);
      quietAnnounce(bare ? tr("studio.live_set.announce_part_plain", i + 1, label)
        : p.status === "off" ? tr("studio.live_set.announce_part_off", i + 1, label, cat)
        : tr("studio.live_set.announce_part", i + 1, label, cat, p.volumePct));
    }
    liveUi.partListeners.forEach((cb) => { try { cb(i, p, liveCtl.liveSetId); } catch (e) { /* a listener never breaks the view */ } });
    document.dispatchEvent(new CustomEvent("velora:part-selected", { detail: { liveSetId: liveCtl.liveSetId, part: i, model: p } }));
  }

  const liveCtl = new LiveSetViewController({
    request: (r) => window.veloraDevice.request(r),
    onModel: renderLiveModel,
    onError: showLiveError,
    onSelectPart: onRealPartSelected,
    onFetched: () => { liveEls.error.hidden = true; }, // the banner goes away only when a read succeeded
    onLoading: (on) => {
      if (on && !liveCtl.model) renderPairedEditor();
      if (!on && !liveCtl.model) renderPairedEditor(); // a first read that failed: the "Reading the Live Set" title must not stay
    },
  });

  // ---- "+ Add timbre" picker (Story 9.11) -----------------------------------------------------------------------------------------------------------
  // An empty part strip (click, Enter or Space) opens a modal picker of the timbres installed on the unit (Library API through the bridge, read-only). Choosing one sends a
  // single SetPartTimbre with the same base_revision as every Part Editor edit; the unit rebuilds the part and the Live Set view is read again (the strip, the diamond and the
  // Footer follow). Esc / Cancel change nothing. timbre_picker.js is the model, timbre_picker_dom.js the DOM.
  const tpHost = document.createElement("div");
  tpHost.className = "tp-host";
  document.body.appendChild(tpHost);
  const timbrePick = {
    ctl: new TimbrePickerController({
      listTimbres: () => window.veloraDevice.libraryTimbres(),
      assign: assignPickedTimbre,
      categoryLabel: (it) => (it.categoryKey ? tr(it.categoryKey) : ""),
      onChange: () => timbrePick.view.render(),
      onClose: (reason, detail, ctx) => {
        timbrePick.view.restoreFocus();
        const n = ctx ? ctx.part + 1 : 0;
        if (reason === "cancelled") quietAnnounce(tr("studio.timbre_picker.closed"));
        else if (reason === "aborted") quietAnnounce(tr(detail === "disconnected" ? "studio.timbre_picker.disconnected" : "studio.timbre_picker.changed"));
        else if (reason === "stale") quietAnnounce(localeMgr.tr("studio.part_editor.stale_banner")); // the Part Editor's banner (with Reload) is the place that explains it
        void n;
      },
    }),
    view: null,
  };
  liveUi.picker = timbrePick;
  timbrePick.view = new TimbrePickerView({
    doc: document, host: tpHost, tr, controller: timbrePick.ctl, announce: quietAnnounce, categoryLabel: (it) => (it.categoryKey ? tr(it.categoryKey) : ""),
  });

  // The assignment the picker sends: refused (nothing sent) when the part is no longer an empty strip of the Live Set the picker was opened on; otherwise one SetPartTimbre through
  // the Part Editor controller (base_revision, STALE_REVISION banner). On success the Live Set view is read again and the result is announced; a library that is gone shows the
  // strip's own "Missing library" state (the unit takes the reference, it never substitutes another timbre).
  async function assignPickedTimbre({ part, libraryId, instrumentKey, liveSetId }) {
    const m = liveCtl.model;
    if (!m || liveCtl.liveSetId !== liveSetId) return { ok: false, error: { code: 0, message: tr("studio.timbre_picker.changed") } };
    if (!m.parts[part] || m.parts[part].status !== "empty") return { ok: false, error: { code: 0, message: tr("studio.timbre_picker.not_empty", part + 1) } };
    const res = await partEditorCtl.assignTimbre({ part, libraryId, instrumentKey, liveSetId });
    if (res.ok) {
      const item = timbrePick.ctl.chosen;
      const picked = item ? item.name : "";
      await liveCtl.select(liveSetId); // the strip, the diamond and the footer are taken from the unit again
      const after = liveCtl.model && liveCtl.model.parts[part];
      if (after && after.status === "missing") quietAnnounce(tr("studio.timbre_picker.assigned_missing", part + 1));
      else quietAnnounce(tr("studio.timbre_picker.assigned", picked, part + 1));
      paintLiveEditFooter();
    }
    return res;
  }

  function openTimbrePicker(i) {
    const m = liveCtl.model;
    if (!pairedReal() || !m || setlistsUi.open || timbrePick.ctl.isOpen) return;
    const p = m.parts[i];
    if (!p || p.status !== "empty") return;
    if (!m.sounding) {
      quietAnnounce(tr("studio.timbre_picker.not_sounding"));
      if (liveEls.error) { liveEls.error.hidden = false; liveEls.error.textContent = tr("studio.timbre_picker.not_sounding"); }
      return;
    }
    const strip = liveUi.grids && liveUi.grids.partCells[i];
    timbrePick.view.open(strip || null);
    timbrePick.ctl.open({ liveSetId: liveCtl.liveSetId, part: i });
  }

  // ---- Write to Velora (Story 9.12) ---------------------------------------------------------------------------------------------------------------------
  // "Write to Velora" (title bar) opens a modal Studio Dialog for the Live Set that sounds and has unsaved edits: what changed, and the device's own scopes (Scene, Whole Live Set,
  // Save as new...). Confirming sends THE device command, WriteLiveSet (the one the unit's Write Dialog sends), carrying base_revision (a stale view is refused) after every edit still
  // waiting in the Part Editor's sender has been flushed. The Footer reads WRITING TO VELORA... and both Primary Buttons are disabled until the unit answers; success is announced
  // ("Written to Velora") and the diamond goes by itself: it is State.edits.current_dirty, read again. A failure is a Studio Banner and the edits stay on the unit (◆). The model is
  // write_to_velora.js, the DOM write_to_velora_dom.js.
  const writeUi = { ctl: null, view: null, banner: null };
  const wrHost = document.createElement("div");
  wrHost.className = "wr-host";
  document.body.appendChild(wrHost);
  const wrBannerEl = document.getElementById("write-banner");

  function paintWriteButton() {
    const b = document.getElementById("btn-write");
    if (!b || !pairedReal()) return;
    const a = writeUi.ctl.availability(liveCtl.liveSetId);
    b.disabled = !a.enabled;
    b.title = a.enabled ? tr("studio.write.tooltip_ready") : tr(a.reason);
  }

  // The names a new Live Set must not take, as the unit judges them: the loaded setlist's Live Sets (every Live Set when none is loaded). Only a hint: the unit stays the authority.
  function knownLiveSetNames() {
    const st = deviceStore.state;
    const loaded = st && st.setlists && st.setlists.loaded_setlist_id;
    const rows = (loaded ? sidebar.entries.get(loaded) : sidebar.all) || [];
    return new Set(rows.filter((r) => r && r.name).map((r) => foldName(r.name)));
  }

  writeUi.ctl = new WriteController({
    request: (r) => window.veloraDevice.request(r),
    flush: () => partEditorCtl.flush(),
    writeContext: (id) => partEditorCtl.writeContext(id),
    noteStale: (err, ctx) => partEditorCtl.noteStale(err, ctx),
    state: () => deviceStore.state,
    isPaired: () => pairedReal(),
    takenNames: knownLiveSetNames,
    deviceName: () => (deviceStore.status && deviceStore.status.model) || "Velora", // HelloAck.model ("Velora 73"); a plain "Velora" while the unit does not say
    onChange: () => {
      writeUi.view.render();
      writeUi.banner.render();
      paintWriteButton();
      paintLiveEditFooter();
    },
    onClose: (reason) => {
      writeUi.view.close();
      if (reason === "cancelled") quietAnnounce(tr("studio.write.closed_cancelled"));
    },
    onAnnounce: (a) => quietAnnounce(announceText(a, tr)),
    onWritten: () => afterWrite(),
    onStale: () => {
      // the Part Editor's own banner (with Reload) explains it when it is on screen; otherwise this banner does
      if (peEls.host.hidden || setlistsUi.open) writeUi.ctl.showBanner("stale");
    },
  });
  writeUi.view = new WriteDialogView({
    doc: document,
    host: wrHost,
    tr,
    controller: writeUi.ctl,
    // the Write button is disabled once everything is written: the focus then goes to the main area, never to nowhere
    restore: () => { const b = document.getElementById("btn-write"); return b && !b.disabled ? b : document.getElementById("main-region"); },
  });
  writeUi.banner = new WriteBannerView({ doc: document, el: wrBannerEl, tr, controller: writeUi.ctl, onReload: () => { partEditorCtl.reload().then((ok) => { if (ok) writeUi.ctl.dismissBanner(); }); } });

  // What the unit holds after a Write: the State (the footer and the diamond follow State.edits.current_dirty), the Live Set view and the sidebar (Save as new adds a Live Set).
  async function afterWrite() {
    try {
      const reply = await window.veloraDevice.request({ body: { GetState: {} } });
      const st = reply && reply.body && reply.body.State;
      if (st) {
        deviceStore.state = st;
        deviceStore.stateListeners.forEach((cb) => { try { cb(st); } catch (e) { /* ignore */ } });
        onDeviceState(st);
      }
    } catch (e) { /* the pushes that follow a Write carry the same State */ }
    if (liveCtl.liveSetId) liveCtl.select(liveCtl.liveSetId);
    sidebar.key = null;
    refreshSidebar();
    if (setlistsUi.open) setlistsCtl.refreshNow();
    paintLiveEditFooter();
    paintWriteButton();
  }

  async function openWriteDialog() {
    if (!pairedReal() || writeUi.ctl.isOpen) return;
    const id = liveCtl.liveSetId;
    const m = liveCtl.model;
    if (!id || !m) return;
    const a = writeUi.ctl.availability(id);
    if (!a.enabled) { quietAnnounce(tr(a.reason)); return; }
    const st = deviceStore.state;
    const loaded = st && st.setlists && st.setlists.loaded_setlist_id;
    if (loaded && !sidebar.entries.has(loaded)) await loadEntries(loaded); // the names of the loaded setlist, for the free-name suggestion
    writeUi.ctl.open({ liveSetId: id, liveSetName: m.name, factory: !!m.factory });
  }

  // For Story 9.9 (Part Editor): the selection of the Live Set view.
  window.veloraLiveSetView = {
    get liveSetId() { return liveCtl.liveSetId; },
    get selectedPart() { return liveCtl.selectedPart; },
    get model() { return liveCtl.model; },
    onPartSelected(cb) { liveUi.partListeners.add(cb); return () => liveUi.partListeners.delete(cb); },
  };

  // ---- sidebar: SETLISTS expands to the setlists and their Live Sets ----
  // The tree is rebuilt when its DATA changes (setlists, entries, expansion) or the sounding Live Set changes; the selected / sounding markers of the rows are
  // updated in place (updateSidebarMarkers). Reads go through a throttled single-flight runner (no request stream when the library revision keeps moving).
  async function refreshSidebarTask() {
    try {
      const reply = await window.veloraDevice.request(setlistsRequest("", true));
      const body = reply && reply.body;
      if (body && body.SetlistsReply) {
        sidebar.error = "";
        sidebar.setlists = setlistRows(body.SetlistsReply);
        sidebar.all = allLiveSetRows(body.SetlistsReply);
        if (sidebar.firstLoad) {
          sidebar.firstLoad = false;
          sidebar.setlists.filter((s) => s.loaded).forEach((s) => sidebar.expanded.add(s.id));
        }
        const ids = new Set(sidebar.setlists.map((s) => s.id));
        for (const id of [...sidebar.expanded]) if (id !== "__all__" && !ids.has(id)) sidebar.expanded.delete(id);
        for (const id of sidebar.expanded) if (id !== "__all__") await loadEntries(id);
      } else {
        sidebar.error = String((body && body.Error && body.Error.message) || "no reply");
      }
    } catch (e) {
      sidebar.error = String(e && e.message ? e.message : e);
    }
    renderSidebarReal();
  }
  const refreshSidebar = createThrottledRunner(refreshSidebarTask, { minMs: 400 });

  async function loadEntries(setlistId) {
    try {
      const reply = await window.veloraDevice.request(setlistsRequest(setlistId, false));
      const body = reply && reply.body;
      if (body && body.SetlistsReply) sidebar.entries.set(setlistId, entryRows(body.SetlistsReply).rows);
    } catch (e) { /* the group stays as it was; the next refresh retries */ }
  }

  function sidebarButton(key, cls, label) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "sidebar-item " + cls;
    b.dataset.key = key;
    const span = document.createElement("span");
    span.className = "sl-label";
    span.textContent = label;
    b.appendChild(span);
    return b;
  }

  // Selected / sounding markers of one Live Set row, set in place.
  function applyRowMarkers(b, row) {
    const selected = row.id === liveCtl.liveSetId;
    const playing = row.id === sidebar.playingId;
    b.classList.toggle("on", selected);
    b.classList.toggle("playing", playing);
    if (selected) b.setAttribute("aria-current", "true"); else b.removeAttribute("aria-current");
    const np = b.querySelector(".np");
    if (np) np.hidden = !playing;
    b.setAttribute("aria-label", row.name + (playing ? `, ${tr("studio.sidebar.sounding_suffix")}` : "") + (row.missing ? `, ${tr("studio.sidebar.entry_missing")}` : ""));
  }

  function liveSetButton(group, row) {
    const b = sidebarButton(`ls:${group}:${row.id}`, "sub", row.name);
    b.dataset.ls = row.id;
    b._row = row;
    const np = document.createElement("span");
    np.className = "np";
    np.setAttribute("aria-hidden", "true");
    np.textContent = "▸";
    b.insertBefore(np, b.firstChild);
    if (row.missing) {
      b.disabled = true;
      const miss = document.createElement("span");
      miss.className = "sl-miss";
      miss.textContent = tr("studio.sidebar.entry_missing");
      b.appendChild(miss);
    } else {
      b.addEventListener("click", () => selectLiveSet(row.id));
    }
    applyRowMarkers(b, row);
    return b;
  }

  function updateSidebarMarkers() {
    if (!pairedReal()) return;
    document.querySelectorAll("#sidebar-setlists-list [data-ls]").forEach((b) => applyRowMarkers(b, b._row));
  }

  function sidebarGroup(id, name, count, rows) {
    const wrap = document.createElement("div");
    wrap.className = "sl-group";
    const open = sidebar.expanded.has(id);
    const head = sidebarButton(`sl:${id}`, "sl-head", `${open ? "▾" : "▸"} ${name}`);
    head.setAttribute("aria-expanded", open ? "true" : "false");
    if (count !== null) {
      const c = document.createElement("span");
      c.className = "sl-count";
      c.textContent = String(count);
      head.appendChild(c);
    }
    head.addEventListener("click", async () => {
      if (sidebar.expanded.has(id)) sidebar.expanded.delete(id); else sidebar.expanded.add(id);
      renderSidebarReal();
      if (sidebar.expanded.has(id) && id !== "__all__" && !sidebar.entries.has(id)) {
        await loadEntries(id);
        renderSidebarReal();
      }
    });
    wrap.appendChild(head);
    if (open) {
      const kids = document.createElement("div");
      kids.setAttribute("role", "group");
      const list = rows || [];
      if (list.length === 0) {
        const hint = document.createElement("div");
        hint.className = "sidebar-item empty-hint sub";
        hint.textContent = tr("studio.sidebar.empty_entries");
        kids.appendChild(hint);
      }
      list.forEach((row) => kids.appendChild(liveSetButton(id, row)));
      wrap.appendChild(kids);
    }
    return wrap;
  }

  function renderSidebarReal() {
    if (!pairedReal()) return;
    const host = document.getElementById("sidebar-setlists-list");
    const focusKey = document.activeElement && host.contains(document.activeElement) ? document.activeElement.dataset.key : null;
    host.textContent = "";
    if (sidebar.error) {
      const e = document.createElement("div");
      e.className = "sidebar-item empty-hint";
      e.textContent = tr("studio.sidebar.setlists_error", sidebar.error);
      host.appendChild(e);
    } else if (sidebar.setlists.length === 0 && sidebar.all.length === 0) {
      const hint = document.createElement("div");
      hint.className = "sidebar-item empty-hint";
      hint.textContent = tr("studio.sidebar.empty_entries");
      host.appendChild(hint);
    }
    sidebar.setlists.forEach((s) => host.appendChild(sidebarGroup(s.id, s.name, s.count, sidebar.entries.get(s.id))));
    if (sidebar.all.length > 0) host.appendChild(sidebarGroup("__all__", tr("studio.sidebar.all_live_sets"), sidebar.all.length, sidebar.all));
    const lib = document.getElementById("sidebar-livesets-list");
    lib.textContent = "";
    const hint = document.createElement("div");
    hint.className = "sidebar-item empty-hint";
    hint.textContent = tr("studio.sidebar.library_paired");
    lib.appendChild(hint);
    if (focusKey) {
      const again = [...host.querySelectorAll("[data-key]")].find((b) => b.dataset.key === focusKey);
      if (again && !again.disabled) again.focus();
    }
  }

  function selectLiveSet(id) {
    closeSetlistsView(false); // choosing a Live Set in the sidebar goes back to the Live Set view
    liveCtl.select(id).then(() => updateSidebarMarkers());
    updateSidebarMarkers();
  }

  // ---- Setlists view (Story 9.13) -----------------------------------------------------------------------------------------------------------------------
  // SETLISTS > "Manage setlists" opens a view in the main area: the setlists and the Live Sets of the selected one, with create / rename / delete / reorder (Alt+Up and
  // Alt+Down, "Move to...", drag and drop), Add / Remove / New Live Set and tags. Every change is the same ws.v1 control-thread command the unit's own screen sends
  // (setlists_view.js: optimistic, rolled back on an error, read again afterwards); setlists_dom.js draws it. It never loads, steps or edits a Live Set.
  const setlistsUi = { open: false, view: null };
  const setlistsCtl = new SetlistsController({
    request: (r) => window.veloraDevice.request(r),
    onChange: () => { if (setlistsUi.view) setlistsUi.view.render(); },
    onError: (e) => { if (setlistsUi.view) setlistsUi.view.showError(e); },
    onAnnounce: (m) => quietAnnounce(tr(m.key, ...m.args)),
    onNotice: (m) => { if (setlistsUi.view) setlistsUi.view.showNotice(m); },
    missingLabel: () => tr("studio.sidebar.entry_missing"),
  });
  const manageBtn = document.getElementById("sidebar-manage-setlists");

  function updateManageButton() {
    if (!manageBtn) return;
    const on = pairedReal();
    manageBtn.disabled = !on;
    manageBtn.title = on ? "" : tr("studio.offline.hint"); // offline / unpaired: disabled with the existing offline hint
  }

  function ensureSetlistsView() {
    if (setlistsUi.view) return;
    setlistsUi.view = new SetlistsView({
      doc: document,
      host: document.getElementById("setlists-view"),
      tr,
      controller: setlistsCtl,
      announce: quietAnnounce,
      onClose: () => closeSetlistsView(true),
    });
  }

  function openSetlistsView() {
    if (!pairedReal()) return;
    ensureSetlistsView();
    setlistsUi.open = true;
    document.getElementById("offline-empty-card").style.display = "none";
    liveEls.container.style.display = "none";
    setlistsUi.view.show();
    setlistsCtl.refreshNow();
    setlistsUi.view.title.focus();
  }

  function hideSetlistsView() {
    if (!setlistsUi.open) return;
    setlistsUi.open = false;
    if (setlistsUi.view) setlistsUi.view.hide();
  }

  function closeSetlistsView(focusSidebar) {
    if (!setlistsUi.open) return;
    hideSetlistsView();
    renderEditor();
    if (focusSidebar && manageBtn) manageBtn.focus();
  }

  manageBtn?.addEventListener("click", openSetlistsView);
  updateManageButton();

  // What every State push does for the view: the model follows it at once; the sidebar follows the setlists' revision (throttled) and the sounding Live Set; and the
  // first time a Live Set sounds with nothing selected, that one is shown.
  function onDeviceState(st) {
    if (!st || !pairedReal()) return;
    liveCtl.onState(st);
    partEditorCtl.onState(st);
    writeUi.ctl.onState(st); // Story 9.12: an open Write dialog follows it; an unconfirmed Write is judged by it after a reconnect
    paintLiveEditFooter();
    paintWriteButton();
    if (setlistsUi.open) setlistsCtl.onState(st);
    const setlists = st.setlists || {};
    const key = `${setlists.library_revision || 0}/${setlists.loaded_setlist_id || ""}`;
    const playing = st.live_set_loaded ? st.live_set_id || "" : "";
    if (key !== sidebar.key) {
      sidebar.key = key;
      refreshSidebar();
    }
    if (playing !== sidebar.playingId) {
      sidebar.playingId = playing;
      updateSidebarMarkers();
    }
    if (!setlistsUi.open && !liveCtl.liveSetId && !liveCtl.pendingId && playing && sidebar.autoTried !== playing) {
      sidebar.autoTried = playing; // once per sounding Live Set: a refused read does not turn into a request per push
      selectLiveSet(playing);
    }
  }

  // Query Tauri backend for initial connection status and device state
  if (window.__TAURI__?.core?.invoke) {
    window.__TAURI__.core.invoke("get_connection_status").then((conn) => {
      if (conn?.connected && !deviceLinkActive) {
        isConnected = true;
        deviceHasFmVa = conn.has_fm_va;
        deviceFirmwareVersion = conn.firmware_version || "1.4.0";

        const connText = document.querySelector("#connection-status .conn-text");
        if (connText) connText.textContent = conn.device_name || "Velora 73 connected";

        window.__TAURI__.core.invoke("get_current_device_state").then((state) => {
          currentDeviceState = state;
          renderEditor();
        }).catch(() => {
          renderEditor();
        });
      }
    }).catch(() => {
      // Mock defaults for preview/testing
      currentDeviceState = {
        device_model: "Velora 73",
        firmware_version: "1.4.0",
        has_fm_va: true,
        live_sets: [
          {
            id: "ls-perfect",
            name: "Live Set 03 · Perfect",
            parts: [
              { volume: 0.72 },
              { volume: 0.65, engine: { engine_type_id: 3 } },
              { volume: 0.58 },
              { volume: 0.55, engine: { engine_type_id: 4 } },
              { volume: 0.58 },
              { volume: 0.64 },
              { volume: 0.50 },
              { volume: 0.44 }
            ]
          }
        ]
      };
      renderEditor();
    });
  } else {
    // Web preview mode default
    currentDeviceState = {
      device_model: "Velora 73",
      firmware_version: "1.4.0",
      has_fm_va: true,
      live_sets: [
        {
          id: "ls-perfect",
          name: "Live Set 03 · Perfect",
          parts: [
            { volume: 0.72 },
            { volume: 0.65, engine: { engine_type_id: 3 } },
            { volume: 0.58 },
            { volume: 0.55, engine: { engine_type_id: 4 } },
            { volume: 0.58 },
            { volume: 0.64 },
            { volume: 0.50 },
            { volume: 0.44 }
          ]
        }
      ]
    };
    renderEditor();
  }

  // ---- Real device link (Foundation: Velora Studio device connection) ------------------------------------------------------------------------------
  // The Rust side (src-tauri/src/device.rs) connects to the unit through the sysd bridge. Until a device address is configured (device_connect, or the
  // VELORA_DEVICE=host:port environment variable) the simulated device above stays the offline fallback. Once configured, the title-bar indicator and the
  // offline explanation follow the real status: disconnected | connecting | unpaired | paired | version-mismatch. Stories 9.8 / 9.13 read the State store.
  const deviceStore = {
    status: null,
    state: null,
    stateListeners: new Set(),
    statusListeners: new Set(),
  };
  window.veloraDevice = {
    get status() { return deviceStore.status; },
    get state() { return deviceStore.state; },
    onState(cb) { deviceStore.stateListeners.add(cb); return () => deviceStore.stateListeners.delete(cb); },
    onStatus(cb) { deviceStore.statusListeners.add(cb); return () => deviceStore.statusListeners.delete(cb); },
    connect(host, port) { return window.__TAURI__.core.invoke("device_connect", { host, port }); },
    pair(code, name) { return window.__TAURI__.core.invoke("device_pair", { code, name }); },
    disconnect() { return window.__TAURI__.core.invoke("device_disconnect"); },
    // request: a ws.v1 ClientMessage as JSON, e.g. { body: { GetState: {} } }; resolves to the ServerMessage as JSON.
    request(request) { return window.__TAURI__.core.invoke("device_request", { request }); },
    // Story 9.11: the timbres installed on the unit (Library API through the bridge, read-only): { timbres: [{ library_id, library_name, instrument_key, name, category }], total, truncated }.
    libraryTimbres() { return window.__TAURI__.core.invoke("device_library_timbres"); },
  };
  let pairPromptedFor = null;
  function applyDeviceStatus(st) {
    if (!st || !st.configured) return; // no real device configured: the simulation stays
    const wasLinked = deviceLinkActive;
    deviceLinkActive = true;
    deviceStore.status = st;
    const paired = st.status === "paired";
    const connText = document.querySelector("#connection-status .conn-text");
    const footerDev = document.getElementById("footer-device-status");
    const hintEl = document.querySelector("#offline-empty-card .offline-hint");
    let label;
    let hint;
    switch (st.status) {
      case "paired":
        label = localeMgr.tr("studio.conn.paired").replace("%s", st.model || "Velora");
        break;
      case "unpaired":
        label = localeMgr.tr("studio.conn.unpaired");
        hint = localeMgr.tr("studio.offline.hint_unpaired");
        break;
      case "version-mismatch":
        label = localeMgr.tr("studio.conn.version_mismatch");
        hint = localeMgr.tr("studio.offline.hint_mismatch");
        break;
      case "connecting":
        label = localeMgr.tr("studio.conn.connecting");
        hint = localeMgr.tr("studio.offline.hint_connecting");
        break;
      default:
        label = localeMgr.tr("studio.conn.no_device");
        hint = localeMgr.tr("studio.offline.hint_unreachable");
    }
    if (connText) connText.textContent = label;
    if (footerDev) footerDev.textContent = label;
    if (hintEl && hint) hintEl.textContent = hint;
    const connEl = document.getElementById("connection-status");
    if (connEl) connEl.dataset.link = st.status;
    const becameConnected = paired && (!isConnected || !wasLinked);
    const wasPaired = deviceLinkPaired;
    isConnected = paired;
    deviceLinkPaired = paired;
    if (paired) {
      if (st.firmware_version) deviceFirmwareVersion = st.firmware_version;
      // Story 9.8: the main area is the real Live Set view. The setlists are read again on every (re)pairing, and the Live Set that was shown is read again too
      // (the unit may have changed while the link was down); the State store then feeds it.
      if (!wasPaired || becameConnected) {
        sidebar.key = null;
        renderEditor();
        if (liveCtl.liveSetId) liveCtl.select(liveCtl.liveSetId);
        // No State is replayed here: the one in the store may be from before an outage. The first push after (re)pairing feeds the view (the client subscribes at once).
      }
      pairPromptedFor = null;
    } else {
      currentDeviceState = null;
      deviceStore.state = null; // a State from before the outage is never shown again
      writeUi.ctl.onLinkLost(); // Story 9.12: an open Write dialog closes (nothing sent); a Write that is out stays UNKNOWN until the unit is read again
      if (wasPaired) {
        const wb = document.getElementById("btn-write");
        if (wb) { wb.disabled = true; wb.title = tr("studio.offline.hint"); }
      }
      liveCtl.reset();
      partEditorCtl.reset();
      timbrePick.ctl.abort("disconnected"); // Story 9.11: an open picker closes; nothing is assigned by it
      if (pe.view) { pe.view.clear(); pe.view = null; }
      hideSetlistsView(); // the Setlists view is disabled without a paired device
      setlistsCtl.reset();
      sidebar.setlists = [];
      sidebar.all = [];
      sidebar.entries.clear();
      sidebar.key = null;
      sidebar.playingId = "";
      sidebar.firstLoad = true;
      sidebar.autoTried = "";
      renderEditor(); // the existing offline card (and any loaded backup)
    }
    updateManageButton();
    deviceStore.statusListeners.forEach((cb) => { try { cb(st); } catch (e) { /* a listener never breaks the link */ } });
    if (paired) markLiveEdit(false); // refreshes the Write button state for the simulated data
    if (st.status !== "unpaired") {
      pairPromptedFor = null;
      closePairDialog();
    } else if (pairPromptedFor !== st.host + ":" + st.port) {
      pairPromptedFor = st.host + ":" + st.port;
      openPairDialog();
    }
  }

  // Placeholder pairing dialog (the real one is Story 9.6): a minimal in-page input (window.prompt is not available in Tauri 2's webviews). A wrong code
  // keeps it open with the reason; Cancel closes it and clicking the connection indicator while "unpaired" opens it again.
  let pairDialogEl = null;
  function closePairDialog() {
    if (pairDialogEl) pairDialogEl.remove();
    pairDialogEl = null;
  }
  function openPairDialog() {
    if (pairDialogEl) return;
    const overlay = document.createElement("div");
    overlay.className = "pair-dialog-backdrop";
    const box = document.createElement("div");
    box.className = "pair-dialog";
    box.setAttribute("role", "dialog");
    box.setAttribute("aria-modal", "true");
    const label = document.createElement("label");
    label.textContent = localeMgr.tr("studio.pair.prompt");
    const input = document.createElement("input");
    input.type = "text";
    input.inputMode = "numeric";
    input.autocomplete = "off";
    input.maxLength = 12;
    label.appendChild(input);
    const err = document.createElement("div");
    err.className = "pair-dialog-error";
    err.setAttribute("role", "alert");
    const row = document.createElement("div");
    row.className = "pair-dialog-actions";
    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.className = "btn";
    cancel.textContent = localeMgr.tr("studio.pair.cancel");
    const ok = document.createElement("button");
    ok.type = "button";
    ok.className = "btn btn-primary";
    ok.textContent = localeMgr.tr("studio.pair.ok");
    row.append(cancel, ok);
    box.append(label, err, row);
    overlay.appendChild(box);
    document.body.appendChild(overlay);
    pairDialogEl = overlay;
    input.focus();
    const submit = () => {
      const code = input.value.trim();
      if (!code) return;
      ok.disabled = true;
      const name = (navigator.platform || "Computer").slice(0, 60);
      window.veloraDevice.pair(code, name).then(closePairDialog).catch((e) => {
        ok.disabled = false;
        err.textContent = localeMgr.tr("studio.pair.failed").replace("%s", String(e));
        input.select();
      });
    };
    ok.addEventListener("click", submit);
    input.addEventListener("keydown", (e) => {
      if (e.key === "Enter") submit();
      if (e.key === "Escape") closePairDialog();
    });
    cancel.addEventListener("click", closePairDialog);
  }
  document.getElementById("connection-status")?.addEventListener("click", () => {
    if (deviceStore.status && deviceStore.status.status === "unpaired") openPairDialog();
  });
  if (window.__TAURI__?.event?.listen && window.__TAURI__?.core?.invoke) {
    window.__TAURI__.event.listen("device://status", (e) => applyDeviceStatus(e.payload));
    window.__TAURI__.event.listen("device://state", (e) => {
      deviceStore.state = e.payload;
      deviceStore.stateListeners.forEach((cb) => { try { cb(e.payload); } catch (err) { /* ignore */ } });
      onDeviceState(e.payload); // Story 9.8: the Live Set view follows the pushes in place
    });
    window.__TAURI__.core.invoke("device_status").then(applyDeviceStatus).catch(() => {});
    window.__TAURI__.core.invoke("device_state").then((st) => { if (st) { deviceStore.state = st; onDeviceState(st); } }).catch(() => {});
  }

  // Backup, Restore and Export actions (Story 9.16, 9.17, 9.18)
  const exportHost = document.getElementById("export-dialog-host") || document.body;
  let exportCtl = null;
  let exportView = null;

  async function openExportDialog({ scope = EXPORT_SCOPE.BUNDLE, preselectLiveSetId = null, preselectTimbreKey = null } = {}) {
    let currentLiveSets = [];
    let currentTimbres = [];

    // Try to get live sets from state or device
    if (window.__TAURI__?.core?.invoke) {
      try {
        const state = await window.__TAURI__.core.invoke("get_current_device_state");
        if (state && Array.isArray(state.live_sets)) {
          currentLiveSets = state.live_sets;
        }
      } catch (e) {
        // fallback
      }
      try {
        const timbresResp = await window.__TAURI__.core.invoke("device_library_timbres");
        if (timbresResp && Array.isArray(timbresResp.timbres)) {
          currentTimbres = timbresResp.timbres;
        }
      } catch (e) {
        // fallback
      }
    }

    if (currentLiveSets.length === 0 && deviceStore?.state?.live_sets) {
      currentLiveSets = deviceStore.state.live_sets;
    }

    const defaultExt = scope === EXPORT_SCOPE.TIMBRES ? ".veloratimbre" : ".velorasets";
    const defaultName = `Export_${new Date().toISOString().slice(0, 10)}${defaultExt}`;

    if (!exportCtl) {
      exportCtl = new ExportController({
        liveSets: currentLiveSets,
        timbres: currentTimbres,
        onExport: async (params) => {
          const footerBackup = document.getElementById("footer-backup-status");
          const originalFooter = footerBackup ? footerBackup.textContent : "";
          if (footerBackup) {
            footerBackup.textContent = localeMgr.tr("studio.export.running_footer").replace("%s", "0 B").replace("%s", "...");
          }
          try {
            const target = params.target_path || defaultName;
            const fullParams = {
              target_path: target,
              scope: params.scope,
              live_set_ids: params.live_set_ids,
              timbre_keys: params.timbre_keys,
              user_bundles: {},
            };
            if (window.__TAURI__?.core?.invoke) {
              const res = await window.__TAURI__.core.invoke("export_selection", { params: fullParams });
              exportCtl.finishExport({
                fileName: res.file_name,
                liveSetCount: res.live_set_count,
                timbreCount: res.timbre_count,
              });
              exportView.close();
            } else {
              exportCtl.finishExport({
                fileName: target,
                liveSetCount: params.live_set_ids.length,
                timbreCount: params.timbre_keys.length,
              });
              exportView.close();
            }
          } catch (err) {
            announce(localeMgr.tr("studio.export.announced_failed").replace("%s", String(err)), true);
            exportCtl.cancelExport();
          } finally {
            if (footerBackup) footerBackup.textContent = originalFooter;
          }
        },
        onCancel: () => {
          if (window.__TAURI__?.core?.invoke) {
            window.__TAURI__.core.invoke("cancel_export").catch(() => {});
          }
        },
        onAnnounce: (msg) => {
          announce(msg);
        },
      });

      exportView = new ExportDialogView({
        doc: document,
        host: exportHost,
        tr: (k, ...args) => {
          let s = localeMgr.tr(k);
          for (const a of args) {
            s = s.replace(/%[ds]/, a);
          }
          return s;
        },
        controller: exportCtl,
        restore: document.getElementById("btn-export") || document.getElementById("main-region"),
      });

      exportCtl.onChange = () => {
        exportView.render();
      };
    } else {
      exportCtl.setAvailableData({ liveSets: currentLiveSets, timbres: currentTimbres });
    }

    exportCtl.open({
      scope,
      preselectLiveSetId,
      preselectTimbreKey,
      targetPath: defaultName,
    });
    exportView.open();
  }

  document.getElementById("btn-export")?.addEventListener("click", () => {
    openExportDialog({ scope: EXPORT_SCOPE.BUNDLE });
  });

  document.getElementById("btn-offline-export")?.addEventListener("click", () => {
    openExportDialog({ scope: EXPORT_SCOPE.BUNDLE });
  });

  // Context menu right-click on Live Set or Timbre
  document.addEventListener("contextmenu", (e) => {
    const liveSetRow = e.target.closest && e.target.closest("[data-live-set-id]");
    if (liveSetRow) {
      e.preventDefault();
      const lsId = liveSetRow.getAttribute("data-live-set-id");
      openExportDialog({ scope: EXPORT_SCOPE.SETS, preselectLiveSetId: lsId });
      return;
    }
    const partStrip = e.target.closest && e.target.closest("[data-part-index]");
    if (partStrip) {
      const partIdx = parseInt(partStrip.getAttribute("data-part-index"), 10);
      const activeLs = liveCtl?.model;
      const part = activeLs?.parts && activeLs.parts[partIdx];
      if (part && part.timbre && part.timbre.library_id) {
        e.preventDefault();
        const timbreKey = `${part.timbre.library_id}:${part.timbre.instrument_key || ""}`;
        openExportDialog({ scope: EXPORT_SCOPE.TIMBRES, preselectTimbreKey: timbreKey });
      }
    }
  });

  // Backup and Restore actions
  document.getElementById("btn-backup")?.addEventListener("click", () => {
    const defaultName = `Velora_Backup_${new Date().toISOString().slice(0, 10)}.velorabackup`;
    const targetPath = prompt("Enter backup file location/name:", defaultName);
    if (!targetPath) return;

    if (window.__TAURI__?.core?.invoke) {
      window.__TAURI__.core.invoke("create_backup", { filePath: targetPath })
        .then((b) => {
          const nowStr = new Date().toLocaleString();
          localStorage.setItem("velora_last_backup_" + currentDeviceId, nowStr);
          announce(localeMgr.tr("studio.backup.success"));
        })
        .catch((err) => {
          announce("Backup failed: " + err, true);
        });
    } else {
      announce(localeMgr.tr("studio.backup.success"));
    }
  });

  const restoreFileInput = document.getElementById("restore-file-input");
  document.getElementById("btn-restore")?.addEventListener("click", () => {
    restoreFileInput?.click();
  });
});
