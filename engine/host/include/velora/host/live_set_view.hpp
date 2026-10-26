#pragma once
// The read-only Live Set view (Story 9.8, FR97 partial). ws.v1 GetLiveSetView answers LiveSetViewReply: one Live Set as Velora Studio draws it, assembled for ANY
// Live Set of the catalog, whether or not it sounds:
//   * the stored Live Set (LiveSetStore::read), or -- when the edit session holds a document for it (it sounds, or it still has unsaved edits) -- that edited copy,
//     so the view agrees with what the device would play and with the unsaved flag (`dirty`, the ◆);
//   * per part: on / off, engine, volume, zone, the timbre name and category through the library resolver (exact paths, no scanning), the organ registration
//     ("88 8000 000" from the part's nine drawbar values, registry defaults where the Live Set stores none) and `missing_library` when the resolver cannot find the
//     timbre's library or instrument. A part that cannot be resolved never shows another timbre's name (its name is empty);
//   * the 8 scene tiles (name, empty flag).
// READ-ONLY by construction: this class holds no way to publish, load, recall or jump (it sees the store through a const read, the edit session through const
// accessors, the plane only for the tier's part count and the sounding Live Set's id). It never moves the revision and pushes nothing; the resolver's work is
// bounded and constant per part (see library_resolver.hpp). Control thread only (it runs inside ControlPlane::handle).
#include <list>
#include <string>
#include <unordered_map>

#include "velora/host/control_plane.hpp"
#include "velora/host/edit_session.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_store.hpp"

namespace velora::host {

// "88 8000 000": drawbar values 0..8 in the order 16', 5 1/3' | 8', 4', 2 2/3', 2' | 1 3/5', 1 1/3', 1'. Values are clamped to 0..8; non-finite ones read as 0.
std::string organ_registration_text(const float (&drawbars)[9]);

class LiveSetViewService final : public ViewHandler {
 public:
  // Registers itself as `plane`'s view handler and unregisters in the destructor. `live`, `resolver` and `plane` outlive it; `edits` is optional (without it every
  // view is the stored Live Set and never dirty); `sounding_id` (optional) names the Live Set that sounds (LiveSetSwitcherPort::sounding_id()), else the edit
  // session's current document is used.
  LiveSetViewService(ControlPlane& plane, liveset::LiveSetStore& live, const LibraryResolver& resolver, const EditSession* edits = nullptr,
                     SwitchPort* port = nullptr);
  ~LiveSetViewService() override;
  LiveSetViewService(const LiveSetViewService&) = delete;
  LiveSetViewService& operator=(const LiveSetViewService&) = delete;

  bool run(const ws::v1::ClientMessage& msg, ws::v1::ServerMessage& reply, HandlerFailure& err) override;

  // The resolved-timbre cache (bounded LRU, control thread only). A successful resolve of (library_id, instrument_key) is remembered with the `current` link's target
  // (the bundle id) it was made under; a hit costs one readlink() instead of the resolver's lstat / open / three preads, and a library that was replaced (the link now
  // names another bundle) misses. Failures are never cached (they are cheap and may be fixed by the next import).
  static constexpr std::size_t kCacheCapacity = 64;
  std::uint64_t cache_hits() const noexcept { return hits_; }
  std::uint64_t cache_misses() const noexcept { return misses_; }
  std::size_t cache_size() const noexcept { return lru_.size(); }

 private:
  struct Resolved {
    std::string key, bundle_id, name;
    std::uint32_t category = 0;
  };
  bool lookup_timbre(const std::string& library_id, const std::string& instrument_key, Resolved& out, std::string& reason);
  std::list<Resolved> lru_;
  std::unordered_map<std::string, std::list<Resolved>::iterator> index_;
  std::uint64_t hits_ = 0, misses_ = 0;
  ControlPlane& plane_;
  liveset::LiveSetStore& live_;
  const LibraryResolver& resolver_;
  const EditSession* edits_;
  SwitchPort* port_;
};

}  // namespace velora::host
