// Velora Studio: the Part Editor view model and controller (Story 9.9). A pure ES module: no DOM, no Tauri, no locale catalog. It turns the generated parameter registry
// (params_registry.js, from proto/params.json) plus what the unit says about the selected part (engine State, live performance values) into the four-section model
// part_editor_dom.js draws, defines the Studio Knob rules (steps, keys, drag, wheel, quantisation, value text, value entry), maps every edit onto the ws.v1 command the
// engine already has, and sends the edits (coalesced per knob, optimistic, reverted on a typed error).
//
// THE REGISTRY IS THE LIST. Nothing in here names a parameter to draw it: a parameter appears because its registry entry says which section, engine and group it belongs
// to (derive_ui in proto/gen_params.py, see proto/README.md "Part Editor grouping"). The few controls that are not registry parameters (part volume, the zone, the insert chain,
// the ASSIGN map) are the "builtin" controls below, each tied to the engine command that edits it.
//
// AUDIBILITY (Story 9.9 review A1, mostly resolved by the engine follow-up). Every registry part parameter reaches its engine through Engine::set_part_param / set_part_param_edit:
// organ parameters (OrganEngine), fm_* (FmEngine), va_* (VaEngine), part_attack / part_release for sample parts (SampleEngine), and part_brightness / part_eq_* for EVERY engine
// (the Mixer's per-part tone stage). FM and VA patch parameters edit the patch the part's NEXT notes start with (VA's continuous ones, and fm_brightness, also move notes that
// already sound, smoothed). What is still STORED BUT NOT AUDIBLE is the short table `INERT_BY_ENGINE` below: part_attack / part_release on organ and FM parts (the organ has no
// amp envelope; an FM part's contour is its six operator envelopes, a second amp envelope would double-shape it) and va_unison_spread (the VA voice sums its unison voices to
// one mono signal before the filter, so there is no stereo spread to move). Those knobs carry a hint (tooltip / aria-description) and their section a note; remove a row when an
// engine starts consuming the parameter. The read-back (GetPerformanceValues) confirms the STORED value, never that it sounds.
//
// ASSIGN note (A4): a part-scope parameter assigned to K1..K8 lands on State.encoders.selected_part (the unit's selected part, SelectPart is denied over the bridge), not on the
// part being edited here. ZONE / ARPEGGIATOR (Story 9.23): edited via additive structural commands SetPartZone { part, optional low_key, high_key, octave, transpose,
// keyboard_on } and SetPartArp { part, arp_* }, with PartState carrying octave, transpose and arp fields.
//
// COMMAND TABLE (what a knob sends; proto/ws_v1.proto is the authority). `base_revision` (ClientMessage field 2) is carried on every one of them and IGNORED by the engine
// until Story 9.10.
//   registry class / scope            parameter                               command
//   performance / part                every one except the two sends          SetPerformanceParam { param_id, part, value }   (queued as a timestamped UMP, applied by the RT plane at the next block when audio runs; not saved)
//   performance / master              (not shown in the Part Editor)          SetPerformanceParam { param_id, part: 0, value }
//   performance / part                send_reverb_level, send_delay_level     BOTH SetPerformanceParam (sounds at once; this is the live value shown) AND SetSendLevels { part, reverb, delay }
//                                                                             (structural: persists in the snapshot and marks the Live Set dirty)
//   structural  / master              (not shown in the Part Editor)          SetMasterParams { values: [{ param_id, value }] }
//   structural  / part                part_output                             SetPartOutput { part, output }
//   structural  / insert              insert_type, insert_bypass, insert_param_N, and add / remove / reorder   SetInsertChain { part, slots }   (always the FULL chain)
//   structural  / part                timbre_ref                              Story 9.11 timbre picker
//   structural  / part                arp_on .. arp_latch                     SetPartArp { part, ... } (Story 9.23)
//   builtin                           part volume                             SetPartVolume { part, volume }
//   builtin                           low key, high key, keyboard on/off, octave, transpose   SetPartZone { part, ... } (Story 9.23)
//   builtin                           ASSIGN K1..K8                           SetAssignSlot { target: CURRENT_LIVE_SET, slot, param_id } / ClearAssignSlot { target, slot }
// Everything is edited on the Live Set that SOUNDS (the engine has one live graph); a Live Set that is shown but not playing is read-only ("not playing on the Velora").

import { REGISTRY } from "./params_registry.js";
import { createThrottledRunner, noteName } from "./live_set_view.js";
import { liveRevision, isStale } from "./live_edit_state.js";

export const SECTION_ORDER = Object.freeze(["sound", "amp_eq", "effects", "zone"]);
// the section titles are the strings Story 12.13's editor already has
export const SECTION_TITLE_KEYS = Object.freeze({ sound: "studio.part_editor.sound", amp_eq: "studio.part_editor.amp_eq", effects: "studio.part_editor.effects", zone: "studio.part_editor.zone_arp_assign" });
export const ASSIGN_SLOTS = 8;
export const ASSIGN_TARGET_CURRENT_LIVE_SET = 1;
export const SEND_PARAMS = Object.freeze({ reverb: "send_reverb_level", delay: "send_delay_level" });
// Reasons (locale keys) shown on a read-only control.
export const REASON_NOT_PLAYING = "studio.part_editor.ro_not_playing";
export const REASON_NO_COMMAND = "studio.part_editor.ro_no_command";
export const REASON_UNAVAILABLE = "studio.part_editor.ro_engine_unavailable";
export const REASON_STALE = "studio.part_editor.stale_banner"; // Story 9.10: the Live Set changed on the unit after this view was loaded; knobs are read-only until Reload

/**
 * The columns the editor shows at a container width. style.css: `repeat(auto-fit, minmax(min(100%, 15.1rem), 1fr))` with a 1rem gap (grows with the TEXT size: rem), and one column
 * when the container is at most 45em of its own font (0.8125 rem) wide. `columnsFor(containerPx, rootPx)` mirrors both (rootPx = the root font size: 16 at 100 % text).
 */
export const REFLOW = Object.freeze({ minColumnRem: 15.1, gapRem: 1, oneColumnEm: 45, bodyRem: 0.8125 });
export function columnsFor(containerPx, rootPx) {
  if (!(containerPx > 0) || !(rootPx > 0)) return 1;
  if (containerPx <= REFLOW.oneColumnEm * REFLOW.bodyRem * rootPx) return 1;
  const gap = REFLOW.gapRem * rootPx;
  return Math.max(1, Math.floor((containerPx + gap) / (REFLOW.minColumnRem * rootPx + gap)));
}

/**
 * Knobs that are STORED but NOT AUDIBLE (still), per engine: a pattern over registry parameter names (performance, part scope) whose engine does not consume them.
 * organ and fm: part_attack / part_release (no amp envelope on the organ; the FM contour is the operator envelopes). va: va_unison_spread (mono unison sum, see the header).
 * sample is absent: part_attack / part_release are its amp envelope overrides and part_brightness / part_eq_* are played by the Mixer's tone stage for every engine.
 * Sends, output, volume, inserts and ASSIGN are never listed.
 */
export const INERT_BY_ENGINE = Object.freeze({ organ: /^part_(attack|release)$/, fm: /^part_(attack|release)$/, va: /^va_unison_spread$/ });
export function isInert(def, engine) {
  const re = INERT_BY_ENGINE[engine];
  return !!re && def.class === "performance" && def.scope === "part" && commandKind(def) === "perf" && re.test(def.name);
}

const num = (v, d) => (typeof v === "number" && Number.isFinite(v) ? v : d);

// ---- registry views ----------------------------------------------------------------------------------------------------------------------------------------

/** The registry's parameters this part's editor shows: not master scope, not hidden, not an insert parameter (the chain editor owns those), for this engine or every engine. */
export function partParams(registry, engine) {
  return registry.params.filter((d) => d.scope !== "insert" && d.section !== "master" && d.section !== "hidden" && (d.engine === "any" || d.engine === engine) && !(Array.isArray(d.hideEngines) && d.hideEngines.includes(engine)));
}

export function insertDefs(registry) {
  const by = (n) => registry.params.find((d) => d.name === n);
  const params = registry.params.filter((d) => /^insert_param_[0-9]+$/.test(d.name)).sort((a, b) => a.id - b.id);
  return { type: by("insert_type"), bypass: by("insert_bypass"), params };
}

/** Parameters an ASSIGN encoder may drive that this part can show (its engine's and the engine-independent ones, plus master-scope ones like tempo). */
export function assignableParams(registry, engine) {
  return registry.params.filter((d) => d.assignable && d.class === "performance" && (d.scope === "master" || d.engine === "any" || d.engine === engine));
}

