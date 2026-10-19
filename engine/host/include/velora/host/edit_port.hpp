#pragma once
// Story 4.9: the seam between the Live Set loader / switcher and the edit session. The loader and the switcher do not depend on the session; the session
// registers itself here. Control thread only (the overlay is copied on the control thread, before a background load starts).
#include <string>

#include "live_set.pb.h"
#include "velora/core/snapshot.hpp"

namespace velora::host {

class LiveSetEditPort {
 public:
  virtual ~LiveSetEditPort() = default;
  // The in-memory edited state of Live Set `id` when it has unsaved edits (the generation for it must be built from THIS, not from its file),
  // else nullptr. The pointer is valid until the next call into the session; copy it.
  virtual const ::velora::liveset::v1::LiveSet* overlay_for(const std::string& id) const = 0;
  // Called on the control thread just BEFORE a generation of another Live Set is published (a Live Set switch): the session captures the performance
  // values the player moved live so they are not lost to the program change that RT applies with the new generation.
  virtual void before_switch() = 0;
  // Called on the control thread right after a generation built from `built_from` was published (a load or a seamless switch): this Live Set now sounds.
  virtual void on_published(const ::velora::liveset::v1::LiveSet& built_from, const core::Snapshot& published) = 0;
};

}  // namespace velora::host
