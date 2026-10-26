// Velora Studio: the DOM side of the Live Set view (Story 9.8): builds the two one-row grids once and updates single cells in place. Everything that comes from the
// device (names, registrations) is written with textContent / setAttribute only, never as markup. The `doc` is passed in (the page's document, or a shim in tests).
import { moveFocus } from "./live_set_view.js";

function cell(doc, extraClass) {
  const el = doc.createElement("div");
  el.setAttribute("role", "gridcell");
  el.tabIndex = -1;
  el.className = extraClass;
  return el;
}

function span(doc, cls) {
  const el = doc.createElement("span");
  if (cls) el.className = cls;
  return el;
}

// Roving tabindex + arrow keys (the wrap rule is moveFocus's). Enter / Space in the part grid selects the part (`onEnter`); the scene grid never acts.
function wireGrid(grid, cells, state, key, onEnter) {
  const setRoving = (idx) => {
    state[key] = idx;
    cells.forEach((c, i) => { c.tabIndex = i === idx ? 0 : -1; });
  };
  setRoving(0);
  const cellOf = (e) => (e.target && e.target.closest ? e.target.closest('[role="gridcell"]') : null);
  grid.addEventListener("focusin", (e) => {
    const c = cellOf(e);
    if (c) setRoving(Number(c.dataset.idx));
  });
  grid.addEventListener("keydown", (e) => {
    const c = cellOf(e);
    if (!c) return;
    const idx = Number(c.dataset.idx);
    if (onEnter && (e.key === "Enter" || e.key === " ")) {
      e.preventDefault();
      onEnter(idx);
      return;
    }
    const next = moveFocus(idx, e.key, cells.length);
    if (next === null) return;
    e.preventDefault();
    setRoving(next);
    cells[next].focus();
  });
}

/** Builds the 8 scene cells and the 8 part cells; returns `{ sceneCells, partCells, focus }` (focus = the roving indexes). */
export function buildGrids(doc, scenesHost, partsHost, { onSelectPart } = {}) {
  const sceneCells = [];
  const partCells = [];
  const focus = { scene: 0, part: 0 };
  const sceneRow = doc.createElement("div");
  sceneRow.setAttribute("role", "row");
  for (let i = 0; i < 8; i++) {
    const el = cell(doc, "scene-tile");
    el.dataset.idx = String(i);
    sceneRow.appendChild(el);
    sceneCells.push(el);
  }
  scenesHost.appendChild(sceneRow);
  const partRow = doc.createElement("div");
  partRow.setAttribute("role", "row");
  for (let i = 0; i < 8; i++) {
    const el = cell(doc, "st");
    el.dataset.idx = String(i);
    el.dataset.partIdx = String(i);
    const head = doc.createElement("div");
    head.className = "h";
    const pn = span(doc);
    const led = doc.createElement("i");
    led.className = "led";
    led.setAttribute("aria-hidden", "true");
    head.appendChild(pn);
    head.appendChild(led);
    const name = doc.createElement("div");
    name.className = "n";
    const cat = doc.createElement("div");
    cat.className = "c";
    const reg = doc.createElement("div");
    reg.className = "reg";
    const meter = doc.createElement("div");
    meter.className = "meter";
    meter.setAttribute("aria-hidden", "true");
    const fill = span(doc);
    meter.appendChild(fill);
    const row = doc.createElement("div");
    row.className = "row";
    const left = span(doc);
    const kr = span(doc);
    row.appendChild(left);
    row.appendChild(kr);
    [head, name, cat, reg, meter, row].forEach((c) => el.appendChild(c));
    el._r = { pn, name, cat, reg, meter, fill, row, left, kr };
    el.addEventListener("click", () => onSelectPart && onSelectPart(i));
    partRow.appendChild(el);
    partCells.push(el);
  }
  partsHost.appendChild(partRow);
  wireGrid(scenesHost, sceneCells, focus, "scene", null);
  wireGrid(partsHost, partCells, focus, "part", (i) => onSelectPart && onSelectPart(i));
  return { sceneCells, partCells, focus };
}

/** The grids' accessible names (called again when the language changes). */
export function setGridLabels(scenesHost, partsHost, tr) {
  scenesHost.setAttribute("aria-label", tr("studio.live_set.grid_scenes"));
  partsHost.setAttribute("aria-label", tr("studio.live_set.grid_parts"));
}