/** Which engine command edits this registry parameter: "perf" | "sends" | "output" | "master" | "insert" | null (no command: read-only, deferred). */
export function commandKind(def) {
  if (def.scope === "insert") return "insert";
  if (def.class === "performance") return def.name === SEND_PARAMS.reverb || def.name === SEND_PARAMS.delay ? "sends" : "perf";
  if (def.scope === "master") return "master";
  if (def.name === "part_output") return "output";
  if (def.name.startsWith("arp_")) return "arp";
  return null; // timbre_ref: no engine command from Studio yet
}

// ---- knob rules --------------------------------------------------------------------------------------------------------------------------------------------

function niceStep(range) {
  const raw = range / 100;
  if (!(raw > 0)) return 1;
  const exp = Math.pow(10, Math.floor(Math.log10(raw)));
  for (const m of [1, 2, 5, 10]) if (m * exp >= raw * (1 - 1e-9)) return m * exp;
  return 10 * exp;
}

export function decimalsOf(step) {
  const s = String(step);
  const e = /e-(\d+)$/.exec(s);
  if (e) return Number(e[1]);
  const i = s.indexOf(".");
  return i < 0 ? 0 : s.length - i - 1;
}

const roundTo = (v, d) => Number(v.toFixed(Math.min(12, d)));

/** The control rules of a parameter definition (or a builtin descriptor with the same fields): range, step, large step and how drag maps. */
export function knobSpec(def, registry = REGISTRY) {
  const type = def.type;
  let min = def.min;
  let max = def.max;
  let enumValues = null;
  if (type === "enum") {
    enumValues = (registry.enums && registry.enums[def.enum]) || [];
    min = 0;
    max = Math.max(0, enumValues.length - 1);
  } else if (type === "bool") {
    min = 0;
    max = 1;
  }
  let step = 1;
  let large = 1;
  if (type === "float") {
    step = def.step || niceStep(max - min);
    large = step * 10;
  } else if (type === "int" || type === "uint") {
    step = 1;
    large = Math.max(1, Math.round((max - min) / 10));
  } else if (type === "enum") {
    large = Math.max(1, Math.floor((max + 1) / 4));
  }
  // A wide time / frequency range (1..5000 ms, 20..20000 Hz) is stepped on a LOGARITHMIC grid of LOG_STEPS points, so a 5 ms attack and a 100 Hz cutoff are reachable by keys and drag.
  const log = type === "float" && (def.unit === "ms" || def.unit === "Hz") && min > 0 && max / min >= 20;
  return { type, unit: def.unit || "", min, max, step, large, log, enumName: def.enum || null, enumValues, decimals: type === "float" ? decimalsOf(step) : 0, def: def.default };
}

export const LOG_STEPS = 100;
const sig3 = (v) => Number(v.toPrecision(3));
function logIndex(spec, v) {
  const x = Math.min(spec.max, Math.max(spec.min, v));
  return Math.round((Math.log(x / spec.min) / Math.log(spec.max / spec.min)) * LOG_STEPS);
}
function fromLogIndex(spec, i) {
  const k = Math.min(LOG_STEPS, Math.max(0, Math.round(i)));
  if (k === 0) return spec.min;
  if (k === LOG_STEPS) return spec.max;
  return Math.min(spec.max, Math.max(spec.min, sig3(spec.min * Math.pow(spec.max / spec.min, k / LOG_STEPS))));
}
/** `n` steps from `cur` (linear: n x step, logarithmic: n grid points). */
function stepBy(spec, cur, n) {
  return spec.log ? fromLogIndex(spec, logIndex(spec, cur) + n) : quantise(spec, cur + n * spec.step);
}

/** Clamp and snap a value to what the control can hold: bool 0/1, enum / int whole numbers, float multiples of the step (the range ends are always reachable). */
export function quantise(spec, v) {
  let x = typeof v === "number" ? v : Number(v);
  if (!Number.isFinite(x)) x = spec.type === "float" ? spec.min : spec.min;
  x = Math.min(spec.max, Math.max(spec.min, x));
  switch (spec.type) {
    case "bool": return x >= 0.5 ? 1 : 0;
    case "enum":
    case "int":
    case "uint": return Math.min(spec.max, Math.max(spec.min, Math.round(x)));
    default: {
      if (spec.log) return fromLogIndex(spec, logIndex(spec, x));
      const snapped = roundTo(Math.round(x / spec.step) * spec.step, spec.decimals + 2);
      return Math.min(spec.max, Math.max(spec.min, snapped));
    }
  }
}

/**
 * What a key does to a knob. `ev` is `{ key, shiftKey }`. Returns `{ handled: false }` for a key the knob ignores, `{ handled: true, entry: true }` for Enter (the DOM opens the
 * value entry) and `{ handled: true, value }` (already quantised) for the rest: Up / Right +1 step, Down / Left -1, Shift = 10 steps, PageUp / PageDown = the large step,
 * Home / End = min / max, Space toggles a switch.
 */
export function applyKey(spec, value, ev) {
  const cur = Number.isFinite(value) ? value : spec.min;
  const n = ev.shiftKey ? 10 : 1;
  switch (ev.key) {
    case "ArrowUp":
    case "ArrowRight": return { handled: true, value: stepBy(spec, cur, n) };
    case "ArrowDown":
    case "ArrowLeft": return { handled: true, value: stepBy(spec, cur, -n) };
    case "PageUp": return { handled: true, value: spec.log ? stepBy(spec, cur, 10) : quantise(spec, cur + spec.large) };
    case "PageDown": return { handled: true, value: spec.log ? stepBy(spec, cur, -10) : quantise(spec, cur - spec.large) };
    case "Home": return { handled: true, value: spec.min };
    case "End": return { handled: true, value: spec.max };
    case " ":
    case "Spacebar": return spec.type === "bool" ? { handled: true, value: cur >= 0.5 ? 0 : 1 } : { handled: false };
    case "Enter": return { handled: true, entry: true };
    default: return { handled: false };
  }
}

/** The wheel: one notch (deltaY < 0 is up) is one step, Shift one large step. */
export function applyWheel(spec, value, ev) {
  if (!ev || !ev.deltaY) return value;
  const dir = ev.deltaY < 0 ? 1 : -1;
  const cur = Number.isFinite(value) ? value : spec.min;
  if (spec.log) return stepBy(spec, cur, dir * (ev.shiftKey ? 10 : 1));
  return quantise(spec, cur + dir * (ev.shiftKey ? spec.large : spec.step));
}

export const DRAG = Object.freeze({ fullRangePx: 200, fineRangePx: 1000, discretePx: 24 });
/** A pointer drag: `dx` / `dy` are pixels moved since the press (right and up are positive). Continuous values cover the range in 200 px (1000 px with Shift); switches, enums and small integers take 24 px a step. */
export function applyDrag(spec, startValue, dx, dy, shift) {
  const delta = (dx || 0) - (dy || 0);
  const range = spec.max - spec.min;
  const start = Number.isFinite(startValue) ? startValue : spec.min;
  if (spec.log) return fromLogIndex(spec, logIndex(spec, start) + (delta / (shift ? DRAG.fineRangePx : DRAG.fullRangePx)) * LOG_STEPS);
  if (spec.type === "bool" || spec.type === "enum" || range <= 12) {
    return quantise(spec, start + Math.round(delta / (shift ? DRAG.discretePx * 4 : DRAG.discretePx)));
  }
  return quantise(spec, start + (delta / (shift ? DRAG.fineRangePx : DRAG.fullRangePx)) * range);
}

// ---- value text and entry ---------------------------------------------------------------------------------------------------------------------------------

const DASH = "—";
const SIGNED_UNITS = new Set(["dB", "semitone", "cents", "octave"]);
const trOr = (tr, key, fallback) => {
  const t = tr ? tr(key) : key;
  return t && t !== key ? t : fallback;
};

export function enumLabel(spec, index, tr) {
  const name = spec.enumValues && spec.enumValues[index];
  if (name === undefined) return DASH;
  return trOr(tr, `studio.enum.${spec.enumName}.${name}`, name.replace(/_/g, " "));
}

function numText(v, decimals) {
  const t = v.toFixed(Math.max(0, Math.min(6, decimals)));
  return t === "-0" || /^-0\.0*$/.test(t) ? t.slice(1) : t;
}

