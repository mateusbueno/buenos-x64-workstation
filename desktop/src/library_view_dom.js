// Story 9.14 / 9.15: Accessible DOM layout and view for Library Management and SFZ/SF2 conversion & USB transfer
import { formatBytes } from "./export_dialog.js";
import { CANONICAL_CATEGORIES, formatTransferLine } from "./library_view.js";

export class LibraryView {
  constructor({
    doc,
    host,
    dialogHost,
    tr = (k, ...args) => k,
    controller,
    announce = () => {},
    onBack = () => {},
  }) {
    this.doc = doc;
    this.host = host;
    this.dialogHost = dialogHost || doc.getElementById("convert-dialog-host") || doc.body;
    this.tr = tr;
    this.controller = controller;
    this.announce = announce;
    this.onBack = onBack;
    this.triggerEl = null;
  }

  show() {
    if (!this.host) return;
    this.host.hidden = false;
    this.render();
    const heading = this.host.querySelector(".library-header-title");
    if (heading) heading.focus();
  }

  hide() {
    if (!this.host) return;
    this.host.hidden = true;
    if (this.controller.isConvertDialogOpen) {
      this.controller.closeConvertDialog();
      this.renderDialog();
    }
  }

  setLabels() {
    this.render();
    this.renderDialog();
  }

