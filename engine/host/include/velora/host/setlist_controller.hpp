#pragma once
// The control thread's setlist logic (Story 4.6, FR82, FR84 partial, FR86, AR26 partial, AR30): the ws.v1 setlist commands on top of the
// SetlistStore (sole writer of setlists, tags and the default ASSIGN map), the LOADED setlist, the stepping position and LIVE SET stepping
// through the Story 4.5 switcher.
//
// Loaded setlist. `load_setlist(id)` makes a setlist the loaded one; the generation that plays keeps playing and NO Live Set is loaded: the
// first Live Set of the setlist loads when the player steps or chooses. Loading the setlist that is already loaded changes nothing.
// `unload_setlist()` goes back to "no setlist": stepping walks All Live Sets.
//
// Position. `last_setlist_position` is the index, in the loaded setlist, of the Live Set that was current when the player last was inside it
// (-1 = none yet). "Current" is the target of the load in progress, else the Live Set that sounds (SwitchPort::effective_id()), so a jump
// that has only been accepted already counts. The position follows every observed change of current Live Set (a step, a JumpToLiveSet from a
// client, a Live Set loaded at start): when the new current Live Set is IN the loaded setlist the position becomes its index; when it is
// outside (an off-script jump) the position is left alone, so stepping returns into the setlist relative to it. A Live Set appears once in a
// setlist, so its index is unambiguous.
//
// Stepping (`step(forward)`, ws.v1 NextLiveSet / PreviousLiveSet).
//   * A setlist is loaded: the target is position+1 (next) or position-1 (previous); with position -1 (nothing played yet) next is the first
//     Live Set and previous is refused. Past the last Live Set "next" does nothing: result kEndOfList (ws.v1 ERROR_CODE_END_OF_SETLIST) and the
//     state's `end_of_setlist` latches; symmetrically kStartOfList / `start_of_setlist` before the first. The flags clear when the current Live
//     Set changes or another setlist is loaded. An empty setlist answers kEmptySetlist.
//   * No setlist loaded: the target is the neighbour of the current Live Set in ALL Live Sets ordered by name (see compare_names): case-
//     insensitive for ASCII letters (non-ASCII bytes compare as bytes: no Unicode or locale folding), then by id for equal names. The same end /
//     start results and flags apply. When the current Live Set is not in the list, next is the first and previous the last.
//   * The step is a LiveSetSwitcher::jump_to() (seamless, background load); the typed switch outcome is returned. A target whose Live Set is
//     missing (not listed and no file) or whose file cannot be read or is invalid is refused with kLoadFailed + reason ("missing",
//     "invalid_live_set", "read_failed") and the position does NOT move; nothing is skipped silently (the player can choose a later Live Set).
//     Failures that only show while the heads load (an instrument that cannot be made resident) are reported by the switcher in
//     State.loading, as for any jump; the position then stays on the requested Live Set (it follows the request).
// A Live Set removed from the loaded setlist re-anchors the position on its predecessor; reorders keep the position on the same Live Set.
//
// Threading: control thread only (the ControlServer's thread in engine-host; the test thread otherwise).
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "velora/host/control_plane.hpp"
#include "velora/host/live_set_switcher.hpp"
#include "velora/host/setlist_store.hpp"

namespace velora::host {

// What the controller needs from the Live Set switcher; LiveSetSwitcher implements it through LiveSetSwitcherPort, tests use a fake.
class SwitchPort {
 public:
  virtual ~SwitchPort() = default;
  virtual SwitchOutcome jump_to(const std::string& id) = 0;
  virtual const std::string& effective_id() const = 0;  // the target of the load in progress, else the Live Set that sounds ("" = none)
  virtual const std::string& sounding_id() const = 0;
  virtual const std::vector<CatalogEntry>& catalog() const = 0;
  virtual void add_to_catalog(CatalogEntry e) = 0;
  virtual void remove_from_catalog(const std::string& id) = 0;
};

class LiveSetSwitcherPort final : public SwitchPort {
 public:
  explicit LiveSetSwitcherPort(LiveSetSwitcher& sw) : sw_(sw) {}
  SwitchOutcome jump_to(const std::string& id) override { return sw_.jump_to(id); }
  const std::string& effective_id() const override { return sw_.effective_id(); }
  const std::string& sounding_id() const override { return sw_.sounding_id(); }
  const std::vector<CatalogEntry>& catalog() const override { return sw_.catalog(); }
  void add_to_catalog(CatalogEntry e) override { sw_.add_to_catalog(std::move(e)); }
  void remove_from_catalog(const std::string& id) override { sw_.remove_from_catalog(id); }