/** The text of a value with its unit, as shown and as aria-valuetext ("+3.5 dB", "300 ms", "62 %", "On", "Hall", "C2"). null / NaN give "—" (no value known). */
export function formatValue(spec, v, tr) {
  if (v === null || v === undefined || (typeof v === "number" && Number.isNaN(v))) return DASH;
  if (spec.type === "bool") return v >= 0.5 ? trOr(tr, "studio.knob.on", "On") : trOr(tr, "studio.knob.off", "Off");
  if (spec.type === "enum") return enumLabel(spec, Math.round(v), tr);
  const unit = spec.unit;
  const sign = (n, text) => (SIGNED_UNITS.has(unit) && spec.min < 0 && n > 0 ? "+" + text : text);
  if (unit === "note") return noteName(Math.round(v)) || DASH;
  if (unit === "ratio" && spec.max <= 1) return `${numText(v * 100, Math.max(0, spec.decimals - 2))} %`;
  if (unit === "deciHz") return `${numText(v / 10, 1)} Hz`;
  if (unit === "percent") return `${numText(v, spec.decimals)} %`;
  const text = spec.log ? numText(v, v >= 100 ? 0 : v >= 10 ? 1 : 2) : numText(v, spec.decimals);
  switch (unit) {
    case "dB": return `${sign(v, text)} dB`;
    case "ms": case "Hz": case "s": case "bpm": return `${text} ${unit}`;
    case "cents": return `${sign(v, text)} ct`;
    case "semitone": return `${sign(v, text)} st`;
    case "octave": return `${sign(v, text)} oct`;
    case "ratio": return spec.min >= 1 ? `${text}:1` : text; // a compressor ratio reads "4:1"; a 0..99 ratio fine tune is just a number
    default: return sign(v, text); // step, level, rate, depth, index, voices, id, flag ...
  }
}

/**
 * Parses what the player typed into a value. Accepts a decimal comma, a unit suffix (the unit of the control; "k" / "kHz" for Hz, "s" for ms, "%" for ratio), the label of an enum
 * value (any case, as shown), On / Off for a switch. Returns `{ ok: true, value }` (quantised) or `{ ok: false, reason: "invalid" | "range" }`; a value out of range is refused,
 * never clamped (the player is told, the knob keeps its value).
 */
export function parseEntry(spec, text, tr) {
  const raw = String(text === undefined || text === null ? "" : text).trim();
  if (!raw) return { ok: false, reason: "invalid" };
  const lower = raw.toLowerCase();
  if (spec.type === "bool") {
    const on = [trOr(tr, "studio.knob.on", "On"), "on", "1", "true", "yes"];
    const off = [trOr(tr, "studio.knob.off", "Off"), "off", "0", "false", "no"];
    if (on.some((t) => t.toLowerCase() === lower)) return { ok: true, value: 1 };
    if (off.some((t) => t.toLowerCase() === lower)) return { ok: true, value: 0 };
    return { ok: false, reason: "invalid" };
  }
  if (spec.type === "enum") {
    for (let i = 0; i <= spec.max; i++) if (enumLabel(spec, i, tr).toLowerCase() === lower || (spec.enumValues[i] || "").toLowerCase() === lower) return { ok: true, value: i };
    if (/^[0-9]+$/.test(raw)) {
      const n = Number(raw);
      return n >= 0 && n <= spec.max ? { ok: true, value: n } : { ok: false, reason: "range" };
    }
    return { ok: false, reason: "invalid" };
  }
  const m = /^([+-]?)\s*([0-9]+(?:[.,][0-9]+)?|[.,][0-9]+)\s*([a-zA-Z%¢:1]*)$/.exec(raw.replace(/−/g, "-"));
  if (!m) return { ok: false, reason: "invalid" };
  let n = Number(m[2].replace(",", "."));
  if (m[1] === "-") n = -n;
  const suffix = m[3].toLowerCase();
  if (suffix) {
    const u = spec.unit;
    const okSuffix = {
      Hz: ["hz", "k", "khz"], deciHz: ["hz"], ms: ["ms", "s"], s: ["s"], dB: ["db"], percent: ["%"], ratio: ["%", ":1"], cents: ["ct", "cents", "¢"], semitone: ["st"],
      octave: ["oct"], bpm: ["bpm"],
    }[u];
    if (!okSuffix || !okSuffix.includes(suffix)) return { ok: false, reason: "invalid" };
    if (u === "Hz" && (suffix === "k" || suffix === "khz")) n *= 1000;
    if (u === "ms" && suffix === "s") n *= 1000;
  }
  if (spec.unit === "deciHz") n *= 10;
  if (spec.unit === "ratio" && spec.max <= 1) n /= 100; // shown as a percentage, typed as one ("50" and "50 %" are both 0.5)
  if (spec.type !== "float" && !Number.isInteger(n)) return { ok: false, reason: "invalid" };
  const eps = spec.type === "float" ? spec.step / 10 : 1e-9;
  if (n < spec.min - eps || n > spec.max + eps) return { ok: false, reason: "range" };
  return { ok: true, value: quantiseEntry(spec, n) };
}

/** Typed values keep one more decimal than the step the keys use (a typed 0.255 is 0.255 for a 0..1 ratio: 0.26 would surprise), still clamped to the range. */
function quantiseEntry(spec, n) {
  if (spec.type !== "float") return quantise(spec, n);
  if (spec.log) return Math.min(spec.max, Math.max(spec.min, Number(n.toPrecision(6))));
  return Math.min(spec.max, Math.max(spec.min, roundTo(n, spec.decimals + 1)));
}

// ---- labels ---------------------------------------------------------------------------------------------------------------------------------------------------

/**
 * The text of a parameter: `studio.param.<name>` when the catalog has it, then the same key with the operator / oscillator number taken out (`fm_op3_freq_fine` ->
 * `studio.param.fm_op_freq_fine`, one translation for six operators), then the registry's locale_key (FM / VA share Story 12.13's strings), else the name with spaces.
 */
export function paramLabel(def, tr) {
  if (tr) {
    const generic = def.name.replace(/^fm_op[1-6]_/, "fm_op_").replace(/^va_osc[12]_/, "va_osc_");
    for (const key of [`studio.param.${def.name}`, `studio.param.${generic}`, def.localeKey]) {
      if (!key) continue;
      const t = tr(key);
      if (t && t !== key) return t;
    }
  }
  return def.name.replace(/_/g, " ");
}

export function groupTitleKey(group) {
  return `studio.group.${group}`;
}

// ---- insert chain ---------------------------------------------------------------------------------------------------------------------------------------------

/** The chain the State reports for a part, as plain slots `{ type, bypass, params[N] }` (missing parameters take the registry default). */
export function chainFromPartState(partState, registry = REGISTRY) {
  const defs = insertDefs(registry);
  const raw = partState && Array.isArray(partState.inserts) ? partState.inserts : [];
  return raw.slice(0, registry.insertSlots).map((s) => ({
    type: num(s && s.type, 0),
    bypass: !!(s && s.bypass),
    params: defs.params.map((d, i) => num(s && Array.isArray(s.params) ? s.params[i] : undefined, d.default)),
  }));
}

const cloneChain = (c) => c.map((s) => ({ type: s.type, bypass: s.bypass, params: s.params.slice() }));

// ---- the model ------------------------------------------------------------------------------------------------------------------------------------------------

function builtinKnob(key, name, labelKey, desc, value, extra = {}) {
  const spec = knobSpec(desc);
  return { key, kind: "builtin", id: name, labelKey, group: extra.group || "", ...spec, value, valueNow: value, ...extra };
}

/**
 * The editor of one part from what is known. `ctx`:
 *   registry, partIndex, engine ("sample" | "organ" | "fm" | "va" | other), state (the latest State or null), perf (Map "id:part" -> live value),
 *   pending (Map key -> { value }) the optimistic overlay, chain (the optimistic chain or null), editable (the Live Set sounds and the engine is available),
 *   available (false: the engine is missing in this firmware), assign (optimistic Map slot -> param id).
 * Every knob is `{ key, kind, id, name, group, type, unit, min, max, step, large, value, valueNow, readOnly, reason, pending, command }`; `value` is null when nothing is known
 * (the Live Set does not sound), the text is made by the DOM layer with `formatValue`.
 */
