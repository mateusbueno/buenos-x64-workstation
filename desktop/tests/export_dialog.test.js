// Unit tests for ExportController (Story 9.18, AD-10, AD-14, FR122, AR29, AR30).
import test from "node:test";
import assert from "node:assert/strict";

import {
  ExportController,
  EXPORT_SCOPE,
  TWO_GIGABYTES,
  formatBytes,
  isFactoryTimbre,
} from "../src/export_dialog.js";

function makeSampleLiveSets() {
  return [
    {
      id: "ls-1",
      name: "Gig Opener",
      parts: [
        {
          timbre: {
            library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8", // Factory Grand Piano
            instrument_key: "pianos/concert_grand.sfz",
          },
        },
        {
          timbre: {
            library_id: "user-lib-synth", // User Timbre
            instrument_key: "lead/lead.sfz",
          },
        },
      ],
    },
    {
      id: "ls-2",
      name: "Slow Ballad",
      parts: [
        {
          timbre: {
            library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8", // Factory Grand Piano shared
            instrument_key: "pianos/concert_grand.sfz",
          },
        },
      ],
    },
    {
      id: "ls-3",
      name: "Organ Solo",
      parts: [
        {
          timbre: {
            library_id: "user-lib-organ",
            instrument_key: "organs/b3.sf2",
          },
        },
      ],
    },
  ];
}

function makeSampleTimbres() {
  return [
    {
      library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8",
      instrument_key: "pianos/concert_grand.sfz",
      name: "Concert Grand",
      category: "Piano",
      size_bytes: 0,
      is_factory: true,
    },
    {
      library_id: "user-lib-synth",
      instrument_key: "lead/lead.sfz",
      name: "Analog Lead",
      category: "Synth Lead",
      size_bytes: 50 * 1024 * 1024, // 50 MB
      is_factory: false,
    },
    {
      library_id: "user-lib-organ",
      instrument_key: "organs/b3.sf2",
      name: "Classic B3",
      category: "Organ",
      size_bytes: 100 * 1024 * 1024, // 100 MB
      is_factory: false,
    },
  ];
}

test("formatBytes formats B, KB, MB, and GB accurately", () => {
  assert.equal(formatBytes(0), "0 B");
  assert.equal(formatBytes(512), "512 B");
  assert.equal(formatBytes(2048), "2.0 KB");
  assert.equal(formatBytes(50 * 1024 * 1024), "50.0 MB");
  assert.equal(formatBytes(2.5 * 1024 * 1024 * 1024), "2.5 GB");
});

test("isFactoryTimbre identifies factory and partner IDs and ignores user IDs", () => {
  assert.equal(isFactoryTimbre("c82b9b7e-9669-424a-9ef8-e0eef0437aa8"), true);
  assert.equal(isFactoryTimbre("C82B9B7E-9669-424A-9EF8-E0EEF0437AA8"), true);
  assert.equal(isFactoryTimbre("factory-epiano"), true);
  assert.equal(isFactoryTimbre("vendor-partner-pack"), true);
  assert.equal(isFactoryTimbre("user-lib-synth"), false);
  assert.equal(isFactoryTimbre("my_imported_sfz"), false);
  assert.equal(isFactoryTimbre(null), false);
});

test("ExportController: initial state and open with defaults", () => {
  const liveSets = makeSampleLiveSets();
  const timbres = makeSampleTimbres();
  const ctl = new ExportController({ liveSets, timbres });

  assert.equal(ctl.isOpen, false);
  ctl.open({ scope: EXPORT_SCOPE.BUNDLE });

  assert.equal(ctl.isOpen, true);
  assert.equal(ctl.scope, EXPORT_SCOPE.BUNDLE);
  // Default open in BUNDLE selects all live sets
  assert.equal(ctl.selectedLiveSetIds.size, 3);
  // Dependencies are automatically locked
  assert.equal(ctl.lockedTimbreKeys.size, 3); // Factory grand, user synth, user organ
});

test("ExportController: bundle dependency locking and reference tag at 0 B", () => {
  const liveSets = makeSampleLiveSets();
  const timbres = makeSampleTimbres();
  const ctl = new ExportController({ liveSets, timbres });

  ctl.open({ scope: EXPORT_SCOPE.BUNDLE, preselectLiveSetId: "ls-1" });
  assert.equal(ctl.selectedLiveSetIds.size, 1);
  assert.ok(ctl.selectedLiveSetIds.has("ls-1"));

  const est = ctl.computeEstimate();
  assert.equal(est.liveSetCount, 1);
  assert.equal(est.timbreCount, 2);
  assert.equal(est.referenceCount, 1); // 1 factory timbre

  // Find factory timbre item
  const factoryItem = est.items.find((i) => i.isReferenceOnly);
  assert.ok(factoryItem);
  assert.equal(factoryItem.sizeBytes, 0);
  assert.equal(factoryItem.isLocked, true);

  // Find user timbre item
  const userItem = est.items.find((i) => !i.isReferenceOnly && i.itemType === "timbre");
  assert.ok(userItem);
  assert.equal(userItem.sizeBytes, 50 * 1024 * 1024);
  assert.equal(userItem.isLocked, true);
});

