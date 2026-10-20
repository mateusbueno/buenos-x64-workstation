#pragma once
// A thin, portable (POSIX) Library API client for the engine (Story 4.11): tells the librarian service which Live Set became active and whether the
// engine is performing, so Recent is recorded and written at an idle-safe moment (AD-9: only the librarian writes the catalog).
//
// Never blocks the caller: report_active() / set_playing() only push into a bounded queue (a short mutex that the worker holds only to pop, never
// during I/O) and return. A worker thread connects to the service's Unix domain socket, does the LibraryHello handshake, sends the queued messages
// and reads (and discards) whatever the server answers. When the service is absent or goes away the worker reconnects with exponential backoff
// (backoff_min_ms doubling up to backoff_max_ms); queued messages wait. When the queue is full the OLDEST report_active entry is dropped (counted);
// a set_playing is only dropped when the queue holds nothing else. The latest set_playing value is re-sent first after every reconnect (a restarted
// service starts with playing=false), so the service never stays "playing" by accident of a lost message.
//
// Link: velora_library_client (generated Library API code + protobuf + threads only; no engine/core, no formatlib).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace velora::host {

struct LibraryClientOptions {
  std::string socket_path;
  std::string client_name = "engine-host";
  std::size_t queue_capacity = 64;
  int backoff_min_ms = 50;
  int backoff_max_ms = 2000;
};

class LibraryClient {
 public:
  explicit LibraryClient(LibraryClientOptions opts);
  ~LibraryClient();
  LibraryClient(const LibraryClient&) = delete;
  LibraryClient& operator=(const LibraryClient&) = delete;

  void start();  // starts the worker; no-op when running
  // Joins the worker. When connected it first gives the queue up to `flush_ms` to drain (a bounded attempt, so a final set_playing(false) reaches
  // the service); messages still queued after that are abandoned.
  void stop(int flush_ms = 500);

  // Both are non-blocking and noexcept in practice (allocation failure aside); safe from the control thread.
  void report_active(const std::string& live_set_id);
  void set_playing(bool playing);

  std::uint64_t sent() const noexcept { return sent_.load(std::memory_order_relaxed); }          // messages written to the socket
  std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }    // dropped by the bounded queue (drop-oldest)
  std::uint64_t connects() const noexcept { return connects_.load(std::memory_order_relaxed); }  // successful handshakes
  bool connected() const noexcept { return connected_.load(std::memory_order_relaxed); }
  std::size_t queued() const;

 private:
  struct Item {
    bool is_playing = false;
    bool playing = false;
    std::string id;
    std::uint64_t seq = 0;
  };
  void run();
  bool session(int fd);  // one connection: false = connection lost / stop
  void push(Item item);

  LibraryClientOptions opts_;
  mutable std::mutex mu_;
  std::deque<Item> queue_;
  std::uint64_t next_item_seq_ = 0;
  std::optional<bool> last_playing_;  // latest set_playing value ever requested
  std::optional<std::string> last_active_;  // latest report_active id: re-sent after a reconnect when no newer report is queued
  std::uint64_t sending_seq_ = 0;            // the item being written right now (never evicted by the bounded queue)
  std::thread thread_;
  std::atomic<bool> stop_{false}, running_{false};
  int wake_[2] = {-1, -1};
  std::atomic<std::uint64_t> sent_{0}, dropped_{0}, connects_{0};
  std::atomic<bool> connected_{false};
};

}  // namespace velora::host