export function buildPartEditorModel(ctx) {
  const registry = ctx.registry || REGISTRY;
  const part = ctx.partIndex;
  const state = ctx.state || null;
  const pstate = state && Array.isArray(state.parts) ? state.parts.find((p) => p && p.part === part) || null : null;
  const pending = ctx.pending || new Map();
  const perf = ctx.perf || new Map();
  const editable = !!ctx.editable;
  const roReason = ctx.stale ? REASON_STALE : ctx.available === false ? REASON_UNAVAILABLE : REASON_NOT_PLAYING;
  const known = !!state && !!ctx.live; // `live`: the shown Live Set is the one that sounds, so State / live values are about it
  const deferred = [];

  const initial = new Map();
  for (const pv of (state && state.performance_init) || []) if (pv && pv.part !== undefined && pv.part !== null) initial.set(`${pv.param_id}:${pv.part}`, pv.value);

  const resolve = (def) => {
    const key = `p:${def.id}`;
    const p = pending.get(key);
    if (p) return { value: p.value, pending: true };
    if (!known) return { value: null, pending: false };
    const kind = commandKind(def);
    if (kind === "sends") {
      // the LIVE value (it moves with a CC, an encoder or a scene); the snapshot's initial level (PartState.send_*) only when no live value is known yet
      const v = pstate ? (def.name === SEND_PARAMS.reverb ? pstate.send_reverb : pstate.send_delay) : undefined;
      return { value: num(perf.get(`${def.id}:${part}`), num(v, def.default)), pending: false };
    }
    if (kind === "output") return { value: pstate ? num(pstate.output, def.default) : def.default, pending: false };
    if (kind === "arp") {
      if (!pstate) return { value: def.default, pending: false };
      const v = pstate[def.name];
      if (def.type === "bool") return { value: v ? 1 : 0, pending: false };
      return { value: num(v, def.default), pending: false };
    }
    if (kind === null) return { value: null, pending: false }; // timbre_ref: the State does not carry it yet
    const live = perf.get(`${def.id}:${part}`);
    if (live !== undefined) return { value: live, pending: false };
    return { value: num(initial.get(`${def.id}:${part}`), def.default), pending: false };
  };

  const paramKnob = (def) => {
    const spec = knobSpec(def, registry);
    const kind = commandKind(def);
    const r = resolve(def);
    let readOnly = false;
    let reason = "";
    if (kind === null) {
      readOnly = true;
      reason = REASON_NO_COMMAND;
      deferred.push({ id: def.name, key: `p:${def.id}`, reason });
    } else if (!editable) {
      readOnly = true;
      reason = roReason;
    }
    return {
      key: `p:${def.id}`, kind: "param", id: def.name, paramId: def.id, localeKey: def.localeKey, group: def.group, ...spec,
      value: r.value, valueNow: r.value, readOnly, reason, pending: r.pending, command: kind, inert: isInert(def, ctx.engine),
    };
  };

  // -- builtin controls --
  const volumeDesc = { type: "float", unit: "ratio", min: 0, max: 1, default: 0.7, step: 0.01 };
  const volPending = pending.get("vol");
  const volume = builtinKnob("vol", "part_volume", "studio.part_editor.volume", volumeDesc, volPending ? volPending.value : known && pstate ? num(pstate.volume, 0) : null,
    { group: "sound.main", readOnly: !editable, reason: editable ? "" : roReason, pending: !!volPending, command: "volume" });

  const zoneKnob = (name, desc, pstateVal) => {
    const key = `zone:${name}`;
    const p = pending.get(key);
    const val = p ? p.value : known ? (pstate && pstate.view_fields ? (desc.type === "bool" ? (pstateVal ? 1 : 0) : num(pstateVal, desc.default)) : desc.default) : null;
    return builtinKnob(key, name, `studio.part_editor.zone_${name}`, desc, val, {
      group: "zone.keyboard",
      readOnly: !editable,
      reason: editable ? "" : roReason,
      pending: !!p,
      command: "zone",
    });
  };
  const zoneKnobs = [
    zoneKnob("low_key", { type: "int", unit: "note", min: 0, max: 127, default: 0 }, pstate ? pstate.low_key : null),
    zoneKnob("high_key", { type: "int", unit: "note", min: 0, max: 127, default: 127 }, pstate ? pstate.high_key : null),
    zoneKnob("keyboard", { type: "bool", unit: "flag", min: 0, max: 1, default: 1 }, pstate ? pstate.keyboard_on : null),
    zoneKnob("octave", { type: "int", unit: "octave", min: -4, max: 4, default: 0 }, pstate ? pstate.octave : null),
    zoneKnob("transpose", { type: "int", unit: "semitone", min: -12, max: 12, default: 0 }, pstate ? pstate.transpose : null),
  ];

  // -- registry-driven groups per section --
  const sections = SECTION_ORDER.map((id) => ({ id, titleKey: SECTION_TITLE_KEYS[id], groups: [] }));
  const secById = new Map(sections.map((s) => [s.id, s]));
  const groupOf = (sec, g) => {
    let grp = sec.groups.find((x) => x.id === g);
    if (!grp) {
      grp = { id: g, titleKey: groupTitleKey(g), knobs: [] };
      sec.groups.push(grp);
    }
    return grp;
  };
  for (const def of partParams(registry, ctx.engine)) {
    const sec = secById.get(def.section) || secById.get("sound"); // an unknown section (a future one) lands in Sound: a new parameter is still shown
    groupOf(sec, def.group || "other").knobs.push(paramKnob(def));
  }
  groupOf(secById.get("sound"), "sound.main").knobs.unshift(volume);
  const zoneGroup = groupOf(secById.get("zone"), "zone.keyboard");
  zoneGroup.knobs.push(...zoneKnobs);
  // the keyboard group first in its section
  const zsec = secById.get("zone");
  zsec.groups.sort((a, b) => (a.id === "zone.keyboard" ? -1 : b.id === "zone.keyboard" ? 1 : 0));
  // sound.main first in Sound
  const ssec = secById.get("sound");
  ssec.groups.sort((a, b) => (a.id === "sound.main" ? -1 : b.id === "sound.main" ? 1 : 0));
  // empty groups are not drawn
  for (const s of sections) s.groups = s.groups.filter((g) => g.knobs.length > 0);

  // -- the envelope (Amp envelope + EQ) --
  const byName = new Map(registry.params.map((d) => [d.name, d]));
  const val = (name) => {
    const def = byName.get(name);
    if (!def) return null;
    const r = resolve(def);
    return r.value;
  };
  let env = null;
  if (ctx.engine === "va" && byName.has("va_amp_attack")) {
    env = { attackMs: val("va_amp_attack"), decayMs: val("va_amp_decay"), sustain: val("va_amp_sustain") === null ? null : val("va_amp_sustain") / 100, releaseMs: val("va_amp_release") };
  } else if (byName.has("part_attack")) {
    env = { attackMs: val("part_attack"), decayMs: 0, sustain: 1, releaseMs: val("part_release") };
  }
  secById.get("amp_eq").inert = secById.get("amp_eq").groups.some((g) => g.knobs.some((k) => k.inert));
  secById.get("amp_eq").envelope = env && [env.attackMs, env.releaseMs].every((x) => x !== null) ? { ...env, ...envelopePolyline(env) } : null;

  // -- effects: the insert chain and the sends --
  const defs = insertDefs(registry);
  const chain = ctx.chain || (known && pstate ? chainFromPartState(pstate, registry) : []);
  const typeSpec = defs.type ? knobSpec(defs.type, registry) : null;
  const bypassSpec = defs.bypass ? knobSpec(defs.bypass, registry) : null;
  const fx = secById.get("effects");
  fx.inserts = (known || ctx.chain ? chain : []).map((slot, i) => ({
    slot: i,
    type: { key: `ins:${i}:type`, kind: "insert", id: "insert_type", paramId: defs.type && defs.type.id, group: "fx.insert", ...typeSpec, value: slot.type, valueNow: slot.type, readOnly: !editable, reason: editable ? "" : roReason, pending: !!ctx.chain, command: "insert" },
    bypass: { key: `ins:${i}:bypass`, kind: "insert", id: "insert_bypass", paramId: defs.bypass && defs.bypass.id, group: "fx.insert", ...bypassSpec, value: slot.bypass ? 1 : 0, valueNow: slot.bypass ? 1 : 0, readOnly: !editable, reason: editable ? "" : roReason, pending: !!ctx.chain, command: "insert" },
    params: slot.type === 0 ? [] : defs.params.map((d, n) => ({ key: `ins:${i}:p${n}`, kind: "insert", id: d.name, paramId: d.id, localeKey: d.localeKey, group: "fx.insert", ...knobSpec(d, registry), value: slot.params[n], valueNow: slot.params[n], readOnly: !editable, reason: editable ? "" : roReason, pending: !!ctx.chain, command: "insert" })),
    canMoveUp: i > 0,
    canMoveDown: i < chain.length - 1,
  }));
  fx.canAdd = editable && chain.length < registry.insertSlots;
  fx.maxSlots = registry.insertSlots;
  fx.addReason = editable ? (chain.length < registry.insertSlots ? "" : "studio.part_editor.insert_full") : roReason;

  // -- ASSIGN · KNOBS (THIS LIVE SET) --
  const encoders = state && state.encoders;
  const map = encoders && Array.isArray(encoders.assign) ? encoders.assign : [];
  const options = assignableParams(registry, ctx.engine).map((d) => ({ paramId: d.id, name: d.name, localeKey: d.localeKey, group: d.group, scope: d.scope }));
  const assignPending = ctx.assign || new Map();
  zsec.assign = {
    target: ASSIGN_TARGET_CURRENT_LIVE_SET,
    editable,
    reason: editable ? "" : roReason,
    options,
    slots: Array.from({ length: ASSIGN_SLOTS }, (_, i) => {
      const p = assignPending.get(i + 1);
      return { slot: i + 1, key: `assign:${i + 1}`, paramId: p !== undefined ? p : known ? num(map[i], 0) : 0, pending: p !== undefined, readOnly: !editable, reason: editable ? "" : roReason };
    }),
  };

  return {
    partIndex: part,
    engine: ctx.engine,
    editable,
    stale: !!ctx.stale,
    known,
    reason: editable ? "" : roReason,
    sections,
    deferred,
    envelopeSources: env,
  };
}

