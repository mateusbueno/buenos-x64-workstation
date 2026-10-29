// Story 9.22: Automated contrast check over both Stage (dark) and Studio (light) themes
// Validates WCAG 2.1 AA: text pairs >= 4.5:1, UI boundaries >= 3.0:1.
import test from "node:test";
import assert from "node:assert/strict";

function hexToRgb(hex) {
  const clean = hex.replace("#", "").trim();
  const num = parseInt(clean, 16);
  return {
    r: (num >> 16) & 255,
    g: (num >> 8) & 255,
    b: num & 255,
  };
}

function sRgbToLinear(c) {
  const norm = c / 255;
  return norm <= 0.04045 ? norm / 12.92 : Math.pow((norm + 0.055) / 1.055, 2.4);
}

function relativeLuminance({ r, g, b }) {
  return 0.2126 * sRgbToLinear(r) + 0.7152 * sRgbToLinear(g) + 0.0722 * sRgbToLinear(b);
}

export function contrastRatio(hex1, hex2) {
  const l1 = relativeLuminance(hexToRgb(hex1));
  const l2 = relativeLuminance(hexToRgb(hex2));
  const lighter = Math.max(l1, l2);
  const darker = Math.min(l1, l2);
  return (lighter + 0.05) / (darker + 0.05);
}

// Canonical color palettes from DESIGN.md / tokens.css
export const THEMES = {
  dark: {
    name: "Stage (Dark)",
    bg: "#0b0d0c",
    surface: "#151917",
    raised: "#202622",
    text: "#e7ebe8",
    muted: "#88938c",
    accent: "#9fbaa8",
    on_accent: "#0b0d0c",
    line_strong: "#5a6860",
    warn: "#cdb86c",
  },
  light: {
    name: "Studio (Light)",
    bg: "#f2f4f2",
    surface: "#e3e8e4",
    raised: "#ffffff",
    text: "#131714",
    muted: "#5a635c",
    accent: "#466853",
    on_accent: "#f2f4f2",
    line_strong: "#728a77",
    warn: "#725f1e",
  },
};

test("WCAG 2.1 AA Contrast: Stage (Dark) theme text pairs are >= 4.5:1", () => {
  const t = THEMES.dark;

  const pairs = [
    { name: "Primary text on background", text: t.text, bg: t.bg },
    { name: "Primary text on surface", text: t.text, bg: t.surface },
    { name: "Primary text on raised", text: t.text, bg: t.raised },
    { name: "Muted text on background", text: t.muted, bg: t.bg },
    { name: "Muted text on surface", text: t.muted, bg: t.surface },
    { name: "Text on accent button", text: t.on_accent, bg: t.accent },
    { name: "Warning text on background", text: t.warn, bg: t.bg },
  ];

  for (const { name, text, bg } of pairs) {
    const cr = contrastRatio(text, bg);
    assert.ok(
      cr >= 4.5,
      `${t.name}: ${name} (${text} on ${bg}) contrast ratio ${cr.toFixed(2)} must be >= 4.5:1`
    );
  }
});

test("WCAG 2.1 AA Contrast: Studio (Light) theme text pairs are >= 4.5:1", () => {
  const t = THEMES.light;

  const pairs = [
    { name: "Primary text on background", text: t.text, bg: t.bg },
    { name: "Primary text on surface", text: t.text, bg: t.surface },
    { name: "Primary text on raised", text: t.text, bg: t.raised },
    { name: "Muted text on background", text: t.muted, bg: t.bg },
    { name: "Muted text on surface", text: t.muted, bg: t.surface },
    { name: "Text on accent button", text: t.on_accent, bg: t.accent },
    { name: "Warning text on background", text: t.warn, bg: t.bg },
  ];

  for (const { name, text, bg } of pairs) {
    const cr = contrastRatio(text, bg);
    assert.ok(
      cr >= 4.5,
      `${t.name}: ${name} (${text} on ${bg}) contrast ratio ${cr.toFixed(2)} must be >= 4.5:1`
    );
  }
});

test("WCAG 2.1 AA Contrast: UI boundaries (line_strong) in both themes are >= 3.0:1", () => {
  for (const t of [THEMES.dark, THEMES.light]) {
    const boundaryPairs = [
      { name: "line_strong boundary on background", line: t.line_strong, bg: t.bg },
      { name: "line_strong boundary on surface", line: t.line_strong, bg: t.surface },
    ];

    for (const { name, line, bg } of boundaryPairs) {
      const cr = contrastRatio(line, bg);
      assert.ok(
        cr >= 3.0,
        `${t.name}: ${name} (${line} on ${bg}) contrast ratio ${cr.toFixed(2)} must be >= 3.0:1`
      );
    }
  }
});
