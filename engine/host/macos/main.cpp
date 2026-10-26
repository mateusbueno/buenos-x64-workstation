// engine-host: macOS developer app. Plays the engine test tone from CoreMIDI notes through
// CoreAudio. Timing figures are for the developer's Mac only: "host, not representative".
#include <poll.h>
#include <sys/resource.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <sys/stat.h>
#include <string>
#include <vector>

#include "coreaudio_line_in.hpp"
#include "coreaudio_output.hpp"
#include "coremidi_input.hpp"
#include "mach_time.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/mixer.hpp"
#include "velora/core/fm_engine.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/va_engine.hpp"
#include "velora/host/boot_into_store.hpp"
#include "velora/host/boot_target.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/control_server.hpp"
#include "velora/host/edit_session.hpp"
#include "velora/host/factory_live_set.hpp"
#include "velora/host/dev_config.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/library_client.hpp"
#include "velora/host/live_set_switcher.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/panel_reader.hpp"
#include "velora/host/pedal_calibrator.hpp"
#include "velora/host/virtual_panel_map.hpp"
#include "velora/host/render_driver.hpp"
#include "velora/host/scene_controller.hpp"
#include "velora/host/setlist_controller.hpp"
#include "velora/host/setlist_store.hpp"
#include "velora/host/tier_profile.hpp"
#include "velora/host/sysd_config_client.hpp"
#ifdef VELORA_ENGINE_DEV_KEYS
#include "velora/host/dev_key_provider.hpp"
#include "velora/host/protected_head_provider.hpp"
#include "velora/host/secure_memory.hpp"
#include "velora/host/trust_list.hpp"
#endif

namespace {
std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit.store(true); }
}  // namespace

int main(int argc, char** argv) {
  using namespace velora;
  std::setvbuf(stdout, nullptr, _IOLBF, 0);  // progress lines reach a log file or a pipe as they happen (the smoke tests read them)
  double duration_s = 0;  // 0 = run until q / SIGINT
  std::string socket_path = "/tmp/velora-engine.sock";
  std::string config_path = VELORA_DEV_CONFIG_DEFAULT;
  bool want_line_in = false, no_audio = false;
  std::string din_source;  // Story 7.7: a CoreMIDI source whose name contains this is the DIN input (Mac only; the board's DIN UART adapter tags kDin itself)
  std::string live_set_path, library_root, live_set_dir, data_dir, library_socket, virtual_panel_path, sysd_socket;
  host::PedalDirection switch_direction = host::PedalDirection::kNormal;  // Story 3.11
  std::string trust_list_path, dev_device_key;  // Story 6.5 (dev builds): protected libraries
  std::optional<ports::TierId> tier_override;   // Story 4.1: dev-only --tier pro|v1 (the Mac has no tier strap)
  bool factory_live_sets_required = false;      // --factory-live-sets-required: a failed import is fatal
  std::string factory_live_sets_path;            // Story 6.9 (dev builds): the signed factory Live Set file imported on first boot
  long scrub_interval_ms = 30000;               // Story 6.5: how often an I/O edge thread re-verifies the resident protected instruments (0 = never)
  long library_idle_ms = 3000, inject_note_after_ms = -1;  // --library-playing-idle-ms; hidden --test-inject-note-after-ms (smoke tests: a note without MIDI)
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--duration") && i + 1 < argc) {
      const char* arg = argv[++i];
      char* end = nullptr;
      duration_s = std::strtod(arg, &end);
      if (end == arg || *end != '\0' || !std::isfinite(duration_s) || duration_s <= 0) {
        std::fprintf(stderr, "engine-host: --duration needs a positive number of seconds\n");
        return 2;
      }
    } else if (!std::strcmp(argv[i], "--socket") && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (!std::strcmp(argv[i], "--config") && i + 1 < argc) {
      config_path = argv[++i];
    } else if (!std::strcmp(argv[i], "--line-in")) {
      want_line_in = true;
    } else if (!std::strcmp(argv[i], "--no-audio")) {
      no_audio = true;  // dev/test only: control plane, switcher and server without CoreAudio / CoreMIDI (the render driver never runs)
    } else if (!std::strcmp(argv[i], "--live-set") && i + 1 < argc) {
      live_set_path = argv[++i];
    } else if (!std::strcmp(argv[i], "--live-set-dir") && i + 1 < argc) {
      live_set_dir = argv[++i];
    } else if (!std::strcmp(argv[i], "--data-dir") && i + 1 < argc) {
      data_dir = argv[++i];
    } else if (!std::strcmp(argv[i], "--library-root") && i + 1 < argc) {
      library_root = argv[++i];
    } else if (!std::strcmp(argv[i], "--library-playing-idle-ms") && i + 1 < argc) {
      library_idle_ms = std::strtol(argv[++i], nullptr, 10);
      if (library_idle_ms <= 0) {
        std::fprintf(stderr, "engine-host: --library-playing-idle-ms needs a positive number\n");
        return 2;
      }
    } else if (!std::strcmp(argv[i], "--test-inject-note-after-ms") && i + 1 < argc) {
      inject_note_after_ms = std::strtol(argv[++i], nullptr, 10);  // test only: counts as a note this many ms after start
#ifdef VELORA_ENGINE_DEV_KEYS  // dev builds only: the option names are not even in the binary of a build without the dev key provider
    } else if (!std::strcmp(argv[i], "--trust-list") && i + 1 < argc) {
      trust_list_path = argv[++i];
    } else if (!std::strcmp(argv[i], "--dev-device-key") && i + 1 < argc) {
      dev_device_key = argv[++i];
    } else if (!std::strcmp(argv[i], "--factory-live-sets-required")) {
      factory_live_sets_required = true;
    } else if (!std::strcmp(argv[i], "--factory-live-sets") && i + 1 < argc) {
      factory_live_sets_path = argv[++i];
    } else if (!std::strcmp(argv[i], "--scrub-interval-ms") && i + 1 < argc) {
      char* end = nullptr;
      scrub_interval_ms = std::strtol(argv[++i], &end, 10);
      if (*end != '\0' || scrub_interval_ms < 0) {
        std::fprintf(stderr, "engine-host: --scrub-interval-ms needs a number of milliseconds (0 = off)\n");
        return 2;
      }
#endif
    } else if (!std::strcmp(argv[i], "--virtual-panel") && i + 1 < argc) {
      virtual_panel_path = argv[++i];  // Story 3.10: the Mac virtual panel mapping file (a USB MIDI control surface becomes the panel)
    } else if (!std::strcmp(argv[i], "--library-socket") && i + 1 < argc) {
      library_socket = argv[++i];  // Story 4.11: tell the librarian service which Live Set became active and when the engine is performing
    } else if (!std::strcmp(argv[i], "--din-source") && i + 1 < argc) {
      din_source = argv[++i];
    } else if (!std::strcmp(argv[i], "--switch-polarity") && i + 1 < argc) {
      const std::string v = argv[++i];  // Story 3.11: the footswitch's wiring; sysd has no field for it (Story 3.8's detection will supply it)
      if (v == "closed") switch_direction = host::PedalDirection::kInverted;
      else if (v == "open" || v == "auto") switch_direction = host::PedalDirection::kNormal;
      else {
        std::fprintf(stderr, "engine-host: --switch-polarity open|closed|auto\n");
        return 2;
      }
    } else if (!std::strcmp(argv[i], "--tier") && i + 1 < argc) {
      ports::TierId t;
      if (!host::parse_tier_name(argv[++i], t)) {
        std::fprintf(stderr, "engine-host: --tier needs pro or v1 (dev-only override of the tier profile), got \"%s\"\n", argv[i]);
        return 2;
      }
      tier_override = t;
    } else if (!std::strcmp(argv[i], "--sysd-socket") && i + 1 < argc) {
      sysd_socket = argv[++i];  // Foundation: the system config (clock source, velocity curve, pedals, Boot into) comes from sysd at start and on change
    } else {
      std::fprintf(stderr,
                   "usage: engine-host [--duration SECONDS] [--socket PATH] [--config engine-dev.json] [--line-in] [--no-audio (dev/test only)]\n"
                   "                   [--live-set FILE --library-root DIR]   (load a Live Set at start; its parts play from the keyboard by zone)\n"
                   "                   [--live-set-dir DIR --library-root DIR]   (the Live Sets of a directory (<id>.liveset) that ws.v1 JumpToLiveSet can switch to,\n"
                   "                                                              seamlessly, loading in the background; without --live-set the first by name loads at start)\n"
                   "                   [--data-dir DIR --library-root DIR]   (the persisted data root: DIR/live_sets/ holds the Live Sets, DIR/setlists/ the setlists, tags and the\n"
                   "                                                          default ASSIGN map; ws.v1 setlist commands and LIVE SET stepping; replaces --live-set-dir)\n"
#ifdef VELORA_ENGINE_DEV_KEYS
                   "                   [--trust-list FILE --dev-device-key FILE [--scrub-interval-ms N]]   (dev builds only: play PROTECTED libraries -- the content-manifest keys to trust\n"
                   "                                                              (tools/keys/trust/<root>.pub-list format) and the dev device private key (device.x25519.priv) that unwraps\n"
                   "                                                              their content keys; an I/O edge thread re-verifies resident protected instruments every N ms, default 30000)\n"
                   "                   [--factory-live-sets FILE]   (dev builds, with --data-dir --library-root --trust-list: import the signed factory Live Set file (factory-live-sets.vfls)\n"
                   "                                                 into DIR/live_sets through the Live Set store before the catalog is scanned; idempotent, user edits and deletions are kept)\n"
                   "                   [--factory-live-sets-required]   (a failed factory import is fatal: exit 1, instead of starting with the Live Sets the engine has)\n"
#endif
                   "                   [--virtual-panel MAP.json]   (Story 3.10: a mapping file (engine/host/dev/virtual-panel.json) turns the faders, knobs and buttons of a MIDI control surface\n"
                   "                                                 into panel MCU frames for the panel reader; wheels reach the keyboard-on parts, BUTTON frames reach subscribed ui clients)\n"
                   "                   [--din-source NAME]   (Story 7.7: the CoreMIDI source whose name contains NAME is the DIN input, every other source is USB; clock is followed from\n"
                   "                                           the port the sysd clock_source selects and ignored from the other)\n"
                   "                   [--tier pro|v1]   (Story 4.1, DEV ONLY: run the Pro or the v1 tier profile; the tier is otherwise read once from the tier source, and the Mac has\n"
                   "                                      no strap, so it runs v1)\n"
                   "                   [--sysd-socket PATH]   (the sysd config service (velora-sysd): the engine runs on the defaults until sysd answers, applies the config on connect and on every\n"
                   "                                           change, keeps the last one when sysd goes away; sysd absent never stops the engine; default off)\n"
                   "                   [--switch-polarity open|closed|auto]   (Story 3.11: the footswitch pedal's wiring: closed = normally closed, inverted; default raw)\n"
                   "                   [--library-socket PATH [--library-playing-idle-ms N]]   (report the active Live Set and the performing state to the librarian service's Library API;\n"
                   "                                                                           performing = a note within the last N ms, default 3000; default off)\n");
      return std::strcmp(argv[i], "--help") ? 2 : 0;
    }
  }
  // Story 4.1 (AD-27): the tier is read ONCE, here, and the profile is immutable for the run. The Mac has no strap (NoStrapTierSource: v1), unless the dev override says Pro.
  host::NoStrapTierSource tier_source;
  const host::TierSelection tier = host::resolve_tier(&tier_source, tier_override);
  const host::TierProfile& profile = *tier.profile;
  std::printf("%s: %u parts, head budget %llu bytes, sequencer %s\n", tier.log_line.c_str(), static_cast<unsigned>(profile.part_count),
              static_cast<unsigned long long>(profile.head_budget_bytes), profile.sequencer_enabled ? "enabled" : "off");
  if (!data_dir.empty()) {
    if (!live_set_dir.empty()) {
      std::fprintf(stderr, "engine-host: --data-dir and --live-set-dir exclude each other (--data-dir uses DIR/live_sets)\n");
      return 2;
    }
    live_set_dir = data_dir + "/live_sets";
    for (const std::string& d : {data_dir, live_set_dir, data_dir + "/setlists"}) {
      if (::mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "engine-host: cannot create %s: %s\n", d.c_str(), std::strerror(errno));
        return 1;
      }
    }
  }