/** The polyline of an ADSR in the 220 x 70 box of the mockup: attack up to the peak, decay down to the sustain level, a hold, release to the floor. Times are drawn on a log scale (a 5 ms and a 5 s stage both stay visible). */
export function envelopePolyline(env) {
  const W = 40;
  const norm = (ms) => Math.log(1 + Math.max(0, num(ms, 0))) / Math.log(1 + 5000);
  const sustain = Math.min(1, Math.max(0, num(env.sustain, 1)));
  const top = 8;
  const floor = 64;
  const sy = Math.round(floor - sustain * (floor - top));
  const x0 = 6;
  const x1 = x0 + 8 + Math.round(W * norm(env.attackMs));
  const x2 = x1 + (num(env.decayMs, 0) > 0 ? 8 + Math.round(W * norm(env.decayMs)) : 0);
  const x3 = x2 + 30;
  const x4 = x3 + 8 + Math.round(W * norm(env.releaseMs));
  const pts = [[x0, floor], [x1, top]];
  if (x2 > x1) pts.push([x2, sy]);
  pts.push([x3, sy], [x4, floor]);
  return { points: pts.map(([x, y]) => ({ x, y })), pointsAttr: pts.map(([x, y]) => `${x},${y}`).join(" ") };
}

// ---- requests (the command table in code) ---------------------------------------------------------------------------------------------------------------------

export const requests = {
  perf: (paramId, part, value) => ({ body: { SetPerformanceParam: { param_id: paramId, part, value } } }),
  sends: (part, reverb, delay) => ({ body: { SetSendLevels: { part, reverb, delay } } }),
  output: (part, output) => ({ body: { SetPartOutput: { part, output } } }),
  master: (paramId, value) => ({ body: { SetMasterParams: { values: [{ param_id: paramId, value }] } } }),
  chain: (part, slots) => ({ body: { SetInsertChain: { part, slots: slots.map((s) => ({ type: s.type, bypass: !!s.bypass, params: s.params.slice() })) } } }),
  volume: (part, volume) => ({ body: { SetPartVolume: { part, volume } } }),
  assign: (slot, paramId) => (paramId
    ? { body: { SetAssignSlot: { target: ASSIGN_TARGET_CURRENT_LIVE_SET, slot, param_id: paramId } } }
    : { body: { ClearAssignSlot: { target: ASSIGN_TARGET_CURRENT_LIVE_SET, slot } } }),
  zone: (part, fields) => ({ body: { SetPartZone: { part, ...fields } } }),
  arp: (part, fields) => ({ body: { SetPartArp: { part, ...fields } } }),
  getPerformanceValues: () => ({ body: { GetPerformanceValues: {} } }),
  getState: () => ({ body: { GetState: {} } }),
};

// ---- the coalescing sender ------------------------------------------------------------------------------------------------------------------------------------

/**
 * Sends edits with at most ONE request in flight per key and at least `throttleMs` between two requests of a key; while one is in flight or waiting, a newer submit for the
 * same key REPLACES the waiting one (latest value wins). So a drag at 120 pointer events a second sends about 30 requests a second for that knob, never a queue, and the last
 * value always goes out. `build()` is called when the request is about to be sent (it reads the latest local state), and returns the ClientMessage JSON or null to skip.
 * `done(key, reply, error, meta)` is called when the answer (or a transport failure) arrives.
 */
export class EditSender {
  constructor({ request, clock, setTimer, clearTimer, throttleMs = 30, structuralThrottleMs = 140, done, baseRevision } = {}) {
    this.request = request;
    this.clock = clock || (() => (typeof performance !== "undefined" ? performance.now() : Date.now()));
    this.setTimer = setTimer || ((f, ms) => setTimeout(f, ms));
    this.clearTimer = clearTimer || ((t) => clearTimeout(t));
    this.throttleMs = throttleMs;
    this.structuralThrottleMs = structuralThrottleMs; // keys starting with "s:" (volume, sends, output, insert chain, master, ASSIGN): each request publishes a generation (the 32-slot ledger) and a State push
    this.done = done || (() => {});
    this.baseRevision = baseRevision || (() => 0);
    this.entries = new Map();
    this.sent = 0;
    this.maxInFlight = 0; // the most requests of ONE key in flight at once (always 1): a measurement for the tests
    this.idleWaiters = [];
  }

  /**
   * Story 9.12: resolves true once nothing is in flight or waiting for any key (every edit made so far has been ANSWERED by the unit), false after `timeoutMs`. Write to Velora flushes
   * with this first, so a knob still being dragged (a coalesced edit waiting its turn) is on the unit before the unit is told to save.
   */
  whenIdle(timeoutMs = 5000) {
    if (!this.busy) return Promise.resolve(true);
    return new Promise((resolve) => {
      const w = { resolve, timer: null };
      w.timer = this.setTimer(() => {
        this.idleWaiters = this.idleWaiters.filter((x) => x !== w);
        resolve(false);
      }, timeoutMs);
      this.idleWaiters.push(w);
    });
  }

  _checkIdle() {
    if (this.idleWaiters.length === 0 || this.busy) return;
    const ws = this.idleWaiters;
    this.idleWaiters = [];
    for (const w of ws) {
      this.clearTimer(w.timer);
      w.resolve(true);
    }
  }

  submit(key, build, meta) {
    let e = this.entries.get(key);
    if (!e) {
      e = { inflight: false, latest: null, timer: null, lastSent: -Infinity, meta: null };
      this.entries.set(key, e);
    }
    // a request that has not gone out yet and is replaced keeps the keys (and sequence numbers) of the edits it stood for: two knobs that share one request (the sends) are both answered
    e.meta = e.latest && e.meta && meta ? { ...e.meta, ...meta, keys: [...new Set([...(e.meta.keys || []), ...(meta.keys || [])])], seqs: { ...(e.meta.seqs || {}), ...(meta.seqs || {}) } } : meta;
    e.latest = build;
    this._pump(key);
  }

  _pump(key) {
    const e = this.entries.get(key);
    if (!e || e.inflight || !e.latest || e.timer !== null) return;
    const wait = e.lastSent + (key.startsWith("s:") ? this.structuralThrottleMs : this.throttleMs) - this.clock();
    if (wait > 0) {
      e.timer = this.setTimer(() => {
        e.timer = null;
        this._pump(key);
      }, wait);
      return;
    }
    const build = e.latest;
    const meta = e.meta;
    e.latest = null;
    const msg = build();
    if (!msg) return;
    const base = this.baseRevision();
    if (base > 0) msg.base_revision = base;
    e.inflight = true;
    e.lastSent = this.clock();
    this.sent++;
    this.maxInFlight = Math.max(this.maxInFlight, 1);
    Promise.resolve()
      .then(() => this.request(msg))
      .then((reply) => ({ reply, error: null }), (error) => ({ reply: null, error }))
      .then(({ reply, error }) => {
        e.inflight = false;
        try {
          this.done(key, reply, error, meta, msg);
        } catch (err) {
          // a failing callback never stops the key's queue
        } finally {
          this._pump(key);
          this._checkIdle();
        }
      });
  }

