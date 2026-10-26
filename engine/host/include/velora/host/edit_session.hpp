#pragma once
// Edits in memory and Write scopes (Story 4.9, FR89, FR90, FR91, AR12, NFR15). The control thread's edit session keeps, per Live Set, the in-memory edited
// state next to the saved one, publishes which Live Sets are dirty and what changed, and is the only code that turns edits into files, and only on an
// explicit Write command.
//
// DATA MODEL. A document per Live Set id: `saved` (the Live Set message as loaded or last written) and `working` (saved + every edit). Edits are applied
// to `working`; what is dirty, and the change summary, is the semantic difference between the two (diff_live_sets()), so an edit moved back to its saved
// value is no longer an edit. Nothing is ever written without Write: no autosave, edits are lost at power-off. A document exists for the Live Set that
// sounds and for every Live Set that still has unsaved edits (a clean document of a Live Set that was left is dropped).
//
// WHERE AN EDIT LANDS (mirrors the Write scopes). An edit made while scene S is current lands in scene S when that scene STORES the field (a presence flag
// set / the parameter id listed in its sparse overrides, see live_set.proto), else in the base Live Set, which every scene that does not override the field
// inherits. With no scene current it lands in the base. Examples, Live Set "Perfect" with scene 2 overriding part 1's volume:
//   * scene 2 current, part 1 fader moved   -> scene 2's volume of part 1 (scene-stored)       -> Write Scene writes it
//   * scene 2 current, drawbar 3 of part 3  -> the base (scene 2 does not store drawbar 3)      -> Write Scene does NOT write it, Whole does
//   * no scene current, reverb send moved   -> the base                                          -> Whole only
// Edits are therefore kept across scene changes (the scene recall works on the edited source: SceneController::set_source) and across Live Set changes
// (the document stays; a jump back builds the generation from the edited state, LiveSetSwitcher overlay; a Live Set that was left is captured first).
//
// WHAT IS TRACKED. Structural commands that were published (SetInsertChain, SetSendLevels, SetPartOutput, SetMasterParams, SetPartVolume, ControlPlane
// calls on_structural()), SetPartTimbre, and the performance values the player moves (drawbars, knobs, sends, tempo...) which are read from the RT
// last-value table by sync_live_values() on every control tick, before a scene recall, before a Live Set switch publishes and after the control->RT fence
// of a Write. The table is only compared once RT has applied the newest generation's initial values (LastValueTable::load_adopted_revision), so a recall or
// switch that RT has not applied yet is never mistaken for an edit. SetTimbreRef (the opaque dev-configuration id) is not part of the Live Set and is not
// tracked. ASSIGN edits (Story 4.10: SetAssignSlot / ClearAssignSlot) land in the working copy and in a new generation (do_assign); the global default is a settings edit written at once.
//
// TIMBRE EDITS. SetPartTimbre changes the part's (library_id, instrument_key) and optionally its engine type in the working copy and rebuilds the Live
// Set from it by the seamless path of a Live Set switch (resolution, residency, reservation and head budget are the loader's; the old generation keeps
// sounding; State.loading shows it). When the new generation is published the current scene is recalled again and the live values the player moved are
// put back (`restoring`). While the rebuild runs structural commands are refused (BUSY, reason reload_in_progress) and so is Write. Timbre changes are
// persisted by Whole Live Set and Save as new only; Scene is refused while one is pending.
//
// WRITE (WriteLiveSet). Writes the CURRENT Live Set only. Order: refuse what needs no live values (no Live Set, a load in progress, bad scope or name),
// raise the control->RT fence and wait for RT's acknowledgement (bounded; ControlPlane::raise_fence), read the last-value table, then write per scope
// through LiveSetStore::write (atomic: temp file, fsync, rename; a failure leaves the old file intact and the edits in memory, still dirty):
//   Scene        the current scene's stored overrides (what landed in the scene); refused with WRITE_SCOPE_UNAVAILABLE (reason timbre_change, no_scene,
//                no_scenes). Base edits stay dirty. Clears only the scene's edits.
//   Whole        the whole working copy (base, every scene, master, ASSIGN). Clears everything.
//   Save as new  a copy of the working copy with a new UUIDv4 and a name, written as a new Live Set, added to the catalog and, with a setlist loaded, to that
//                setlist right after the current Live Set (its index there, else the setlist position, +1; appended when there is no position). The current
//                Live Set is untouched and keeps its edits (still dirty); the new Live Set starts clean. Names: trimmed; a name already used in the loaded
//                setlist (ASCII case-insensitive) is NAME_IN_USE with a suggestion; without a setlist there is no uniqueness check.
// Nothing dirty for the scope (Scene, Whole): no file is touched, the reply says nothing_to_write. All control-thread-only.
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "live_set.pb.h"
#include "velora/host/control_plane.hpp"
#include "velora/host/edit_port.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/live_set_store.hpp"
#include "velora/host/live_set_switcher.hpp"
#include "velora/host/scene_controller.hpp"
#include "velora/host/setlist_controller.hpp"
#include "velora/host/setlist_store.hpp"