#ifdef VELORA_ENGINE_DEV_KEYS
  if (!trust_list_path.empty() != !dev_device_key.empty()) {
    std::fprintf(stderr, "engine-host: --trust-list and --dev-device-key go together\n");
    return 2;
  }
#endif
#ifdef VELORA_ENGINE_DEV_KEYS
  // Story 6.9: first boot. The control thread (this one) imports the signed factory Live Sets through the single writer (LiveSetStore) before anything scans the
  // directory. The store is closed again right after, so the engine's own store below takes the writer lock as before. A refusal is reported and the engine
  // starts with the Live Sets it has (a gig must go on); the typed cause and any unresolved (library_id, instrument_key) are on stderr.
  if (factory_live_sets_required && factory_live_sets_path.empty()) {
    std::fprintf(stderr, "engine-host: --factory-live-sets-required needs --factory-live-sets\n");
    return 2;
  }
  if (!factory_live_sets_path.empty()) {
    if (data_dir.empty() || library_root.empty() || trust_list_path.empty()) {
      std::fprintf(stderr, "engine-host: --factory-live-sets needs --data-dir, --library-root and --trust-list\n");
      return 2;
    }
    velora::format::TrustedKeys trusted;
    std::string terr;
    if (!host::load_trust_list_file(trust_list_path, trusted, terr)) {
      std::fprintf(stderr, "engine-host: %s\n", terr.c_str());
      return 1;
    }
    host::LibraryResolver factory_resolver(host::make_posix_fs(), library_root);
    factory_resolver.set_protected_trust(std::make_shared<const velora::format::TrustedKeys>(trusted));
    host::liveset::StoreOptions fso;
    fso.dir = live_set_dir;
    fso.part_count = profile.part_count;
    std::unique_ptr<host::liveset::LiveSetStore> factory_store;
    const host::liveset::Status fst = host::liveset::LiveSetStore::open(fso, factory_store);
    if (fst != host::liveset::Status::kOk) {
      std::fprintf(stderr, "engine-host: live set store %s: cannot open: %s (another engine-host running on it?)\n", live_set_dir.c_str(), host::liveset::to_string(fst));
      return 1;
    }
    host::factory::InstallOptions fio;
    fio.trusted = &trusted;
    fio.part_count = profile.part_count;
    fio.resolves = host::factory::resolver_of(factory_resolver);
    const host::factory::InstallResult fr = host::factory::install_factory_live_sets_file(*factory_store, factory_live_sets_path, fio);
    if (fr.ok()) {
      std::printf("factory live sets: %s %s: %zu imported, %zu updated, %zu unchanged, %zu deleted by the user, %zu conflict(s)\n", fr.release_name.c_str(), fr.release_version.c_str(),
                  fr.imported.size(), fr.updated.size(), fr.unchanged.size(), fr.deleted_by_user.size(), fr.conflicts.size());
    } else {
      std::fprintf(stderr, "engine-host: factory live sets %s: %s: %s\n", factory_live_sets_path.c_str(), host::factory::to_string(fr.status), fr.detail.c_str());
      if (factory_live_sets_required) return 1;
    }
  }
