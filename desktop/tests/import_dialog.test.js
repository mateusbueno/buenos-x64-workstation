// Unit tests for ImportController (Story 9.19, FR122, FR92, AR30, UX-DR77, UX-DR91, UX-DR114).
import test from "node:test";
import assert from "node:assert/strict";

import {
  ImportController,
  COLLISION_ACTION,
} from "../src/import_dialog.js";

function makeSampleManifest() {
  return {
    file_name: "test_export.velorasets",
    file_path: "/tmp/test_export.velorasets",
    format_version: 1,
    scope: "bundle",
    device_model: "Velora 73",
    firmware_version: "1.4.0",
    collision_count: 1,
    missing_libraries: ["Grand Piano Studio"],
    missing_libraries_report: "1 Live Sets need libraries: Grand Piano Studio (factory)",
    items: [
      {
        id: "ls-1",
        name: "Gig Opener",
        category: "Live Set",
        item_type: "live_set",
        has_collision: true,
        collision_type: "live_set_id",
        is_reference_only: false,
        missing_libraries: ["Grand Piano Studio (factory)"],
      },
      {
        id: "ls-2",
        name: "Acoustic Solo",
        category: "Live Set",
        item_type: "live_set",
        has_collision: false,
        collision_type: null,
        is_reference_only: false,
        missing_libraries: [],
      },
      {
        id: "user-lib:synth.sfz",
        name: "Synth Lead",
        category: "User Timbre",
        item_type: "timbre",
        has_collision: true,
        collision_type: "timbre_key",
        is_reference_only: false,
        missing_libraries: [],
      },
    ],
  };
}

test("ImportController: initial state and manifest loading", () => {
  const ctl = new ImportController();
  assert.equal(ctl.isOpen, false);
  assert.equal(ctl.isImporting, false);

  ctl.loadManifest(makeSampleManifest());
  assert.equal(ctl.isOpen, true);
  assert.equal(ctl.items.length, 3);

  // Default action for colliding items is KEEP_BOTH
  const collidingItem = ctl.items.find((i) => i.id === "ls-1");
  assert.equal(collidingItem.collision_action, COLLISION_ACTION.KEEP_BOTH);

  const summary = ctl.computeSummary();
  assert.equal(summary.totalItems, 3);
  assert.equal(summary.collisionCount, 2);
  assert.equal(summary.keepBothCount, 3);
  assert.equal(summary.canImport, true);
});

test("ImportController: per-item action change and apply-all", () => {
  let changedSummary = null;
  const ctl = new ImportController({
    onChange: (sum) => {
      changedSummary = sum;
    },
  });
  ctl.loadManifest(makeSampleManifest());

  // Set action on single item
  ctl.setItemAction("ls-1", COLLISION_ACTION.REPLACE);
  assert.equal(ctl.items.find((i) => i.id === "ls-1").collision_action, COLLISION_ACTION.REPLACE);
  assert.equal(changedSummary.replaceCount, 1);
  assert.equal(changedSummary.keepBothCount, 2);

  // Apply to all
  ctl.applyAllAction(COLLISION_ACTION.SKIP);
  assert.equal(ctl.items.find((i) => i.id === "ls-1").collision_action, COLLISION_ACTION.SKIP);
  assert.equal(ctl.items.find((i) => i.id === "user-lib:synth.sfz").collision_action, COLLISION_ACTION.SKIP);
  assert.equal(changedSummary.skipCount, 2);
});

test("ImportController: format_version > 1 refusal", () => {
  const ctl = new ImportController();
  ctl.loadManifest({
    file_name: "newer_project.velorasets",
    file_path: "/tmp/newer_project.velorasets",
    format_version: 2,
    items: [],
  });

  const summary = ctl.computeSummary();
  assert.equal(summary.canImport, false);
  assert.equal(
    summary.errorBanner,
    "Couldn't import newer_project.velorasets: file made by a newer Velora Studio. Update the app."
  );
});

test("ImportController: missing factory library reporting does not block import", () => {
  const ctl = new ImportController();
  ctl.loadManifest(makeSampleManifest());

  const summary = ctl.computeSummary();
  assert.equal(summary.missingLibrariesReport, "1 Live Sets need libraries: Grand Piano Studio (factory)");
  assert.equal(summary.canImport, true);
});

test("ImportController: execution lifecycle and aria-live announcements", () => {
  let importedPayload = null;
  let announcedMsg = null;
  const ctl = new ImportController({
    deviceName: "Velora 73",
    onImport: (payload) => {
      importedPayload = payload;
    },
    onAnnounce: (msg) => {
      announcedMsg = msg;
    },
  });

  ctl.loadManifest(makeSampleManifest());
  ctl.setItemAction("ls-1", COLLISION_ACTION.REPLACE);

  const started = ctl.startImport();
  assert.equal(started, true);
  assert.equal(ctl.isImporting, true);
  assert.deepEqual(importedPayload.decisions, [
    { id: "ls-1", action: COLLISION_ACTION.REPLACE },
    { id: "ls-2", action: COLLISION_ACTION.KEEP_BOTH },
    { id: "user-lib:synth.sfz", action: COLLISION_ACTION.KEEP_BOTH },
  ]);

  ctl.updateProgress(2, 3);
  assert.equal(ctl.progress.current, 2);
  assert.equal(ctl.progress.total, 3);

  ctl.finishImport({ importedCount: 2, skippedCount: 1 });
  assert.equal(ctl.isImporting, false);
  assert.equal(ctl.isOpen, false);
  assert.equal(announcedMsg, "Imported 2 items · 1 skipped");
});

test("ImportController: cancel import lifecycle", () => {
  let cancelCalled = false;
  let announcedMsg = null;
  const ctl = new ImportController({
    onCancel: () => {
      cancelCalled = true;
    },
    onAnnounce: (msg) => {
      announcedMsg = msg;
    },
  });

  ctl.loadManifest(makeSampleManifest());
  ctl.startImport();
  ctl.cancelImport();

  assert.equal(cancelCalled, true);
  assert.equal(ctl.isImporting, false);
  assert.equal(announcedMsg, "Import cancelled");
});

test("ImportController: handle connection lost mid-import", () => {
  let announcedMsg = null;
  const ctl = new ImportController({
    onAnnounce: (msg) => {
      announcedMsg = msg;
    },
  });

  ctl.loadManifest(makeSampleManifest());
  ctl.startImport();
  ctl.handleConnectionLost({ importedCount: 1, totalCount: 3 });

  assert.equal(ctl.isImporting, false);
  const expectedMsg = "connection lost · 1 of 3 imported. Reconnect and import again; finished items are kept.";
  assert.equal(ctl.errorBanner, expectedMsg);
  assert.equal(announcedMsg, expectedMsg);
});