  render() {
    if (!this.host) return;
    this.host.textContent = "";

    const ctl = this.controller;

    // Header container
    const header = this.doc.createElement("div");
    header.className = "library-header";

    const titleGroup = this.doc.createElement("div");
    const eyebrow = this.doc.createElement("small");
    eyebrow.className = "setting-label";
    eyebrow.textContent = "VELORA STUDIO";

    const title = this.doc.createElement("h1");
    title.className = "library-header-title";
    title.tabIndex = -1;
    title.textContent = this.tr("studio.library.title");

    titleGroup.appendChild(eyebrow);
    titleGroup.appendChild(title);

    const actions = this.doc.createElement("div");
    actions.className = "actions-section";

    const convertBtn = this.doc.createElement("button");
    convertBtn.type = "button";
    convertBtn.className = "btn btn-primary";
    convertBtn.id = "btn-open-convert-dialog";
    convertBtn.textContent = this.tr("studio.library.btn_convert");
    convertBtn.addEventListener("click", () => {
      this.triggerEl = convertBtn;
      // Open file picker or mock dialog
      const fileInput = this.doc.getElementById("library-file-input");
      if (fileInput) {
        fileInput.click();
      } else {
        ctl.openConvertDialog(["Sample Library.sfz"]);
        this.renderDialog();
      }
    });

    const closeBtn = this.doc.createElement("button");
    closeBtn.type = "button";
    closeBtn.className = "btn";
    closeBtn.id = "btn-close-library-view";
    closeBtn.textContent = "✕";
    closeBtn.setAttribute("aria-label", this.tr("studio.converter.close"));
    closeBtn.addEventListener("click", () => {
      this.hide();
      this.onBack();
    });

    actions.appendChild(convertBtn);
    actions.appendChild(closeBtn);

    header.appendChild(titleGroup);
    header.appendChild(actions);
    this.host.appendChild(header);

    // Story 9.15: Transfer progress banner
    if (ctl.transferState) {
      const banner = this.doc.createElement("div");
      banner.className = "transfer-banner";
      banner.setAttribute("role", "status");
      banner.setAttribute("aria-live", "polite");

      const text = this.doc.createElement("span");
      text.className = "transfer-banner-text";
      text.textContent = formatTransferLine(
        ctl.transferState.libraryName,
        ctl.transferState.bytesTransferred,
        ctl.transferState.totalBytes
      );

      const cancelBtn = this.doc.createElement("button");
      cancelBtn.type = "button";
      cancelBtn.className = "btn btn-sm btn-cancel-transfer";
      cancelBtn.textContent = this.tr("studio.library.btn_cancel_transfer");
      cancelBtn.addEventListener("click", () => {
        ctl.cancelTransfer();
      });

      banner.appendChild(text);
      banner.appendChild(cancelBtn);
      this.host.appendChild(banner);
    }

    // Story 9.15: Transfer error banner
    if (ctl.transferError) {
      const errBanner = this.doc.createElement("div");
      errBanner.className = "transfer-error-banner";
      errBanner.setAttribute("role", "alert");

      const errText = this.doc.createElement("span");
      errText.className = "transfer-error-text";
      errText.textContent = ctl.transferError;

      const dismissBtn = this.doc.createElement("button");
      dismissBtn.type = "button";
      dismissBtn.className = "btn btn-sm btn-dismiss-error";
      dismissBtn.textContent = "✕";
      dismissBtn.setAttribute("aria-label", "Dismiss error");
      dismissBtn.addEventListener("click", () => {
        ctl.dismissTransferError();
      });

      errBanner.appendChild(errText);
      errBanner.appendChild(dismissBtn);
      this.host.appendChild(errBanner);
    }

    // Tab switcher (On Computer vs On Velora)
    const tabNav = this.doc.createElement("nav");
    tabNav.className = "library-tabs";
    tabNav.setAttribute("role", "tablist");
    tabNav.setAttribute("aria-label", "Library location tabs");

    const computerTab = this.doc.createElement("button");
    computerTab.type = "button";
    computerTab.role = "tab";
    computerTab.id = "tab-library-computer";
    computerTab.className = `library-tab-btn ${ctl.activeTab === "computer" ? "active" : ""}`;
    computerTab.setAttribute("aria-selected", ctl.activeTab === "computer" ? "true" : "false");
    computerTab.textContent = `${this.tr("studio.library.on_computer")} (${ctl.onComputerLibraries.length})`;
    computerTab.addEventListener("click", () => {
      ctl.activeTab = "computer";
      this.render();
    });

    const veloraTab = this.doc.createElement("button");
    veloraTab.type = "button";
    veloraTab.role = "tab";
    veloraTab.id = "tab-library-velora";
    veloraTab.className = `library-tab-btn ${ctl.activeTab === "velora" ? "active" : ""}`;
    veloraTab.setAttribute("aria-selected", ctl.activeTab === "velora" ? "true" : "false");
    veloraTab.textContent = `${this.tr("studio.library.on_velora")} (${ctl.onVeloraLibraries.length})`;
    veloraTab.addEventListener("click", () => {
      ctl.activeTab = "velora";
      this.render();
    });

    const handleTabKey = (e) => {
      if (e.key === "ArrowRight" || e.key === "ArrowLeft") {
        e.preventDefault();
        ctl.activeTab = ctl.activeTab === "computer" ? "velora" : "computer";
        this.render();
        const newTab = this.host.querySelector(ctl.activeTab === "computer" ? "#tab-library-computer" : "#tab-library-velora");
        if (newTab) newTab.focus();
      }
    };
    computerTab.addEventListener("keydown", handleTabKey);
    veloraTab.addEventListener("keydown", handleTabKey);

    tabNav.appendChild(computerTab);
    tabNav.appendChild(veloraTab);
    this.host.appendChild(tabNav);

    // Tab content panel
    const tabPanel = this.doc.createElement("div");
    tabPanel.className = "library-tab-panel";
    tabPanel.setAttribute("role", "tabpanel");
    tabPanel.setAttribute("aria-labelledby", ctl.activeTab === "computer" ? "tab-library-computer" : "tab-library-velora");

    const currentLibs = ctl.activeTab === "computer" ? ctl.onComputerLibraries : ctl.onVeloraLibraries;

    if (currentLibs.length === 0) {
      const emptyCard = this.doc.createElement("div");
      emptyCard.className = "library-empty-state";
      const icon = this.doc.createElement("div");
      icon.className = "library-empty-icon";
      icon.setAttribute("aria-hidden", "true");
      icon.textContent = "📁";
      const msg = this.doc.createElement("p");
      msg.className = "library-empty-text";
      msg.textContent = ctl.activeTab === "computer"
        ? this.tr("studio.library.empty_computer")
        : this.tr("studio.library.empty_velora");

      emptyCard.appendChild(icon);
      emptyCard.appendChild(msg);

      if (ctl.activeTab === "computer") {
        const ctaBtn = this.doc.createElement("button");
        ctaBtn.type = "button";
        ctaBtn.className = "btn btn-primary";
        ctaBtn.textContent = this.tr("studio.library.btn_convert");
        ctaBtn.addEventListener("click", () => convertBtn.click());
        emptyCard.appendChild(ctaBtn);
      }

      tabPanel.appendChild(emptyCard);
    } else {
      // Table of sound libraries
      const table = this.doc.createElement("table");
      table.className = "library-table";

      const thead = this.doc.createElement("thead");
      const headRow = this.doc.createElement("tr");

      const thCat = this.doc.createElement("th");
      thCat.textContent = this.tr("studio.library.category_col");
      const thLib = this.doc.createElement("th");
      thLib.textContent = this.tr("studio.library.library_col");
      const thInst = this.doc.createElement("th");
      thInst.textContent = this.tr("studio.library.instruments_col");
      const thSize = this.doc.createElement("th");
      thSize.textContent = this.tr("studio.library.size_col");
      const thAct = this.doc.createElement("th");
      thAct.textContent = this.tr("studio.library.actions_col");

      headRow.appendChild(thCat);
      headRow.appendChild(thLib);
      headRow.appendChild(thInst);
      headRow.appendChild(thSize);
      headRow.appendChild(thAct);
      thead.appendChild(headRow);
      table.appendChild(thead);

      const tbody = this.doc.createElement("tbody");
      for (const lib of currentLibs) {
        const trEl = this.doc.createElement("tr");
        trEl.className = "library-row";

        // Category pill
        const tdCat = this.doc.createElement("td");
        const catBadge = this.doc.createElement("span");
        catBadge.className = "badge badge-category";
        const catKey = lib.category || "category.piano";
        const catSlug = catKey.replace(/^category\./, "");
        catBadge.textContent = this.tr(`${catKey}.full`, catSlug.toUpperCase());
        tdCat.appendChild(catBadge);

        // Name
        const tdName = this.doc.createElement("td");
        tdName.className = "library-cell-name";
        const nameStrong = this.doc.createElement("strong");
        nameStrong.textContent = lib.name || lib.library_id || "Sound Library";
        tdName.appendChild(nameStrong);

        // Instruments
        const tdInst = this.doc.createElement("td");
        tdInst.textContent = String(lib.instrument_count ?? lib.instrumentCount ?? 1);

        // Size
        const tdSize = this.doc.createElement("td");
        tdSize.textContent = formatBytes(lib.size_bytes ?? lib.sizeBytes ?? 0);

        // Actions
        const tdAct = this.doc.createElement("td");
        tdAct.className = "library-actions-cell";
        if (ctl.activeTab === "computer") {
          const sendBtn = this.doc.createElement("button");
          sendBtn.type = "button";
          sendBtn.className = "btn btn-primary btn-sm btn-send-to-velora";
          sendBtn.textContent = this.tr("studio.library.btn_send_to_velora");
          sendBtn.setAttribute("aria-label", `${this.tr("studio.library.btn_send_to_velora")} ${lib.name || lib.library_id}`);
          sendBtn.disabled = !ctl.isPaired || Boolean(ctl.transferState?.isTransferring);
          sendBtn.addEventListener("click", () => {
            ctl.sendToVelora(lib);
          });

          const delBtn = this.doc.createElement("button");
          delBtn.type = "button";
          delBtn.className = "btn btn-danger btn-sm btn-delete-library";
          delBtn.textContent = this.tr("studio.library.delete_btn");
          delBtn.setAttribute("aria-label", `${this.tr("studio.library.delete_btn")} ${lib.name || lib.library_id}`);
          delBtn.disabled = Boolean(ctl.transferState?.isTransferring);
          delBtn.addEventListener("click", async () => {
            if (typeof window !== "undefined" && window.confirm) {
              const ok = window.confirm(this.tr("studio.library.delete_confirm", lib.name || lib.library_id));
              if (!ok) return;
            }
            await ctl.deleteComputerLibrary(lib.library_id);
            this.render();
          });

          tdAct.appendChild(sendBtn);
          tdAct.appendChild(delBtn);
        } else {
          tdAct.textContent = "—";
        }

        trEl.appendChild(tdCat);
        trEl.appendChild(tdName);
        trEl.appendChild(tdInst);
        trEl.appendChild(tdSize);
        trEl.appendChild(tdAct);
        tbody.appendChild(trEl);
      }
      table.appendChild(tbody);
      tabPanel.appendChild(table);
    }

    this.host.appendChild(tabPanel);
  }

