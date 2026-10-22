#include "velora/ui/ws_client.hpp"
#include "velora/host/control_server.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/host/render_driver.hpp"

#include <iostream>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>

using namespace velora;
using namespace velora::ui;
using namespace velora::host;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " << #c << "\n"; ++g_fail; } } while (0)

struct AudioRig {
  std::string sock_path;
  core::BlockRenderer br;
  HostEventQueue ctl_q;
  HostEventQueue midi_q;
  RenderDriver drv{br, midi_q, {}, &ctl_q};
  ControlPlane plane{br.snapshots(), br.last_values(), ctl_q, [] { return 1'000'000'000ull; }};
  std::unique_ptr<ControlServer> srv;

  std::atomic<bool> audio_running{false};
  std::atomic<uint64_t> audio_blocks_rendered{0};
  std::thread audio_thread;

  AudioRig() {
    sock_path = "/tmp/velora-resilience-test-" + std::to_string(::getpid()) + ".sock";

    ws::v1::State st;
    for (uint32_t i = 0; i < 8; ++i) {
      auto* p = st.add_parts();
      p->set_part(i);
      p->set_output(ws::v1::PART_OUTPUT_MAIN);
    }
    auto* pv = st.add_performance_init();
    pv->set_param_id(ws::v1::PARAM_TEMPO_BPM);
    pv->set_value(120.0f);
    st.set_live_set_name("Initial Live Set");
    std::string err;
    plane.load_initial(st, err);

    srv = std::make_unique<ControlServer>(plane, sock_path);
    if (!srv->start(err)) {
      std::cerr << "Failed to start server: " << err << "\n";
    }
  }

  void start_audio() {
    audio_running.store(true);
    audio_thread = std::thread([this]() {
      float l[64], r[64];
      uint64_t t = 1'000'000'000ull;
      while (audio_running.load()) {
        drv.render_block(l, r, t);
        t += 1'333'333ull;  // 64 frames @ 48 kHz ~= 1.333 ms
        audio_blocks_rendered.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
      }
    });
  }

  void stop_audio() {
    audio_running.store(false);
    if (audio_thread.joinable()) {
      audio_thread.join();
    }
  }

  ~AudioRig() {
    stop_audio();
    if (srv) {
      srv->stop();
    }
    ::unlink(sock_path.c_str());
  }
};

int main() {
  AudioRig rig;
  rig.start_audio();

  // 1. Let audio run for some blocks
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  uint64_t blocks_before_ui = rig.audio_blocks_rendered.load();
  CHECK(blocks_before_ui > 0);

  // 2. Start UI process 1 (connected over ws.v1)
  auto client1 = std::make_unique<WsClient>(rig.sock_path);
  CHECK(client1->start());

  for (int i = 0; i < 50 && !client1->is_connected(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(client1->is_connected());

  for (int i = 0; i < 50 && !client1->has_snapshot(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(client1->has_snapshot());

  // 3. While notes sound and audio runs, abruptly terminate ("kill -9") UI process 1
  uint64_t blocks_at_kill = rig.audio_blocks_rendered.load();
  client1.reset();  // Drops connection abruptly without protocol shutdown

  // 4. Verify audio continues rendering uninterrupted
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint64_t blocks_after_kill = rig.audio_blocks_rendered.load();
  CHECK(blocks_after_kill > blocks_at_kill + 50);

  // 5. Restart UI (client 2), verify it reconnects and resyncs from snapshot
  auto client2 = std::make_unique<WsClient>(rig.sock_path);
  CHECK(client2->start());

  for (int i = 0; i < 50 && !client2->is_connected(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(client2->is_connected());

  for (int i = 0; i < 50 && !client2->has_snapshot(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(client2->has_snapshot());

  // Verify state received
  CHECK(client2->last_revision() >= 0);

  client2->stop();
  rig.stop_audio();

  if (g_fail == 0) {
    std::cout << "ui_resilience_test passed\n";
    return 0;
  }
  return 1;
}
