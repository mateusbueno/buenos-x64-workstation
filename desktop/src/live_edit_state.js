// Story 9.10: what Velora Studio shows about live editing, as pure functions of the State the unit pushes (State.edits). DOM-free.
//
//   liveEditFooter(state)  { on, scope }   `on`: the Footer reads "● LIVE EDIT ON · CHANGES PLAY ON THE VELORA" (an edit is in memory on the unit, or Studio edits arrived a moment
//                                          ago); `scope`: which Write scope would save what is unsaved, as the device's rules say it:
//                                            { kind: "none" }                        nothing unsaved
//                                            { kind: "scene", scene: n }             "UNSAVED: SCENE n" (n is 1-based): the Write Scene scope is available
//                                            { kind: "live_set" }                    "UNSAVED: LIVE SET": a timbre change (Write Scene is refused for it), or no scene is current
//   unsavedScope(edits)    the scope alone.
//   isStale(err)           the typed error of an edit built on an older Live Set revision (ERROR_CODE_STALE_REVISION).
//   liveRevision(state)    State.edits.live_set_revision (0 when the unit is older than 9.10: no base_revision is then sent and nothing is ever stale).

export const ERROR_STALE_REVISION = 47;

const num = (v, d = 0) => (typeof v === "number" && Number.isFinite(v) ? v : d);

export function liveRevision(state) {
  return num(state && state.edits && state.edits.live_set_revision, 0);
}

export function unsavedScope(edits) {
  const e = edits || {};
  if (!e.current_dirty) return { kind: "none" };
  const scene = num(e.current_scene_for_write, -1);
  if (e.has_timbre_edit || !e.scene_write_available || scene < 0) return { kind: "live_set" };
  return { kind: "scene", scene: scene + 1 };
}

/** `state`: the State of the unit; only the Live Set that sounds has edits to report (the Footer is about it). */
export function liveEditFooter(state) {
  if (!state || !state.live_set_loaded) return { on: false, scope: { kind: "none" } };
  const edits = state.edits || {};
  const scope = unsavedScope({ ...edits, current_scene_for_write: state.current_scene });
  return { on: scope.kind !== "none" || !!edits.studio_edit_active, scope };
}

export function isStale(err) {
  return !!err && num(err.code, 0) === ERROR_STALE_REVISION;
}