#endif
  if ((live_set_path.empty() && live_set_dir.empty()) != library_root.empty()) {
    std::fprintf(stderr, "engine-host: --live-set / --live-set-dir and --library-root go together\n");
    return 2;
  }
  // Story 4.5: the Live Sets that can be switched to. Listing the directory reads each file; invalid ones are reported and left out.
  std::vector<host::CatalogEntry> catalog;
  if (!live_set_dir.empty()) {
    std::vector<std::string> problems;
    catalog = host::scan_live_set_dir(live_set_dir, profile.part_count, &problems);
    for (const std::string& pr : problems) std::fprintf(stderr, "engine-host: live set directory: skipped %s\n", pr.c_str());
    std::printf("live set directory %s: %zu Live Set(s)\n", live_set_dir.c_str(), catalog.size());
    if (!live_set_path.empty()) {  // the Live Set we start with is always one a jump can come back to
      host::CatalogEntry e;
      std::string cerr2;
      if (host::catalog_entry_for_file(live_set_path, profile.part_count, e, cerr2)) {
        bool known = false;
        for (const host::CatalogEntry& c : catalog) known = known || c.id == e.id;
        if (!known) catalog.push_back(std::move(e));
      }
    }
    if (live_set_path.empty() && data_dir.empty()) {
      if (catalog.empty()) {
        std::fprintf(stderr, "engine-host: %s holds no Live Set to start with\n", live_set_dir.c_str());
        return 1;
      }
      live_set_path = catalog.front().path;  // the first by name
    }  // --data-dir: the boot target below names the Live Set to start with (Story 8.9)
  }
  // Story 8.9: the boot target. With a data root and no explicit --live-set, the sysd `boot_into` setting (read with a bounded wait; the defaults when sysd does not
  // answer) and the Boot-into pointer (Story 8.8) name the Live Set, scene and setlist to start on; a missing target falls back (logged) to the first Live Set of All
  // Live Sets, and an empty library gets an empty new Live Set. Boot never waits for a target or for sysd beyond kSysdBootWaitMs.
  host::boottarget::Target boot_target;
  bool boot_resolved = false;
  // An empty new Live Set in the data root (persisted: the product decision in-memory vs persisted is open, see the README), added to the catalog. Control thread, start.
  auto make_empty_live_set = [&](std::string& path_out) -> bool {
    host::liveset::StoreOptions blo;
    blo.dir = live_set_dir;
    blo.part_count = profile.part_count;
    host::setlist::StoreOptions bso;
    bso.dir = data_dir + "/setlists";
    std::unique_ptr<host::liveset::LiveSetStore> tl;
    std::unique_ptr<host::setlist::SetlistStore> ts;
    std::string id;
    if (host::liveset::LiveSetStore::open(blo, tl) != host::liveset::Status::kOk || host::setlist::SetlistStore::open(bso, ts) != host::setlist::Status::kOk ||
        ts->create_live_set(*tl, "Live Set", id) != host::setlist::Status::kOk)
      return false;
    path_out = tl->path_for(id);
    catalog.push_back(host::CatalogEntry{id, "Live Set", path_out});
    std::printf("data dir %s: made an empty Live Set (%s)\n", data_dir.c_str(), id.c_str());
    return true;
  };
  if (!data_dir.empty() && live_set_path.empty()) {
    ws::v1::Config boot_cfg = host::default_config();
    if (!sysd_socket.empty()) {
      if (!host::boottarget::fetch_sysd_config(sysd_socket, host::boottarget::kSysdBootWaitMs, boot_cfg))
        std::printf("boot target: sysd did not answer within %d ms: Boot into LAST_SOUND (default)\n", host::boottarget::kSysdBootWaitMs);
    }
    host::boottarget::Inputs bi;
    bi.mode = boot_cfg.boot_into().mode();
    bi.default_live_set_id = boot_cfg.boot_into().default_live_set_id();
    host::bootinto::Pointer ptr;
    if (host::bootinto::load(data_dir, ptr) == host::bootinto::LoadStatus::kOk) bi.pointer = ptr;
    bi.live_sets = catalog;
    // The setlists are read through a short-lived store (closed again before the engine's own opens it, as the factory import does).
    std::map<std::string, std::vector<std::string>> members;
    {
      host::setlist::StoreOptions bso;
      bso.dir = data_dir + "/setlists";
      std::unique_ptr<host::setlist::SetlistStore> tmp;
      if (host::setlist::SetlistStore::open(bso, tmp) == host::setlist::Status::kOk) {
        for (const auto& sl : tmp->library().setlists()) members[sl.id()] = std::vector<std::string>(sl.live_set_ids().begin(), sl.live_set_ids().end());
      }
    }
    if (catalog.empty()) {  // an empty library: the unit still starts on a sounding (empty) Live Set
      std::string np;
      std::printf("data dir %s holds no Live Set yet\n", data_dir.c_str());
      if (make_empty_live_set(np)) bi.live_sets = catalog;
      else std::fprintf(stderr, "engine-host: warning: could not make an empty Live Set in %s\n", live_set_dir.c_str());
    }
    bi.setlist_members = [&members](const std::string& id) -> const std::vector<std::string>* {
      const auto it = members.find(id);
      return it == members.end() ? nullptr : &it->second;
    };
    boot_target = host::boottarget::resolve(bi);
    boot_resolved = true;
    for (const std::string& n : boot_target.notes) std::printf("boot target: %s\n", n.c_str());
    for (const host::CatalogEntry& e : catalog)
      if (e.id == boot_target.live_set_id) live_set_path = e.path;
    std::printf("boot target: %s mode=%s live set %s scene %u setlist %s\n", host::boottarget::to_string(boot_target.source), ws::v1::BootIntoMode_Name(boot_cfg.boot_into().mode()).c_str(),
                boot_target.live_set_id.empty() ? "-" : boot_target.live_set_id.c_str(), static_cast<unsigned>(boot_target.scene) + 1, boot_target.setlist_id.empty() ? "NO SETLIST" : boot_target.setlist_id.c_str());
  }
  // A fresh data root (no Live Set yet): the engine starts on the dev configuration with the switcher, the stores and the setlist controller, so that
  // CreateLiveSet and JumpToLiveSet can bootstrap an empty install.
  bool bootstrap = live_set_path.empty() && !data_dir.empty();
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  auto renderer = std::make_unique<core::BlockRenderer>();
  auto queue = std::make_unique<host::HostEventQueue>();
  auto control_queue = std::make_unique<host::ControlEventQueue>();
  host::RenderDriver driver(*renderer, *queue, {}, control_queue.get());
  host::MidiIngest ingest(*queue);

  // Control plane: the first snapshot generation comes from the committed dev configuration and is
  // published before audio runs; an invalid file stops the engine here with a clear error.
  host::DamageRegistry damage;  // Story 6.5: damaged blocks of protected libraries found by the I/O edge threads; shown in State.content. Declared BEFORE the plane: the plane
                                // detaches its listener from it when it is destroyed.
  host::ControlPlane plane(renderer->snapshots(), renderer->last_values(), *control_queue,
                           [] { return host::macos::now_ns(); });
  plane.set_tier_profile(profile);  // Story 4.1: the part cap, the FX budget and State.tier come from the profile (before load_initial)
  // With --live-set the Live Set is the first generation (below); the dev configuration is not published first.
  if (live_set_path.empty()) {
    ws::v1::State initial;
    std::string cerr;
    if (!host::load_dev_config(config_path, initial, cerr) || !plane.load_initial(initial, cerr)) {
      std::fprintf(stderr, "engine-host: config: %s\n", cerr.c_str());
      return 1;
    }
  }

  // The sample engine and the organ are bound only for a Live Set (the dev configuration plays the test tone).
  core::SampleEngine::Config sample_cfg;
  sample_cfg.max_voices = profile.sample_voice_capacity;  // the profile's sample floor plus the 64 voice fade headroom, with slack
  sample_cfg.extended_voices = true;  // Story 5.2: any imported instrument may use filters and CC crossfades, so every voice is budgeted for them
  sample_cfg.release_velocity_triggers = true;  // Story 3.6: release-trigger samples follow the key's release velocity (the Note Off the keybed reader sends)
  core::SampleEngine sample(sample_cfg);
  core::OrganEngine organ(profile.part_count, profile.organ_voice_capacity);  // Story 4.1: generators for the tier's parts, the tier's voice pool
  core::FmEngine fm(48000.0f, profile.fm_voice_capacity);
  core::VaEngine va(48000.0f, profile.va_voice_capacity);
  host::LibraryResolver resolver(host::make_posix_fs(), library_root.empty() ? std::string(host::kDefaultLibraryRoot) : library_root);
  host::RamHeadProvider ram_heads;
  host::HeadProvider* head_provider = &ram_heads;
