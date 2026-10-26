#pragma once
// Boot target resolution (Story 8.9, FR62, FR65, UX-DR78, AR39 partial).
//
// After power-on the unit starts where the show needs it, by the sysd `boot_into` setting:
//   * LAST_SOUND (default): the Live Set and scene of the Story 8.8 pointer, with the pointer's setlist loaded.
//   * START_OF_SETLIST: the first Live Set of the last loaded setlist (the pointer's setlist_id), at scene 1, that setlist loaded.
//   * DEFAULT_LIVE_SET: `default_live_set_id` at scene 1, with NO setlist loaded.
// Fallbacks (each one a note in Target::notes; the caller logs them): a target that is missing, deleted or whose file is invalid (the catalog lists only valid
// files), no pointer, a deleted / empty / unknown setlist, an unknown mode or a default id that is not a Live Set id all end on the FIRST Live Set of All Live
// Sets (the order stepping uses); an empty library ends with an empty live_set_id (the caller makes a new empty Live Set). A missing LIBRARY is not a target
// problem: the Live Set loads and its affected parts show Missing library (the loader's behaviour), silent, never substituted.
// The resolution is pure (no I/O, no audio): tests drive it with plain data. Control thread, at start.
//
// sysd. The engine reads the first sysd config with a BOUNDED wait (fetch_sysd_config, kSysdBootWaitMs); when sysd does not answer in time the boot uses the
// default config (LAST_SOUND). The wait never blocks anything but the start of the engine, and only that long.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "velora/host/boot_into_store.hpp"
#include "velora/host/live_set_switcher.hpp"
#include "ws_v1.pb.h"

namespace velora::host::boottarget {

// How long the engine waits for the first sysd config before booting on the defaults (LAST_SOUND). Bounded on purpose: sysd absent never stalls the boot.
constexpr int kSysdBootWaitMs = 1500;

enum class Source : std::uint8_t {
  kPointer = 0,        // LAST_SOUND: the Boot-into pointer
  kStartOfSetlist,     // START_OF_SETLIST: the first Live Set of the pointer's setlist
  kDefaultLiveSet,     // DEFAULT_LIVE_SET: the configured Live Set
  kFirstOfAll,         // a fallback: the first Live Set of All Live Sets
  kEmptyLibrary,       // no Live Set at all: the caller creates an empty one
};
const char* to_string(Source s);

struct Inputs {
  ws::v1::BootIntoMode mode = ws::v1::BOOT_INTO_MODE_LAST_SOUND;
  std::string default_live_set_id;
  std::optional<bootinto::Pointer> pointer;  // BootIntoStore::load(); empty = no pointer (missing / corrupt)
  std::vector<CatalogEntry> live_sets;        // every valid Live Set
  // The ids of the Live Sets of a setlist in order; null when there is no such setlist.
  std::function<const std::vector<std::string>*(const std::string& setlist_id)> setlist_members;
};

struct Target {
  Source source = Source::kEmptyLibrary;
  std::string live_set_id;       // "" only with kEmptyLibrary
  std::uint32_t scene = 0;       // 0-based scene to recall (0 = "scene 1")
  std::string setlist_id;        // the setlist to load ("" = NO SETLIST)
  std::vector<std::string> notes;  // why a fallback was taken (empty when the target is the one the setting names)
};

Target resolve(const Inputs& in);

// All Live Sets in stepping order (name, case-insensitive for ASCII, then id).
std::vector<CatalogEntry> sorted_all(std::vector<CatalogEntry> live_sets);

// Starts a SysdConfigClient on `socket_path`, waits up to `wait_ms` for the first config and returns it (true). False (and `out` = default_config()) when sysd did
// not answer in time. The client is stopped before returning; the engine's own client delivers the config again afterwards.
bool fetch_sysd_config(const std::string& socket_path, int wait_ms, ws::v1::Config& out);

}  // namespace velora::host::boottarget