namespace velora::host {

// One difference between a saved Live Set and its edited state. `key` is stable (see ws_v1.proto ChangeItem); part / param_id / scene are -1 when they
// do not apply (0-based otherwise); `text` is the English rendering ("Part 2 volume": part and scene numbers 1-based).
struct ChangeItem {
  std::string key;
  int part = -1;
  int param_id = -1;
  int scene = -1;
  std::string text;
  int slot = -1;  // assign.slot: the 0-based ASSIGN slot
  friend bool operator==(const ChangeItem&, const ChangeItem&) = default;
};

// The semantic difference between `saved` and `edited` (values compare with a tolerance of 1e-6 of the parameter's range, so a value moved back to where
// it was is not a change). Order: tempo, parts, master, ASSIGN, scenes.
std::vector<ChangeItem> diff_live_sets(const ::velora::liveset::v1::LiveSet& saved, const ::velora::liveset::v1::LiveSet& edited);

// Name rules of Save as new. `fold_name` trims ASCII whitespace and folds ASCII letters (the comparison key); non-ASCII bytes compare as bytes.
std::string trim_name(const std::string& s);
std::string fold_name(const std::string& s);
// A free name close to `wanted`: an existing trailing " N" is stripped ("Perfect 2" -> "Perfect"), then "<stem> 2", "<stem> 3", ... the first whose folded
// form is not in `taken_folded`. At most 64 bytes (the stem is cut on a character boundary when needed). O(taken) lookups.
std::string suggest_free_name(const std::string& wanted, const std::unordered_set<std::string>& taken_folded);

struct EditSessionOptions {
  std::uint32_t part_count = liveset::kV1PartCount;
  std::function<double()> clock;       // seconds, monotonic: the fence timeout (default steady_clock)
  double fence_timeout_s = 2.0;        // how long a Write waits for RT's acknowledgement before it is refused (BUSY, reason fence_timeout)
  std::function<void()> fence_idle;    // each pass of the fence wait (default: sleep 100 us); engine-host --no-audio renders a block here, tests pump the driver
  std::function<void()> pump;          // called once per tick while the session waits for RT to apply a rebuilt generation (engine-host --no-audio renders a block)
  std::function<bool()> load_in_progress;        // the switcher is loading a Live Set: Write and timbre edits are refused
  std::function<std::uint64_t()> active_job_token;  // LiveSetSwitcher::active_token(): ties a rebuild to its job (0 = none); unset = judged by load_in_progress
  double restore_timeout_s = 5.0;      // how long the session waits for RT to apply a rebuilt generation before it gives up restoring
  std::function<SwitchOutcome()> reload_current;  // rebuilds the Live Set that sounds from its edited state (LiveSetSwitcher::reload_current)
  liveset::RandomSource* rng = nullptr;  // ids of Save as new; null = the operating system's
};

class EditSession final : public EditHandler, public LiveSetEditPort {
 public:
  // Registers itself as `plane`'s edit handler and as the scene controller's before-recall hook; `scenes`, `live`, `plane` outlive it. `setlists` and
  // `setlist_ctl` (optional, both or none) give Save as new its setlist; `port` (optional) is the Live Set catalog (names, ids). If a Live Set already
  // sounds (loaded before the session existed) it becomes the current document.
  EditSession(ControlPlane& plane, SceneController& scenes, liveset::LiveSetStore& live, setlist::SetlistStore* setlists, SetlistController* setlist_ctl,
              SwitchPort* port, EditSessionOptions options = {});
  ~EditSession() override;
  EditSession(const EditSession&) = delete;
  EditSession& operator=(const EditSession&) = delete;

  // Wiring of the loader and the switcher (the session hears about published generations and supplies the edited state of a Live Set they rebuild).
  // The session detaches itself from both in its destructor.
  void attach(LiveSetLoader& loader) {
    loader_ = &loader;
    loader.set_edit_port(this);
  }
  void attach(LiveSetSwitcher& switcher) {
    switcher_ = &switcher;
    switcher.set_edit_port(this);
  }
  // A Live Set was deleted (DeleteLiveSet): its unsaved document is dropped. The only way a dirty document is ever discarded besides a Write.
  void forget(const std::string& id);