#ifdef VELORA_ENGINE_DEV_KEYS
  std::unique_ptr<host::DevKeyProvider> dev_keys;
  std::unique_ptr<host::ProtectedHeadProvider> protected_heads;
  if (!trust_list_path.empty()) {
    // Core dumps off before any key exists in this process (the content key and the decoded PCM must not reach a core file).
    if (!host::disable_core_dumps()) {
      std::fprintf(stderr, "engine-host: cannot disable core dumps (RLIMIT_CORE); refusing to hold content keys\n");
      return 1;
    }
    struct rlimit core_rl;
    ::getrlimit(RLIMIT_CORE, &core_rl);
    std::printf("core_limit=%llu/%llu\n", (unsigned long long)core_rl.rlim_cur, (unsigned long long)core_rl.rlim_max);
    // Every live content key holds one locked page and the dev provider's device key one more: check RLIMIT_MEMLOCK before the first key exists.
    const std::size_t memlock = host::memlock_budget_bytes(), page = host::secure_page_bytes();
    if (memlock < 4 * page) {
      std::fprintf(stderr, "engine-host: RLIMIT_MEMLOCK is %zu bytes; protected libraries need about (instruments + 2) locked pages of %zu bytes (raise `ulimit -l`)\n", memlock, page);
      return 1;
    }
    if (memlock < 64 * page) std::fprintf(stderr, "engine-host: warning: RLIMIT_MEMLOCK is %zu bytes (about %zu protected instruments at once)\n", memlock, memlock / page - 2);
    velora::format::TrustedKeys trusted;
    std::string kerr;
    if (!host::load_trust_list_file(trust_list_path, trusted, kerr)) {
      std::fprintf(stderr, "engine-host: %s\n", kerr.c_str());
      return 1;
    }
    resolver.set_protected_trust(std::make_shared<const velora::format::TrustedKeys>(std::move(trusted)));
    dev_keys = host::DevKeyProvider::from_file(dev_device_key, {}, kerr);
    if (!dev_keys) {
      std::fprintf(stderr, "engine-host: %s\n", kerr.c_str());
      return 1;
    }
    protected_heads = std::make_unique<host::ProtectedHeadProvider>(host::make_posix_fs(), *dev_keys, &damage);
    head_provider = protected_heads.get();
    plane.set_damage_registry(&damage);
    std::printf("protected libraries: on (dev key provider, device key id %02x%02x%02x%02x%02x%02x%02x%02x)\n", dev_keys->device_key_id()[0], dev_keys->device_key_id()[1],
                dev_keys->device_key_id()[2], dev_keys->device_key_id()[3], dev_keys->device_key_id()[4], dev_keys->device_key_id()[5], dev_keys->device_key_id()[6],
                dev_keys->device_key_id()[7]);
  }
