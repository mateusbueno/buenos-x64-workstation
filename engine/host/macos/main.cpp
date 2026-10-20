// engine-host: macOS developer app. Plays the engine test tone from CoreMIDI notes through
// CoreAudio. Timing figures are for the developer's Mac only: "host, not representative".
#include <poll.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "velora/core/organ_engine.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/control_server.hpp"
#include "velora/host/edit_session.hpp"
#include "velora/host/dev_config.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/library_client.hpp"
#include "velora/host/live_set_switcher.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/render_driver.hpp"
#include "velora/host/scene_controller.hpp"
#include "velora/host/setlist_controller.hpp"
#include "velora/host/setlist_store.hpp"

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
  std::string live_set_path, library_root, live_set_dir, data_dir, library_socket;
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
    } else if (!std::strcmp(argv[i], "--library-socket") && i + 1 < argc) {
      library_socket = argv[++i];  // Story 4.11: tell the librarian service which Live Set became active and when the engine is performing
    } else {
      std::fprintf(stderr,
                   "usage: engine-host [--duration SECONDS] [--socket PATH] [--config engine-dev.json] [--line-in] [--no-audio (dev/test only)]\n"
                   "                   [--live-set FILE --library-root DIR]   (load a Live Set at start; its parts play from the keyboard by zone)\n"
                   "                   [--live-set-dir DIR --library-root DIR]   (the Live Sets of a directory (<id>.liveset) that ws.v1 JumpToLiveSet can switch to,\n"
                   "                                                              seamlessly, loading in the background; without --live-set the first by name loads at start)\n"
                   "                   [--data-dir DIR --library-root DIR]   (the persisted data root: DIR/live_sets/ holds the Live Sets, DIR/setlists/ the setlists, tags and the\n"
                   "                                                          default ASSIGN map; ws.v1 setlist commands and LIVE SET stepping; replaces --live-set-dir)\n"
                   "                   [--library-socket PATH [--library-playing-idle-ms N]]   (report the active Live Set and the performing state to the librarian service's Library API;\n"
                   "                                                                           performing = a note within the last N ms, default 3000; default off)\n");
      return std::strcmp(argv[i], "--help") ? 2 : 0;
    }
  }
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
  if ((live_set_path.empty() && live_set_dir.empty()) != library_root.empty()) {
    std::fprintf(stderr, "engine-host: --live-set / --live-set-dir and --library-root go together\n");
    return 2;
  }
  // Story 4.5: the Live Sets that can be switched to. Listing the directory reads each file; invalid ones are reported and left out.
  std::vector<host::CatalogEntry> catalog;
  if (!live_set_dir.empty()) {
    std::vector<std::string> problems;
    catalog = host::scan_live_set_dir(live_set_dir, host::liveset::kV1PartCount, &problems);
    for (const std::string& pr : problems) std::fprintf(stderr, "engine-host: live set directory: skipped %s\n", pr.c_str());
    std::printf("live set directory %s: %zu Live Set(s)\n", live_set_dir.c_str(), catalog.size());
    if (!live_set_path.empty()) {  // the Live Set we start with is always one a jump can come back to
      host::CatalogEntry e;
      std::string cerr2;
      if (host::catalog_entry_for_file(live_set_path, host::liveset::kV1PartCount, e, cerr2)) {
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
    } else if (live_set_path.empty() && !catalog.empty()) {
      live_set_path = catalog.front().path;  // --data-dir: the first by name
    }
  }
  // A fresh data root (no Live Set yet): the engine starts on the dev configuration with the switcher, the stores and the setlist controller, so that
  // CreateLiveSet and JumpToLiveSet can bootstrap an empty install.
  const bool bootstrap = live_set_path.empty() && !data_dir.empty();
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  auto renderer = std::make_unique<core::BlockRenderer>();
  auto queue = std::make_unique<host::HostEventQueue>();
  auto control_queue = std::make_unique<host::ControlEventQueue>();
  host::RenderDriver driver(*renderer, *queue, {}, control_queue.get());
  host::MidiIngest ingest(*queue);

  // Control plane: the first snapshot generation comes from the committed dev configuration and is
  // published before audio runs; an invalid file stops the engine here with a clear error.
  host::ControlPlane plane(renderer->snapshots(), renderer->last_values(), *control_queue,
                           [] { return host::macos::now_ns(); });
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
  sample_cfg.max_voices = 340;  // the 256 voice floor plus the 64 voice fade headroom, with slack
  sample_cfg.extended_voices = true;  // Story 5.2: any imported instrument may use filters and CC crossfades, so every voice is budgeted for them
  core::SampleEngine sample(sample_cfg);
  core::OrganEngine organ;
  host::LibraryResolver resolver(host::make_posix_fs(), library_root.empty() ? std::string(host::kDefaultLibraryRoot) : library_root);
  host::RamHeadProvider heads;
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
    if (!renderer->bind_engine(sample) || !renderer->bind_engine(organ)) {
      std::fprintf(stderr, "engine-host: cannot bind the sample engine and the organ\n");
      return 1;
    }
    // Story 4.3: the Live Set is the first generation and reserves the voice pool for the whole Live Set (FX, organ,
    // sample floor) before audio runs. A Live Set that cannot be loaded stops the engine here with the reason; nothing is half loaded.
    host::LiveSetLoaderOptions lopt;
    lopt.is_rendering = [&audio_running] { return audio_running.load(); };
    scenes = std::make_unique<host::SceneController>(plane);
    lopt.scenes = scenes.get();
    loader = std::make_unique<host::LiveSetLoader>(*renderer, plane, resolver, heads, &sample, lopt);
    if (bootstrap) {
      // No Live Set to load: the flat FX reservation of the dev configuration (as without --live-set); the first jump replaces it seamlessly.
      core::VoiceArbiter::LiveSetConfig reservation;
      reservation.budget = 1u << 20;
      reservation.fx_cost = plane.fx_budget();
      reservation.waive_sample_floor = true;  // the engines are bound for the first jump, but nothing plays on them yet
      reservation.waive_organ_reservation = true;
      const auto st = renderer->arbiter().reserve(reservation);
      if (st != core::VoiceArbiter::ReserveStatus::kOk) {
        std::fprintf(stderr, "engine-host: cannot reserve the FX budget (status %d)\n", static_cast<int>(st));
        return 1;
      }
      std::printf("data dir %s holds no Live Set yet: running on the dev configuration\n", data_dir.c_str());
    } else {
      const host::LiveSetLoadResult loaded = loader->load_file(live_set_path);
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
    {
      // After audio starts the voice pool is switched without stopping: the reservation is applied by the RT thread with the new generation.
      host::SwitcherOptions sopt;
      switcher = std::make_unique<host::LiveSetSwitcher>(*loader, plane, sopt);
      switcher->set_catalog(catalog);
    }
    if (!data_dir.empty()) {
      host::liveset::StoreOptions lso;
      lso.dir = live_set_dir;
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
      std::printf("data dir %s: %d setlist(s), %d tag(s)\n", data_dir.c_str(), setlist_store->library().setlists_size(), setlist_store->library().tags_size());
    }
  } else {
    // FX budget (Story 2.7): reserved for the effects FIRST, before voices; the control plane refuses a command that would exceed it.
    // The reservation is the flat worst case (the plane's budget) rather than fill_fx_costs() of the initial snapshot: the arbiter
    // reservation is static for the run while commands may raise the FX cost up to that budget, so a smaller reservation would
    // let voices occupy budget the control plane still promises to the effects.
    core::VoiceArbiter::LiveSetConfig reservation;
    reservation.budget = 1u << 20;
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
    if (!midi.open(ingest, err)) {
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
  host::ControlServer control(plane, socket_path);
  if (switcher) {
    host::LiveSetSwitcher* sw = switcher.get();
    host::SetlistController* sc = setlists.get();
    host::EditSession* ed = edits.get();
    // The control tick: loading status at least once per second, commit of a finished load, the setlist position following jumps, and the edit session
    // (live values captured into the edits, the dirty flags).
    // Story 4.11: the Live Set that sounds is reported to the librarian (Recent) when it changes; the client never blocks this thread.
    control.set_tick([sw, sc, ed, lc = library_client.get(), last = std::string()]() mutable {
      if (lc && !sw->sounding_id().empty() && sw->sounding_id() != last) {
        last = sw->sounding_id();
        lc->report_active(last);
      }
      const bool a = sw->tick();
      const bool b = sc ? sc->tick() : false;
      const bool c = ed ? ed->tick() : false;
      return a || b || c;
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

  if (library_client) {
    if (performing) library_client->set_playing(false);  // the engine is leaving: Recent may be written now
    library_client->stop();                              // bounded attempt to deliver it
  }
  audio.stop();
  renderer->set_line_in(nullptr);
  line_in.stop();
  midi.close();
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