  /** In flight or waiting for any key. */
  get busy() {
    for (const e of this.entries.values()) if (e.inflight || e.latest || e.timer !== null) return true;
    return false;
  }

  cancelAll() {
    for (const e of this.entries.values()) {
      if (e.timer !== null) this.clearTimer(e.timer);
      e.timer = null;
      e.latest = null;
    }
    this._checkIdle();
  }
}

// ---- the controller -------------------------------------------------------------------------------------------------------------------------------------------

function errorOf(reply, thrown) {
  if (thrown) return { code: 0, message: String(thrown && thrown.message ? thrown.message : thrown), details: {} };
  const body = reply && reply.body;
  if (body && body.Error) return { code: num(body.Error.code, 0), message: String(body.Error.message || ""), details: body.Error.details || {} };
  return null;
}

/**
 * Edits the selected part. DOM-free: `request` is the device request helper (ClientMessage JSON in, ServerMessage JSON out), `clock` / timers are injectable.
 *   select({ liveSetId, part, engine, available })  which part is edited (the Live Set view's selection); reads the live values
 *   onState(state)                                  every State push / reply: confirms structural edits, follows the scene
 *   model()                                         the editor model (see buildPartEditorModel), with the optimistic values laid over the confirmed ones
 *   edit(key, value)                                a knob changed: quantised, shown at once, sent coalesced; returns the value shown or null when the knob is read-only
 *   addInsert() / removeInsert(slot) / moveInsert(slot, dir) / assign(slot, paramId)
 * An Ack means the engine took the edit: structural edits are then confirmed by the next State, performance edits by the live value read back. A typed Error (or a failed
 * transport) puts the knob back to the confirmed value and calls `onError({ key, code, message, details })`.
 */
export class PartEditorController {
  constructor({ registry, request, clock, setTimer, clearTimer, throttleMs = 30, structuralThrottleMs = 140, settleMs = 800, perfRefreshMs = 80, rereadMs = [150, 500], onChange, onError, onStale, onReloaded } = {}) {
    this.registry = registry || REGISTRY;
    this.request = request;
    this.clock = clock || (() => (typeof performance !== "undefined" ? performance.now() : Date.now()));
    this.setTimer = setTimer || ((f, ms) => setTimeout(f, ms));
    this.clearTimer = clearTimer || ((t) => clearTimeout(t));
    this.settleMs = settleMs;
    this.perfRefreshMs = perfRefreshMs;
    this.rereadMs = rereadMs; // a read-back that differs from the acked value is read again after these delays before it is believed
    this.onChange = onChange || (() => {});
    this.onStale = onStale || (() => {});
    this.onReloaded = onReloaded || (() => {});
    this.onError = onError || (() => {});
    this.state = null;
    this.partIndex = -1;
    this.engine = "sample";
    this.liveSetId = "";
    this.available = true;
    this.perf = new Map();
    this.baseRev = 0; // the Live Set revision (State.edits.live_set_revision) the shown Live Set was loaded at; 0 = unknown / an older unit: no base_revision is sent
    this.baseFor = ""; // the Live Set id baseRev belongs to
    this.stale = null; // { current } after the unit refused an edit as STALE_REVISION: the knobs are read-only and the banner offers Reload
    this._reloadToken = 0;
    this.pending = new Map(); // key -> { value, seq, acked, ackedAt, ackRev, kind }
    this.chain = null; // { slots, seq, acked, ackedAt, ackRev } the optimistic chain of the selected part
    this.assignPending = new Map(); // slot -> { value, seq, acked, ackRev }
    this._seq = 0;
    this._token = 0;
    this._perfTimer = null;
    this.sender = new EditSender({
      request: (m) => this.request(m),
      clock: this.clock,
      setTimer: this.setTimer,
      clearTimer: this.clearTimer,
      throttleMs,
      structuralThrottleMs,
      baseRevision: () => this.baseRev, // Story 9.10: the Live Set revision this view was LOADED at (not the latest State: that would hide a change made on the unit)
      done: (key, reply, error, meta, msg) => this._done(key, reply, error, meta, msg),
    });
    this._perfRunner = createThrottledRunner(() => this._refreshPerfTask(), { minMs: 60, clock: this.clock, setTimer: this.setTimer, clearTimer: this.clearTimer });
  }

  // -- context --
  select({ liveSetId = "", part = -1, engine = "sample", available = true } = {}) {
    const moved = part !== this.partIndex || liveSetId !== this.liveSetId;
    const same = !moved && engine === this.engine && (available !== false) === this.available;
    this.liveSetId = liveSetId;
    this.partIndex = part;
    this.engine = engine;
    this.available = available !== false;
    if (moved) {
      this.sender.cancelAll(); // requests still waiting for the previous part never go out (their builders also refuse: see _guard)
      this._dropPending();
    }
    if (liveSetId !== this.baseFor) {
      // another Live Set is opened: it is loaded afresh, so the revision of the State we hold (when it is about this Live Set) is its base
      this.baseFor = "";
      this.baseRev = 0;
      this.stale = null;
      this._adoptBase(this.state);
    }
    if (!same) this.refreshPerf(); // the Live Set view calls this for every change of the part's strip: only a different part / engine reads the live values again
    this._notify();
  }

  /** Takes the Live Set revision of `st` as the base of this view when `st` is about the shown, sounding Live Set and no base is held for it yet. */
  _adoptBase(st) {
    if (!st || !this.liveSetId || this.baseFor === this.liveSetId || !st.live_set_loaded || st.live_set_id !== this.liveSetId) return;
    this.baseFor = this.liveSetId;
    this.baseRev = liveRevision(st);
  }

  reset() {
    this._token++;
    this._reloadToken++;
    this.baseRev = 0;
    this.baseFor = "";
    this.stale = null;
    this.state = null;
    this.partIndex = -1;
    this.liveSetId = "";
    this.perf.clear();
    this._dropPending();
    this.sender.cancelAll();
    this._perfRunner.cancel();
    if (this._perfTimer !== null) this.clearTimer(this._perfTimer);
    this._perfTimer = null;
    this._notify();
  }

  /** A request builder that gives up (null) when the selection moved since the edit was made: an edit never lands on another part or Live Set. */
  _guard(build) {
    const part = this.partIndex;
    const ls = this.liveSetId;
    return () => (this.partIndex !== part || this.liveSetId !== ls ? null : build(part));
  }

  _dropPending() {
    this.pending.clear();
    this.assignPending.clear();
    this.chain = null;
    this._token++;
  }

  /** The shown Live Set sounds on the unit (so State and live values are about it). */
  get live() {
    const st = this.state;
    return !!st && !!st.live_set_loaded && !!this.liveSetId && st.live_set_id === this.liveSetId;
  }

  get editable() {
    return !this.stale && this.live && this.available && this.partIndex >= 0 && this.partIndex < num(this.state.parts && this.state.parts.length, 0);
  }

  onState(st) {
    if (!st || typeof st !== "object") return;
    const prev = this.state;
    this.state = st;
    this._adoptBase(st);
    const rev = num(st.revision, 0);
    // a structural edit the engine acked is confirmed once a State at or after its revision arrives
    for (const [k, p] of this.pending) if (p.kind === "struct" && p.acked && rev >= p.ackRev) this.pending.delete(k);
    if (this.chain && this.chain.acked && rev >= this.chain.ackRev) this.chain = null;
    for (const [s, p] of this.assignPending) if (p.acked && rev >= p.ackRev) this.assignPending.delete(s);
    if (!prev || num(prev.revision, 0) !== rev || prev.current_scene !== st.current_scene || prev.live_set_id !== st.live_set_id) this.refreshPerf();
    this._notify();
  }

  // -- live values --
  refreshPerf() {
    if (this.partIndex < 0 || !this.request) return;
    this._perfRunner();
  }