#endif
  std::unique_ptr<host::bootinto::BootIntoStore> boot_store;      // Story 8.8: journaled Boot-into pointer (control thread only); outlives the control server
  std::unique_ptr<host::bootinto::BootIntoTracker> boot_tracker;  // dirty tracking + idle-safe policy
  std::unique_ptr<host::SceneController> scenes;  // Story 4.4: ws.v1 RecallScene / NextScene / PreviousScene; destroyed before the plane
  std::unique_ptr<host::LiveSetLoader> loader;     // kept for the run: the switcher commits through it
  std::unique_ptr<host::LiveSetSwitcher> switcher; // Story 4.5: ws.v1 JumpToLiveSet / CancelLoad; destroyed before the loader and the plane
  std::unique_ptr<host::LiveSetSwitcherPort> switch_port;  // Story 4.6: what the setlist controller uses of the switcher
  std::unique_ptr<host::liveset::LiveSetStore> live_store;  // Story 4.6: sole writer of the Live Set files (create / delete Live Set commands)
  std::unique_ptr<host::setlist::SetlistStore> setlist_store;  // Story 4.6: sole writer of setlists, tags and the default ASSIGN map
  std::unique_ptr<host::SetlistController> setlists;           // Story 4.6: ws.v1 setlist commands and LIVE SET stepping; destroyed after the edit session
  std::unique_ptr<host::EditSession> edits;                    // Story 4.9: edits in memory, the dirty flags and Write; destroyed first
  std::atomic<bool> audio_running{false};  // set once the audio stream starts; the loader refuses to run while it is true
  if (!live_set_path.empty() || bootstrap) {
    if (!renderer->bind_engine(sample) || !renderer->bind_engine(organ) || !renderer->bind_engine(fm) || !renderer->bind_engine(va)) {
      std::fprintf(stderr, "engine-host: cannot bind engines (sample, organ, fm, va)\n");
      return 1;
    }
    // Story 4.3: the Live Set is the first generation and reserves the voice pool for the whole Live Set (FX, organ,
    // sample floor) before audio runs. A Live Set that cannot be loaded stops the engine here with the reason; nothing is half loaded.
    host::LiveSetLoaderOptions lopt = host::loader_options_for(profile);  // Story 4.1: part count, voice budget, floors and head budget of the tier
    lopt.is_rendering = [&audio_running] { return audio_running.load(); };
    scenes = std::make_unique<host::SceneController>(plane);
    lopt.scenes = scenes.get();
    loader = std::make_unique<host::LiveSetLoader>(*renderer, plane, resolver, *head_provider, &sample, lopt);
    // No Live Set to load: the flat FX reservation of the dev configuration (as without --live-set); the first jump replaces it seamlessly.
    auto reserve_dev_config = [&]() -> bool {
      core::VoiceArbiter::LiveSetConfig reservation;
      reservation.budget = profile.arbiter_budget;
      reservation.fx_cost = plane.fx_budget();
      reservation.waive_sample_floor = true;  // the engines are bound for the first jump, but nothing plays on them yet
      reservation.waive_organ_reservation = true;
      reservation.waive_fm_floor = true;
      reservation.waive_va_floor = true;
      const auto st = renderer->arbiter().reserve(reservation);
      if (st != core::VoiceArbiter::ReserveStatus::kOk) {
        std::fprintf(stderr, "engine-host: cannot reserve the FX budget (status %d)\n", static_cast<int>(st));
        return false;
      }
      return true;
    };
    if (bootstrap) {
      if (!reserve_dev_config()) return 1;
      std::printf("data dir %s holds no Live Set yet: running on the dev configuration\n", data_dir.c_str());
    } else {
      host::LiveSetLoadResult loaded = loader->load_file(live_set_path);
      if (!loaded.ok() && boot_resolved) {
        // Story 8.9: the boot target cannot be loaded (nothing is half loaded): the unit still starts. At most kBootLoadAttempts Live Sets are tried (the target, then
        // the first of All Live Sets, ...); when none loads an empty new Live Set is made, and when even that fails the engine runs on the dev configuration.
        constexpr int kBootLoadAttempts = 3;
        int attempts = 1;
        std::fprintf(stderr, "engine-host: boot target %s: %s%s%s; trying the other Live Sets\n", live_set_path.c_str(), host::to_string(loaded.status),
                     loaded.detail.empty() ? "" : " ", loaded.detail.c_str());
        for (const host::CatalogEntry& e : host::boottarget::sorted_all(catalog)) {
          if (attempts >= kBootLoadAttempts) break;
          if (e.path == live_set_path) continue;
          ++attempts;
          loaded = loader->load_file(e.path);
          if (loaded.ok()) {
            live_set_path = e.path;
            boot_target.setlist_id.clear();  // the setlist and scene belonged to the target that did not load
            boot_target.scene = 0;
            boot_target.live_set_id = e.id;
            std::printf("boot target: fell back to live set %s scene 1 NO SETLIST\n", e.id.c_str());
            break;
          }
          std::fprintf(stderr, "engine-host: live set %s: %s%s%s\n", e.path.c_str(), host::to_string(loaded.status), loaded.detail.empty() ? "" : " ", loaded.detail.c_str());
        }
        std::string np;
        if (!loaded.ok() && make_empty_live_set(np)) {
          loaded = loader->load_file(np);
          if (loaded.ok()) {
            live_set_path = np;
            boot_target.setlist_id.clear();
            boot_target.scene = 0;
            boot_target.live_set_id = catalog.back().id;
            std::printf("boot target: no Live Set loaded; started on the new empty Live Set %s scene 1 NO SETLIST\n", boot_target.live_set_id.c_str());
          }
        }
        if (!loaded.ok()) {
          std::string cerr3;
          ws::v1::State initial;
          if (host::load_dev_config(config_path, initial, cerr3) && plane.load_initial(initial, cerr3) && reserve_dev_config()) {
            bootstrap = true;  // the dev-configuration bootstrap: the first jump loads a Live Set
            std::printf("boot target: no Live Set could be loaded: running on the dev configuration\n");
          }
        }
      }
      if (!bootstrap) {
        if (!loaded.ok()) {
          std::fprintf(stderr, "engine-host: live set %s: %s%s%s\n", live_set_path.c_str(), host::to_string(loaded.status),
                       loaded.detail.empty() ? "" : " ", loaded.detail.c_str());
          return 1;
        }
      for (std::size_t p = 0; p < loaded.parts.size(); ++p)
        std::printf("live set part %zu: %s\n", p, host::to_string(loaded.parts[p].state));
      std::printf("live set: %s -> generation %llu, %zu bytes of samples resident\n", live_set_path.c_str(),
                  (unsigned long long)loaded.revision, loaded.resident_bytes);
      }
    }
    {
      // After audio starts the voice pool is switched without stopping: the reservation is applied by the RT thread with the new generation.
      host::SwitcherOptions sopt;
      sopt.head_budget_bytes = profile.head_budget_bytes;  // Story 4.1: the tier's head budget (the Mac default of 64 GB is for tests)
      switcher = std::make_unique<host::LiveSetSwitcher>(*loader, plane, sopt);
      switcher->set_catalog(catalog);
    }
    if (!data_dir.empty()) {
      host::liveset::StoreOptions lso;
      lso.dir = live_set_dir;
      lso.part_count = profile.part_count;
      host::setlist::StoreOptions sso;
      sso.dir = data_dir + "/setlists";
      std::size_t removed = 0;
      const host::liveset::Status ls_st = host::liveset::LiveSetStore::open(lso, live_store);
      if (ls_st != host::liveset::Status::kOk) {
        std::fprintf(stderr, "engine-host: live set store %s: cannot open: %s (another engine-host running on it?)\n", live_set_dir.c_str(),
                     host::liveset::to_string(ls_st));
        return 1;
      }
      if (const host::liveset::Status rc = live_store->reconcile(removed); rc != host::liveset::Status::kOk)
        std::fprintf(stderr, "engine-host: warning: live set store %s: leftovers could not all be removed: %s\n", live_set_dir.c_str(), host::liveset::to_string(rc));
      const host::setlist::Status ss_st = host::setlist::SetlistStore::open(sso, setlist_store);
      if (ss_st != host::setlist::Status::kOk) {
        std::fprintf(stderr, "engine-host: setlists %s: cannot open: %s (the file is left untouched)\n", sso.dir.c_str(), host::setlist::to_string(ss_st));
        return 1;
      }
      if (const host::setlist::Status rc = setlist_store->reconcile(removed); rc != host::setlist::Status::kOk)
        std::fprintf(stderr, "engine-host: warning: setlists %s: leftovers could not all be removed: %s\n", sso.dir.c_str(), host::setlist::to_string(rc));
      switch_port = std::make_unique<host::LiveSetSwitcherPort>(*switcher);
      setlists = std::make_unique<host::SetlistController>(plane, *setlist_store, *live_store, *switch_port);
      // Story 4.9: edits stay in memory until a Write command. The fence a Write raises is acknowledged by the render thread; without audio (--no-audio,
      // dev/test only) nobody renders, so the wait itself renders one block per pass.
      host::EditSessionOptions eo;
      eo.part_count = profile.part_count;
      host::LiveSetSwitcher* swp = switcher.get();
      eo.load_in_progress = [swp] { return swp->loading(); };
      eo.reload_current = [swp] { return swp->reload_current(); };
      eo.active_job_token = [swp] { return swp->active_token(); };
      if (no_audio) {
        host::RenderDriver* drv = &driver;
        eo.fence_idle = [drv] {
          float l[core::kBlockFrames], r[core::kBlockFrames];
          drv->render_block(l, r, host::macos::now_ns());
        };
        eo.pump = eo.fence_idle;  // a rebuilt generation is applied by the next block; nobody else renders without audio
      }
      edits = std::make_unique<host::EditSession>(plane, *scenes, *live_store, setlist_store.get(), setlists.get(), switch_port.get(), eo);
      edits->attach(*loader);
      edits->attach(*switcher);
      if (boot_resolved && !bootstrap) {
        // Story 8.9: the boot target's setlist and scene, applied before the control server serves anything. NO SETLIST is the state a Live Set loaded at start
        // leaves (nothing to do); a scene that does not exist keeps the Live Set as loaded.
        if (!boot_target.setlist_id.empty()) {
          const host::setlist::Status ls_rc = setlists->load_setlist(boot_target.setlist_id);
          if (ls_rc != host::setlist::Status::kOk) std::printf("boot target: setlist %s not loaded (%s): NO SETLIST\n", boot_target.setlist_id.c_str(), host::setlist::to_string(ls_rc));
        }
        if (boot_target.scene < scenes->scene_count()) {
          const host::SceneOutcome so = scenes->recall_scene(boot_target.scene);
          if (!so.ok()) std::printf("boot target: scene %u not recalled (%s)\n", static_cast<unsigned>(boot_target.scene) + 1, host::to_string(so.result));
        } else if (boot_target.scene > 0) {
          std::printf("boot target: scene %u does not exist (the Live Set has %u): scene 1\n", static_cast<unsigned>(boot_target.scene) + 1, static_cast<unsigned>(scenes->scene_count()));
          if (scenes->scene_count() > 0) scenes->recall_scene(0);
        } else if (scenes->scene_count() > 0) {
          scenes->recall_scene(0);
        }
      }
      std::printf("data dir %s: %d setlist(s), %d tag(s)\n", data_dir.c_str(), setlist_store->library().setlists_size(), setlist_store->library().tags_size());
    }
  } else {
    // FX budget (Story 2.7): reserved for the effects FIRST, before voices; the control plane refuses a command that would exceed it.
    // The reservation is the flat worst case (the plane's budget) rather than fill_fx_costs() of the initial snapshot: the arbiter
    // reservation is static for the run while commands may raise the FX cost up to that budget, so a smaller reservation would
    // let voices occupy budget the control plane still promises to the effects.
    core::VoiceArbiter::LiveSetConfig reservation;
    reservation.budget = profile.arbiter_budget;
    reservation.fx_cost = plane.fx_budget();
    const auto st = renderer->arbiter().reserve(reservation);
    if (st != core::VoiceArbiter::ReserveStatus::kOk) {
      std::fprintf(stderr, "engine-host: cannot reserve the FX budget (status %d)\n", static_cast<int>(st));
      return 1;
    }
  }

  host::macos::CoreAudioOutput audio;
  host::macos::CoreAudioLineIn line_in;
  // On every exit path (including early returns) audio stops and the renderer lets go of the line-in before it is destroyed.
  struct LineInGuard {
    core::BlockRenderer& renderer;
    host::macos::CoreAudioOutput& audio;
    ~LineInGuard() {
      audio.stop();
      renderer.set_line_in(nullptr);
    }
  } line_in_guard{*renderer, audio};
  host::macos::CoreMidiInput midi;
  std::string err;
  midi.set_din_source(din_source);
  // Story 3.10: the Mac virtual panel. A MIDI control surface's mapped CCs / notes / pitch bend become panel MCU frames fed to the panel reader (the same
  // reader the panel MCU's UART feeds on the SoC); the reader's UMP joins the MIDI lane (the CoreMIDI thread stays its only producer) and BUTTON frames are
  // pushed to subscribed ui clients over ws.v1. Everything the map does not name goes on to the ingest unchanged.
  host::KeyboardOnParts keyboard_parts;
  std::unique_ptr<host::VirtualPanelLeds> panel_leds;  // before the reader: the reader's TX hook points at it
  std::unique_ptr<host::PanelReader> panel_reader;
  std::unique_ptr<host::VirtualPanel> virtual_panel;
  std::unique_ptr<host::VirtualPanelUmpSink> panel_sink;
  std::atomic<host::ControlServer*> panel_push{nullptr};  // set once the control server exists; read on the MIDI thread
  if (!virtual_panel_path.empty()) {
    host::VirtualPanelMap vmap;
    std::string verr;
    if (!host::VirtualPanelMap::load_file(virtual_panel_path, vmap, verr)) {
      std::fprintf(stderr, "engine-host: virtual panel: %s\n", verr.c_str());
      return 2;
    }
    panel_reader = std::make_unique<host::PanelReader>(*queue, keyboard_parts);
    panel_reader->set_button_sink([&panel_push](const host::PanelButtonEvent& ev) {
      if (host::ControlServer* c = panel_push.load(std::memory_order_acquire)) c->push_panel_event(ev.id, ev.pressed, ev.device_ns);
    });
    host::PanelReader* pr = panel_reader.get();
    virtual_panel = std::make_unique<host::VirtualPanel>(
        std::move(vmap), [] { return host::macos::now_ns(); },
        [pr](const std::uint8_t* data, std::size_t len, std::uint64_t rx_ns) { pr->on_bytes(data, len, rx_ns); });
    // Story 3.12: what the reader writes to the panel link (LED_SET frames, time sync requests) is received by the simulated MCU's LED model.
    panel_leds = std::make_unique<host::VirtualPanelLeds>();
    host::VirtualPanelLeds* pl = panel_leds.get();
    panel_reader->set_tx([pl](const std::uint8_t* data, std::size_t len) { pl->on_bytes(data, len); });
    virtual_panel->start();
    panel_sink = std::make_unique<host::VirtualPanelUmpSink>(*virtual_panel, ingest);
    // Story 3.11: the reader's housekeeping (time sync, pedal values held back by the throttle) runs with every UMP batch, on the thread that feeds on_bytes().
    // (Only packets from the control surface wake it: a trailing pedal value of a burst is sent by the next packet or frame; a dedicated timer on that thread is a follow-up.)
    panel_sink->set_tick([pr](std::uint64_t ns) { pr->poll(ns); });
    std::printf("virtual panel: %s\n", virtual_panel_path.c_str());
  }
  ports::UmpSink& midi_sink = panel_sink ? static_cast<ports::UmpSink&>(*panel_sink) : static_cast<ports::UmpSink&>(ingest);
  if (no_audio) {
    std::printf("audio and midi: off (--no-audio, dev/test only)\n");
  } else {
    if (!audio.open(core::kSampleRate, core::kBlockFrames, driver, err)) {
      std::fprintf(stderr, "engine-host: audio: %s\n", err.c_str());
      return 1;
    }
    const auto info = audio.info();
    std::printf("audio: %u Hz, device buffer %u frames (requested %u)%s\n", info.sample_rate,
                info.buffer_frames, core::kBlockFrames,
                info.buffer_frames == core::kBlockFrames ? "" : " [closest the device granted]");
    if (!midi.open(midi_sink, err)) {
      std::fprintf(stderr, "engine-host: midi: %s\n", err.c_str());
      return 1;
    }
    std::printf("midi: %s UMP input, %zu source(s) connected%s\n",
                midi.protocol() == kMIDIProtocol_2_0 ? "MIDI 2.0" : "MIDI 1.0 (up-converted)",
                midi.connected_sources(),
                midi.connected_sources() ? "" : " (none yet; new sources are picked up automatically)");
  }
  // Story 4.11 (optional, default off): the thin Library API client. Reconnects with backoff when the librarian service is absent.
  std::unique_ptr<host::LibraryClient> library_client;
  if (!library_socket.empty()) {
    host::LibraryClientOptions lo;
    lo.socket_path = library_socket;
    library_client = std::make_unique<host::LibraryClient>(lo);
    library_client->start();
    std::printf("library: Library API client on %s (reconnects when the librarian service is down)\n", library_socket.c_str());
  }
  // Foundation: the sysd config client. Its worker thread owns all I/O (reconnect with backoff while sysd is absent); the callback runs on the control
  // thread, from the control tick, once per revision. The stories that give a setting its behaviour (clock source 7.7, velocity curve 3.6, pedals 3.11,
  // Boot into 8.9) consume the Config here; this wiring only logs it.
  // Story 3.6: the velocity curve holder. The keybed link is not wired into engine-host yet; when it is, it creates its KeybedReader and calls
  // velocity_curves.attach(reader) (the reader then takes the current curve and every later change from sysd, for the next note, without a restart).
  host::VelocityCurveHolder velocity_curves;
  std::unique_ptr<host::SysdConfigClient> sysd_client;
  if (!sysd_socket.empty()) {
    host::SysdConfigClientOptions so;
    so.socket_path = sysd_socket;
    sysd_client = std::make_unique<host::SysdConfigClient>(so);
    sysd_client->set_callback([ing = &ingest, rend = renderer.get(), vc = &velocity_curves, pr = panel_reader.get(), sw = switch_direction, first = true](const ws::v1::Config& c, std::uint64_t revision) mutable {
      // Story 3.11: the pedal polarity and calibration reach the panel reader's pedal processor, applied from the next pedal frame (no restart).
      if (pr) host::apply_pedal_config(c, pr->pedal_settings(), sw);
      // Story 7.7: the clock source takes effect at the next block, without touching a voice. An unknown value keeps the last applied one.
      const core::ClockSource before = ing->clock_source();
      if (host::apply_clock_source(c, *ing, *rend)) {
        if (first || ing->clock_source() != before)
          std::printf("clock source: %s applied (follower and port filter)\n", ws::v1::ClockSource_Name(c.clock_source()).c_str());
        first = false;
      } else
        std::printf("sysd config: clock_source %d not applied (kept %d)\n", static_cast<int>(c.clock_source()), static_cast<int>(ing->clock_source()));
      // Story 3.6: the velocity curve applies to the next Note On of every attached keybed reader. An unknown value keeps the last curve.
      if (!host::apply_velocity_curve(c, *vc)) std::printf("sysd config: velocity_curve %d not applied\n", static_cast<int>(c.velocity_curve()));
      std::printf("sysd config: revision %llu clock_source=%s velocity_curve=%s boot_into=%s\n", (unsigned long long)revision,
                  ws::v1::ClockSource_Name(c.clock_source()).c_str(), ws::v1::VelocityCurve_Name(c.velocity_curve()).c_str(),
                  ws::v1::BootIntoMode_Name(c.boot_into().mode()).c_str());
      std::fflush(stdout);
    });
    sysd_client->start();
    std::printf("sysd: config client on %s (defaults until sysd answers; reconnects when sysd is down)\n", sysd_socket.c_str());
  }
  // Story 3.12: a ws.v1 SetLed goes to the panel reader, the only writer of the panel link (control thread -> reader's thread-safe set_led).
  if (panel_reader) plane.set_led_sink([pr = panel_reader.get()](std::uint8_t id, std::uint8_t b, std::uint8_t a) { pr->set_led(id, b, a, host::macos::now_ns()); });
  // Story 3.11: pedal calibration commands (ws.v1 StartPedalCalibration / FinishPedalCalibration / CancelPedalCalibration): the raw measurement is the panel
  // reader's, the result is written to sysd. Without a panel reader the commands answer a typed error.
  std::unique_ptr<host::PedalCalibrator> pedal_calibrator;
  if (panel_reader) {
    host::PedalCalibrator::Writer writer;
    if (sysd_client) writer = [c = sysd_client.get()](const ws::v1::ConfigPatch& p) { return c->set_config(p, 400); };  // the control thread waits at most ~400 ms for sysd (an async Finish with a WRITING result is the proper follow-up)
    pedal_calibrator = std::make_unique<host::PedalCalibrator>(panel_reader->pedal_probe(), std::move(writer), [] { return host::macos::now_ns(); });
    plane.set_pedal_handler(pedal_calibrator.get());
    // The pedals start uncalibrated and in the raw direction (the footswitch with its --switch-polarity wiring); sysd's config replaces this when it arrives.
    host::apply_pedal_config(host::default_config(), panel_reader->pedal_settings(), switch_direction);
  }
  host::ControlServer control(plane, socket_path);
  panel_push.store(&control, std::memory_order_release);
  // Teardown order: `control` is destroyed before `midi` (declared earlier), and the CoreMIDI thread must never reach a destroyed server. On every exit
  // path (returns included) this guard stops MIDI first and detaches the panel push.
  struct PanelPushGuard {
    host::macos::CoreMidiInput& midi;
    std::atomic<host::ControlServer*>& push;
    ~PanelPushGuard() {
      midi.close();
      push.store(nullptr, std::memory_order_release);
    }
  } panel_push_guard{midi, panel_push};
  // The keyboard-on parts follow the published generation (control thread, every tick): the panel reader sends the wheels to those parts.
  std::function<bool()> panel_tick;
  if (panel_reader) {
    panel_tick = [pl = &plane, kp = &keyboard_parts] {
      if (const core::Snapshot* s = pl->current_snapshot()) kp->update_from(*s);
      return false;
    };
  }
  if (switcher) {
    host::LiveSetSwitcher* sw = switcher.get();
    host::SetlistController* sc = setlists.get();
    host::EditSession* ed = edits.get();
    // The control tick: loading status at least once per second, commit of a finished load, the setlist position following jumps, and the edit session
    // (live values captured into the edits, the dirty flags).
    // Story 4.11: the Live Set that sounds is reported to the librarian (Recent) when it changes; the client never blocks this thread.
    if (!data_dir.empty()) {
      host::bootinto::StoreOptions bo;
      bo.dir = data_dir;
      boot_store = std::make_unique<host::bootinto::BootIntoStore>(std::move(bo));
      host::bootinto::TrackerProbes bp;
      bp.now_ns = [] { return host::macos::now_ns(); };
      bp.held_notes = [pl = &plane] { return pl->held_notes(); };
      bp.pedal_down = [pl = &plane] { return pl->pedal_down(); };
      // No import runs inside the engine process (imports verify in the librarian), so there is no verify phase to wait for here; a deployment that
      // runs imports in-process sets this probe (Story 8.8: never a write while an import verifies).
      bp.import_verifying = [] { return false; };
      boot_tracker = std::make_unique<host::bootinto::BootIntoTracker>(*boot_store, std::move(bp));
      if (boot_resolved && !bootstrap && scenes && setlists) {
        // Story 8.9: the tracker starts AFTER the boot target is resolved and applied: the position the unit booted on is its baseline, so the first observation
        // cannot overwrite the stored pointer with it (only a real user change makes the pointer dirty).
        host::bootinto::Pointer base;
        base.live_set_id = switcher->sounding_id();
        base.scene_index = static_cast<std::uint32_t>(std::max(0, scenes->current_scene()));
        base.setlist_id = setlists->loaded_setlist_id();
        boot_tracker->seed(base);
      }
    }
    control.set_tick([sw, sc, ed, pl = &plane, lc = library_client.get(), last = std::string(), bt = boot_tracker.get(), scn = scenes.get(), pt = &panel_tick, sy = sysd_client.get()]() mutable {
      if (*pt) (*pt)();
      if (sy) sy->tick();  // the sysd config callback, once per revision (non-blocking)
      if (lc && !sw->sounding_id().empty() && sw->sounding_id() != last) {
        last = sw->sounding_id();
        lc->report_active(last);
      }
      if (bt && scn) {  // Story 8.8: the position the player is on; dirty in memory only, written at idle-safe moments (binds the store to this thread on first call)
        host::bootinto::BootIntoTracker::Position pos;
        pos.live_set_id = sw->sounding_id();
        pos.scene = std::max(0, scn->current_scene());  // Story 8.9: a Live Set no scene was recalled on yet is on scene 1 (the base), so the pointer follows every Live Set change
        pos.loading = sw->loading();
        if (sc) pos.setlist_id = sc->loaded_setlist_id();
        bt->update(pos);
      }
      const bool a = sw->tick();
      const bool b = sc ? sc->tick() : false;
      const bool c = ed ? ed->tick() : false;
      const bool d = pl->take_content_changed();  // Story 6.5: an I/O edge thread reported a damaged block of a protected library: push State.content
      return a || b || c || d;
    });
  } else if (panel_tick || sysd_client) {
    control.set_tick([pt = panel_tick, sy = sysd_client.get()]() mutable {
      const bool r = pt ? pt() : false;
      if (sy) sy->tick();
      return r;
    });
  }
  if (setlists) {
    // The server thread is the control thread: the single-writer stores (opened on this thread) are handed over to it before it serves a command.
    host::liveset::LiveSetStore* lst = live_store.get();
    host::setlist::SetlistStore* sst = setlist_store.get();
    control.set_thread_start([lst, sst] {
      lst->rebind_control_thread();
      sst->rebind_control_thread();
    });
  }
  if (!control.start(err)) {
    std::fprintf(stderr, "engine-host: control: %s\n", err.c_str());
    return 1;
  }
  std::printf("control: ws.v1 on %s, generation %llu from %s\n", socket_path.c_str(),
              (unsigned long long)plane.revision(), live_set_path.empty() ? config_path.c_str() : live_set_path.c_str());
  // Optional line-in (mixed into the master only, at the line_in_level parameter): if there is no input device or no permission
  // the engine still starts and the line-in stays silent.
  if (want_line_in && !no_audio) {
    std::string lerr;
    if (line_in.open(core::kSampleRate, lerr) && line_in.start(lerr)) {
      renderer->set_line_in(&line_in);
      std::printf("line-in: default input device captured (mixed at line_in_level, default 0)\n");
    } else {
      std::printf("line-in: unavailable (%s); continuing with a silent line-in\n", lerr.c_str());
    }
  }
  audio_running.store(true);  // after this the voice pool is no longer ours to reserve
  if (!no_audio && !audio.start(err)) {
    std::fprintf(stderr, "engine-host: audio: %s\n", err.c_str());
    return 1;
  }
  std::printf("running; play notes on a MIDI source. Press q + Enter or Ctrl-C to quit.\n");

