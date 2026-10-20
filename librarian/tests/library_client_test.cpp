// Story 4.11: the engine-side LibraryClient (engine/host) against the real service: reconnect with backoff, the bounded drop-oldest queue, and
// "never blocks the caller". Waits are bounded polls for an event; nothing here asserts a duration.
#include <poll.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>

#include "lib_test_util.hpp"
#include "velora/host/library_client.hpp"
#include "velora/librarian/library_service.hpp"

using namespace velora::librarian;
using velora::host::LibraryClient;
using velora::host::LibraryClientOptions;
using ltest::Client;
using ltest::TempDir;
using Ids = std::vector<std::string>;

namespace {

struct Env {
  TempDir d;
  std::string sock;
  std::unique_ptr<LibraryService> svc;
  Env() { sock = d.sub("s.sock"); }
  bool start_service() {
    LibraryServiceOptions o;
    o.data_dir = d.path;
    o.socket_path = sock;
    svc = std::make_unique<LibraryService>(o);
    std::string err;
    return svc->start(err);
  }
  LibraryClientOptions copts(std::size_t cap = 64) const {
    LibraryClientOptions o;
    o.socket_path = sock;
    o.queue_capacity = cap;
    o.backoff_min_ms = 5;
    o.backoff_max_ms = 40;
    return o;
  }
  // The service's current view, fetched with a plain client.
  std::optional<ws::v1::LibraryState> state() {
    Client c;
    if (!c.connect(sock) || !c.handshake()) return std::nullopt;
    auto g = c.get();
    if (!g || !g->has_state()) return std::nullopt;
    return g->state();
  }
  bool recent_is(const Ids& want) {
    return ltest::wait_until([&] {
      auto s = state();
      return s && ltest::strs(s->recent()) == want;
    });
  }
};

void test_delivery() {
  Env e;
  CHECK(e.start_service());
  LibraryClient lc(e.copts());
  lc.start();
  lc.report_active("one");
  lc.report_active("two");
  CHECK(e.recent_is(Ids{"two", "one"}));
  CHECK(lc.connected() && lc.connects() == 1 && lc.dropped() == 0);
  lc.set_playing(true);
  CHECK(ltest::wait_until([&] {
    auto s = e.state();
    return s && s->playing();
  }));
  lc.report_active("three");
  CHECK(ltest::wait_until([&] {
    auto s = e.state();
    return s && s->recent_size() == 3 && s->recent_unwritten();  // playing: RAM only
  }));
  lc.set_playing(false);
  CHECK(ltest::wait_until([&] {
    auto s = e.state();
    return s && !s->playing() && !s->recent_unwritten();
  }));
  CHECK(lc.sent() == 5);
}

void test_drop_oldest() {
  Env e;
  LibraryClient lc(e.copts(4));
  lc.start();  // no service yet: nothing can be sent, the queue is bounded
  for (int i = 0; i < 10; ++i) lc.report_active("r" + std::to_string(i));
  CHECK(lc.dropped() == 6 && lc.queued() == 4 && !lc.connected());
  CHECK(e.start_service());
  CHECK(e.recent_is(Ids{"r9", "r8", "r7", "r6"}));  // the four newest, in order
  CHECK(lc.dropped() == 6 && lc.queued() == 0);
}

void test_playing_survives_overflow() {
  Env e;
  LibraryClient lc(e.copts(3));
  lc.start();
  lc.set_playing(true);
  for (int i = 1; i <= 5; ++i) lc.report_active("r" + std::to_string(i));
  CHECK(lc.dropped() == 3 && lc.queued() == 3);  // reports go first; the set_playing is kept
  CHECK(e.start_service());
  CHECK(e.recent_is(Ids{"r5", "r4"}));
  auto s = e.state();
  CHECK(s && s->playing());
}

void test_reconnect() {
  Env e;
  CHECK(e.start_service());
  LibraryClient lc(e.copts());
  lc.start();
  lc.set_playing(true);
  lc.report_active("a");
  CHECK(e.recent_is(Ids{"a"}));
  const std::uint64_t first = lc.connects();
  CHECK(first >= 1);
  e.svc->stop();  // the service goes away (orderly: it writes the pending Recent)
  CHECK(ltest::wait_until([&] { return !lc.connected(); }));
  lc.report_active("b");  // queued while the service is down
  CHECK(e.start_service());
  CHECK(e.recent_is(Ids{"b", "a"}));
  CHECK(lc.connects() > first);
  // The restarted service starts with playing=false; the client says what the engine is doing now.
  CHECK(ltest::wait_until([&] {
    auto s = e.state();
    return s && s->playing();
  }));
  // A service that was killed hard (no orderly stop) is also reconnected to.
  e.svc->stop();
  CHECK(ltest::wait_until([&] { return !lc.connected(); }));
  CHECK(e.start_service());
  lc.report_active("c");
  CHECK(e.recent_is(Ids{"c", "b", "a"}));
  // A service that comes back EMPTY (its catalog is gone) is told again which Live Set sounds: the last report is re-sent on reconnect.
  e.svc->stop();
  CHECK(ltest::wait_until([&] { return !lc.connected(); }));
  for (const char* f : {"catalog.db", "catalog.db-wal", "catalog.db-shm"}) ::unlink(e.d.sub(f).c_str());
  CHECK(e.start_service());
  CHECK(e.recent_is(Ids{"c"}));
}

// stop() makes a bounded attempt to deliver what is queued (the final set_playing(false) of an exiting engine).
void test_stop_flushes() {
  Env e;
  CHECK(e.start_service());
  {
    LibraryClient lc(e.copts());
    lc.start();
    CHECK(ltest::wait_until([&] { return lc.connected(); }));
    lc.set_playing(true);
    lc.report_active("z");
    lc.set_playing(false);
    lc.stop();
    CHECK(lc.queued() == 0);
  }
  auto s = e.state();
  CHECK(s && !s->playing() && ltest::strs(s->recent()) == Ids{"z"} && !s->recent_unwritten());
}

// The caller never waits for I/O: with no service, with a service that is not reading, and in a tight loop.
void test_never_blocks() {
  {
    Env e;
    LibraryClient lc(e.copts(64));
    lc.start();
    for (int i = 0; i < 200000; ++i) {
      lc.report_active("x");
      if (i % 1000 == 0) lc.set_playing(i % 2000 == 0);
    }
    CHECK(lc.queued() == 64);
    CHECK(lc.dropped() == 200000 + 200 - 64);
  }
  {  // a service that completes the handshake and then never reads another byte
    Env e;
    const int lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, e.sock.c_str(), sizeof a.sun_path - 1);
    CHECK(::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::listen(lfd, 4) == 0);
    std::atomic<bool> release{false};
    std::thread stuck([&] {
      while (!release.load()) {
        pollfd pf{lfd, POLLIN, 0};
        if (::poll(&pf, 1, 50) <= 0) continue;
        const int fd = ::accept(lfd, nullptr, nullptr);
        if (fd < 0) continue;
        // read the Hello frame, answer HelloAck, then sit on the connection without reading
        char hdr[4];
        if (::recv(fd, hdr, 4, MSG_WAITALL) == 4) {
          const std::uint32_t n = (std::uint8_t(hdr[0]) << 24) | (std::uint8_t(hdr[1]) << 16) | (std::uint8_t(hdr[2]) << 8) | std::uint8_t(hdr[3]);
          std::string body(n, '\0');
          if (::recv(fd, body.data(), n, MSG_WAITALL) == static_cast<ssize_t>(n)) {
            ws::v1::LibraryServerMessage ack;
            ack.mutable_hello_ack()->set_version(1);
            std::string b;
            (void)ack.SerializeToString(&b);
            ::send(fd, ltest::frame_of(b).data(), ltest::frame_of(b).size(), 0);
          }
        }
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ::close(fd);
      }
    });
    LibraryClient lc(e.copts(64));
    lc.start();
    CHECK(ltest::wait_until([&] { return lc.connected(); }));
    const std::string big(2000, 'v');  // fills the socket buffers quickly
    for (int i = 0; i < 100000; ++i) lc.report_active(big);
    CHECK(lc.queued() <= 64);
    release.store(true);
    stuck.join();
    ::close(lfd);
    ::unlink(e.sock.c_str());
  }
}

}  // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  test_delivery();
  test_drop_oldest();
  test_playing_survives_overflow();
  test_reconnect();
  test_stop_flushes();
  test_never_blocks();
  if (g_fail) {
    std::printf("librarian_client_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("librarian_client_test: ok\n");
  return 0;
}