  async _refreshPerfTask() {
    const token = this._token;
    const t0 = this.clock();
    let reply;
    try {
      reply = await this.request(requests.getPerformanceValues());
    } catch (e) {
      return; // the next State or edit reads again; the knobs keep what they show
    }
    const vals = reply && reply.body && reply.body.PerformanceValues && reply.body.PerformanceValues.values;
    if (!Array.isArray(vals)) return;
    const next = new Map();
    for (const v of vals) if (v && typeof v.value === "number") next.set(`${v.param_id}:${num(v.part, 0)}`, roundF32(v.value));
    this.perf = next;
    if (token === this._token) {
      for (const [k, p] of this.pending) {
        if (p.kind !== "perf" || !p.acked || p.ackedAt > t0) continue;
        const id = Number(k.slice(2));
        const got = next.get(`${id}:${this.partIndex}`);
        if (got === undefined || got === roundF32(p.value)) {
          this.pending.delete(k); // the unit holds what was sent
        } else if (p.retries < this.rereadMs.length) {
          // Ack means QUEUED for the RT plane, not applied: read again before believing a value that differs
          this.setTimer(() => this._perfRunner(), this.rereadMs[p.retries++]);
        } else {
          this.pending.delete(k);
          this.onError({ key: k, code: 0, message: "", notice: "not_applied", details: {} }); // the knob shows what the unit holds, and says so
        }
      }
    }
    this._notify();
  }

  // -- model --
  model() {
    const pendingValues = new Map();
    for (const [k, p] of this.pending) pendingValues.set(k, { value: p.value });
    const assign = new Map();
    for (const [s, p] of this.assignPending) assign.set(s, p.value);
    return buildPartEditorModel({
      registry: this.registry,
      partIndex: this.partIndex,
      engine: this.engine,
      state: this.state,
      perf: this.perf,
      pending: pendingValues,
      chain: this.chain ? this.chain.slots : null,
      assign,
      editable: this.editable,
      stale: !!this.stale,
      available: this.available,
      live: this.live,
    });
  }

  _notify() {
    try {
      this.onChange();
    } catch (e) {
      this.onError({ key: "", code: 0, message: String(e && e.message ? e.message : e), details: {}, render: true });
    }
  }

  _find(key) {
    const m = this.model();
    for (const s of m.sections) {
      for (const g of s.groups) for (const k of g.knobs) if (k.key === key) return k;
      for (const i of s.inserts || []) for (const k of [i.type, i.bypass, ...i.params]) if (k.key === key) return k;
    }
    return null;
  }

  // -- edits --
  edit(key, value) {
    if (!this.editable) return null;
    const knob = this._find(key);
    if (!knob || knob.readOnly) return null;
    const v = quantise(knob, value);
    if (knob.kind === "insert") return this._editInsert(key, v);
    const seq = ++this._seq;
    const meta = { keys: [key], seqs: { [key]: seq } };
    const live = knob.command === "perf" || knob.command === "sends"; // confirmed by reading the live value back
    this.pending.set(key, { value: v, seq, acked: false, ackedAt: 0, ackRev: 0, kind: live ? "perf" : "struct", retries: 0 });
    const reg = this.registry;
    switch (knob.command) {
      case "volume": this.sender.submit("s:vol", this._guard((part) => requests.volume(part, this._val("vol"))), meta); break;
      case "perf": this.sender.submit(key, this._guard((part) => requests.perf(knob.paramId, part, this._val(key))), meta); break;
      case "output": this.sender.submit(`s:out:${knob.paramId}`, this._guard((part) => requests.output(part, this._val(key))), meta); break;
      case "master": this.sender.submit(`s:master:${knob.paramId}`, this._guard(() => requests.master(knob.paramId, this._val(key))), meta); break;
      case "sends": {
        // BOTH: the live value (sounds at once, the value shown) and the structural initial level (persists, marks the Live Set dirty)
        const rev = reg.params.find((d) => d.name === SEND_PARAMS.reverb);
        const del = reg.params.find((d) => d.name === SEND_PARAMS.delay);
        this.sender.submit(key, this._guard((part) => requests.perf(knob.paramId, part, this._val(key))), meta);
        this.sender.submit(`s:sends:${this.partIndex}`, this._guard((part) => requests.sends(part, this._val(`p:${rev.id}`), this._val(`p:${del.id}`))), { ...meta, aux: true });
        break;
      }
      case "zone": {
        const field = knob.id === "keyboard" ? "keyboard_on" : knob.id;
        const val = knob.id === "keyboard" ? this._val(key) >= 0.5 : this._val(key);
        this.sender.submit(`s:zone:${knob.id}`, this._guard((part) => requests.zone(part, { [field]: val })), meta);
        break;
      }
      case "arp": {
        const val = (knob.id === "arp_on" || knob.id === "arp_latch") ? this._val(key) >= 0.5 : this._val(key);
        this.sender.submit(`s:arp:${knob.id}`, this._guard((part) => requests.arp(part, { [knob.id]: val })), meta);
        break;
      }
      default:
        this.pending.delete(key);
        return null;
    }
    this._notify();
    return v;
  }

  /** The value the knob shows now (the optimistic one when there is one, else the confirmed one). */
  _val(key) {
    const p = this.pending.get(key);
    if (p) return p.value;
    const k = this._find(key);
    return k && k.value !== null ? k.value : 0;
  }

  _slots() {
    if (this.chain) return cloneChain(this.chain.slots);
    const ps = this.state && Array.isArray(this.state.parts) ? this.state.parts.find((p) => p && p.part === this.partIndex) : null;
    return chainFromPartState(ps, this.registry);
  }

  _submitChain(slots) {
    const seq = ++this._seq;
    this.chain = { slots, seq, acked: false, ackRev: 0, ackedAt: 0 };
    this.sender.submit(`s:chain:${this.partIndex}`, this._guard((part) => requests.chain(part, this.chain ? this.chain.slots : slots)), { keys: ["chain"], seqs: { chain: seq } });
    this._notify();
  }

  _editInsert(key, v) {
    const m = /^ins:(\d+):(type|bypass|p(\d+))$/.exec(key);
    if (!m) return null;
    const slots = this._slots();
    const s = slots[Number(m[1])];
    if (!s) return null;
    if (m[2] === "type") {
      s.type = v;
      if (v === 0) s.bypass = false;
    } else if (m[2] === "bypass") s.bypass = v >= 0.5;
    else s.params[Number(m[3])] = v;
    this._submitChain(slots);
    return v;
  }

  addInsert() {
    if (!this.editable) return false;
    const slots = this._slots();
    if (slots.length >= this.registry.insertSlots) return false;
    const defs = insertDefs(this.registry);
    slots.push({ type: 1, bypass: false, params: defs.params.map((d) => d.default) }); // the first real effect type; the player picks another with the type knob
    this._submitChain(slots);
    return true;
  }

  removeInsert(slot) {
    if (!this.editable) return false;
    const slots = this._slots();
    if (slot < 0 || slot >= slots.length) return false;
    slots.splice(slot, 1);
    this._submitChain(slots);
    return true;
  }

  moveInsert(slot, dir) {
    if (!this.editable) return false;
    const slots = this._slots();
    const to = slot + (dir < 0 ? -1 : 1);
    if (slot < 0 || slot >= slots.length || to < 0 || to >= slots.length) return false;
    [slots[slot], slots[to]] = [slots[to], slots[slot]];
    this._submitChain(slots);
    return true;
  }

  assign(slot, paramId) {
    if (!this.editable || !Number.isInteger(slot) || slot < 1 || slot > ASSIGN_SLOTS) return false;
    const id = num(paramId, 0);
    if (id !== 0 && !assignableParams(this.registry, this.engine).some((d) => d.id === id)) return false;
    const seq = ++this._seq;
    this.assignPending.set(slot, { value: id, seq, acked: false, ackRev: 0 });
    this.sender.submit(`s:assign:${slot}`, this._guard(() => requests.assign(slot, this.assignPending.has(slot) ? this.assignPending.get(slot).value : id)), { keys: [`assign:${slot}`], seqs: { [`assign:${slot}`]: seq }, slot });
    this._notify();
    return true;
  }