export function updateSceneCell(el, s, tr) {
  el.className = "scene-tile" + (s.current ? " cur" : "") + (s.empty ? " empty" : "");
  const text = s.empty ? tr("studio.live_set.scene_add", s.index + 1) : tr("studio.live_set.scene_label", s.index + 1, s.name);
  el.textContent = text;
  el.title = text;
  el.setAttribute("aria-label", s.empty ? `${s.index + 1}, ${tr("studio.live_set.scene_empty")}` : `${text}${s.current ? ", " + tr("studio.live_set.scene_current") : ""}`);
  if (s.current) el.setAttribute("aria-current", "true"); else el.removeAttribute("aria-current");
}

export function engineName(p, tr) {
  if (p.engine === "organ") return tr("studio.live_set.organ");
  if (p.engine === "fm") return tr("studio.live_set.engine_fm");
  if (p.engine === "va") return tr("studio.live_set.engine_va");
  return "";
}

/** The text a part strip shows as its name (also what is announced). */
export function partName(p, tr) {
  if (p.status === "missing") return tr("studio.live_set.missing_library");
  if (p.status === "unavailable") return tr("studio.live_set.engine_unavailable");
  if (p.status === "empty") return tr("studio.live_set.add_timbre");
  return p.timbreName || engineName(p, tr);
}

export function categoryLabel(p, tr) {
  return p.categoryKey ? tr(p.categoryKey).toUpperCase() : "";
}

/** Updates one part strip in place: classes, text (textContent only) and the accessible name (name, category, volume or OFF, zone, organ registration). */
export function updatePartCell(el, p, { tr, selected }) {
  const r = el._r;
  const warn = p.status === "missing" || p.status === "unavailable";
  const bare = warn || p.status === "empty";
  el.className = "st" + (selected ? " sel" : "") + (p.status === "off" ? " off" : "") + (p.status === "empty" ? " empty" : "") + (warn ? " missing" : "");
  el.setAttribute("aria-selected", selected ? "true" : "false");
  r.pn.textContent = tr("studio.live_set.part", p.index + 1);
  const name = partName(p, tr);
  if (warn) {
    r.name.className = "n miss";
    r.name.textContent = "⚠ " + name; // warn glyph + text: never colour alone, never another timbre's name
    r.name.removeAttribute("title");
    r.name.removeAttribute("aria-disabled");
  } else if (p.status === "empty") {
    r.name.className = "n add";
    r.name.textContent = name;
    r.name.title = tr("studio.live_set.add_timbre_soon"); // a disabled placeholder until Story 9.11
    r.name.setAttribute("aria-disabled", "true");
  } else {
    r.name.className = "n";
    r.name.textContent = name;
    r.name.title = name;
    r.name.removeAttribute("aria-disabled");
  }
  const cat = bare ? "" : categoryLabel(p, tr);
  r.cat.textContent = cat;
  r.cat.hidden = cat === "";
  r.reg.textContent = p.registration;
  r.reg.hidden = bare || p.registration === "";
  r.meter.hidden = bare;
  r.row.hidden = bare;
  r.fill.style.width = `${p.volumePct}%`;
  r.left.textContent = "";
  if (p.status === "off") {
    const tag = r.left.ownerDocument ? r.left.ownerDocument.createElement("span") : null;
    if (tag) {
      tag.className = "tag";
      tag.textContent = tr("studio.live_set.off");
      r.left.appendChild(tag);
    } else {
      r.left.textContent = tr("studio.live_set.off");
    }
  } else {
    r.left.textContent = tr("studio.live_set.vol", p.volumePct);
  }
  r.kr.textContent = p.keyRange;
  const bits = [tr("studio.live_set.part_aria", p.index + 1, name, bare ? "" : cat).replace(/,\s*$/, "")];
  if (!bare) {
    bits.push(p.status === "off" ? tr("studio.live_set.off") : tr("studio.live_set.vol", p.volumePct));
    if (p.keyRange) bits.push(p.keyRange);
    if (p.registration) bits.push(p.registration);
  }
  el.setAttribute("aria-label", bits.filter(Boolean).join(", "));
}