test("ExportController: toggling Live Sets updates dependencies and total size", () => {
  const liveSets = makeSampleLiveSets();
  const timbres = makeSampleTimbres();
  const ctl = new ExportController({ liveSets, timbres });

  ctl.open({ scope: EXPORT_SCOPE.BUNDLE });
  ctl.toggleSelectAll(false);
  assert.equal(ctl.selectedLiveSetIds.size, 0);
  assert.equal(ctl.lockedTimbreKeys.size, 0);

  let est = ctl.computeEstimate();
  assert.equal(est.canExport, false);
  assert.equal(est.liveSetCount, 0);
  assert.equal(est.timbreCount, 0);

  // Toggle ls-2 (only uses factory piano)
  ctl.toggleLiveSet("ls-2");
  est = ctl.computeEstimate();
  assert.equal(est.canExport, true);
  assert.equal(est.liveSetCount, 1);
  assert.equal(est.timbreCount, 1);
  assert.equal(est.referenceCount, 1);
  assert.equal(est.userTimbreCount, 0);
  assert.equal(est.totalSizeBytes, 1024); // only the live set, 0 B for reference timbre
  assert.ok(est.summaryText.includes("1 Live Sets, 1 timbres (1 by reference)"));

  // Toggle ls-3 (uses user organ 100 MB)
  ctl.toggleLiveSet("ls-3");
  est = ctl.computeEstimate();
  assert.equal(est.liveSetCount, 2);
  assert.equal(est.timbreCount, 2);
  assert.equal(est.referenceCount, 1);
  assert.equal(est.userTimbreCount, 1);
  assert.equal(est.totalSizeBytes, 2048 + 100 * 1024 * 1024);
});

test("ExportController: Timbres scope restricts factory timbres", () => {
  const liveSets = makeSampleLiveSets();
  const timbres = makeSampleTimbres();
  const ctl = new ExportController({ liveSets, timbres });

  ctl.open({ scope: EXPORT_SCOPE.TIMBRES });
  assert.equal(ctl.selectedTimbreKeys.size, 0);

  const factoryKey = "c82b9b7e-9669-424a-9ef8-e0eef0437aa8:pianos/concert_grand.sfz";
  ctl.toggleTimbre(factoryKey);
  // Factory timbre cannot be selected in Timbres scope
  assert.equal(ctl.selectedTimbreKeys.has(factoryKey), false);

  const userKey = "user-lib-synth:lead/lead.sfz";
  ctl.toggleTimbre(userKey);
  assert.equal(ctl.selectedTimbreKeys.has(userKey), true);

  const est = ctl.computeEstimate();
  assert.equal(est.canExport, true);
  assert.equal(est.timbreCount, 1);
  assert.equal(est.liveSetCount, 0);
  assert.equal(est.totalSizeBytes, 50 * 1024 * 1024);
});

test("ExportController: 2 GB warning line triggers on large bundles", () => {
  const liveSets = [
    {
      id: "ls-big",
      name: "Gig Huge",
      parts: [{ timbre: { library_id: "user-huge", instrument_key: "huge.sfz" } }],
    },
  ];
  const timbres = [
    {
      library_id: "user-huge",
      instrument_key: "huge.sfz",
      name: "Huge Instrument",
      category: "Keys",
      size_bytes: 2.5 * 1024 * 1024 * 1024, // 2.5 GB
      is_factory: false,
    },
  ];

  const ctl = new ExportController({ liveSets, timbres });
  ctl.open({ scope: EXPORT_SCOPE.BUNDLE });

  const est = ctl.computeEstimate();
  assert.equal(est.exceeds2GB, true);
  assert.ok(est.totalSizeBytes > TWO_GIGABYTES);
  assert.equal(est.canExport, true); // Warns but allows if space suffices
});

test("ExportController: insufficient destination free space blocks Export", () => {
  const liveSets = makeSampleLiveSets();
  const timbres = makeSampleTimbres();
  const ctl = new ExportController({
    liveSets,
    timbres,
    freeSpaceBytes: 10 * 1024 * 1024, // 10 MB free space
  });

  ctl.open({ scope: EXPORT_SCOPE.BUNDLE, preselectLiveSetId: "ls-1" }); // uses 50 MB synth
  const est = ctl.computeEstimate();

  assert.equal(est.insufficientFreeSpace, true);
  assert.equal(est.canExport, false); // Blocked because required (50 MB) > free (10 MB)
});

test("ExportController: export execution lifecycle, progress, and cancel", () => {
  let exportedParams = null;
  let cancelledCalled = false;
  let announcedMsg = "";

  const liveSets = makeSampleLiveSets();
  const timbres = makeSampleTimbres();
  const ctl = new ExportController({
    liveSets,
    timbres,
    onExport: (p) => {
      exportedParams = p;
    },
    onCancel: () => {
      cancelledCalled = true;
    },
    onAnnounce: (msg) => {
      announcedMsg = msg;
    },
  });

  ctl.open({ scope: EXPORT_SCOPE.SETS, preselectLiveSetId: "ls-1" });
  assert.equal(ctl.isExporting, false);

  const started = ctl.startExport();
  assert.equal(started, true);
  assert.equal(ctl.isExporting, true);
  assert.ok(exportedParams);
  assert.equal(exportedParams.scope, "sets");
  assert.deepEqual(exportedParams.live_set_ids, ["ls-1"]);

  // Progress update
  ctl.updateProgress(1024, 2048);
  assert.equal(ctl.progress.currentBytes, 1024);

  // Cancel
  ctl.cancelExport();
  assert.equal(cancelledCalled, true);
  assert.equal(ctl.isExporting, false);
  assert.equal(announcedMsg, "Export cancelled");

  // Re-start and finish
  ctl.open({ scope: EXPORT_SCOPE.SETS, preselectLiveSetId: "ls-1" });
  ctl.startExport();
  ctl.finishExport({ fileName: "Gig.velorasets", liveSetCount: 1, timbreCount: 0 });
  assert.equal(ctl.isOpen, false);
  assert.equal(announcedMsg, "Exported 1 Live Sets and 0 timbres to Gig.velorasets");
});