 private:
  LiveSetSwitcher& sw_;
};

// "All Live Sets" order: names compared case-insensitively for ASCII letters (a-z folded to A-Z; other bytes as they are, so UTF-8 sorts by
// code point order of the folded bytes), then ids (byte order). A strict weak ordering over (name, id).
bool live_set_name_less(const CatalogEntry& a, const CatalogEntry& b);

enum class StepResult : std::uint8_t { kMoved = 0, kEndOfList, kStartOfList, kEmptyList, kLoadFailed };
const char* to_string(StepResult r);
struct StepOutcome {
  StepResult result = StepResult::kEmptyList;
  std::string live_set_id, live_set_name;
  int position = -1;           // index in the loaded setlist (or in All Live Sets when none is loaded) of the target
  bool in_setlist = false;
  SwitchOutcome jump;          // what the switcher answered (kMoved only)
  std::string reason;          // kLoadFailed: missing | invalid_live_set | read_failed
};

class SetlistController final : public SetlistHandler {
 public:
  // Registers itself as `plane`'s setlist handler. `store`, `live`, `port` and `plane` must outlive it. The catalog of `port` is the list of
  // Live Sets (fill it before: engine-host does from the Live Set directory).
  SetlistController(ControlPlane& plane, setlist::SetlistStore& store, liveset::LiveSetStore& live, SwitchPort& port);
  ~SetlistController() override;
  SetlistController(const SetlistController&) = delete;
  SetlistController& operator=(const SetlistController&) = delete;

  // ---- loaded setlist and stepping (also reachable over ws.v1) -------------------------------------------------------------------------
  setlist::Status load_setlist(const std::string& id);   // kNoSuchSetlist when unknown
  void unload_setlist();
  StepOutcome step(bool forward);

  const std::string& loaded_setlist_id() const { return loaded_; }
  int last_setlist_position() const;                      // -1 = none
  bool end_of_setlist() const;
  bool start_of_setlist() const;
  std::uint64_t status_seq() const { sync(); return seq_; }
  std::uint64_t not_durable_commits() const { return not_durable_; }

  // The control tick: lets the position follow jumps the switcher made on its own. True when the visible setlist status changed.
  bool tick();

  // SetlistHandler.
  bool run(const ws::v1::ClientMessage& msg, ws::v1::ServerMessage& reply, HandlerFailure& err, bool& bump_revision, bool& push_state) override;
  void fill_status(ws::v1::State* out) const override;

 private:
  void sync() const;           // follow the current Live Set (mutable cache: also called from const readers)
  void reanchor();             // after a change of the loaded setlist's contents
  void fill_status_into(ws::v1::SetlistStatus* s) const;
  const CatalogEntry* find_catalog(const std::string& id) const;
  bool known_live_set(const std::string& id, CatalogEntry* out);   // in the catalog, or a valid file (then added to the catalog)
  std::vector<CatalogEntry> all_sorted() const;
  StepOutcome step_in(bool forward, const ::velora::setlist::v1::Setlist& sl);
  StepOutcome step_all(bool forward);
  StepOutcome jump_target(const std::string& id, StepOutcome o);

  ControlPlane& plane_;
  setlist::SetlistStore& store_;
  liveset::LiveSetStore& live_;
  SwitchPort& port_;

  std::string loaded_;                       // loaded setlist id, "" = none
  mutable int position_ = -1;
  mutable std::string anchor_;               // the Live Set at position_ (for re-anchoring)
  mutable std::string last_seen_;            // the current Live Set the position was last synced with
  mutable bool end_ = false, start_ = false;
  mutable std::uint64_t seq_ = 0;
  std::uint64_t reported_seq_ = 0;           // seq_ at the last tick()
  std::uint64_t not_durable_ = 0;
};

}  // namespace velora::host
