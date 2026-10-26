#pragma once
// The engine's client of the sysd config service (Foundation: sysd config service; AR17, AR39, AD-12).
//
// sysd owns the system configuration (clock source, velocity curve, pedal calibration, Boot into). The engine receives it at start and on every change
// over ws.v1 on sysd's Unix domain socket: Hello, SubscribeConfig, then a full Config push at once and after each change.
//
// Threading. All socket I/O runs on the client's own worker thread; nothing here is ever called from, or blocks, the RT plane or the control tick:
//   * the worker connects (reconnecting with exponential backoff, backoff_min_ms doubling up to backoff_max_ms, while sysd is absent or gone), speaks the
//     protocol and stores the latest Config received, under a short mutex that is never held across I/O;
//   * the control thread calls poll() / tick() (non-blocking: a mutex held for a copy) and the CALLBACK RUNS THERE, on the caller's thread. A story that
//     adds behaviour for a setting (clock source, velocity curve, ...) consumes the Config in that callback; it needs no protocol change.
// Revision guard. The callback fires once per revision actually delivered: a push whose revision is not newer than the last one of the connection is
// ignored (stale or duplicate); on a new connection the first Config is delivered when it differs from the last-known one (a sysd that restarted without
// its file starts again at a low revision, never hidden by an old high one). If several revisions arrive between two polls only the latest is delivered.
// Absent sysd. The client runs the engine with the defaults (config() = default_config() until a Config arrived) and keeps the last-known Config when the
// connection drops. It never makes the engine fail to start.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "velora/core/block_renderer.hpp"
#include "velora/core/clock_follower.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/velocity_curve_holder.hpp"
#include "ws_v1.pb.h"

namespace velora::host {

// The configuration an engine runs with before sysd answered (and that sysd serves when it has no valid file): INTERNAL, NORMAL, pedals uncalibrated,
// Boot into LAST_SOUND.
ws::v1::Config default_config();

// Story 7.7: the engine's clock source for a sysd value. False (and `out` untouched) for UNSPECIFIED or a value this build does not know: a consumer treats the
// config defensively and keeps what it had.
bool clock_source_from_config(ws::v1::ClockSource s, core::ClockSource& out) noexcept;

// Story 7.7: applies the config's clock source without interrupting sound: the ingest's port filter first (the input edge thread stops forwarding the old
// port's clock), then the renderer (an atomic; the RT plane resets the follower at its next block, no voice is touched). Control thread. True when it applied.
bool apply_clock_source(const ws::v1::Config& c, MidiIngest& ingest, core::BlockRenderer& renderer) noexcept;

// Story 3.6: the keybed velocity curve for a sysd value (NORMAL / LIGHT / HEAVY / FIXED). False (and `out` untouched) for UNSPECIFIED or a value this build does not know.
bool velocity_curve_from_config(ws::v1::VelocityCurve c, VelocityCurve& out) noexcept;

// Story 3.6: applies the config's velocity curve to the holder (and so to every attached keybed reader): effective for the next Note On, no restart, sounding notes
// unchanged. Control thread. True when it applied (an unknown value keeps the last curve).
bool apply_velocity_curve(const ws::v1::Config& c, VelocityCurveHolder& holder);

// Story 3.11: the typed result of SysdConfigClient::set_config().
struct SysdSetResult {
  enum class Status : std::uint8_t {
    kOk = 0,         // sysd applied the patch (or it changed nothing): `config` / `revision` are sysd's reply
    kNotConnected,   // no sysd connection at the moment of the call: nothing was sent
    kBusy,           // another set_config() is in flight (one at a time): nothing was sent
    kTimeout,        // no answer within the timeout: unknown whether sysd applied it
    kRefused,        // sysd answered an error: `code` / `reason` (details.reason); nothing changed
    kLost,           // the connection dropped before the answer
  };
  Status status = Status::kNotConnected;
  ws::v1::ErrorCode code = ws::v1::ERROR_CODE_UNSPECIFIED;
  std::string reason;
  std::uint64_t revision = 0;
  bool not_durable = false;
  ws::v1::Config config;
  bool ok() const noexcept { return status == Status::kOk; }
};

struct SysdConfigClientOptions {
  std::string socket_path;
  std::string client_name = "engine-host";
  int backoff_min_ms = 50;
  int backoff_max_ms = 2000;
};

class SysdConfigClient {
 public:
  using Callback = std::function<void(const ws::v1::Config& config, std::uint64_t revision)>;

