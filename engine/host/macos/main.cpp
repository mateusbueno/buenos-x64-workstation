// engine-host: macOS developer app. Plays the engine test tone from CoreMIDI notes through
// CoreAudio. Timing figures are for the developer's Mac only: "host, not representative".
#include <poll.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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
#include "velora/host/dev_config.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/live_set_switcher.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/render_driver.hpp"
#include "velora/host/scene_controller.hpp"

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
  std::string live_set_path, library_root, live_set_dir;
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
    } else if (!std::strcmp(argv[i], "--library-root") && i + 1 < argc) {
      library_root = argv[++i];
    } else {
      std::fprintf(stderr,
                   "usage: engine-host [--duration SECONDS] [--socket PATH] [--config engine-dev.json] [--line-in] [--no-audio (dev/test only)]\n"
                   "                   [--live-set FILE --library-root DIR]   (load a Live Set at start; its parts play from the keyboard by zone)\n"
                   "                   [--live-set-dir DIR --library-root DIR]   (the Live Sets of a directory (<id>.liveset) that ws.v1 JumpToLiveSet can switch to,\n"
                   "                                                              seamlessly, loading in the background; without --live-set the first by name loads at start)\n");
      return std::strcmp(argv[i], "--help") ? 2 : 0;
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
    if (live_set_path.empty()) {
      if (catalog.empty()) {
        std::fprintf(stderr, "engine-host: %s holds no Live Set to start with\n", live_set_dir.c_str());
        return 1;
      }
      live_set_path = catalog.front().path;  // the first by name
    }
  }
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
  core::SampleEngine sample(sample_cfg);
  core::OrganEngine organ;
  host::LibraryResolver resolver(host::make_posix_fs(), library_root.empty() ? std::string(host::kDefaultLibraryRoot) : library_root);
  host::RamHeadProvider heads;
  std::unique_ptr<host::SceneController> scenes;  // Story 4.4: ws.v1 RecallScene / NextScene / PreviousScene; destroyed before the plane
  std::unique_ptr<host::LiveSetLoader> loader;     // kept for the run: the switcher commits through it
  std::unique_ptr<host::LiveSetSwitcher> switcher; // Story 4.5: ws.v1 JumpToLiveSet / CancelLoad; destroyed before the loader and the plane
  std::atomic<bool> audio_running{false};  // set once the audio stream starts; the loader refuses to run while it is true
  if (!live_set_path.empty()) {
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
    {
      // After audio starts the voice pool is switched without stopping: the reservation is applied by the RT thread with the new generation.
      host::SwitcherOptions sopt;
      switcher = std::make_unique<host::LiveSetSwitcher>(*loader, plane, sopt);
      switcher->set_catalog(catalog);
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
  host::ControlServer control(plane, socket_path);
  if (switcher) {
    host::LiveSetSwitcher* sw = switcher.get();
    control.set_tick([sw] { return sw->tick(); });  // the control tick: loading status at least once per second, commit of a finished load
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
    if (duration_s > 0 && std::chrono::duration<double>(now - t_start).count() >= duration_s) break;
  }

  audio.stop();
  renderer->set_line_in(nullptr);
  line_in.stop();
  midi.close();
  control.stop();
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
