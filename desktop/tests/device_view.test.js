// Story 9.21: Unit tests for DeviceViewController
import test from "node:test";
import assert from "node:assert/strict";

import {
  DeviceViewController,
  DEFAULT_STAGE_STORAGE_BYTES,
  DEFAULT_PRO_STORAGE_BYTES,
  SYSTEM_OVERHEAD_BYTES,
} from "../src/device_view.js";
import { CATALOGS } from "../src/catalogs.js";

const tr = (key, ...args) => {
  const cat = CATALOGS.en || {};
  let s = cat[key] || key;
  args.forEach((a) => {
    s = s.replace("%s", a);
  });
  return s;
};

test("DeviceViewController starts in offline default state", () => {
  const ctl = new DeviceViewController({ tr });
  assert.equal(ctl.deviceStatus.isConnected, false);
  assert.equal(ctl.deviceStatus.isPaired, false);
  assert.equal(ctl.tier, "Stage");
  assert.equal(ctl.storage.totalBytes, DEFAULT_STAGE_STORAGE_BYTES);
  assert.equal(ctl.storage.usedBytes, SYSTEM_OVERHEAD_BYTES);
  assert.equal(ctl.storage.freeBytes, DEFAULT_STAGE_STORAGE_BYTES - SYSTEM_OVERHEAD_BYTES);
  assert.equal(ctl.storage.libraries.length, 0);
  assert.equal(ctl.backups.lastBackupTime, null);
  assert.equal(ctl.diagnostics.exporting, false);
  assert.equal(ctl.diagnostics.error, null);
});

test("DeviceViewController updates status for Stage unit", () => {
  let changed = 0;
  const ctl = new DeviceViewController({
    tr,
    onChange: () => { changed++; },
  });

  ctl.setDeviceStatus({
    configured: true,
    status: "paired",
    model: "Velora 73",
    firmware_version: "1.4.0",
    host: "192.168.1.50",
    port: 8080,
  });

  assert.equal(ctl.deviceStatus.isConnected, true);
  assert.equal(ctl.deviceStatus.isPaired, true);
  assert.equal(ctl.deviceStatus.model, "Velora 73");
  assert.equal(ctl.tier, "Stage");
  assert.equal(ctl.imageVariant, "Production");
  assert.equal(ctl.storage.totalBytes, DEFAULT_STAGE_STORAGE_BYTES);
  assert.ok(changed > 0);
});

test("DeviceViewController detects Pro tier and development image variant", () => {
  const ctl = new DeviceViewController({ tr });

  ctl.setDeviceStatus({
    configured: true,
    status: "paired",
    model: "Velora Pro 88",
    firmware_version: "1.5.0-dev",
    host: "127.0.0.1",
    port: 8080,
  });

  assert.equal(ctl.tier, "Pro");
  assert.equal(ctl.imageVariant, "Development");
  assert.equal(ctl.storage.totalBytes, DEFAULT_PRO_STORAGE_BYTES);
});

test("DeviceViewController calculates storage breakdown from sound libraries", () => {
  const ctl = new DeviceViewController({ tr });
  ctl.setDeviceStatus({
    configured: true,
    status: "paired",
    model: "Velora 73",
  });

  ctl.setDeviceState({
    sound_libraries: [
      { name: "Grand Piano Factory", kind: "Factory", instrument_count: 32, size_bytes: 500_000_000 },
      { name: "Vintage EP Collection", kind: "User", instrument_count: 16, size_bytes: 350_000_000 },
    ],
  });

  assert.equal(ctl.storage.libraries.length, 2);
  assert.equal(ctl.storage.libraries[0].name, "Grand Piano Factory");
  assert.equal(ctl.storage.libraries[0].kind, "Factory");
  assert.equal(ctl.storage.libraries[0].instrumentCount, 32);
  assert.equal(ctl.storage.libraries[0].sizeBytes, 500_000_000);

  const expectedUsed = SYSTEM_OVERHEAD_BYTES + 500_000_000 + 350_000_000;
  assert.equal(ctl.storage.usedBytes, expectedUsed);
  assert.equal(ctl.storage.freeBytes, DEFAULT_STAGE_STORAGE_BYTES - expectedUsed);
});

test("DeviceViewController updates backup timestamp", () => {
  let changed = 0;
  const ctl = new DeviceViewController({
    tr,
    onChange: () => { changed++; },
  });

  ctl.setBackupTime("2026-10-29 12:00:00");
  assert.equal(ctl.backups.lastBackupTime, "2026-10-29 12:00:00");
  assert.equal(changed, 1);
});

test("DeviceViewController exportDiagnostics runs successfully and announces", async () => {
  const announcements = [];
  const invoked = [];

  const mockDiagnostics = {
    device_id: "velora-73-001",
    model: "Velora 73",
    firmware_version: "1.4.0",
    tier: "Stage",
    image_variant: "Production",
    uptime_ms: 3600000,
    export_timestamp: "2026-10-29T12:00:00Z",
    counters: {
      xrun: 0,
      voice_steal: 0,
      voice_exhausted: 0,
      stream_underrun: 0,
    },
    render_time_histogram: {
      edges_us: [100, 200, 500],
      counts: [10, 50, 5, 0],
    },
    latency_histogram: {
      edges_us: [100, 200, 500],
      counts: [5, 20, 2, 0],
    },
  };

  const ctl = new DeviceViewController({
    tr,
    invoke: async (cmd, args) => {
      invoked.push({ cmd, args });
      if (cmd === "get_device_diagnostics" || cmd === "get_diagnostics") {
        return mockDiagnostics;
      }
      if (cmd === "save_diagnostics_export" || cmd === "export_diagnostics") {
        return { success: true, file_path: "/tmp/velora_diag.json", byte_count: 512 };
      }
      throw new Error(`Unknown command: ${cmd}`);
    },
    onAnnounce: (msg) => {
      announcements.push(msg);
    },
  });

  ctl.setDeviceStatus({
    configured: true,
    status: "paired",
    model: "Velora 73",
    firmware_version: "1.4.0",
  });

  const res = await ctl.exportDiagnostics();
  assert.ok(res);
  assert.equal(res.file_path, "/tmp/velora_diag.json");
  assert.equal(ctl.diagnostics.exporting, false);
  assert.equal(ctl.diagnostics.error, null);
  assert.equal(announcements.length, 1);
  assert.ok(announcements[0].includes("/tmp/velora_diag.json") || announcements[0].includes("Saved"));
});

test("DeviceViewController exportDiagnostics handles failure and updates error state", async () => {
  const announcements = [];
  const ctl = new DeviceViewController({
    tr,
    invoke: async () => {
      throw new Error("Bridge connection timeout");
    },
    onAnnounce: (msg) => {
      announcements.push(msg);
    },
  });

  await assert.rejects(async () => {
    await ctl.exportDiagnostics();
  }, /Bridge connection timeout/);

  assert.equal(ctl.diagnostics.exporting, false);
  assert.equal(ctl.diagnostics.error, "Bridge connection timeout");
  assert.equal(announcements.length, 1);
});