  explicit SysdConfigClient(SysdConfigClientOptions opts);
  ~SysdConfigClient();
  SysdConfigClient(const SysdConfigClient&) = delete;
  SysdConfigClient& operator=(const SysdConfigClient&) = delete;

  void start();  // starts the worker; no-op when running
  void stop();   // joins the worker; the last-known Config stays readable

  // The callback tick() uses. Set it before start() (it is read on the control thread only).
  void set_callback(Callback cb) { callback_ = std::move(cb); }
  // Control thread: delivers the latest undelivered Config to `cb` (once per revision). True when `cb` ran.
  bool poll(const Callback& cb);
  bool tick() { return callback_ ? poll(callback_) : false; }

  // The last Config received (default_config() until one arrived) and its revision (0 until one arrived). Any thread; a copy.
  ws::v1::Config config() const;
  std::uint64_t revision() const;

  // Story 3.11: writes a ConfigPatch to sysd (SetConfig) on the worker's connection and waits for the typed answer, at most `timeout_ms`. Called from the control
  // thread (it blocks it for up to the timeout, never the RT plane); all socket I/O stays on the worker thread. Retry-free: a failure is reported, nothing is
  // re-sent. One request at a time. The reply's Config also goes through the revision guard like a push (the callback sees it once, from tick()).
  SysdSetResult set_config(const ws::v1::ConfigPatch& patch, int timeout_ms = 2000);

  bool connected() const noexcept { return connected_.load(std::memory_order_relaxed); }
  std::uint64_t connects() const noexcept { return connects_.load(std::memory_order_relaxed); }    // completed handshakes
  std::uint64_t received() const noexcept { return received_.load(std::memory_order_relaxed); }    // Config messages accepted by the revision guard
  std::uint64_t ignored() const noexcept { return ignored_.load(std::memory_order_relaxed); }      // stale or duplicate Config messages refused by the guard
  std::uint64_t configs_seen() const noexcept { return configs_seen_.load(std::memory_order_relaxed); }  // every Config message read, accepted or ignored (tests wait on it)
  std::uint64_t delivered() const noexcept { return delivered_.load(std::memory_order_relaxed); }  // callback invocations

  // Pure guard, exposed for tests: is a Config with `revision` new after `last_revision` of the same connection?
  static bool newer_in_connection(std::uint64_t last_revision, std::uint64_t revision) noexcept { return revision > last_revision; }

 private:
  void run();
  bool session(int fd);  // one connection: true when at least one valid Config was received (resets the backoff)
  struct Conn {
    bool have = false;
    std::uint64_t last = 0;
  };
  void on_config(const ws::v1::ConfigReply& r, Conn& conn);
  void service_request(int fd);                                   // worker: sends a queued set_config()
  void finish_request(std::uint64_t id, SysdSetResult&& res);     // worker: fulfils the in-flight one (id must match)
  void fail_request(SysdSetResult::Status st);                    // worker: the connection ended

  SysdConfigClientOptions opts_;
  Callback callback_;
  mutable std::mutex mu_;
  ws::v1::Config last_;  // guarded by mu_
  std::uint64_t last_revision_ = 0;
  bool have_last_ = false;
  bool pending_ = false;
  ws::v1::Config pending_config_;
  std::uint64_t pending_revision_ = 0;
  struct Request {
    bool active = false, queued = false, done = false;
    std::uint64_t id = 0;
    ws::v1::ClientMessage msg;
    SysdSetResult res;
  };
  std::mutex req_mu_;
  std::condition_variable req_cv_;
  Request req_;
  std::uint64_t next_req_id_ = 100;
  std::thread thread_;
  std::atomic<bool> stop_{false}, running_{false}, connected_{false};
  int wake_[2] = {-1, -1};
  std::atomic<std::uint64_t> connects_{0}, received_{0}, ignored_{0}, delivered_{0}, configs_seen_{0};
};

}  // namespace velora::host