  renderDialog() {
    if (!this.dialogHost) return;
    this.dialogHost.textContent = "";

    const ctl = this.controller;
    if (!ctl.isConvertDialogOpen) return;

    // Modal backdrop
    const backdrop = this.doc.createElement("div");
    backdrop.className = "dialog-backdrop";

    // Dialog container
    const dialog = this.doc.createElement("div");
    dialog.className = "convert-dialog";
    dialog.setAttribute("role", "dialog");
    dialog.setAttribute("aria-modal", "true");
    dialog.setAttribute("aria-labelledby", "convert-dialog-title");

    // Title bar
    const titleBar = this.doc.createElement("div");
    titleBar.className = "dialog-header";

    const title = this.doc.createElement("h2");
    title.id = "convert-dialog-title";
    title.textContent = this.tr("studio.converter.dialog_title");

    const restoreTrigger = () => {
      if (this.triggerEl && typeof this.triggerEl.focus === "function") {
        this.triggerEl.focus();
        this.triggerEl = null;
      }
    };

    const closeBtn = this.doc.createElement("button");
    closeBtn.type = "button";
    closeBtn.className = "btn";
    closeBtn.textContent = "✕";
    closeBtn.setAttribute("aria-label", this.tr("studio.converter.close"));
    closeBtn.disabled = ctl.isConverting;
    closeBtn.addEventListener("click", () => {
      ctl.closeConvertDialog();
      this.renderDialog();
      restoreTrigger();
    });

    titleBar.appendChild(title);
    titleBar.appendChild(closeBtn);
    dialog.appendChild(titleBar);

    // Body
    const body = this.doc.createElement("div");
    body.className = "dialog-body";

    if (ctl.conversionError) {
      const errAlert = this.doc.createElement("div");
      errAlert.className = "alert alert-danger";
      errAlert.setAttribute("role", "alert");
      errAlert.textContent = ctl.conversionError;
      body.appendChild(errAlert);
    }

    if (ctl.conversionTasks.length === 0) {
      const emptyNote = this.doc.createElement("p");
      emptyNote.textContent = this.tr("studio.converter.select_files");
      body.appendChild(emptyNote);
    } else {
      // Task rows with Category Dropdown & (Suggested) tag
      const table = this.doc.createElement("table");
      table.className = "convert-tasks-table";

      const thead = this.doc.createElement("thead");
      const trh = this.doc.createElement("tr");
      const thFile = this.doc.createElement("th");
      thFile.textContent = this.tr("studio.library.library_col");
      const thCat = this.doc.createElement("th");
      thCat.textContent = this.tr("studio.library.category_col");
      trh.appendChild(thFile);
      trh.appendChild(thCat);
      thead.appendChild(trh);
      table.appendChild(thead);

      const tbody = this.doc.createElement("tbody");
      ctl.conversionTasks.forEach((task, idx) => {
        const row = this.doc.createElement("tr");

        const tdFile = this.doc.createElement("td");
        const nameSpan = this.doc.createElement("strong");
        nameSpan.textContent = task.name;
        tdFile.appendChild(nameSpan);

        if (task.suggestedCategory && task.suggestedCategory !== "none") {
          const suggTag = this.doc.createElement("span");
          suggTag.className = "badge badge-suggested";
          suggTag.textContent = `(${this.tr("studio.converter.suggested")})`;
          tdFile.appendChild(suggTag);
        }

        const tdCat = this.doc.createElement("td");
        const select = this.doc.createElement("select");
        select.className = "select-category";
        select.disabled = ctl.isConverting;
        select.setAttribute("aria-label", `${this.tr("studio.library.category_col")} ${task.name}`);

        for (const cat of CANONICAL_CATEGORIES) {
          const opt = this.doc.createElement("option");
          opt.value = cat.key;
          opt.textContent = this.tr(`${cat.key}.full`, cat.name);
          if (task.category === cat.key || task.category === cat.slug) {
            opt.selected = true;
          }
          select.appendChild(opt);
        }

        select.addEventListener("change", (e) => {
          ctl.setTaskCategory(idx, e.target.value);
        });

        tdCat.appendChild(select);

        // Apply to all button on first row
        if (idx === 0 && ctl.conversionTasks.length > 1) {
          const applyAllBtn = this.doc.createElement("button");
          applyAllBtn.type = "button";
          applyAllBtn.className = "btn btn-sm btn-outline";
          applyAllBtn.textContent = this.tr("studio.converter.apply_all");
          applyAllBtn.disabled = ctl.isConverting;
          applyAllBtn.addEventListener("click", () => {
            ctl.applyCategoryToAll(select.value);
            this.renderDialog();
          });
          tdCat.appendChild(applyAllBtn);
        }

        row.appendChild(tdFile);
        row.appendChild(tdCat);
        tbody.appendChild(row);
      });
      table.appendChild(tbody);
      body.appendChild(table);
    }

    // Progress Section
    if (ctl.isConverting || ctl.conversionProgress) {
      const progBox = this.doc.createElement("div");
      progBox.className = "conversion-progress-box";

      const prog = ctl.conversionProgress || {
        task_index: 0,
        total_tasks: ctl.conversionTasks.length,
        current_file: "",
        status: "converting",
      };

      const pct = prog.total_tasks > 0 ? Math.round((prog.task_index / prog.total_tasks) * 100) : 0;

      const progBar = this.doc.createElement("div");
      progBar.className = "progress-bar";
      progBar.setAttribute("role", "progressbar");
      progBar.setAttribute("aria-valuenow", String(pct));
      progBar.setAttribute("aria-valuemin", "0");
      progBar.setAttribute("aria-valuemax", "100");

      const progFill = this.doc.createElement("div");
      progFill.className = "progress-fill";
      progFill.style.width = `${pct}%`;
      progBar.appendChild(progFill);

      const progStatus = this.doc.createElement("div");
      progStatus.className = "progress-status-text";
      progStatus.textContent = ctl.isConverting
        ? this.tr("studio.converter.converting", prog.task_index || 1, prog.total_tasks)
        : prog.status === "completed"
        ? this.tr("studio.converter.completed", prog.task_index, 0, 0)
        : prog.status;

      progBox.appendChild(progBar);
      progBox.appendChild(progStatus);
      body.appendChild(progBox);
    }

    dialog.appendChild(body);

    // Footer
    const footer = this.doc.createElement("div");
    footer.className = "dialog-footer";

    if (ctl.isConverting) {
      const cancelBtn = this.doc.createElement("button");
      cancelBtn.type = "button";
      cancelBtn.className = "btn btn-danger";
      cancelBtn.id = "btn-cancel-conversion";
      cancelBtn.textContent = this.tr("studio.converter.cancel");
      cancelBtn.addEventListener("click", async () => {
        await ctl.cancelConversion();
        this.renderDialog();
        restoreTrigger();
      });
      footer.appendChild(cancelBtn);
    } else {
      const cancelBtn = this.doc.createElement("button");
      cancelBtn.type = "button";
      cancelBtn.className = "btn";
      cancelBtn.textContent = this.tr("studio.converter.cancel");
      cancelBtn.addEventListener("click", () => {
        ctl.closeConvertDialog();
        this.renderDialog();
        restoreTrigger();
      });

      const startBtn = this.doc.createElement("button");
      startBtn.type = "button";
      startBtn.className = "btn btn-primary";
      startBtn.id = "btn-start-conversion";
      startBtn.textContent = this.tr("studio.converter.start_conversion");
      startBtn.disabled = ctl.conversionTasks.length === 0;
      startBtn.addEventListener("click", async () => {
        await ctl.startConversion();
        this.renderDialog();
        this.render();
      });

      footer.appendChild(cancelBtn);
      footer.appendChild(startBtn);
    }

    dialog.appendChild(footer);
    backdrop.appendChild(dialog);
    this.dialogHost.appendChild(backdrop);

    // Trap focus and support Escape
    dialog.addEventListener("keydown", (e) => {
      if (e.key === "Tab") {
        const focusables = Array.from(dialog.querySelectorAll('button:not([disabled]), select:not([disabled]), input:not([disabled]), [tabindex="0"]'));
        if (focusables.length > 0) {
          const first = focusables[0];
          const last = focusables[focusables.length - 1];
          if (e.shiftKey && this.doc.activeElement === first) {
            e.preventDefault();
            last.focus();
          } else if (!e.shiftKey && this.doc.activeElement === last) {
            e.preventDefault();
            first.focus();
          }
        }
      } else if (e.key === "Escape") {
        e.stopPropagation();
        if (ctl.isConverting) {
          ctl.cancelConversion().then(() => {
            this.renderDialog();
            restoreTrigger();
          });
        } else {
          ctl.closeConvertDialog();
          this.renderDialog();
          restoreTrigger();
        }
      }
    });

    const firstInteractive = dialog.querySelector("button, select");
    if (firstInteractive) firstInteractive.focus();
  }
}