  /**
   * Story 9.11: assigns a timbre to an EMPTY part from the picker: one `SetPartTimbre{part, library_id, instrument_key}` (never an engine type: the part keeps its engine) with the same
   * `base_revision` as every other edit (the revision this Live Set was loaded at; the rebuild the unit does for it does not move it, Studio's own edits never do). Not coalesced: a pick is one
   * discrete edit, sent at once (the sender's queues are for drags). `liveSetId` is the Live Set the picker was opened on: when the view has since moved to another one (or the link dropped, or a
   * Reload changed the base) nothing is sent / the answer is not believed. Resolves `{ ok: true, revision }` (the unit took it: the part is rebuilt in the background, State follows),
   * `{ ok: false, stale: true }` (STALE_REVISION: the banner is raised exactly as for a knob, nothing was assigned) or `{ ok: false, error }` (any other typed refusal or a transport failure:
   * nothing was assigned, the part is as it was).
   */
  async assignTimbre({ part, libraryId, instrumentKey, liveSetId } = {}) {
    const fail = (message, code = 0) => ({ ok: false, error: { code, message, details: {} } });
    if (!Number.isInteger(part) || part < 0 || !libraryId || !instrumentKey) return fail("invalid timbre");
    if (!this.live || this.liveSetId !== liveSetId) return fail("the Live Set is not the one that sounds on the Velora");
    if (this.stale) return { ok: false, stale: true }; // the banner is already up: Reload first
    const base = this.baseRev;
    const token = this._reloadToken;
    const msg = { body: { SetPartTimbre: { part, library_id: libraryId, instrument_key: instrumentKey } } };
    if (base > 0) msg.base_revision = base;
    let reply;
    try {
      reply = await this.request(msg);
    } catch (e) {
      return fail(String(e && e.message ? e.message : e));
    }
    const err = errorOf(reply, null);
    if (isStale(err)) {
      // an answer to a request sent on a base the view has since left (Reload, another Live Set, a reconnect) says nothing about the view as it is now
      if (token === this._reloadToken && base === this.baseRev) {
        this._markStale(err);
        return { ok: false, stale: true };
      }
      return fail("the Live Set changed meanwhile");
    }
    if (err) return { ok: false, error: err };
    if (!reply || !reply.body || !reply.body.Ack) return fail("the Velora gave no answer");
    this._notify();
    return { ok: true, revision: num(reply.revision, 0) };
  }

  /**
   * Story 9.12: resolves true once every edit already made has been answered by the unit (see EditSender.whenIdle). Write to Velora calls it before it sends the Write, so a coalesced
   * edit still waiting for its turn is on the unit first; false when the unit did not answer in `timeoutMs` (the Write must not be sent then).
   */
  flush(timeoutMs = 5000) {
    return this.sender.whenIdle(timeoutMs);
  }

  /**
   * Story 9.12: what a Write to Velora needs to judge the view it was made from: the Live Set revision this view was loaded at (`base`, 0 = unknown / an older unit), whether it is
   * known for `liveSetId`, and the reload token (so an answer to a Write sent on a base the view has since left is not believed).
   */
  writeContext(liveSetId) {
    return { base: this.baseFor === liveSetId ? this.baseRev : 0, known: this.baseFor === liveSetId, stale: !!this.stale, token: this._reloadToken };
  }

  /**
   * Story 9.12: the unit refused a WRITE as STALE_REVISION: the same banner and read-only knobs as a refused edit. Ignored when the view has moved on since the Write was sent
   * (`ctx` is what writeContext() returned then). Returns whether the banner was raised.
   */
  noteStale(err, ctx) {
    if (!ctx || ctx.token !== this._reloadToken || ctx.base !== this.baseRev) return false;
    this._markStale(err);
    return true;
  }

  // -- answers --
  _done(sendKey, reply, thrown, meta, msg) {
    const err = errorOf(reply, thrown);
    if (isStale(err)) {
      // an answer to a request sent on a base the view has since left (Reload, another Live Set, a reconnect) says nothing about the view as it is now
      if (!msg || num(msg.base_revision, 0) === this.baseRev) this._markStale(err);
      return;
    }
    const rev = num(reply && reply.revision, 0);
    const now = this.clock();
    let perfAcked = false;
    for (const key of meta.keys) {
      const seq = meta.seqs ? meta.seqs[key] : meta.seq;
      if (key === "chain") {
        const c = this.chain;
        if (!c || c.seq !== seq) continue; // a newer edit is on its way
        if (err) {
          this.chain = null;
          this._report(key, err);
        } else {
          c.acked = true;
          c.ackedAt = now;
          c.ackRev = rev || num(this.state && this.state.revision, 0) + 1;
          if (this._confirmed(c.ackRev)) this.chain = null; // the State that shows it arrived before the Ack
          else this._settle(() => { if (this.chain === c) { this.chain = null; if (JSON.stringify(this._slots()) !== JSON.stringify(c.slots)) this._unconfirmed(key); } });
        }
      } else if (key.startsWith("assign:")) {
        const slot = meta.slot;
        const p = this.assignPending.get(slot);
        if (!p || p.seq !== seq) continue;
        if (err) {
          this.assignPending.delete(slot);
          this._report(key, err);
        } else {
          p.acked = true;
          p.ackRev = rev || num(this.state && this.state.revision, 0) + 1;
          if (this._confirmed(p.ackRev)) this.assignPending.delete(slot);
          else this._settle(() => {
            if (this.assignPending.get(slot) !== p) return;
            this.assignPending.delete(slot);
            const map = this.state && this.state.encoders && this.state.encoders.assign;
            if (!map || num(map[slot - 1], 0) !== p.value) this._unconfirmed(key);
          });
        }
      } else {
        const p = this.pending.get(key);
        if (!p || p.seq !== seq) continue;
        if (err) {
          this.pending.delete(key);
          this._report(key, err);
          this.refreshPerf(); // show what the unit really holds, not a value from before
        } else if (meta.aux) {
          // the structural half of an edit whose live half decides the display: nothing to confirm here
        } else if (p.kind === "perf") {
          p.acked = true;
          p.ackedAt = now;
          perfAcked = true;
        } else {
          p.acked = true;
          p.ackedAt = now;
          p.ackRev = rev || num(this.state && this.state.revision, 0) + 1;
          if (this._confirmed(p.ackRev)) this.pending.delete(key);
          else this._settle(() => {
            if (this.pending.get(key) !== p) return;
            this.pending.delete(key);
            const k = this._find(key);
            if (!k || k.value === null || roundF32(k.value) !== roundF32(p.value)) this._unconfirmed(key);
          });
        }
      }
    }
    if (perfAcked) this._scheduleRead();
    this._notify();
  }

  /**
   * The unit refused an edit as STALE_REVISION: the Live Set was changed on the unit after this view was loaded. Everything optimistic is dropped (the knobs show what the unit
   * holds), edits still waiting never go out, the knobs are read-only and the banner "Changed on the Velora since you opened it · Reload" is shown.
   */
  _markStale(err) {
    const wasStale = !!this.stale;
    const cur = Number(err.details && err.details.current_revision); // details are strings on the wire
    this.stale = { current: Number.isFinite(cur) ? cur : 0 };
    this.sender.cancelAll();
    this._dropPending();
    this.refreshPerf();
    if (!wasStale) this.onStale();
    this._notify();
  }

  /**
   * Reload (the banner's button): applies the current snapshot: reads the State (the Live Set, its revision and parts) and the live values again, takes the revision it carries as the
   * new base and makes the knobs editable again. `onReloaded` lets the page re-read the Live Set view too. Resolves true when the unit answered.
   */
  async reload() {
    const token = ++this._reloadToken;
    let reply;
    try {
      reply = await this.request(requests.getState());
    } catch (e) {
      this.onError({ key: "", code: 0, message: String(e && e.message ? e.message : e), details: {} });
      return false;
    }
    if (token !== this._reloadToken) return false; // another reload, or the link went down meanwhile
    const st = reply && reply.body && reply.body.State;
    if (!st) {
      const err = errorOf(reply, null);
      this.onError({ key: "", code: err ? err.code : 0, message: err ? err.message : "", details: err ? err.details : {} });
      return false;
    }
    this.sender.cancelAll();
    this._dropPending();
    this.state = st;
    this.baseFor = "";
    this.baseRev = 0;
    this.stale = null;
    this._adoptBase(st);
    this.refreshPerf();
    this.onReloaded(st);
    this._notify();
    return true;
  }

  _confirmed(rev) {
    return !!this.state && num(this.state.revision, 0) >= rev;
  }

  /** After a structural Ack with no State to confirm it within `settleMs`, the knob shows whatever the State says (the truth) instead of keeping the optimistic value. */
  _settle(fn) {
    this.setTimer(() => {
      fn();
      this._notify();
    }, this.settleMs);
  }

  _unconfirmed(key) {
    this.onError({ key, code: 0, message: "", notice: "unconfirmed", details: {} });
  }

  _scheduleRead() {
    if (this._perfTimer !== null) this.clearTimer(this._perfTimer);
    this._perfTimer = this.setTimer(() => {
      this._perfTimer = null;
      this._perfRunner();
    }, this.perfRefreshMs);
  }

  _report(key, err) {
    this.onError({ key, code: err.code, message: err.message, details: err.details });
  }
}

/** float32 values come back from the unit as doubles with float noise (0.6 -> 0.6000000238418579): round to what a float32 means. */
export function roundF32(v) {
  return Number(Math.fround(v).toPrecision(7));
}

export { REGISTRY };
