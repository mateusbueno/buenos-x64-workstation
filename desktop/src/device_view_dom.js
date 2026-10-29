// Story 9.21: Accessible DOM layout and view for Device status, storage breakdown, and diagnostics export
import { formatBytes } from "./export_dialog.js";

export class DeviceView {
  constructor({
    doc,
    host,
    tr = (k, ...args) => k,
    controller,
    announce = () => {},
    onBack = () => {},
    onBackup = () => {},
    onRestore = () => {},
  }) {
    this.doc = doc;
    this.host = host;
    this.tr = tr;
    this.controller = controller;
    this.announce = announce;
    this.onBack = onBack;
    this.onBackup = onBackup;
    this.onRestore = onRestore;
  }

  show() {
    if (!this.host) return;
    this.host.hidden = false;
    this.render();
    const heading = this.host.querySelector(".device-header-title");
    if (heading) heading.focus();
  }

  hide() {
    if (!this.host) return;
    this.host.hidden = true;
  }

  setLabels() {
    this.render();
  }

  render() {
    if (!this.host) return;
    this.host.textContent = "";

    const ctl = this.controller;
    const isPaired = ctl.deviceStatus?.isPaired;

    // Header container
    const header = this.doc.createElement("div");
    header.className = "device-header";

    const titleGroup = this.doc.createElement("div");
    const eyebrow = this.doc.createElement("small");
    eyebrow.className = "setting-label";
    eyebrow.textContent = this.tr("studio.device.title").toUpperCase();

    const title = this.doc.createElement("h1");
    title.className = "device-header-title";
    title.tabIndex = -1;
    title.textContent = ctl.deviceStatus.model || this.tr("studio.device.title");

    titleGroup.appendChild(eyebrow);
    titleGroup.appendChild(title);

    const actions = this.doc.createElement("div");
    actions.className = "actions-section";

    const closeBtn = this.doc.createElement("button");
    closeBtn.type = "button";
    closeBtn.className = "btn";
    closeBtn.id = "btn-close-device-view";
    closeBtn.textContent = this.tr("studio.device.close");
    closeBtn.addEventListener("click", () => {
      ctl.close();
      this.onBack();
    });

    actions.appendChild(closeBtn);
    header.appendChild(titleGroup);
    header.appendChild(actions);
    this.host.appendChild(header);

    // If not paired: render offline fallback hint
    if (!isPaired) {
      const offlineCard = this.doc.createElement("div");
      offlineCard.className = "offline-card";
      offlineCard.id = "device-offline-card";

      const icon = this.doc.createElement("div");
      icon.className = "offline-icon";
      icon.setAttribute("aria-hidden", "true");
      icon.textContent = "⚠";

      const offTitle = this.doc.createElement("h2");
      offTitle.className = "offline-title";
      offTitle.textContent = this.tr("studio.device.offline_title");

      const offBody = this.doc.createElement("p");
      offBody.className = "offline-body";
      offBody.textContent = this.tr("studio.device.offline_body");

      const offHint = this.doc.createElement("div");
      offHint.className = "offline-hint";
      offHint.textContent = this.tr("studio.device.offline_hint");

      offlineCard.append(icon, offTitle, offBody, offHint);
      this.host.appendChild(offlineCard);
      return;
    }

    // Scrollable content body
    const body = this.doc.createElement("div");
    body.className = "device-body";

    // 1. Hardware Overview Card
    const overviewCard = this.doc.createElement("section");
    overviewCard.className = "device-card";
    overviewCard.id = "device-overview-section";
    overviewCard.setAttribute("role", "region");
    overviewCard.setAttribute("aria-label", this.tr("studio.device.overview"));

    const ovTitle = this.doc.createElement("h3");
    ovTitle.className = "device-card-title";
    ovTitle.textContent = this.tr("studio.device.overview");
    overviewCard.appendChild(ovTitle);

    const ovGrid = this.doc.createElement("div");
    ovGrid.className = "device-info-grid";

    const addInfoRow = (labelKey, value, badgeCls = "") => {
      const item = this.doc.createElement("div");
      item.className = "device-info-item";
      const lbl = this.doc.createElement("span");
      lbl.className = "device-info-label";
      lbl.textContent = this.tr(labelKey);
      const val = this.doc.createElement("span");
      val.className = "device-info-value" + (badgeCls ? ` ${badgeCls}` : "");
      val.textContent = value;
      item.append(lbl, val);
      ovGrid.appendChild(item);
    };

    const localizedTier = ctl.tier === "Pro"
      ? this.tr("studio.device.tier_pro")
      : this.tr("studio.device.tier_stage");

    const localizedVariant = ctl.imageVariant === "Development"
      ? this.tr("studio.device.variant_dev")
      : ctl.imageVariant === "Factory Test"
      ? this.tr("studio.device.variant_factory_test")
      : this.tr("studio.device.variant_production");

    addInfoRow("studio.device.model", ctl.deviceStatus.model || "Velora 73");
    addInfoRow("studio.device.tier", localizedTier, ctl.tier === "Pro" ? "badge-pro" : "badge-stage");
    addInfoRow("studio.device.firmware", ctl.deviceStatus.firmwareVersion || "1.4.0");
    addInfoRow("studio.device.variant", localizedVariant, "badge-variant");
    addInfoRow("studio.device.device_id", ctl.deviceStatus.deviceId || "unknown");

    overviewCard.appendChild(ovGrid);
    body.appendChild(overviewCard);

    // 2. Storage Breakdown Card
    const storageCard = this.doc.createElement("section");
    storageCard.className = "device-card";
    storageCard.id = "device-storage-section";
    storageCard.setAttribute("role", "region");
    storageCard.setAttribute("aria-label", this.tr("studio.device.storage"));

    const stTitle = this.doc.createElement("h3");
    stTitle.className = "device-card-title";
    stTitle.textContent = this.tr("studio.device.storage");

    const usedFormatted = formatBytes(ctl.storage.usedBytes);
    const freeFormatted = formatBytes(ctl.storage.freeBytes);
    const totalFormatted = formatBytes(ctl.storage.totalBytes);
    const usedPct = Math.min(100, Math.round((ctl.storage.usedBytes / ctl.storage.totalBytes) * 100)) || 0;

    const breakdownText = this.doc.createElement("div");
    breakdownText.className = "storage-breakdown-text";
    breakdownText.textContent = this.tr("studio.device.storage_breakdown", usedFormatted, freeFormatted, totalFormatted);

    // Storage Meter
    const meter = this.doc.createElement("div");
    meter.className = "storage-meter";
    meter.setAttribute("role", "meter");
    meter.setAttribute("aria-valuenow", String(usedPct));
    meter.setAttribute("aria-valuemin", "0");
    meter.setAttribute("aria-valuemax", "100");
    meter.setAttribute("aria-valuetext", `${usedFormatted} of ${totalFormatted} (${usedPct}%)`);

    const meterFill = this.doc.createElement("div");
    meterFill.className = "storage-meter-fill";
    meterFill.style.width = `${usedPct}%`;
    meter.appendChild(meterFill);

    storageCard.append(stTitle, breakdownText, meter);

    // Libraries Table
    const libHeader = this.doc.createElement("h4");
    libHeader.className = "device-section-subtitle";
    libHeader.textContent = this.tr("studio.device.libraries");
    storageCard.appendChild(libHeader);

    if (ctl.storage.libraries.length === 0) {
      const emptyLibs = this.doc.createElement("div");
      emptyLibs.className = "empty-hint";
      emptyLibs.textContent = this.tr("studio.device.no_libraries");
      storageCard.appendChild(emptyLibs);
    } else {
      const table = this.doc.createElement("table");
      table.className = "device-libraries-table";
      table.setAttribute("role", "table");

      const thead = this.doc.createElement("thead");
      const trHead = this.doc.createElement("tr");

      [
        "studio.device.col_library",
        "studio.device.col_kind",
        "studio.device.col_instruments",
        "studio.device.col_size",
      ].forEach((colKey) => {
        const th = this.doc.createElement("th");
        th.setAttribute("scope", "col");
        th.textContent = this.tr(colKey);
        trHead.appendChild(th);
      });
      thead.appendChild(trHead);
      table.appendChild(thead);

      const tbody = this.doc.createElement("tbody");
      ctl.storage.libraries.forEach((lib) => {
        const row = this.doc.createElement("tr");

        const tdName = this.doc.createElement("td");
        tdName.className = "lib-name-cell";
        tdName.textContent = lib.name;

        const tdKind = this.doc.createElement("td");
        tdKind.className = "lib-kind-cell";
        const kindBadge = this.doc.createElement("span");
        kindBadge.className = lib.kind === "Factory" ? "badge-factory" : "badge-user";
        kindBadge.textContent = this.tr(lib.kind === "Factory" ? "studio.device.kind_factory" : "studio.device.kind_user");
        tdKind.appendChild(kindBadge);

        const tdCount = this.doc.createElement("td");
        tdCount.className = "lib-count-cell";
        tdCount.textContent = String(lib.instrumentCount);

        const tdSize = this.doc.createElement("td");
        tdSize.className = "lib-size-cell";
        tdSize.textContent = formatBytes(lib.sizeBytes);

        row.append(tdName, tdKind, tdCount, tdSize);
        tbody.appendChild(row);
      });

      table.appendChild(tbody);
      storageCard.appendChild(table);
    }

    body.appendChild(storageCard);

    // 3. Firmware Card
    const fwCard = this.doc.createElement("section");
    fwCard.className = "device-card";
    fwCard.id = "device-firmware-section";
    fwCard.setAttribute("role", "region");
    fwCard.setAttribute("aria-label", this.tr("studio.device.firmware_title"));

    const fwTitle = this.doc.createElement("h3");
    fwTitle.className = "device-card-title";
    fwTitle.textContent = this.tr("studio.device.firmware_title");

    const fwStatus = this.doc.createElement("p");
    fwStatus.className = "device-status-text";
    fwStatus.textContent = this.tr("studio.device.firmware_up_to_date", ctl.firmware.version);

    const fwAction = this.doc.createElement("div");
    fwAction.className = "device-card-actions";
    const fwBtn = this.doc.createElement("button");
    fwBtn.type = "button";
    fwBtn.className = "btn";
    fwBtn.id = "btn-check-firmware-updates";
    fwBtn.textContent = this.tr("studio.device.btn_check_updates");
    fwBtn.addEventListener("click", () => {
      this.announce(this.tr("studio.device.firmware_up_to_date", ctl.firmware.version));
    });
    fwAction.appendChild(fwBtn);

    fwCard.append(fwTitle, fwStatus, fwAction);
    body.appendChild(fwCard);

    // 4. Backups Card
    const bkCard = this.doc.createElement("section");
    bkCard.className = "device-card";
    bkCard.id = "device-backups-section";
    bkCard.setAttribute("role", "region");
    bkCard.setAttribute("aria-label", this.tr("studio.device.backups_title"));

    const bkTitle = this.doc.createElement("h3");
    bkTitle.className = "device-card-title";
    bkTitle.textContent = this.tr("studio.device.backups_title");

    const bkStatus = this.doc.createElement("p");
    bkStatus.className = "device-status-text";
    bkStatus.textContent = ctl.backups.lastBackupTime
      ? this.tr("studio.footer.last_backup", ctl.backups.lastBackupTime)
      : this.tr("studio.footer.no_backup_yet");

    const bkActions = this.doc.createElement("div");
    bkActions.className = "device-card-actions";

    const createBkBtn = this.doc.createElement("button");
    createBkBtn.type = "button";
    createBkBtn.className = "btn";
    createBkBtn.id = "btn-device-create-backup";
    createBkBtn.textContent = this.tr("studio.device.btn_create_backup");
    createBkBtn.addEventListener("click", () => this.onBackup());

    const restoreBkBtn = this.doc.createElement("button");
    restoreBkBtn.type = "button";
    restoreBkBtn.className = "btn";
    restoreBkBtn.id = "btn-device-restore-backup";
    restoreBkBtn.textContent = this.tr("studio.device.btn_restore_backup");
    restoreBkBtn.addEventListener("click", () => this.onRestore());

    bkActions.append(createBkBtn, restoreBkBtn);
    bkCard.append(bkTitle, bkStatus, bkActions);
    body.appendChild(bkCard);

    // 5. Diagnostics Card
    const diagCard = this.doc.createElement("section");
    diagCard.className = "device-card";
    diagCard.id = "device-diagnostics-section";
    diagCard.setAttribute("role", "region");
    diagCard.setAttribute("aria-label", this.tr("studio.device.diagnostics_title"));

    const diagTitle = this.doc.createElement("h3");
    diagTitle.className = "device-card-title";
    diagTitle.textContent = this.tr("studio.device.diagnostics_title");

    const diagDesc = this.doc.createElement("p");
    diagDesc.className = "device-card-desc";
    diagDesc.textContent = this.tr("studio.device.diagnostics_desc");

    if (ctl.diagnostics.error) {
      const errBanner = this.doc.createElement("div");
      errBanner.className = "device-error-banner";
      errBanner.setAttribute("role", "alert");
      errBanner.textContent = this.tr("studio.device.diagnostics_failed", ctl.diagnostics.error);
      diagCard.appendChild(errBanner);
    }

    const diagActions = this.doc.createElement("div");
    diagActions.className = "device-card-actions";

    const exportBtn = this.doc.createElement("button");
    exportBtn.type = "button";
    exportBtn.className = "btn btn-primary";
    exportBtn.id = "btn-export-diagnostics";
    exportBtn.disabled = ctl.diagnostics.exporting;
    exportBtn.textContent = ctl.diagnostics.exporting
      ? this.tr("studio.device.diagnostics_exporting")
      : this.tr("studio.device.btn_export_diagnostics");

    exportBtn.addEventListener("click", async () => {
      exportBtn.disabled = true;
      exportBtn.textContent = this.tr("studio.device.diagnostics_exporting");
      try {
        await ctl.exportDiagnostics();
      } catch (e) {
        // Handled by controller error notification
      } finally {
        exportBtn.disabled = ctl.diagnostics.exporting;
        exportBtn.textContent = ctl.diagnostics.exporting
          ? this.tr("studio.device.diagnostics_exporting")
          : this.tr("studio.device.btn_export_diagnostics");
      }
    });

    diagActions.appendChild(exportBtn);
    diagCard.append(diagTitle, diagDesc, diagActions);
    body.appendChild(diagCard);

    this.host.appendChild(body);

    // If an active section was targeted, focus/scroll to it
    if (ctl.activeSection && ctl.activeSection !== "overview") {
      const targetSec = this.host.querySelector(`#device-${ctl.activeSection}-section`);
      if (targetSec) {
        targetSec.scrollIntoView?.({ behavior: "smooth" });
        targetSec.tabIndex = -1;
        targetSec.focus?.();
      }
    }
  }
}
