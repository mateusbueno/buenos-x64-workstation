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

#include "coreaudio_output.hpp"
#include "coremidi_input.hpp"
#include "mach_time.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/control_server.hpp"
#include "velora/host/dev_config.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/render_driver.hpp"

namespace {
std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit.store(true); }
}  // namespace

int main(int argc, char** argv) {
  using namespace velora;
  double duration_s = 0;  // 0 = run until q / SIGINT
  std::string socket_path = "/tmp/velora-engine.sock";
  std::string config_path = VELORA_DEV_CONFIG_DEFAULT;
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
    } else {
      std::fprintf(stderr, "usage: engine-host [--duration SECONDS] [--socket PATH] [--config engine-dev.json]\n");
      return std::strcmp(argv[i], "--help") ? 2 : 0;
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
  {
    ws::v1::State initial;
    std::string cerr;
    if (!host::load_dev_config(config_path, initial, cerr) || !plane.load_initial(initial, cerr)) {
      std::fprintf(stderr, "engine-host: config: %s\n", cerr.c_str());
      return 1;
    }
  }

  host::macos::CoreAudioOutput audio;
  host::macos::CoreMidiInput midi;
  std::string err;
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
  host::ControlServer control(plane, socket_path);
  if (!control.start(err)) {
    std::fprintf(stderr, "engine-host: control: %s\n", err.c_str());
    return 1;
  }
  std::printf("control: ws.v1 on %s, generation %llu from %s\n", socket_path.c_str(),
              (unsigned long long)plane.revision(), config_path.c_str());
  if (!audio.start(err)) {
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
    if (now - last_scan > std::chrono::milliseconds(500)) {
      last_scan = now;
      const std::size_t added = midi.rescan();
      if (added) std::printf("midi: %zu new source(s) connected\n", added);
    }
    if (duration_s > 0 && std::chrono::duration<double>(now - t_start).count() >= duration_s) break;
  }

  audio.stop();
  midi.close();
  control.stop();
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
  if (duration_s > 0 && audio.callbacks() == 0) {
    std::fprintf(stderr, "engine-host: audio path never ran (0 callbacks)\n");
    return 1;
  }
  return 0;
}
