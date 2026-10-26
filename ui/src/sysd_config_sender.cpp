#include "velora/ui/sysd_config_sender.hpp"

#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

#include "sysdctl_client.hpp"

namespace velora::ui {
namespace {

bool from_proto(ws::v1::ClockSource s, ClockSourceMode& out) {
  switch (s) {
    case ws::v1::CLOCK_SOURCE_INTERNAL: out = ClockSourceMode::Internal; return true;
    case ws::v1::CLOCK_SOURCE_DIN: out = ClockSourceMode::DIN; return true;
    case ws::v1::CLOCK_SOURCE_USB: out = ClockSourceMode::USB; return true;
    default: return false;
  }
}

ws::v1::ClockSource to_proto(ClockSourceMode m) {
  switch (m) {
    case ClockSourceMode::DIN: return ws::v1::CLOCK_SOURCE_DIN;
    case ClockSourceMode::USB: return ws::v1::CLOCK_SOURCE_USB;
    default: return ws::v1::CLOCK_SOURCE_INTERNAL;
  }
}

class SocketSender final : public SysdConfigSender {
 public:
  explicit SocketSender(std::string path) : path_(std::move(path)), thread_([this] { run(); }) {}
  ~SocketSender() override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

  void request_clock_source(ClockSourceMode mode) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      pending_ = mode;  // the latest wanted value wins
    }
    cv_.notify_all();
  }

  bool poll(Event& out) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (events_.empty()) return false;
    out = std::move(events_.front());
    events_.pop_front();
    return true;
  }

 private:
  void push(Event e) {
    std::lock_guard<std::mutex> lk(mu_);
    if (events_.size() >= 64) events_.pop_front();
    events_.push_back(std::move(e));
  }
  bool report(const ws::v1::Config& c) {
    Event e;
    e.kind = Event::Kind::Config;
    if (!from_proto(c.clock_source(), e.clock_source)) return false;  // an unknown value is not shown
    push(std::move(e));
    return true;
  }
  void fail(const std::string& why, bool unreachable) {
    Event e;
    e.kind = Event::Kind::Failed;
    e.error = why;
    e.unreachable = unreachable;
    push(std::move(e));
  }
  bool take(std::optional<ClockSourceMode>& req) {
    std::lock_guard<std::mutex> lk(mu_);
    if (stop_) return false;
    req = pending_;
    pending_.reset();
    return true;
  }
  bool stopping() {
    std::lock_guard<std::mutex> lk(mu_);
    return stop_;
  }
  void wait_ms(int ms) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return stop_ || pending_.has_value(); });
  }

  void run() {
    sysdctl::Connection conn;
    int backoff = 100;
    while (!stopping()) {
      std::string err;
      if (!conn.open(path_, err, 2000) || !conn.subscribe(true, err)) {
        conn.close();
        // sysd absent: a request made now cannot be delivered; say so instead of keeping it for later (the ui reverts and shows a notice).
        std::optional<ClockSourceMode> req;
        if (take(req) && req) fail(err, true);
        wait_ms(backoff);
        backoff = std::min(backoff * 2, 2000);
        continue;
      }
      backoff = 100;
      session(conn);
      conn.close();
    }
  }

  void session(sysdctl::Connection& conn) {
    while (!stopping()) {
      std::optional<ClockSourceMode> req;
      if (!take(req)) return;
      if (req) {
        ws::v1::ConfigPatch patch;
        patch.set_clock_source(to_proto(*req));
        ws::v1::ConfigReply rep;
        std::string err;
        ws::v1::Error refused;
        if (!conn.set(patch, rep, err, &refused)) {
          const bool transport = refused.code() == ws::v1::ERROR_CODE_UNSPECIFIED;
          fail(err, transport);
          if (transport) return;  // no typed answer (send failed / no reply): the connection is suspect, reconnect
          continue;  // sysd refused (a typed error): the connection is fine
        }
        if (!report(rep.config())) fail("sysd answered with an unknown clock source", false);  // not what the player asked: revert, do not keep the pick
        continue;
      }
      ws::v1::ServerMessage m;
      if (conn.next_push(m, 100)) {
        if (m.has_config_reply()) report(m.config_reply().config());
        continue;
      }
      // false: a timeout (nothing arrived) or a lost connection; tell them apart without consuming anything.
      char b;
      const ssize_t k = ::recv(conn.fd(), &b, 1, MSG_PEEK | MSG_DONTWAIT);
      if (k == 0) return;  // sysd closed the connection: reconnect
    }
  }

  std::string path_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_{false};
  std::optional<ClockSourceMode> pending_;
  std::deque<Event> events_;
  std::thread thread_;
};

}  // namespace

std::unique_ptr<SysdConfigSender> make_socket_sysd_sender(const std::string& socket_path) { return std::make_unique<SocketSender>(socket_path); }

}  // namespace velora::ui