#ifdef VELORA_ENGINE_DEV_KEYS
  // Story 6.5: the I/O edge thread of the protected libraries. It re-reads, hash-checks and decrypts the blocks of every resident protected instrument against the
  // signed index (EdgeBlockReader::scrub) and hands any block that fails to the instrument's damage map (the voices fade to silence for it) and to the damage
  // registry (State.content). The audio thread never takes part in it.
  std::atomic<bool> scrub_stop{false};
  std::thread scrub_thread;
  if (protected_heads && scrub_interval_ms > 0) {
    scrub_thread = std::thread([&] {
      while (!scrub_stop.load(std::memory_order_acquire)) {
        for (long waited = 0; waited < scrub_interval_ms && !scrub_stop.load(std::memory_order_acquire); waited += 20) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (const auto& inst : protected_heads->live_weak()) {  // weak: a scrub never keeps a generation (and its key) alive
          if (scrub_stop.load(std::memory_order_acquire)) break;
          host::EdgeBlockReader edge(inst);
          const host::ScrubResult r = edge.scrub(&scrub_stop);
          if (r.damaged != 0) std::fprintf(stderr, "engine-host: protected library: %llu damaged block(s) found; their voices are silent\n", (unsigned long long)r.damaged);
        }
      }
    });
  }
#endif
  const auto t_start = std::chrono::steady_clock::now();
  auto last_scan = t_start;
  std::uint64_t last_notes = 0;
  auto last_note_at = t_start;
  bool performing = false, injected = false;
  bool stdin_open = true;
  while (!g_quit.load()) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);
    if (!stdin_open) {
      usleep(100 * 1000);  // stdin is gone: just pace the loop
    } else {
      pollfd pfd{STDIN_FILENO, POLLIN, 0};
      if (poll(&pfd, 1, 100) > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
      char c;
      const ssize_t n = read(STDIN_FILENO, &c, 1);
      if (n == 0) {
        stdin_open = false;
        if (duration_s <= 0) break;  // stdin closed and no timed run: nothing more to wait for
      } else if (n > 0 && (c == 'q' || c == 'Q')) {
        break;
      }
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (!no_audio && now - last_scan > std::chrono::milliseconds(500)) {
      last_scan = now;
      const std::size_t added = midi.rescan();
      if (added) std::printf("midi: %zu new source(s) connected\n", added);
    }
    if (library_client) {
      // "Performing" = a note arrived within the last library_idle_ms; the librarian writes Recent only while this is false.
      const std::uint64_t notes = ingest.counters().notes.load();
      bool note = notes != last_notes;
      last_notes = notes;
      if (inject_note_after_ms >= 0 && !injected && now - t_start >= std::chrono::milliseconds(inject_note_after_ms)) {
        injected = note = true;
      }
      if (note) {
        last_note_at = now;
        if (!performing) {
          performing = true;
          library_client->set_playing(true);
        }
      } else if (performing && now - last_note_at > std::chrono::milliseconds(library_idle_ms)) {
        performing = false;
        library_client->set_playing(false);
      }
    }
    if (duration_s > 0 && std::chrono::duration<double>(now - t_start).count() >= duration_s) break;
  }

#ifdef VELORA_ENGINE_DEV_KEYS
  scrub_stop.store(true, std::memory_order_release);
  if (scrub_thread.joinable()) scrub_thread.join();
#endif
  if (library_client) {
    if (performing) library_client->set_playing(false);  // the engine is leaving: Recent may be written now
    library_client->stop();                              // bounded attempt to deliver it
  }
  audio.stop();
  renderer->set_line_in(nullptr);
  line_in.stop();
  midi.close();
  panel_push.store(nullptr, std::memory_order_release);
  control.stop();
  edits.reset();     // before the setlist controller, the scene controller and the plane it registered with
  setlists.reset();  // before the switcher and the plane it registered with
  switch_port.reset();
  switcher.reset();  // cancels a load in progress and joins its worker
  const auto& rc = driver.counters();
  const auto& mc = ingest.counters();
  std::printf("-- timing/stats (host, not representative) --\n");
  std::printf("blocks=%llu callbacks=%llu events=%llu late=%llu deferred=%llu beyond_horizon=%llu\n",
              (unsigned long long)rc.blocks.load(), (unsigned long long)audio.callbacks(),
              (unsigned long long)rc.events.load(), (unsigned long long)rc.late.load(),
              (unsigned long long)rc.deferred.load(), (unsigned long long)rc.beyond_horizon.load());
  std::printf("midi notes=%llu ignored=%llu queue_full=%llu renderer_rejected=%llu\n",
              (unsigned long long)mc.notes.load(), (unsigned long long)mc.ignored.load(),
              (unsigned long long)mc.queue_full.load(), (unsigned long long)rc.renderer_rejected.load());
  std::printf("scheduling delay %.1f ms fixed (host, not representative)\n",
              driver.mapper().config().delay_ns / 1e6);
  if (!no_audio && duration_s > 0 && audio.callbacks() == 0) {
    std::fprintf(stderr, "engine-host: audio path never ran (0 callbacks)\n");
    return 1;
  }
  return 0;
}
