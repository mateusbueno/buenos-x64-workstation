#include "velora/host/boot_target.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "velora/host/setlist_controller.hpp"
#include "velora/host/sysd_config_client.hpp"

namespace velora::host::boottarget {

const char* to_string(Source s) {
  switch (s) {
    case Source::kPointer: return "pointer";
    case Source::kStartOfSetlist: return "start_of_setlist";
    case Source::kDefaultLiveSet: return "default_live_set";
    case Source::kFirstOfAll: return "first_of_all";
    case Source::kEmptyLibrary: return "empty_library";
  }
  return "unknown";
}

std::vector<CatalogEntry> sorted_all(std::vector<CatalogEntry> live_sets) {
  std::sort(live_sets.begin(), live_sets.end(), live_set_name_less);
  return live_sets;
}

namespace {
bool has(const std::vector<CatalogEntry>& v, const std::string& id) {
  return !id.empty() && std::any_of(v.begin(), v.end(), [&](const CatalogEntry& e) { return e.id == id; });
}
}  // namespace

Target resolve(const Inputs& in) {
  Target t;
  const std::vector<CatalogEntry> all = sorted_all(in.live_sets);
  auto fallback = [&](const std::string& why) {
    t.notes.push_back(why);
    t.scene = 0;
    t.setlist_id.clear();
    if (all.empty()) {
      t.source = Source::kEmptyLibrary;
      t.live_set_id.clear();
      t.notes.push_back("no Live Set exists: an empty new Live Set is made");
    } else {
      t.source = Source::kFirstOfAll;
      t.live_set_id = all.front().id;
    }
    return t;
  };
  auto setlist_ok = [&](const std::string& id) { return !id.empty() && in.setlist_members && in.setlist_members(id) != nullptr; };

  ws::v1::BootIntoMode mode = in.mode;
  if (mode != ws::v1::BOOT_INTO_MODE_LAST_SOUND && mode != ws::v1::BOOT_INTO_MODE_START_OF_SETLIST && mode != ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET) {
    t.notes.push_back("boot_into mode " + std::to_string(static_cast<int>(mode)) + " is not known: LAST_SOUND");
    mode = ws::v1::BOOT_INTO_MODE_LAST_SOUND;
  }
  if (mode == ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET && !(in.default_live_set_id.size() == 36 && host::liveset::is_canonical_uuid(in.default_live_set_id))) {
    t.notes.push_back("default_live_set_id is not a Live Set id: LAST_SOUND");
    mode = ws::v1::BOOT_INTO_MODE_LAST_SOUND;
  }

  switch (mode) {
    case ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET: {
      if (!has(all, in.default_live_set_id)) return fallback("the default Live Set " + in.default_live_set_id + " does not exist");
      t.source = Source::kDefaultLiveSet;
      t.live_set_id = in.default_live_set_id;  // scene 1, NO SETLIST
      return t;
    }
    case ws::v1::BOOT_INTO_MODE_START_OF_SETLIST: {
      if (!in.pointer || in.pointer->setlist_id.empty()) return fallback("start of setlist: no setlist was loaded last");
      const std::string& sid = in.pointer->setlist_id;
      const std::vector<std::string>* members = in.setlist_members ? in.setlist_members(sid) : nullptr;
      if (!members) return fallback("start of setlist: setlist " + sid + " does not exist");
      for (const std::string& m : *members) {
        if (has(all, m)) {
          if (m != members->front()) t.notes.push_back("start of setlist: the first Live Set of the setlist is missing; the first one that exists is used");
          t.source = Source::kStartOfSetlist;
          t.live_set_id = m;
          t.setlist_id = sid;
          return t;
        }
      }
      return fallback("start of setlist: setlist " + sid + " has no Live Set that exists");
    }
    default: break;
  }
  // LAST_SOUND
  if (!in.pointer) return fallback("last sound: no Boot-into pointer (missing or corrupt)");
  if (!has(all, in.pointer->live_set_id)) return fallback("last sound: Live Set " + in.pointer->live_set_id + " no longer exists");
  t.source = Source::kPointer;
  t.live_set_id = in.pointer->live_set_id;
  t.scene = in.pointer->scene_index;
  if (setlist_ok(in.pointer->setlist_id)) {
    t.setlist_id = in.pointer->setlist_id;
  } else if (!in.pointer->setlist_id.empty()) {
    t.notes.push_back("last sound: setlist " + in.pointer->setlist_id + " no longer exists: NO SETLIST");
  }
  return t;
}

bool fetch_sysd_config(const std::string& socket_path, int wait_ms, ws::v1::Config& out) {
  out = default_config();
  if (socket_path.empty()) return false;
  SysdConfigClientOptions o;
  o.socket_path = socket_path;
  o.client_name = "engine-host-boot";
  o.backoff_min_ms = 20;
  o.backoff_max_ms = 200;
  SysdConfigClient c(o);
  c.start();
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  bool got = false;
  while (!(got = c.received() > 0) && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (got) out = c.config();
  c.stop();
  return got;
}

}  // namespace velora::host::boottarget
