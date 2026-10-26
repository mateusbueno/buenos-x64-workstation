// Story 3.11, end to end: the pedal calibration routine against a REAL sysd on a temp directory, with the virtual panel generating the panel MCU frames.
//   virtual panel (MIDI CC -> PEDAL_SUSTAIN frames) -> PanelReader (pedal path) -> CC 64 UMPs
//   ws.v1 StartPedalCalibration / FinishPedalCalibration (ControlPlane::handle) -> PedalCalibrator -> SysdConfigClient::set_config -> sysd (validates, revision + 1,
//   persists, pushes) -> the client callback -> apply_pedal_config -> the reader maps over the calibrated range from the next frame
// plus: persistence across a sysd + engine restart, the typed refusals leaving the config unchanged, a polarity change set through sysd, sysd absent.
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sysdctl_client.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/panel_reader.hpp"
#include "velora/host/pedal_calibrator.hpp"
#include "velora/host/sysd_config_client.hpp"
#include "velora/host/virtual_panel_map.hpp"
#include "velora/sysd/config_server.hpp"

namespace fs = std::filesystem;
namespace sd = velora::sysd;
namespace hc = velora::host;
using Clock = std::chrono::steady_clock;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

namespace {

struct TmpDir {
  std::string path;
  TmpDir() {
    std::string t = "/tmp/vpc-XXXXXX";
    if (!::mkdtemp(t.data())) std::abort();
    path = t;
  }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

bool wait_for(const std::function<bool()>& pred, int timeout_ms = 5000) {
  const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred()) {
    if (Clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

struct Sysd {
  std::unique_ptr<sd::ConfigStore> store;
  std::unique_ptr<sd::ConfigService> svc;
  std::unique_ptr<sd::ConfigServer> server;
  bool start(const std::string& dir, const std::string& sock) {
    sd::StoreOptions so;
    so.dir = dir;
    store = std::make_unique<sd::ConfigStore>(so);
    svc = std::make_unique<sd::ConfigService>(*store);
    server = std::make_unique<sd::ConfigServer>(*svc, sock);
    std::string err;
    if (!server->start(err)) {
      std::printf("sysd start: %s\n", err.c_str());
      return false;
    }
    return true;
  }
  void stop() {
    server.reset();
    svc.reset();
    store.reset();
  }
};

constexpr std::uint64_t kMs = 1'000'000ull;

const char* kMap = R"({ "version": 1, "midi": [ { "cc": 20, "control": "PEDAL_SUSTAIN" }, { "cc": 21, "control": "PEDAL_EXPRESSION" } ] })";

// The engine side: client + reader + calibrator + plane + the virtual panel, driven by one test thread (the control thread).
struct Engine {
  hc::HostEventQueue q;
  hc::KeyboardOnParts parts;
  std::unique_ptr<hc::PanelReader> reader;
  std::unique_ptr<hc::SysdConfigClient> client;
  std::unique_ptr<hc::VirtualPanel> panel;
  std::unique_ptr<velora::core::BlockRenderer> br = std::make_unique<velora::core::BlockRenderer>();
  std::unique_ptr<hc::HostEventQueue> ctl_q = std::make_unique<hc::HostEventQueue>();
  std::unique_ptr<hc::ControlPlane> plane;
  std::unique_ptr<hc::PedalCalibrator> cal;
  std::uint64_t now = 100ull * 1'000'000'000ull;
  std::uint64_t next_id = 1;
  std::vector<hc::StampedUmp> out;
  int applied = 0;
  std::uint64_t token = 0;  // the run token of the last Start

  void start(const std::string& sock) {
    parts.set_mask(0b1);
    reader = std::make_unique<hc::PanelReader>(q, parts);
    plane = std::make_unique<hc::ControlPlane>(br->snapshots(), br->last_values(), *ctl_q, [this] { return now; });
    hc::SysdConfigClientOptions o;
    o.socket_path = sock;
    o.backoff_min_ms = 5;
    o.backoff_max_ms = 40;
    client = std::make_unique<hc::SysdConfigClient>(o);
    client->set_callback([this](const ws::v1::Config& c, std::uint64_t) {
      hc::apply_pedal_config(c, reader->pedal_settings());
      ++applied;
    });
    cal = std::make_unique<hc::PedalCalibrator>(reader->pedal_probe(), [c = client.get()](const ws::v1::ConfigPatch& p) { return c->set_config(p, 3000); },
                                                [this] { return now; });
    plane->set_pedal_handler(cal.get());
    hc::VirtualPanelMap map;
    std::string err;
    CHECK(hc::VirtualPanelMap::parse(kMap, map, err));
    panel = std::make_unique<hc::VirtualPanel>(std::move(map), [this] { return now; },
                                               [this](const std::uint8_t* d, std::size_t n, std::uint64_t rx) { reader->on_bytes(d, n, rx); });
    panel->start();
    client->start();
  }
  ~Engine() {
    if (client) client->stop();
  }
  bool tick_until(const std::function<bool()>& pred, int timeout_ms = 5000) {
    return wait_for([&] {
      client->tick();
      return pred();
    }, timeout_ms);
  }
  void drain() {
    hc::StampedUmp s;
    while (q.pop(s)) out.push_back(s);
  }
  // A pedal at position f (0..1 of the whole raw range) as the virtual panel's MIDI control surface sends it.
  void pedal(int cc, double f) {
    now += 5 * kMs;
    CHECK(panel->on_midi_cc(0, cc, static_cast<std::uint32_t>(f * 0xFFFFFFFFull)));
    reader->poll(now + 2 * kMs);  // the engine's control tick: a value held back by the one-per-block throttle goes out as the latest one
    drain();
  }
  // The last CC `cc` value32 reaching part 0 (nullopt-ish: 0xFFFFFFFF + flag).
  bool last_cc(std::uint32_t cc, std::uint32_t& v) const {
    for (std::size_t i = out.size(); i-- > 0;)
      if (out[i].ump.is_control_change() && out[i].ump.controller_index() == cc) {
        v = out[i].ump.controller_value32();
        return true;
      }
    return false;
  }
  ws::v1::PedalCalibrationReply command(ws::v1::ClientMessage m) {
    m.set_request_id(next_id++);
    bool changed = false;
    const ws::v1::ServerMessage r = plane->handle(m, changed);
    CHECK(r.request_id() == m.request_id() && r.has_pedal_calibration_reply());
    return r.pedal_calibration_reply();
  }
  ws::v1::PedalCalibrationReply start(ws::v1::PedalKind k) {
    ws::v1::ClientMessage m;
    m.mutable_start_pedal_calibration()->set_pedal(k);
    const auto r = command(m);
    token = r.run_token();
    return r;
  }
  ws::v1::PedalCalibrationReply finish() {
    ws::v1::ClientMessage m;
    m.mutable_finish_pedal_calibration()->set_run_token(token);
    return command(m);
  }
  ws::v1::PedalCalibrationReply cancel() {
    ws::v1::ClientMessage m;
    m.mutable_cancel_pedal_calibration()->set_run_token(token);
    return command(m);
  }
};

void test_end_to_end() {
  using R = ws::v1::PedalCalibrationResult;
  TmpDir t;
  const std::string sock = t.path + "/s.sock";
  Sysd s;
  CHECK(s.start(t.path, sock));
  hc::PedalCal measured;
  {
    Engine e;
    e.start(sock);
    CHECK(e.tick_until([&] { return e.applied >= 1; }));
    // Uncalibrated: the full 0..4095 range. Mid travel is about half; the extremes are exact.
    e.pedal(20, 0.5);
    std::uint32_t v = 0;
    CHECK(e.last_cc(64, v) && v > 0x7FC00000u && v < 0x80400000u);
    e.pedal(20, 0.0);
    CHECK(e.last_cc(64, v) && v == 0);
    // Calibration by command: the player rests at ~12 %, presses to ~93 %, releases.
    CHECK(e.start(ws::v1::PEDAL_KIND_SUSTAIN).result() == R::PEDAL_CALIBRATION_RESULT_COLLECTING);
    for (double f : {0.12, 0.3, 0.6, 0.93, 0.5, 0.12}) e.pedal(20, f);
    const auto rep = e.finish();
    CHECK(rep.result() == R::PEDAL_CALIBRATION_RESULT_STORED && rep.config_revision() == 1);
    CHECK(rep.min() > 450 && rep.min() < 530 && rep.max() > 3780 && rep.max() < 3840);  // ~0.12 * 4095 and ~0.93 * 4095 (raw counts)
    measured.min = static_cast<std::uint16_t>(rep.min());
    measured.max = static_cast<std::uint16_t>(rep.max());
    // sysd stored it (revision + 1) and pushed it back: the reader maps over the calibrated range from the next frame.
    velora::sysdctl::Connection ctl;
    std::string err;
    ws::v1::ConfigReply cr;
    CHECK(ctl.open(sock, err) && ctl.get(cr, err));
    CHECK(cr.revision() == 1 && cr.config().sustain().min() == measured.min && cr.config().sustain().max() == measured.max && cr.config().expression().max() == 0);
    CHECK(e.tick_until([&] { return e.applied >= 2; }));
    CHECK(e.reader->pedal_settings().get(hc::PedalId::kSustain) == measured);
    // Full travel of the calibrated pedal gives the full continuous range: min -> exactly 0, max -> exactly max, mid travel -> half.
    const double span = 4095.0;
    e.pedal(20, measured.min / span);
    CHECK(e.last_cc(64, v) && v == 0);
    e.pedal(20, measured.max / span);
    CHECK(e.last_cc(64, v) && v == 0xFFFFFFFFu);
    e.pedal(20, (measured.min + measured.max) / 2.0 / span);
    CHECK(e.last_cc(64, v) && v > 0x7FC00000u && v < 0x80400000u);
    e.pedal(20, 0.0);  // below the calibrated minimum: clamped
    CHECK(e.last_cc(64, v) && v == 0);
    // The expression pedal is its own calibration: still uncalibrated.
    e.pedal(21, 0.5);
    CHECK(e.last_cc(11, v) && v > 0x7FC00000u && v < 0x80400000u);
    CHECK(e.reader->pedal_settings().get(hc::PedalId::kExpression) == hc::PedalCal{});
    // Typed refusals leave the config as it is: too small a span, no movement, cancel.
    CHECK(e.start(ws::v1::PEDAL_KIND_SUSTAIN).result() == R::PEDAL_CALIBRATION_RESULT_COLLECTING);
    for (double f : {0.40, 0.45}) e.pedal(20, f);  // ~205 counts
    CHECK(e.finish().result() == R::PEDAL_CALIBRATION_RESULT_TOO_SMALL_SPAN);
    CHECK(e.start(ws::v1::PEDAL_KIND_SUSTAIN).result() == R::PEDAL_CALIBRATION_RESULT_COLLECTING);
    CHECK(e.finish().result() == R::PEDAL_CALIBRATION_RESULT_NO_MOVEMENT);
    CHECK(e.start(ws::v1::PEDAL_KIND_SUSTAIN).result() == R::PEDAL_CALIBRATION_RESULT_COLLECTING);
    for (double f : {0.0, 1.0}) e.pedal(20, f);
    CHECK(e.cancel().result() == R::PEDAL_CALIBRATION_RESULT_CANCELLED);
    CHECK(e.finish().result() == R::PEDAL_CALIBRATION_RESULT_NOT_RUNNING);
    CHECK(ctl.get(cr, err) && cr.revision() == 1 && cr.config().sustain().min() == measured.min);
    // The expression pedal calibrates the same way and does not touch the sustain one.
    CHECK(e.start(ws::v1::PEDAL_KIND_EXPRESSION).result() == R::PEDAL_CALIBRATION_RESULT_COLLECTING);
    for (double f : {0.05, 0.9, 0.05}) e.pedal(21, f);
    const auto er = e.finish();
    CHECK(er.result() == R::PEDAL_CALIBRATION_RESULT_STORED && er.config_revision() == 2);
    CHECK(ctl.get(cr, err) && cr.config().expression().min() == er.min() && cr.config().sustain().min() == measured.min);
    // A polarity set through sysd (what `velora-sysdctl set sustain ,,closed` sends) applies from the next frame, no restart: the same travel, reversed.
    ws::v1::ConfigPatch pp;
    pp.mutable_sustain()->set_polarity(ws::v1::PEDAL_POLARITY_NORMALLY_CLOSED);
    CHECK(ctl.set(pp, cr, err));
    CHECK(e.tick_until([&] { return e.reader->pedal_settings().get(hc::PedalId::kSustain).direction == hc::PedalDirection::kInverted; }));
    e.pedal(20, measured.min / span);
    CHECK(e.last_cc(64, v) && v == 0xFFFFFFFFu);
    e.pedal(20, measured.max / span);
    CHECK(e.last_cc(64, v) && v == 0);
  }
  // Persistence across a restart of sysd and of the engine: the calibration and the polarity come back and apply on connect.
  s.stop();
  CHECK(s.start(t.path, sock));
  {
    Engine e;
    e.start(sock);
    CHECK(e.tick_until([&] { return e.applied >= 1; }));
    const hc::PedalCal c = e.reader->pedal_settings().get(hc::PedalId::kSustain);
    CHECK(c.min == measured.min && c.max == measured.max && c.direction == hc::PedalDirection::kInverted);
    std::uint32_t v = 0;
    e.pedal(20, measured.max / 4095.0);
    CHECK(e.last_cc(64, v) && v == 0);
    // sysd goes away: a finished calibration is a typed "unavailable", nothing is stored, the engine keeps its last config and keeps playing.
    s.stop();
    CHECK(wait_for([&] { return !e.client->connected(); }));
    CHECK(e.start(ws::v1::PEDAL_KIND_SUSTAIN).result() == R::PEDAL_CALIBRATION_RESULT_COLLECTING);
    for (double f : {0.0, 1.0}) e.pedal(20, f);
    const auto rep = e.finish();
    CHECK(rep.result() == R::PEDAL_CALIBRATION_RESULT_SYSD_UNAVAILABLE && rep.min() == 0 && rep.max() == 4095);
    CHECK(e.reader->pedal_settings().get(hc::PedalId::kSustain).min == measured.min);
    e.pedal(20, 0.5);  // still sounds on the last config
    CHECK(e.last_cc(64, v));
  }
  CHECK(s.start(t.path, sock));
  velora::sysdctl::Connection ctl;
  std::string err;
  ws::v1::ConfigReply cr;
  CHECK(ctl.open(sock, err) && ctl.get(cr, err) && cr.config().sustain().min() == measured.min && cr.config().sustain().max() == measured.max);
}

}  // namespace

int main() {
  test_end_to_end();
  std::printf("sysd_pedal_calibration_test: %s\n", g_fail ? "FAILED" : "ok");
  return g_fail ? 1 : 0;
}