  // The control tick (after the switcher's): captures live values, finishes a timbre rebuild (recalls the scene, restores live values), drops documents of
  // Live Sets that no longer exist. True when the published edit status changed (the server then pushes a State).
  bool tick();
  // Reads the RT last-value table into the current document (no-op unless RT applied the newest generation). True when an edit was recorded.
  bool sync_live_values();

  // 4.10 hook: runs `f` on the current document's working copy (it may change anything the Live Set format holds, e.g. the ASSIGN map). The edit is
  // tracked like any other. False when no Live Set sounds.
  bool apply_external_edit(const std::function<void(::velora::liveset::v1::LiveSet&)>& f);

  // ---- observation (tests, State) ----------------------------------------------------------------------------------------------------
  const std::string& current_id() const { return current_id_; }
  bool is_dirty(const std::string& id) const;
  std::vector<std::string> dirty_ids() const;
  std::vector<ChangeItem> summary(const std::string& id) const;
  const ::velora::liveset::v1::LiveSet* working_of(const std::string& id) const;
  const ::velora::liveset::v1::LiveSet* saved_of(const std::string& id) const;
  std::uint64_t edit_seq() const { return edit_seq_; }
  std::uint32_t writes_done() const { return writes_done_; }
  bool restoring() const { return restoring_; }
  bool reload_active() const { return reload_active_; }

  // EditHandler (ControlPlane seam).
  bool run(const ws::v1::ClientMessage& msg, ws::v1::ServerMessage& reply, HandlerFailure& err, bool& bump_revision, bool& push_state) override;
  void on_structural(const ws::v1::ClientMessage& msg) override;
  bool structural_blocked(std::string& reason) const override;
  void refresh() override { sync_live_values(); }
  void fill_status(ws::v1::State* out) const override;

  // LiveSetEditPort (loader / switcher seam).
  const ::velora::liveset::v1::LiveSet* overlay_for(const std::string& id) const override;
  std::uint64_t overlay_revision(const std::string& id) const override;
  void before_switch() override;
  void on_published(const ::velora::liveset::v1::LiveSet& built_from, const core::Snapshot& published) override;

 private:
  struct Doc {
    ::velora::liveset::v1::LiveSet saved, working;
    std::uint64_t version = 1;
    mutable std::uint64_t diff_version = 0;
    mutable std::vector<ChangeItem> items;
    mutable bool has_timbre = false;
  };

  Doc* current_doc();
  const Doc* find_doc(const std::string& id) const;
  const std::vector<ChangeItem>& items_of(const Doc& d) const;
  int current_scene() const;
  void touch(Doc& d);  // the working copy changed: version, edit_seq, and (for the current document) the scenes' source
  void adopt_current_from_scenes();
  void restore();
  double now() const;

  bool do_set_timbre(const ws::v1::SetPartTimbre& m, HandlerFailure& err, bool& bump);
  bool do_assign(ws::v1::AssignTarget target, std::uint32_t slot, std::uint32_t param_id, bool clear, HandlerFailure& err, bool& bump);
  bool do_write(const ws::v1::WriteLiveSet& m, ws::v1::ServerMessage& reply, HandlerFailure& err, bool& bump);
  bool fence_and_wait(HandlerFailure& err);
  bool busy_reason(std::string& reason) const;
  std::unordered_set<std::string> taken_names(const std::string& setlist_id, bool all_when_none) const;

  ControlPlane& plane_;
  SceneController& scenes_;
  liveset::LiveSetStore& live_;
  setlist::SetlistStore* setlists_;
  SetlistController* setlist_ctl_;
  SwitchPort* port_;
  EditSessionOptions opt_;
  liveset::OsRandom os_rng_;

  std::map<std::string, Doc> docs_;
  std::string current_id_;
  std::uint64_t edit_seq_ = 0, reported_seq_ = 0;
  std::uint32_t writes_done_ = 0;
  LiveSetLoader* loader_ = nullptr;
  LiveSetSwitcher* switcher_ = nullptr;
  mutable std::set<std::string> overlay_given_;  // ids whose edited state was handed to the switcher for a build that has not been published yet
  std::uint64_t reload_token_ = 0;  // the job of the rebuild (SwitchOutcome::token)
  double restore_deadline_ = 0;
  int restore_scene_ = -2;          // -2: the scene has not been recalled yet; else the scene recalled (or -1)
  bool reload_active_ = false;   // a timbre edit asked for a rebuild that has not been published yet
  bool restoring_ = false;       // the rebuilt generation is published; the scene and the live values are being put back
  int pending_scene_ = -1;       // the scene to recall again after a rebuild
};

}  // namespace velora::host
