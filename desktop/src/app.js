// Velora Studio App Shell (Story 9.5)

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
    "studio.settings.close": "Close"
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
    "studio.settings.close": "Cerrar"
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
    "studio.settings.close": "Fechar"
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

// App Initialization
window.addEventListener("DOMContentLoaded", () => {
  const localeMgr = new LocaleManager();
  const themeMgr = new ThemeManager();
  new FocusManager();

  // Apply initial state
  localeMgr.apply();
  themeMgr.apply();

  // Settings Modal Controls
  const modal = document.getElementById("settings-modal");
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

  const openModal = () => {
    if (!modal) return;
    modal.classList.add("active");
    if (backdrop) backdrop.classList.add("active");
    if (themeSelect) themeSelect.focus();
  };

  const closeModal = () => {
    if (!modal) return;
    modal.classList.remove("active");
    if (backdrop) backdrop.classList.remove("active");
  };

  document.getElementById("btn-open-settings")?.addEventListener("click", openModal);
  document.getElementById("side-settings")?.addEventListener("click", openModal);
  document.getElementById("btn-offline-settings")?.addEventListener("click", openModal);
  document.getElementById("btn-close-settings")?.addEventListener("click", closeModal);
  document.getElementById("btn-close-settings-x")?.addEventListener("click", closeModal);
  backdrop?.addEventListener("click", closeModal);

  window.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && modal?.classList.contains("active")) {
      closeModal();
    }
  });

  // Query Tauri backend if running inside Tauri webview
  if (window.__TAURI__?.core?.invoke) {
    window.__TAURI__.core.invoke("get_connection_status").then((conn) => {
      if (conn?.connected) {
        const connText = document.querySelector("#connection-status .conn-text");
        if (connText) connText.textContent = conn.device_name || "Velora 73 connected";
      }
    }).catch(() => {
      // Offline fallback
    });

    window.__TAURI__.core.invoke("get_backup_status").then((backup) => {
      if (backup?.last_backup_timestamp) {
        const backupEl = document.getElementById("footer-backup-status");
        if (backupEl) {
          const tpl = localeMgr.tr("studio.footer.last_backup");
          backupEl.textContent = tpl.replace("%s", backup.last_backup_timestamp);
        }
      }
    }).catch(() => {
      // Offline fallback
    });
  }
});
