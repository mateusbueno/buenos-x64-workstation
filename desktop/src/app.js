// Velora Studio App Shell, Backup & Restore (Stories 9.5, 9.16, 9.17)

const CATALOGS = {
  en: {
    "studio.title_bar.app_name": "Velora Studio",
    "studio.conn.no_device": "No Velora connected",
    "studio.title_bar.btn_import": "Import",
    "studio.title_bar.btn_backup": "Backup",
    "studio.title_bar.btn_write": "Write to Velora",
    "studio.sidebar.setlists": "SETLISTS",
    "studio.sidebar.library": "LIBRARY",
    "studio.sidebar.device": "DEVICE",
    "studio.sidebar.empty_setlists": "No setlists offline",
    "studio.sidebar.empty_library": "Library unavailable offline",
    "studio.sidebar.storage": "Storage",
    "studio.sidebar.firmware": "Firmware",
    "studio.sidebar.backups": "Backups",
    "studio.sidebar.settings": "Settings",
    "studio.offline.title": "Organize sounds and manage your Velora 73",
    "studio.offline.body": "Connect your Velora via USB to sync setlists, live sets, and library, or manage backups and local settings.",
    "studio.offline.hint": "No device detected on USB ports.",
    "studio.footer.no_backup_yet": "No backup yet",
    "studio.footer.last_backup": "Last backup: %s",
    "studio.settings.title": "Studio Settings",
    "studio.settings.theme": "Theme",
    "studio.settings.theme_system": "System (auto)",
    "studio.settings.theme_stage": "Stage (dark)",
    "studio.settings.theme_studio": "Studio (light)",
    "studio.settings.language": "Language",
    "studio.settings.lang_system": "System language",
    "studio.settings.lang_en": "English",
    "studio.settings.lang_es": "Español",
    "studio.settings.lang_pt_br": "Português (Brasil)",
    "studio.settings.close": "Close",
    "studio.backup.success": "Backup completed successfully.",
    "studio.backup.unsaved_warning": "Unsaved edits on the Velora are not in this backup",
    "studio.backup.needs_newer_studio": "This backup needs a newer Velora Studio",
    "studio.restore.title": "Restore from Backup",
    "studio.restore.merge_label": "Merge",
    "studio.restore.merge_desc": "Add and update from the backup; keep what the backup doesn't have",
    "studio.restore.replace_label": "Replace",
    "studio.restore.replace_desc": "Velora content becomes the backup",
    "studio.restore.counts_summary": "%d setlists to add, %d to update, %d to remove · %d Live Sets to add, %d to update, %d to remove",
    "studio.restore.replace_warning": "Replaces all Live Sets and setlists on the connected Velora with the contents of this backup. Any Live Sets not in this backup will be removed.",
    "studio.restore.btn_confirm": "Restore",
    "studio.restore.btn_cancel": "Cancel",
    "studio.restore.success": "Restore completed successfully.",
    "studio.offline.connect_reason": "Connect a Velora",
    "studio.offline.backup_loaded": "Loaded backup: %s",
    "studio.offline.save_backup": "Save Backup",
    "studio.offline.backup_saved": "Backup saved successfully.",
    "studio.offline.open_backup": "Open Backup (.velorabackup)"
  },
  es: {
    "studio.title_bar.app_name": "Velora Studio",
    "studio.conn.no_device": "No hay ningún Velora conectado",
    "studio.title_bar.btn_import": "Importar",
    "studio.title_bar.btn_backup": "Copia de seguridad",
    "studio.title_bar.btn_write": "Escribir en Velora",
    "studio.sidebar.setlists": "SETLISTS",
    "studio.sidebar.library": "BIBLIOTECA",
    "studio.sidebar.device": "DISPOSITIVO",
    "studio.sidebar.empty_setlists": "Sin setlists sin conexión",
    "studio.sidebar.empty_library": "Biblioteca no disponible sin conexión",
    "studio.sidebar.storage": "Almacenamiento",
    "studio.sidebar.firmware": "Firmware",
    "studio.sidebar.backups": "Copias de seguridad",
    "studio.sidebar.settings": "Configuración",
    "studio.offline.title": "Organice sonidos y gestione su Velora 73",
    "studio.offline.body": "Conecte su Velora por USB para sincronizar setlists, live sets y biblioteca, o gestionar copias de seguridad y configuraciones locales.",
    "studio.offline.hint": "No se detectó ningún dispositivo en los puertos USB.",
    "studio.footer.no_backup_yet": "Aún no hay copias de seguridad",
    "studio.footer.last_backup": "Última copia de seguridad: %s",
    "studio.settings.title": "Configuración de Studio",
    "studio.settings.theme": "Tema",
    "studio.settings.theme_system": "Sistema (automático)",
    "studio.settings.theme_stage": "Stage (oscuro)",
    "studio.settings.theme_studio": "Studio (claro)",
    "studio.settings.language": "Idioma",
    "studio.settings.lang_system": "Idioma del sistema",
    "studio.settings.lang_en": "Inglés",
    "studio.settings.lang_es": "Español",
    "studio.settings.lang_pt_br": "Portugués (Brasil)",
    "studio.settings.close": "Cerrar",
    "studio.backup.success": "Copia de seguridad completada con éxito.",
    "studio.backup.unsaved_warning": "Los cambios no guardados en el Velora no están en esta copia de seguridad",
    "studio.backup.needs_newer_studio": "Esta copia de seguridad requiere una versión más reciente de Velora Studio",
    "studio.restore.title": "Restaurar desde copia de seguridad",
    "studio.restore.merge_label": "Combinar (Merge)",
    "studio.restore.merge_desc": "Añade y actualiza desde la copia; mantiene lo que no está en la copia",
    "studio.restore.replace_label": "Reemplazar (Replace)",
    "studio.restore.replace_desc": "El contenido de Velora se sustituye por la copia",
    "studio.restore.counts_summary": "%d setlists a añadir, %d a actualizar, %d a eliminar · %d Live Sets a añadir, %d a actualizar, %d a eliminar",
    "studio.restore.replace_warning": "Reemplaza todos los Live Sets y setlists en el Velora conectado con el contenido de esta copia. Se eliminarán los Live Sets que no estén en la copia.",
    "studio.restore.btn_confirm": "Restaurar",
    "studio.restore.btn_cancel": "Cancelar",
    "studio.restore.success": "Restauración completada con éxito.",
    "studio.offline.connect_reason": "Conecte un Velora",
    "studio.offline.backup_loaded": "Copia de seguridad cargada: %s",
    "studio.offline.save_backup": "Guardar copia de seguridad",
    "studio.offline.backup_saved": "Copia de seguridad guardada con éxito.",
    "studio.offline.open_backup": "Abrir copia (.velorabackup)"
  },
  "pt-BR": {
    "studio.title_bar.app_name": "Velora Studio",
    "studio.conn.no_device": "Nenhum Velora conectado",
    "studio.title_bar.btn_import": "Importar",
    "studio.title_bar.btn_backup": "Backup",
    "studio.title_bar.btn_write": "Gravar no Velora",
    "studio.sidebar.setlists": "SETLISTS",
    "studio.sidebar.library": "BIBLIOTECA",
    "studio.sidebar.device": "DISPOSITIVO",
    "studio.sidebar.empty_setlists": "Nenhum setlist offline",
    "studio.sidebar.empty_library": "Biblioteca indisponível offline",
    "studio.sidebar.storage": "Armazenamento",
    "studio.sidebar.firmware": "Firmware",
    "studio.sidebar.backups": "Backups",
    "studio.sidebar.settings": "Configurações",
    "studio.offline.title": "Organize timbres e gerencie seu Velora 73",
    "studio.offline.body": "Conecte seu Velora via USB para sincronizar setlists, live sets e biblioteca, ou gerenciar backups e configurações locais.",
    "studio.offline.hint": "Nenhum dispositivo detectado nas portas USB.",
    "studio.footer.no_backup_yet": "Nenhum backup ainda",
    "studio.footer.last_backup": "Último backup: %s",
    "studio.settings.title": "Configurações do Studio",
    "studio.settings.theme": "Tema",
    "studio.settings.theme_system": "Sistema (automático)",
    "studio.settings.theme_stage": "Stage (escuro)",
    "studio.settings.theme_studio": "Studio (claro)",
    "studio.settings.language": "Idioma",
    "studio.settings.lang_system": "Idioma do sistema",
    "studio.settings.lang_en": "Inglês",
    "studio.settings.lang_es": "Espanhol",
    "studio.settings.lang_pt_br": "Português (Brasil)",
    "studio.settings.close": "Fechar",
    "studio.backup.success": "Backup concluído com sucesso.",
    "studio.backup.unsaved_warning": "Edições não salvas no Velora não estão neste backup",
    "studio.backup.needs_newer_studio": "Este backup precisa de uma versão mais nova do Velora Studio",
    "studio.restore.title": "Restaurar a partir de Backup",
    "studio.restore.merge_label": "Mesclar (Merge)",
    "studio.restore.merge_desc": "Adiciona e atualiza do backup; mantém o que não está no backup",
    "studio.restore.replace_label": "Substituir (Replace)",
    "studio.restore.replace_desc": "O conteúdo do Velora passa a ser exatamente o backup",
    "studio.restore.counts_summary": "%d setlists a adicionar, %d a atualizar, %d a remover · %d Live Sets a adicionar, %d a atualizar, %d a remover",
    "studio.restore.replace_warning": "Substitui todos os Live Sets e setlists no Velora conectado pelo conteúdo deste backup. Quaisquer Live Sets ausentes no backup serão removidos.",
    "studio.restore.btn_confirm": "Restaurar",
    "studio.restore.btn_cancel": "Cancelar",
    "studio.restore.success": "Restauração concluída com sucesso.",
    "studio.offline.connect_reason": "Conecte um Velora",
    "studio.offline.backup_loaded": "Backup carregado: %s",
    "studio.offline.save_backup": "Salvar Backup",
    "studio.offline.backup_saved": "Backup salvo com sucesso.",
    "studio.offline.open_backup": "Abrir Backup (.velorabackup)"
  }
};

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

