#include "velora/ui/ws_client.hpp"
#include "velora/host/control_server.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/host/render_driver.hpp"

#include <iostream>
#include <unistd.h>
#include <chrono>
#include <thread>

using namespace velora;
using namespace velora::ui;
using namespace velora::host;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " << #c << "\n"; ++g_fail; } } while (0)

struct ServerRig {
  std::string sock_path;
  core::BlockRenderer br;
  HostEventQueue ctl_q;
  ControlPlane plane{br.snapshots(), br.last_values(), ctl_q, [] { return 1'000'000'000ull; }};
  std::unique_ptr<ControlServer> srv;

  ServerRig() {
    sock_path = "/tmp/velora-ui-test-" + std::to_string(::getpid()) + ".sock";
    
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
      std::cerr << "Failed to start test server: " << err << "\n";
    }
  }

  ~ServerRig() {
    if (srv) {
      srv->stop();
    }
    ::unlink(sock_path.c_str());
  }
};

int main() {
  ServerRig rig;

  WsClient client(rig.sock_path);
  CHECK(client.start());

  // Wait for connection and handshake
  for (int i = 0; i < 50 && !client.is_connected(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(client.is_connected());

  // Wait for initial snapshot
  for (int i = 0; i < 50 && !client.has_snapshot(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(client.has_snapshot());

  // Verify snapshot revision
  CHECK(client.last_revision() >= 0);

  // Send a command through WsClient
  ws::v1::ClientMessage cmd;
  cmd.set_request_id(100);
  auto* pp = cmd.mutable_set_performance_param();
  pp->set_part(0);
  pp->set_param_id(ws::v1::PARAM_TEMPO_BPM);
  pp->set_value(128.0f);
  CHECK(client.send_command(cmd));

  // Stop client cleanly
  client.stop();
  CHECK(!client.is_connected());

  if (g_fail == 0) {
    std::cout << "ws_client_test passed\n";
    return 0;
  }
  return 1;
}