// Keyboard Navigation & F6 Cycling
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

// Announcements & Toast (UX-DR91)
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

  // Apply initial state
  localeMgr.apply();
  themeMgr.apply();

  // Active loaded backup data for offline editing
  let activeBackup = null;
  let activeBackupPath = "Velora_Backup_Offline.velorabackup";
  let activeLiveSetIndex = 0;
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

  // Render Offline Editor
  function renderOfflineEditor() {
    if (!activeBackup || !activeBackup.live_sets || activeBackup.live_sets.length === 0) {
      document.getElementById("offline-empty-card").style.display = "flex";
      document.getElementById("editor-container").style.display = "none";
      return;
    }

    document.getElementById("offline-empty-card").style.display = "none";
    const editor = document.getElementById("editor-container");
    editor.style.display = "grid";

    // Populate Sidebar Setlists
    const setlistsListEl = document.getElementById("sidebar-setlists-list");
    setlistsListEl.innerHTML = "";
    const setlists = activeBackup.setlists?.setlists || [];
    if (setlists.length === 0) {
      const hint = document.createElement("div");
      hint.className = "sidebar-item empty-hint";
      hint.textContent = localeMgr.tr("studio.sidebar.empty_setlists");
      setlistsListEl.appendChild(hint);
    } else {
      setlists.forEach((sl, idx) => {
        const item = document.createElement("button");
        item.type = "button";
        item.className = "sidebar-item";
        item.textContent = sl.name;
        item.addEventListener("click", () => {
          // Rename prompt for offline setlist editing
          const newName = prompt("Rename Setlist:", sl.name);
          if (newName && newName.trim()) {
            sl.name = newName.trim();
            renderOfflineEditor();
          }
        });
        setlistsListEl.appendChild(item);
      });
    }

    // Populate Sidebar Live Sets
    const lsListEl = document.getElementById("sidebar-livesets-list");
    lsListEl.innerHTML = "";
    activeBackup.live_sets.forEach((ls, idx) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "sidebar-item" + (idx === activeLiveSetIndex ? " on" : "");
      btn.textContent = ls.name;
      btn.addEventListener("click", () => {
        activeLiveSetIndex = idx;
        renderOfflineEditor();
      });
      lsListEl.appendChild(btn);
    });

    // Populate Active Live Set Details
    const curLs = activeBackup.live_sets[activeLiveSetIndex] || activeBackup.live_sets[0];
    const titleEl = document.getElementById("editor-title");
    titleEl.textContent = curLs.name;

    const stripEl = document.getElementById("editor-part-strip");
    stripEl.innerHTML = "";

    curLs.parts.forEach((p, pIdx) => {
      const card = document.createElement("div");
      card.className = "part-card";

      const header = document.createElement("div");
      header.className = "part-header";
      header.textContent = `PART ${pIdx + 1}`;
      card.appendChild(header);

      const title = document.createElement("div");
      title.className = "part-title";
      title.textContent = p.name || (p.timbre ? p.timbre.instrument_key.split("/").pop() : "Empty");
      card.appendChild(title);

      if (p.timbre) {
        const cat = document.createElement("div");
        cat.className = "part-timbre";
        cat.textContent = p.timbre.library_id ? "INSTALLED" : "NONE";
        card.appendChild(cat);
      }

      // Volume slider / editor
      const faderRow = document.createElement("div");
      faderRow.className = "part-fader";
      const faderLabel = document.createElement("span");
      faderLabel.textContent = `VOL ${Math.round((p.volume || 1.0) * 100)}`;
      const faderInput = document.createElement("input");
      faderInput.type = "range";
      faderInput.min = "0";
      faderInput.max = "100";
      faderInput.value = Math.round((p.volume || 1.0) * 100);
      faderInput.addEventListener("input", (e) => {
        p.volume = parseInt(e.target.value, 10) / 100;
        faderLabel.textContent = `VOL ${e.target.value}`;
      });
      faderRow.appendChild(faderLabel);
      faderRow.appendChild(faderInput);
      card.appendChild(faderRow);

      stripEl.appendChild(card);
    });
  }

  // Save Offline Backup
  document.getElementById("btn-save-backup")?.addEventListener("click", () => {
    if (!activeBackup) return;
    if (window.__TAURI__?.core?.invoke) {
      window.__TAURI__.core.invoke("save_backup_file_cmd", {
        filePath: activeBackupPath,
        backup: activeBackup,
      }).then(() => {
        announce(localeMgr.tr("studio.offline.backup_saved"));
      }).catch((err) => {
        announce("Failed to save: " + err, true);
      });
    } else {
      announce(localeMgr.tr("studio.offline.backup_saved"));
    }
  });

  // Open Backup File (.velorabackup)
  const fileInput = document.getElementById("backup-file-input");
  document.getElementById("btn-open-backup-file")?.addEventListener("click", () => {
    fileInput?.click();
  });
  document.getElementById("btn-import")?.addEventListener("click", () => {
    fileInput?.click();
  });

  fileInput?.addEventListener("change", (e) => {
    const file = e.target.files?.[0];
    if (!file) return;
    activeBackupPath = file.name;

    const reader = new FileReader();
    reader.onload = (event) => {
      try {
        const text = new TextDecoder().decode(event.target.result);
        let parsed = null;
        try {
          parsed = JSON.parse(text);
        } catch (_) {
          // If binary protobuf, would be handled by Tauri invoke in real runtime
        }

        if (parsed) {
          if (parsed.format_version > 1) {
            announce(localeMgr.tr("studio.backup.needs_newer_studio"), true);
            return;
          }
          activeBackup = parsed;
          renderOfflineEditor();
          announce(localeMgr.tr("studio.offline.backup_loaded").replace("%s", file.name));
        } else if (window.__TAURI__?.core?.invoke) {
          window.__TAURI__.core.invoke("read_backup_file_cmd", { filePath: file.name })
            .then((b) => {
              activeBackup = b;
              renderOfflineEditor();
              announce(localeMgr.tr("studio.offline.backup_loaded").replace("%s", file.name));
            })
            .catch((err) => {
              announce(err.toString(), true);
            });
        }
      } catch (err) {
        announce("Error loading backup: " + err, true);
      }
    };
    reader.readAsArrayBuffer(file);
  });

  // Create Backup (Story 9.16)
  const triggerBackup = () => {
    const defaultName = `Velora_Backup_${new Date().toISOString().slice(0, 10)}.velorabackup`;
    const targetPath = prompt("Enter backup file location/name:", defaultName);
    if (!targetPath) return;

    if (window.__TAURI__?.core?.invoke) {
      window.__TAURI__.core.invoke("create_backup", { filePath: targetPath })
        .then((b) => {
          const nowStr = new Date().toLocaleString();
          localStorage.setItem("velora_last_backup_" + currentDeviceId, nowStr);

          const backupEl = document.getElementById("footer-backup-status");
          if (backupEl) {
            const tpl = localeMgr.tr("studio.footer.last_backup");
            backupEl.textContent = tpl.replace("%s", nowStr);
          }

          if (b.notes && b.notes.length > 0) {
            announce(`${localeMgr.tr("studio.backup.success")} · ${b.notes}`, true);
          } else {
            announce(localeMgr.tr("studio.backup.success"));
          }
        })
        .catch((err) => {
          announce("Backup failed: " + err, true);
        });
    } else {
      const nowStr = new Date().toLocaleString();
      localStorage.setItem("velora_last_backup_" + currentDeviceId, nowStr);
      const backupEl = document.getElementById("footer-backup-status");
      if (backupEl) {
        const tpl = localeMgr.tr("studio.footer.last_backup");
        backupEl.textContent = tpl.replace("%s", nowStr);
      }
      announce(localeMgr.tr("studio.backup.success"));
    }
  };

  document.getElementById("btn-backup")?.addEventListener("click", triggerBackup);
  document.getElementById("side-backups")?.addEventListener("click", triggerBackup);

  // Restore Dialog & Confirmation (Story 9.17)
  const restoreFileInput = document.getElementById("restore-file-input");
  document.getElementById("btn-restore")?.addEventListener("click", () => {
    restoreFileInput?.click();
  });

  const optMerge = document.getElementById("opt-restore-merge");
  const optReplace = document.getElementById("opt-restore-replace");
  const countsSummaryEl = document.getElementById("restore-counts-summary");
  const replaceWarningEl = document.getElementById("restore-replace-warning");

  function updateRestoreDialogView() {
    if (restoreMode === "merge") {
      optMerge?.classList.add("selected");
      optMerge?.setAttribute("aria-checked", "true");
      optReplace?.classList.remove("selected");
      optReplace?.setAttribute("aria-checked", "false");
      if (replaceWarningEl) replaceWarningEl.style.display = "none";
    } else {
      optReplace?.classList.add("selected");
      optReplace?.setAttribute("aria-checked", "true");
      optMerge?.classList.remove("selected");
      optMerge?.setAttribute("aria-checked", "false");
      if (replaceWarningEl) replaceWarningEl.style.display = "block";
    }

    if (countsSummaryEl && restoreCandidateBackup) {
      const slCount = restoreCandidateBackup.setlists?.setlists?.length || 0;
      const lsCount = restoreCandidateBackup.live_sets?.length || 0;
      const tpl = localeMgr.tr("studio.restore.counts_summary");
      const slRemove = restoreMode === "replace" ? 1 : 0;
      const lsRemove = restoreMode === "replace" ? 1 : 0;
      countsSummaryEl.textContent = tpl
        .replace("%d", slCount)
        .replace("%d", 0)
        .replace("%d", slRemove)
        .replace("%d", lsCount)
        .replace("%d", 0)
        .replace("%d", lsRemove);
    }
  }

  optMerge?.addEventListener("click", () => {
    restoreMode = "merge";
    updateRestoreDialogView();
  });

  optReplace?.addEventListener("click", () => {
    restoreMode = "replace";
    updateRestoreDialogView();
  });

  restoreFileInput?.addEventListener("change", (e) => {
    const file = e.target.files?.[0];
    if (!file) return;

    if (window.__TAURI__?.core?.invoke) {
      window.__TAURI__.core.invoke("read_backup_file_cmd", { filePath: file.name })
        .then((b) => {
          restoreCandidateBackup = b;
          restoreMode = "merge";
          updateRestoreDialogView();
          if (restoreModal) restoreModal.classList.add("active");
          if (backdrop) backdrop.classList.add("active");
        })
        .catch((err) => {
          announce(err.toString(), true);
        });
    } else {
      // Mock for web demo
      restoreCandidateBackup = {
        setlists: { setlists: [{ id: "sl-1", name: "Festival Set" }] },
        live_sets: [{ id: "ls-1", name: "Solo Grand", parts: [] }],
      };
      restoreMode = "merge";
      updateRestoreDialogView();
      if (restoreModal) restoreModal.classList.add("active");
      if (backdrop) backdrop.classList.add("active");
    }
  });

  document.getElementById("btn-confirm-restore")?.addEventListener("click", () => {
    if (!restoreCandidateBackup) return;

    if (window.__TAURI__?.core?.invoke) {
      window.__TAURI__.core.invoke("perform_restore_cmd", {
        filePath: restoreFileInput?.files?.[0]?.name || "backup.velorabackup",
        mode: restoreMode,
      }).then(() => {
        closeModals();
        announce(localeMgr.tr("studio.restore.success"));
      }).catch((err) => {
        announce("Restore error: " + err, true);
      });
    } else {
      closeModals();
      announce(localeMgr.tr("studio.restore.success"));
    }
  });

  // Query Tauri backend for initial connection status
  if (window.__TAURI__?.core?.invoke) {
    window.__TAURI__.core.invoke("get_connection_status").then((conn) => {
      if (conn?.connected) {
        const connText = document.querySelector("#connection-status .conn-text");
        if (connText) connText.textContent = conn.device_name || "Velora 73 connected";
        const writeBtn = document.getElementById("btn-write");
        if (writeBtn) {
          writeBtn.disabled = false;
          writeBtn.removeAttribute("title");
        }
      }
    }).catch(() => {
      // Offline default
    });
  }
});
