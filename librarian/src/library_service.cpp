#include "velora/librarian/library_service.hpp"

#include <cctype>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <grp.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>

#include "import_worker.hpp"
#include "velora/format/category.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/librarian/import.hpp"
#include "velora/librarian/preflight.hpp"

namespace velora::librarian {

namespace {

bool set_nonblocking(int fd) {
  const int fl = fcntl(fd, F_GETFL, 0);
  return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

void no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
#endif
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

std::uint32_t be32(const unsigned char* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

ws::v1::LibraryServerMessage make_error(std::uint64_t rid, ws::v1::LibraryErrorCode code, const std::string& message) {
  ws::v1::LibraryServerMessage m;
  m.set_request_id(rid);
  m.mutable_error()->set_code(code);
  m.mutable_error()->set_message(message);
  return m;
}

// Moves `id` to the front of `list` (most recent first), removing an older copy and capping the list. True when the list changed.
bool touch_recent(std::vector<std::string>& list, const std::string& id) {
  if (!list.empty() && list.front() == id) return false;
  list.erase(std::remove(list.begin(), list.end(), id), list.end());
  list.insert(list.begin(), id);
  if (list.size() > kMaxRecent) list.resize(kMaxRecent);
  return true;
}

bool erase_id(std::vector<std::string>& list, const std::string& id) {
  const auto it = std::remove(list.begin(), list.end(), id);
  const bool had = it != list.end();
  list.erase(it, list.end());
  return had;
}

}  // namespace

LibraryService::LibraryService(LibraryServiceOptions opts) : opts_(std::move(opts)) {}

LibraryService::~LibraryService() { stop(); }

bool LibraryService::start(std::string& err, CatalogStatus* status) {
  if (started_) {
    err = "already started";
    return false;
  }
  CatalogStatus st = Catalog::open(opts_.catalog_dir.empty() ? opts_.data_dir : opts_.catalog_dir, catalog_);
  if (status) *status = st;
  if (st != CatalogStatus::kOk) {
    err = std::string("catalog: ") + to_string(st);
    return false;
  }
  st = catalog_->favorites(favorites_);
  if (st == CatalogStatus::kOk) st = catalog_->recent(recent_);
  if (st != CatalogStatus::kOk) {
    err = std::string("catalog read: ") + to_string(st);
    if (status) *status = st;
    catalog_.reset();
    return false;
  }
  playing_ = false;
  recent_dirty_ = false;

  // Story 5.6: the single writer of `<data>/lib`. The lock is taken before anything is changed; the reconcile pass then makes the catalog and the
  // library tree agree, before a single client is served.
  {
    const std::string lib_dir = opts_.data_dir + "/" + host::kLibDirName;
    if (::mkdir(lib_dir.c_str(), 0755) != 0 && errno != EEXIST) {
      err = lib_dir + ": " + std::strerror(errno);
      if (status) *status = CatalogStatus::kIo;
      catalog_.reset();
      return false;
    }
    const std::string lock_path = lib_dir + "/" + kServiceLockFileName;
    lib_lock_fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lib_lock_fd_ < 0 || ::flock(lib_lock_fd_, LOCK_EX | LOCK_NB) != 0) {
      const bool busy = lib_lock_fd_ >= 0 && errno == EWOULDBLOCK;
      err = busy ? "another writer (a librarian import or service) holds " + lock_path : lock_path + ": " + std::strerror(errno);
      if (lib_lock_fd_ >= 0) ::close(lib_lock_fd_);
      lib_lock_fd_ = -1;
      if (status) *status = busy ? CatalogStatus::kBusy : CatalogStatus::kIo;
      catalog_.reset();
      return false;
    }
    catalog_->set_fault_hook([this](const char* p) {
      if (std::strcmp(p, "library.before_commit") == 0) point("catalog-row-written", cur_job_, cur_item_);
      return fault_hook_ ? fault_hook_(p) : false;
    });
    ReconcileOptions ro;
    ro.hook = [this](const char* p) { point(p, 0, 0); };
    const CatalogStatus rs = reconcile_libraries(*catalog_, opts_.data_dir, reconcile_report_, ro);
    if (rs != CatalogStatus::kOk) {
      err = std::string("reconcile: ") + to_string(rs);
      if (status) *status = rs;
      ::close(lib_lock_fd_);
      lib_lock_fd_ = -1;
      catalog_.reset();
      return false;
    }
  }

  sockaddr_un addr{};
  if (opts_.socket_path.empty() || opts_.socket_path.size() >= sizeof(addr.sun_path)) {
    err = "socket path is empty or too long: " + opts_.socket_path;
    if (lib_lock_fd_ >= 0) ::close(lib_lock_fd_);
    lib_lock_fd_ = -1;
    catalog_.reset();
    return false;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, opts_.socket_path.c_str(), opts_.socket_path.size() + 1);
  const std::string& path = opts_.socket_path;
  auto fail = [&](const std::string& why) {
    err = why;
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;
    if (wake_[0] >= 0) ::close(wake_[0]);
    if (wake_[1] >= 0) ::close(wake_[1]);
    wake_[0] = wake_[1] = -1;
    if (lib_lock_fd_ >= 0) ::close(lib_lock_fd_);
    lib_lock_fd_ = -1;
    catalog_.reset();
    return false;
  };
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return fail(std::string("socket: ") + std::strerror(errno));
  struct stat sb{};
  if (::lstat(path.c_str(), &sb) == 0 && !S_ISSOCK(sb.st_mode)) return fail(path + " exists and is not a socket");
  auto bind_once = [&] {
    const mode_t old = ::umask(0177);  // created 0600 from the start
    const int rc = ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    const int e = errno;
    ::umask(old);
    errno = e;
    return rc;
  };
  if (bind_once() != 0) {
    if (errno != EADDRINUSE) return fail(std::string("bind ") + path + ": " + std::strerror(errno));
    const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
    const bool connected = probe >= 0 && ::connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    const int cerrno = errno;
    if (probe >= 0) ::close(probe);
    const bool stale = !connected && (cerrno == ECONNREFUSED || cerrno == ENOENT);  // only a refused connection proves it is stale
    if (!stale) return fail("another service is already listening on " + path);
    ::unlink(path.c_str());
    if (bind_once() != 0) return fail(std::string("bind ") + path + ": " + std::strerror(errno));
  }
  if (!opts_.socket_group.empty()) {
    const struct group* g = ::getgrnam(opts_.socket_group.c_str());
    if (!g) {
      ::unlink(path.c_str());
      return fail("unknown socket group '" + opts_.socket_group + "'");
    }
    if (::chown(path.c_str(), static_cast<uid_t>(-1), g->gr_gid) != 0) {
      const std::string why = std::strerror(errno);
      ::unlink(path.c_str());
      return fail("chown group " + opts_.socket_group + ": " + why);
    }
    ::chmod(path.c_str(), 0660);
  } else {
    ::chmod(path.c_str(), 0600);
  }
  if (::listen(listen_fd_, 16) != 0 || !set_nonblocking(listen_fd_)) {
    ::unlink(path.c_str());
    return fail(std::string("listen: ") + std::strerror(errno));
  }
  if (::pipe(wake_) != 0 || !set_nonblocking(wake_[0]) || !set_nonblocking(wake_[1])) {
    ::unlink(path.c_str());
    return fail(std::string("pipe: ") + std::strerror(errno));
  }
  stop_.store(false);
  stopping_.store(false);
  started_ = true;
  detail::ImportWorker::Config wc;
  wc.data_dir = opts_.data_dir;
  wc.preflight_start_hook = opts_.preflight_start_hook;
  wc.point_hook = [this](const char* p, std::uint64_t job, std::size_t item) { point(p, job, item); };
  wc.wake = [this] {
    const char b = 1;
    [[maybe_unused]] const ssize_t w = ::write(wake_[1], &b, 1);
  };
  worker_ = std::make_unique<detail::ImportWorker>(std::move(wc));
  worker_->start();
  thread_ = std::thread([this] { run(); });
  return true;
}

void LibraryService::stop() {
  if (!started_) return;
  started_ = false;
  stopping_.store(true);
  // The running import stops at its next safe point; the service thread keeps handling the worker's messages until the worker is gone.
  if (worker_) worker_->stop_and_join();
  stop_.store(true);
  const char b = 1;
  [[maybe_unused]] const ssize_t w = ::write(wake_[1], &b, 1);
  if (thread_.joinable()) thread_.join();
  process_worker();  // what the worker posted last: the terminal events of the jobs that were cancelled by this stop
  for (Client& c : clients_)
    if (!c.dead && !c.tx.empty()) flush(c);
  worker_.reset();
  jobs_.clear();
  pending_preflights_.clear();
  preflights_.clear();
  estimates_.clear();
  for (Client& c : clients_) ::close(c.fd);
  clients_.clear();
  if (recent_dirty_) flush_recent(nullptr);  // orderly shutdown: the last chance to persist Recent, playing or not
  sync_counters();
  catalog_.reset();  // checkpoints the WAL and releases the writer lock
  if (lib_lock_fd_ >= 0) ::close(lib_lock_fd_);
  lib_lock_fd_ = -1;
  ::close(listen_fd_);
  ::close(wake_[0]);
  ::close(wake_[1]);
  listen_fd_ = wake_[0] = wake_[1] = -1;
  ::unlink(opts_.socket_path.c_str());
}

void LibraryService::sync_counters() {
  if (!catalog_) return;
  recent_writes_.store(catalog_->recent_writes(), std::memory_order_relaxed);
  favorite_writes_.store(catalog_->favorite_writes(), std::memory_order_relaxed);
}

void LibraryService::run() {
  std::vector<pollfd> fds;
  while (!stop_.load()) {
    fds.clear();
    fds.push_back({wake_[0], POLLIN, 0});
    fds.push_back({listen_fd_, POLLIN, 0});
    for (const Client& c : clients_)
      fds.push_back({c.fd, static_cast<short>((c.closing ? 0 : POLLIN) | (c.tx.empty() ? 0 : POLLOUT)), 0});
    ::poll(fds.data(), fds.size(), 20);
    if (fds[0].revents & POLLIN) {
      char buf[16];
      while (::read(wake_[0], buf, sizeof buf) > 0) {
      }
    }
    process_worker();
    if (fds[1].revents & POLLIN) accept_clients();
    for (std::size_t i = 0; i + 2 < fds.size() && i < clients_.size(); ++i) {
      Client& c = clients_[i];
      const short ev = fds[i + 2].revents;
      if (ev & (POLLERR | POLLNVAL)) c.dead = true;
      if (!c.dead && (ev & (POLLIN | POLLHUP))) read_client(c);
      if (!c.dead && !c.tx.empty()) flush(c);
      if (!c.dead && c.closing && c.tx.empty()) c.dead = true;
    }
    for (Client& c : clients_)
      if (!c.dead && !c.tx.empty()) flush(c);
    const auto now = std::chrono::steady_clock::now();
    for (Client& c : clients_)
      if (!c.handshaken && !c.dead && now - c.accepted > std::chrono::milliseconds(opts_.handshake_timeout_ms)) c.dead = true;
    for (auto it = clients_.begin(); it != clients_.end();) {
      if (it->dead) {
        if (playing_ && playing_owner_ == it->id) {  // the engine/ui that said "performing" is gone
          playing_ = false;
          playing_owner_ = 0;
          idle_flush();
        }
        cancel_preflights_of(it->id);
        ::close(it->fd);
        it = clients_.erase(it);
      } else {
        ++it;
      }
    }
    check_playing_lease();
    sync_counters();
  }
}

void LibraryService::accept_clients() {
  for (;;) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) ::usleep(50'000);
      return;
    }
    if (clients_.size() >= opts_.max_clients || !set_nonblocking(fd)) {
      ::close(fd);
      continue;
    }
    no_sigpipe(fd);
    Client c;
    c.fd = fd;
    c.id = next_client_id_++;
    clients_.push_back(std::move(c));
    accepted_.fetch_add(1, std::memory_order_relaxed);
  }
}

void LibraryService::read_client(Client& c) {
  char buf[8192];
  for (;;) {
    const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
    if (n > 0) {
      if (!c.closing && !c.dead) c.rx.append(buf, static_cast<std::size_t>(n));
      if (c.rx.size() > kMaxFrame + 4 + sizeof buf) break;
      continue;
    }
    if (n == 0) {  // the peer closed (or half-closed): what it sent before is still parsed, applied and answered below
      c.eof = true;
      break;
    }
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) c.dead = true;
    break;
  }
  while (!c.closing && !c.dead && c.rx.size() >= 4) {
    const std::uint32_t len = be32(reinterpret_cast<const unsigned char*>(c.rx.data()));
    if (len > kMaxFrame) {  // cannot resynchronise: report and close
      auto e = make_error(0, ws::v1::LIBRARY_ERROR_CODE_FRAME_TOO_LARGE, "frame exceeds " + std::to_string(kMaxFrame) + " bytes");
      (*e.mutable_error()->mutable_details())["max_frame"] = std::to_string(kMaxFrame);
      send(c, e);
      c.closing = true;
      c.rx.clear();
      break;
    }
    if (c.rx.size() < 4 + len) break;
    const std::string payload = c.rx.substr(4, len);
    c.rx.erase(0, 4 + len);
    frames_in_.fetch_add(1, std::memory_order_relaxed);
    process_frame(c, payload);
  }
  if (c.eof && !c.dead) c.closing = true;  // closes once the replies are out
}

void LibraryService::process_frame(Client& c, const std::string& payload) {
  ws::v1::LibraryClientMessage msg;
  if (!msg.ParseFromString(payload)) {
    send(c, make_error(0, ws::v1::LIBRARY_ERROR_CODE_MALFORMED_MESSAGE, "frame is not a valid LibraryClientMessage"));
    if (!c.handshaken) c.closing = true;
    return;
  }
  if (!c.handshaken) {
    if (msg.body_case() != ws::v1::LibraryClientMessage::kHello) {
      send(c, make_error(msg.request_id(), ws::v1::LIBRARY_ERROR_CODE_HANDSHAKE_REQUIRED, "the first message on a connection must be LibraryHello"));
      c.closing = true;
      return;
    }
    if (msg.hello().version() != kProtocolVersion) {
      auto e = make_error(msg.request_id(), ws::v1::LIBRARY_ERROR_CODE_UNSUPPORTED_VERSION, "unsupported protocol version");
      (*e.mutable_error()->mutable_details())["supported"] = std::to_string(kProtocolVersion);
      (*e.mutable_error()->mutable_details())["requested"] = std::to_string(msg.hello().version());
      send(c, e);
      c.closing = true;
      return;
    }
    c.handshaken = true;
    ws::v1::LibraryServerMessage ack;
    ack.set_request_id(msg.request_id());
    ws::v1::LibraryHelloAck* h = ack.mutable_hello_ack();
    h->set_version(kProtocolVersion);
    for (const char* f : {"library.v1", "favorites.v1", "recent.v1", "subscribe.v1", "import.v1", "catalog.v2", "preflight.v1"}) h->add_features(f);
    h->set_catalog_schema_version(catalog_->schema_version());
    send(c, ack);
    return;
  }
  handle(c, msg);
}

void LibraryService::reply_error(Client& c, std::uint64_t rid, ws::v1::LibraryErrorCode code, const std::string& message) {
  send(c, make_error(rid, code, message));
}

void LibraryService::reply_ack(Client& c, std::uint64_t rid, bool changed) {
  ws::v1::LibraryServerMessage m;
  m.set_request_id(rid);
  m.mutable_ack()->set_changed(changed);
  send(c, m);
}

void LibraryService::fill_state(ws::v1::LibraryState* s) const {
  for (const std::string& f : favorites_) s->add_favorites(f);
  for (const std::string& r : recent_) s->add_recent(r);
  s->set_catalog_schema_version(catalog_->schema_version());
  s->set_playing(playing_);
  s->set_recent_unwritten(recent_dirty_);
  s->set_recent_writes(catalog_->recent_writes());
  s->set_favorite_writes(catalog_->favorite_writes());
}

void LibraryService::push_favorites() {
  ws::v1::LibraryServerMessage m;
  for (const std::string& f : favorites_) m.mutable_favorites_changed()->add_favorites(f);
  if (!m.has_favorites_changed()) m.mutable_favorites_changed();
  for (Client& c : clients_)
    if (c.subscribed && !c.dead) send(c, m);
}

void LibraryService::push_recent() {
  ws::v1::LibraryServerMessage m;
  for (const std::string& r : recent_) m.mutable_recent_changed()->add_recent(r);
  if (!m.has_recent_changed()) m.mutable_recent_changed();
  for (Client& c : clients_)
    if (c.subscribed && !c.dead) send(c, m);
}

// Writes the RAM Recent to SQLite (one transaction). The caller decides that the moment is idle-safe.
bool LibraryService::flush_recent(bool* wrote) {
  if (wrote) *wrote = false;
  if (!recent_dirty_) return true;
  const CatalogStatus st = catalog_->write_recent(recent_);
  if (st != CatalogStatus::kOk) {
    storage_status_ = st;
    return false;
  }
  recent_dirty_ = false;
  if (wrote) *wrote = true;
  sync_counters();
  return true;
}

// The idle-safe write: now, if the engine/ui is not performing. A failure leaves recent_dirty_ set; the next change, SetPlaying(false) or
// FlushRecent retries.
void LibraryService::reload_favorites() {
  std::vector<std::string> tmp;
  if (catalog_->favorites(tmp) == CatalogStatus::kOk) favorites_ = std::move(tmp);  // never empty the RAM list because a read failed
}

void LibraryService::check_playing_lease() {
  if (!playing_ || opts_.playing_lease_ms <= 0) return;
  if (now() - playing_since_ >= std::chrono::milliseconds(opts_.playing_lease_ms)) {
    playing_ = false;
    playing_owner_ = 0;
    idle_flush();
  }
}

void LibraryService::idle_flush() {
  if (!playing_ && recent_dirty_) flush_recent(nullptr);
}

CatalogStatus LibraryService::cascade_delete(const std::vector<std::string>& gone, const std::set<std::string>* existing, bool& changed_f,
                                             bool& changed_r) {
  changed_f = changed_r = false;
  const bool was_dirty = recent_dirty_;
  const std::vector<std::string> old_recent = recent_;
  std::vector<std::string> gone_favs;
  for (const std::string& id : gone) {
    if (erase_id(favorites_, id)) {
      changed_f = true;
      gone_favs.push_back(id);
    }
    if (erase_id(recent_, id)) changed_r = true;
  }
  CatalogStatus st = CatalogStatus::kOk;
  if (!playing_) {
    // Idle: both tables in one transaction. Recent rows in SQLite are then already consistent with the RAM list for these ids; if RAM held
    // other unwritten changes it stays dirty (and is written below).
    if (existing) {
      ReconcileResult rr;
      st = catalog_->reconcile(*existing, rr);
    } else {
      for (const std::string& id : gone) {
        RemoveResult rr;
        st = catalog_->remove_live_set(id, rr);
        if (st != CatalogStatus::kOk) break;
      }
    }
    recent_dirty_ = was_dirty;
  } else {
    // Playing: the favorite rows go now (explicit data, not Recent); the recent rows wait for the idle-safe write.
    for (const std::string& id : gone_favs) {
      bool ch = false;
      st = catalog_->remove_favorite(id, ch);
      if (st != CatalogStatus::kOk) break;
    }
    if (st == CatalogStatus::kOk && changed_r) recent_dirty_ = true;
  }
  if (st != CatalogStatus::kOk) {
    storage_status_ = st;
    reload_favorites();   // the catalog is the truth for favorites
    recent_ = old_recent;  // a refused command changes nothing
    recent_dirty_ = was_dirty;
    changed_f = changed_r = false;
    return st;
  }
  if (!playing_) idle_flush();  // other unwritten Recent changes go out now
  return CatalogStatus::kOk;
}

void LibraryService::handle(Client& c, const ws::v1::LibraryClientMessage& msg) {
  using M = ws::v1::LibraryClientMessage;
  const std::uint64_t rid = msg.request_id();
  auto bad_id = [&](const std::string& id) {
    auto e = make_error(rid, ws::v1::LIBRARY_ERROR_CODE_INVALID_ID, "live_set_id must be 1..64 bytes of [A-Za-z0-9._-]");
    (*e.mutable_error()->mutable_details())["length"] = std::to_string(id.size());
    send(c, e);
  };
  auto storage_failed = [&](CatalogStatus st) {
    auto e = make_error(rid, ws::v1::LIBRARY_ERROR_CODE_STORAGE_FAILED, "the catalog could not be written; nothing changed");
    (*e.mutable_error()->mutable_details())["reason"] = to_string(st);
    send(c, e);
  };
  switch (msg.body_case()) {
    case M::kHello:
      reply_error(c, rid, ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND, "already handshaken");
      return;
    case M::kSubscribe: {
      c.subscribed = msg.subscribe().enable();
      reply_ack(c, rid, false);
      if (c.subscribed) {  // the current state right away
        ws::v1::LibraryServerMessage push;
        fill_state(push.mutable_state());
        send(c, push);
      }
      return;
    }
    case M::kAddFavorite: {
      const std::string& id = msg.add_favorite().live_set_id();
      if (!valid_live_set_id(id)) return bad_id(id);
      bool changed = false;
      const CatalogStatus st = catalog_->add_favorite(id, changed);
      if (st == CatalogStatus::kLimitReached) {
        auto e = make_error(rid, ws::v1::LIBRARY_ERROR_CODE_LIMIT_REACHED, "too many favorites");
        (*e.mutable_error()->mutable_details())["limit"] = std::to_string(kMaxFavorites);
        send(c, e);
        return;
      }
      if (st != CatalogStatus::kOk) return storage_failed(st);
      sync_counters();
      reply_ack(c, rid, changed);
      if (changed) {
        reload_favorites();
        push_favorites();
      }
      return;
    }
    case M::kRemoveFavorite: {
      const std::string& id = msg.remove_favorite().live_set_id();
      if (!valid_live_set_id(id)) return bad_id(id);
      bool changed = false;
      const CatalogStatus st = catalog_->remove_favorite(id, changed);
      if (st != CatalogStatus::kOk) return storage_failed(st);
      sync_counters();
      reply_ack(c, rid, changed);
      if (changed) {
        reload_favorites();
        push_favorites();
      }
      return;
    }
    case M::kGetFavoritesAndRecent: {
      ws::v1::LibraryServerMessage m;
      m.set_request_id(rid);
      fill_state(m.mutable_state());
      send(c, m);
      return;
    }
    case M::kReportActive: {
      const std::string& id = msg.report_active().live_set_id();
      if (!valid_live_set_id(id)) return bad_id(id);
      const bool changed = touch_recent(recent_, id);
      if (changed) {
        recent_dirty_ = true;
        idle_flush();  // written now unless the engine/ui is performing
      }
      reply_ack(c, rid, changed);
      if (changed) push_recent();
      return;
    }
    case M::kLiveSetDeleted: {
      const std::string& id = msg.live_set_deleted().live_set_id();
      if (!valid_live_set_id(id)) return bad_id(id);
      bool cf = false, cr = false;
      const CatalogStatus cst = cascade_delete({id}, nullptr, cf, cr);
      if (cst != CatalogStatus::kOk) return storage_failed(cst);
      sync_counters();
      reply_ack(c, rid, cf || cr);
      if (cf) push_favorites();
      if (cr) push_recent();
      return;
    }
    case M::kReconcile: {
      const auto& rc = msg.reconcile();
      for (const std::string& id : rc.existing_live_set_ids())
        if (!valid_live_set_id(id)) {
          c.reconcile_acc.clear();
          return bad_id(id);
        }
      if (c.reconcile_acc.size() + static_cast<std::size_t>(rc.existing_live_set_ids_size()) > kMaxReconcileIds) {
        c.reconcile_acc.clear();
        auto e = make_error(rid, ws::v1::LIBRARY_ERROR_CODE_LIMIT_REACHED, "too many ids in one Reconcile");
        (*e.mutable_error()->mutable_details())["limit"] = std::to_string(kMaxReconcileIds);
        send(c, e);
        return;
      }
      for (const std::string& id : rc.existing_live_set_ids()) c.reconcile_acc.push_back(id);
      if (rc.more()) {  // a chunk: nothing is applied until the final one
        reply_ack(c, rid, false);
        return;
      }
      std::set<std::string> existing(c.reconcile_acc.begin(), c.reconcile_acc.end());
      c.reconcile_acc.clear();
      if (existing.empty() && !rc.allow_empty() && (!favorites_.empty() || !recent_.empty())) {
        auto e = make_error(rid, ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND,
                            "an empty Reconcile would delete every favorite and recent row; set allow_empty if that is intended");
        (*e.mutable_error()->mutable_details())["reason"] = "empty_reconcile";
        send(c, e);
        return;
      }
      std::vector<std::string> gone;
      for (const std::string& f : favorites_)
        if (!existing.count(f)) gone.push_back(f);
      for (const std::string& r : recent_)
        if (!existing.count(r) && std::find(gone.begin(), gone.end(), r) == gone.end()) gone.push_back(r);
      bool cf = false, cr = false;
      const CatalogStatus cst = cascade_delete(gone, &existing, cf, cr);
      if (cst != CatalogStatus::kOk) return storage_failed(cst);
      sync_counters();
      reply_ack(c, rid, cf || cr);
      if (cf) push_favorites();
      if (cr) push_recent();
      return;
    }
    case M::kSetPlaying: {
      const bool p = msg.set_playing().playing();
      const bool changed = p != playing_;
      playing_ = p;
      if (p) {  // a lease held by this connection
        playing_owner_ = c.id;
        playing_since_ = now();
      } else {
        playing_owner_ = 0;
      }
      idle_flush();  // the idle-safe moment has come
      reply_ack(c, rid, changed);
      return;
    }
    case M::kFlushRecent: {
      bool wrote = false;
      if (!flush_recent(&wrote)) return storage_failed(storage_status_);
      ws::v1::LibraryServerMessage m;
      m.set_request_id(rid);
      m.mutable_flush_reply()->set_written(wrote);
      m.mutable_flush_reply()->set_recent_writes(catalog_->recent_writes());
      send(c, m);
      return;
    }
    case M::kImportRequest:
      return handle_import_request(c, rid, msg.import_request());
    case M::kListLibraries:
      return handle_list_libraries(c, rid, msg.list_libraries());
    case M::kListInstruments:
      return handle_list_instruments(c, rid, msg.list_instruments());
    case M::kCancelImport:
      return handle_cancel_import(c, rid, msg.cancel_import().job_id());
    case M::kPreflightRequest:
      return handle_preflight_request(c, rid, msg.preflight_request());
    case M::kListPreflightRows:
      return handle_list_preflight_rows(c, rid, msg.list_preflight_rows());
    case M::kCancelPreflight:
      return handle_cancel_preflight(c, rid, msg.cancel_preflight().request_id());
    case M::kImportStatus: {
      const auto it = jobs_.find(msg.import_status().job_id());
      if (it == jobs_.end()) return reply_error(c, rid, ws::v1::LIBRARY_ERROR_CODE_UNKNOWN_JOB, "no such import job");
      ws::v1::LibraryServerMessage m;
      m.set_request_id(rid);
      *m.mutable_import_progress() = it->second->last_event;
      send(c, m);
      return;
    }
    case M::BODY_NOT_SET:
      break;
  }
  reply_error(c, rid, ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND, "no command set or unknown command");
}

void LibraryService::send(Client& c, const ws::v1::LibraryServerMessage& msg) {
  std::string body;
  (void)msg.SerializeToString(&body);
  if (body.size() > kMaxFrame) {  // never emit a frame the peer must reject
    ws::v1::LibraryServerMessage e = make_error(msg.request_id(), ws::v1::LIBRARY_ERROR_CODE_INTERNAL, "reply exceeds the maximum frame size");
    body.clear();
    (void)e.SerializeToString(&body);
  }
  const std::uint32_t len = static_cast<std::uint32_t>(body.size());
  const char hdr[4] = {char(len >> 24), char(len >> 16), char(len >> 8), char(len)};
  if (c.tx.size() + body.size() + 4 > kMaxQueuedBytes) {  // slow consumer: drop it, never block
    c.dead = true;
    dropped_slow_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  c.tx.append(hdr, 4);
  c.tx.append(body);
}

void LibraryService::flush(Client& c) {
  while (!c.tx.empty()) {
    const ssize_t n = ::send(c.fd, c.tx.data(), c.tx.size(), kSendFlags);
    if (n > 0) {
      c.tx.erase(0, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    c.dead = true;
    return;
  }
}

// --- Story 5.6: import jobs and the libraries catalog ---------------------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;
namespace fmtlib = velora::format;

// The whole request is validated on the service thread (one canonical path, one stat per item), so the number of items is bounded to keep favorites / Recent /
// playing handling responsive; a UI splits a bigger import into several requests.
constexpr std::size_t kMaxItems = 1000;
constexpr std::size_t kMaxReportEntries = 100;  // offenders and messages kept per item result
constexpr std::size_t kMaxReportString = 400;   // bytes of each of those strings
constexpr std::size_t kMaxFinishedJobs = 64;

// A string for a proto3 `string` field: valid UTF-8 (each bad byte becomes U+FFFD; file names are arbitrary bytes) and at most `max` bytes (cut at a
// character boundary). protobuf refuses to parse invalid UTF-8 in a proto3 string, so every string that comes from the file system goes through this.
std::string clean_utf8(const std::string& s, std::size_t max = 4096) {
  std::string o;
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t n = 0;
    if (c < 0x80) n = 1;
    else if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c >= 0xE0 && c <= 0xEF) n = 3;
    else if (c >= 0xF0 && c <= 0xF4) n = 4;
    bool ok = n != 0 && i + n <= s.size();
    std::uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (std::size_t k = 1; ok && k < n; ++k) {
      const unsigned char d = static_cast<unsigned char>(s[i + k]);
      if ((d & 0xC0) != 0x80) ok = false;
      cp = (cp << 6) | (d & 0x3F);
    }
    if (ok && ((n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)))) ok = false;
    const std::string piece = ok ? s.substr(i, n) : std::string("\xEF\xBF\xBD");
    if (o.size() + piece.size() > max) break;
    o += piece;
    i += ok ? n : 1;
  }
  return o;
}

// Closed typed error with details.
ws::v1::LibraryServerMessage typed_error(std::uint64_t rid, ws::v1::LibraryErrorCode code, const std::string& message, const std::string& path,
                                          const std::string& reason) {
  ws::v1::LibraryServerMessage m = make_error(rid, code, message);
  if (!path.empty()) (*m.mutable_error()->mutable_details())["path"] = clean_utf8(path);
  if (!reason.empty()) (*m.mutable_error()->mutable_details())["reason"] = reason;
  m.mutable_error()->set_message(clean_utf8(m.error().message()));
  return m;
}

std::int64_t mtime_ns(const struct stat& sb) {
#ifdef __APPLE__
  return static_cast<std::int64_t>(sb.st_mtimespec.tv_sec) * 1000000000ll + sb.st_mtimespec.tv_nsec;
#else
  return static_cast<std::int64_t>(sb.st_mtim.tv_sec) * 1000000000ll + sb.st_mtim.tv_nsec;
#endif
}

constexpr std::size_t kMaxEstimateMisses = 50;
constexpr std::size_t kMaxDepStats = 200000;
bool deps_unchanged(const std::vector<FileDep>& deps) {
  for (const FileDep& d : deps) {
    struct stat sb{};
    if (::stat(d.path.c_str(), &sb) != 0 || static_cast<std::uint64_t>(sb.st_size) != d.size || mtime_ns(sb) != d.mtime_ns) return false;
  }
  return true;
}
constexpr std::size_t kMaxStoredPreflights = 3;
constexpr std::size_t kMaxEstimates = 100000;
constexpr std::size_t kMaxLibrariesInDone = 200;  // PreflightDone lists this many library summaries (names are cut at 255 bytes): far below the frame limit

bool importable_extension(const fs::path& p) {
  std::string e = p.extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".sfz" || e == ".sf2";
}

bool valid_display_name(const std::string& n) {
  if (n.size() > 256) return false;
  return std::none_of(n.begin(), n.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

// 0 -> "none", otherwise the category key; nullopt for an id outside 1..17.
std::optional<fmtlib::Category> assignable_from_id(std::uint32_t id) {
  const auto c = fmtlib::category_from_id(id);
  if (!c || !fmtlib::is_assignable(*c)) return std::nullopt;
  return c;
}

}  // namespace

void LibraryService::handle_import_request(Client& c, std::uint64_t rid, const ws::v1::ImportRequest& req) {
  using ws::v1::LibraryErrorCode;
  if (stopping_.load()) {
    send(c, typed_error(rid, ws::v1::LIBRARY_ERROR_CODE_SHUTTING_DOWN, "the service is stopping and accepts no new import", {}, {}));
    return;
  }
  auto refuse = [&](LibraryErrorCode code, const std::string& message, const std::string& path, const std::string& reason) {
    send(c, typed_error(rid, code, message, path, reason));
  };
  std::error_code ec;
  if (req.source().empty() || req.source().size() > 4096 || req.source().find('\0') != std::string::npos)
    return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "the import source is empty or too long", {}, "source_missing");
  const fs::path source = fs::canonical(fs::path(req.source()), ec);
  if (ec) return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "the import source cannot be read: " + ec.message(), req.source(), "source_unreadable");
  const bool source_is_dir = fs::is_directory(source, ec);
  if (!source_is_dir && !(fs::is_regular_file(source, ec) && importable_extension(source)))
    return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "the source is neither a folder nor an .sfz / .sf2 file", req.source(), "not_importable");
  if (source_is_dir) {
    fs::directory_iterator probe(source, ec);  // a folder that cannot be listed
    if (ec) return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "the source folder cannot be read: " + ec.message(), req.source(), "source_unreadable");
  }
  if (req.items_size() == 0)
    return refuse(ws::v1::LIBRARY_ERROR_CODE_CATEGORY_REQUIRED, "an import needs at least one item, each with a category", req.source(), "missing");
  if (static_cast<std::size_t>(req.items_size()) > kMaxItems)
    return refuse(ws::v1::LIBRARY_ERROR_CODE_LIMIT_REACHED, "too many items in one import", {}, "items");

  // Everything below validates the WHOLE request; nothing is written, queued or created until all of it passes.
  auto job = std::make_shared<detail::ImportJob>();
  job->source = clean_utf8(source.string());
  std::set<std::string> seen;
  const fs::path base = source_is_dir ? source : source.parent_path();
  for (const ws::v1::ImportItem& it : req.items())
    if (it.action() != ws::v1::IMPORT_ACTION_DEFAULT && it.action() != ws::v1::IMPORT_ACTION_IMPORT && it.action() != ws::v1::IMPORT_ACTION_SKIP)
      return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND, "unknown import action " + std::to_string(static_cast<int>(it.action())), {}, "unknown_action");
  std::uint32_t skipped_by_action = 0;
  for (const ws::v1::ImportItem& it : req.items()) {
    if (it.action() == ws::v1::IMPORT_ACTION_SKIP) {  // Story 5.7: left out of the request (no category needed, no space counted)
      ++skipped_by_action;
      continue;
    }
    fs::path p;
    if (it.path().empty()) {
      if (source_is_dir) return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "an item of a folder import needs a path", {}, "item_path_missing");
      p = source;
    } else {
      if (it.path().size() > 4096 || it.path().find('\0') != std::string::npos)
        return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "the item path is too long or holds a NUL byte", {}, "item_unreadable");
      fs::path q(it.path());
      if (q.is_relative()) q = base / q;
      p = fs::canonical(q, ec);
      if (ec) return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "the item cannot be read: " + ec.message(), it.path(), "item_unreadable");
    }
    const fs::path rel = p.lexically_relative(source_is_dir ? source : source);
    const bool inside = source_is_dir ? (!rel.empty() && *rel.begin() != "..") : p == source;
    if (!inside) return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "the item lies outside the import source", it.path(), "outside_source");
    if (!fs::is_regular_file(p, ec) || !importable_extension(p))
      return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "the item is not an .sfz / .sf2 file", p.string(), "not_importable");
    if (::access(p.c_str(), R_OK) != 0)
      return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "the item cannot be read", p.string(), "item_unreadable");
    if (!seen.insert(p.string()).second)
      return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "the item is listed twice", p.string(), "duplicate_item");
    if (!valid_display_name(it.name()))
      return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND, "invalid display name", p.string(), "invalid_name");
    detail::JobItem ji;
    ji.path = p.string();
    // The category rule (Story 5.6): every item needs one; "none" and unknown ids are refused as typed CATEGORY_REQUIRED errors.
    if (!it.has_category())
      return refuse(ws::v1::LIBRARY_ERROR_CODE_CATEGORY_REQUIRED, "every imported instrument needs a category", p.string(), "missing");
    if (it.category() == 0)
      return refuse(ws::v1::LIBRARY_ERROR_CODE_CATEGORY_REQUIRED, "\"none\" is not a category an instrument can carry", p.string(), "none");
    const auto cat = assignable_from_id(it.category());
    if (!cat) return refuse(ws::v1::LIBRARY_ERROR_CODE_CATEGORY_REQUIRED, "unknown category id " + std::to_string(it.category()), p.string(), "unknown");
    ji.category_key = std::string(fmtlib::key(*cat));
    ji.name = it.name();
    std::set<std::string> keys;
    for (const ws::v1::ImportInstrumentOverride& ov : it.instruments()) {
      if (ov.key().empty() || ov.key().size() > 256 || !keys.insert(ov.key()).second)
        return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND, "invalid or repeated instrument key in an override", p.string(), "invalid_instrument_key");
      if (!valid_display_name(ov.name()))
        return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND, "invalid display name", p.string(), "invalid_name");
      ImportOptions::InstrumentMeta im;
      im.key = ov.key();
      im.name = ov.name();
      if (ov.has_category()) {
        if (ov.category() == 0)
          return refuse(ws::v1::LIBRARY_ERROR_CODE_CATEGORY_REQUIRED, "\"none\" is not a category an instrument can carry (" + ov.key() + ")", p.string(), "none");
        const auto oc = assignable_from_id(ov.category());
        if (!oc) return refuse(ws::v1::LIBRARY_ERROR_CODE_CATEGORY_REQUIRED, "unknown category id " + std::to_string(ov.category()) + " for " + ov.key(), p.string(), "unknown");
        im.category = std::string(fmtlib::key(*oc));
      }
      ji.meta.push_back(std::move(im));
    }
    struct stat sb{};
    ji.bytes = ::stat(p.c_str(), &sb) == 0 ? static_cast<std::uint64_t>(sb.st_size) : 0;
    job->bytes_total += ji.bytes;
    job->items.push_back(std::move(ji));
  }
  if (job->items.empty())
    return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND, "every item of the request has the action skip", {}, "nothing_to_import");
  // Space (Story 5.7): the decoded size of what the request would write. A figure from the preflight is reused only while every file it depends on (sfz,
  // includes, samples) is unchanged; otherwise a headers-only estimate is made here, on the service thread, for at most kMaxEstimateMisses items per request
  // (more: LIMIT_REACHED reason estimate_budget, run a preflight first or split the request) and at most kMaxDepStats stats of cached dependencies. A file the
  // estimate calls skipped is charged its own size, never nothing. The estimate is not confined to the source root (the importer accepts samples elsewhere).
  std::uint64_t pcm_total = 0, free_now = 0;
  std::size_t misses = 0, stats_left = kMaxDepStats;
  for (detail::JobItem& ji : job->items) {
    bool hit = false;
    const auto cached = estimates_.find(ji.path);
    if (cached != estimates_.end() && cached->second.deps.size() <= stats_left) {
      stats_left -= cached->second.deps.size();
      hit = deps_unchanged(cached->second.deps);
      if (hit) pcm_total += cached->second.pcm;
    }
    if (hit) continue;
    if (++misses > kMaxEstimateMisses)
      return refuse(ws::v1::LIBRARY_ERROR_CODE_LIMIT_REACHED, "too many files without a fresh preflight estimate; run a preflight on the source or split the request", {}, "estimate_budget");
    ImportEstimate e = estimate_import(ji.path);
    pcm_total += e.skipped ? ji.bytes : e.pcm_bytes;
    if (!e.skipped) {
      if (estimates_.size() >= kMaxEstimates) estimates_.clear();
      estimates_[ji.path] = Estimate{e.pcm_bytes, std::move(e.deps)};
    }
  }
  const std::uint64_t required = required_import_bytes(pcm_total, job->items.size());
  const bool free_known = free_bytes(free_now);
  if (free_known && (required > free_now || reserved_bytes_ > free_now - required)) {
    auto e = typed_error(rid, ws::v1::LIBRARY_ERROR_CODE_INSUFFICIENT_SPACE, "not enough free space for this import", {}, "space");
    (*e.mutable_error()->mutable_details())["required_bytes"] = std::to_string(required);
    (*e.mutable_error()->mutable_details())["free_bytes"] = std::to_string(free_now);
    (*e.mutable_error()->mutable_details())["reserved_bytes"] = std::to_string(reserved_bytes_);  // already promised to queued / running imports
    send(c, e);
    return;
  }
  job->id = next_job_id_;
  job->owner = c.id;
  std::size_t ahead = 0;
  if (!worker_ || !worker_->enqueue(job, opts_.max_queued_jobs, ahead)) {
    if (stopping_.load() || (worker_ && worker_->stopping())) {  // stop() began between the check above and here
      send(c, typed_error(rid, ws::v1::LIBRARY_ERROR_CODE_SHUTTING_DOWN, "the service is stopping and accepts no new import", {}, {}));
      return;
    }
    auto e = typed_error(rid, ws::v1::LIBRARY_ERROR_CODE_QUEUE_FULL, "too many import jobs are waiting", {}, {});
    (*e.mutable_error()->mutable_details())["limit"] = std::to_string(opts_.max_queued_jobs);
    send(c, e);
    return;
  }
  ++next_job_id_;
  job->reserved = required;
  reserved_bytes_ += required;
  jobs_[job->id] = job;
  ws::v1::LibraryServerMessage m;
  m.set_request_id(rid);
  ws::v1::ImportAccepted* a = m.mutable_import_accepted();
  a->set_job_id(job->id);
  a->set_item_count(static_cast<std::uint32_t>(job->items.size()));
  a->set_bytes_total(job->bytes_total);
  a->set_queue_position(static_cast<std::uint32_t>(ahead));
  a->set_skipped_count(skipped_by_action);
  a->set_required_bytes(required);
  a->set_free_bytes(free_known ? free_now : 0);
  a->set_free_known(free_known);
  send(c, m);
  push_job_event(*job, nullptr);  // QUEUED
  trim_finished_jobs();
}

void LibraryService::release_reservation(detail::ImportJob& job) {
  reserved_bytes_ -= std::min(job.reserved, reserved_bytes_);
  job.reserved = 0;
}

void LibraryService::trim_finished_jobs() {
  std::size_t finished = 0;
  for (const auto& kv : jobs_)
    if (kv.second->finished()) ++finished;
  for (auto it = jobs_.begin(); finished > kMaxFinishedJobs && it != jobs_.end();) {
    if (it->second->finished()) {
      it = jobs_.erase(it);
      --finished;
    } else {
      ++it;
    }
  }
}

void LibraryService::handle_cancel_import(Client& c, std::uint64_t rid, std::uint64_t job_id) {
  const auto it = jobs_.find(job_id);
  if (it == jobs_.end()) return reply_error(c, rid, ws::v1::LIBRARY_ERROR_CODE_UNKNOWN_JOB, "no such import job");
  detail::ImportJob& job = *it->second;
  if (job.finished()) return reply_ack(c, rid, false);
  job.cancel.store(true);
  if (job.state == ws::v1::IMPORT_STATE_QUEUED && worker_ && worker_->remove_queued(it->second)) {
    // It never started: finished right here.
    job.state = ws::v1::IMPORT_STATE_CANCELLED;
    job.current_item.clear();
    release_reservation(job);
    reply_ack(c, rid, true);
    push_job_event(job, nullptr);
    point("job-finished", job.id, 0);
    return;
  }
  reply_ack(c, rid, true);  // running (or just taken by the worker): it stops at its next safe point and the terminal event follows
}

void LibraryService::handle_list_libraries(Client& c, std::uint64_t rid, const ws::v1::ListLibraries& req) {
  std::vector<LibraryRecord> rows;
  const CatalogStatus st = catalog_->list_libraries(rows, false);
  if (st != CatalogStatus::kOk) {
    auto e = make_error(rid, ws::v1::LIBRARY_ERROR_CODE_STORAGE_FAILED, "the catalog could not be read");
    (*e.mutable_error()->mutable_details())["reason"] = to_string(st);
    send(c, e);
    return;
  }
  ws::v1::LibraryServerMessage m;
  m.set_request_id(rid);
  ws::v1::LibraryList* l = m.mutable_library_list();
  l->set_total(static_cast<std::uint32_t>(rows.size()));
  l->set_offset(req.offset());
  const std::size_t limit = req.limit() == 0 ? 500 : std::min<std::size_t>(req.limit(), 2000);
  std::size_t bytes = 0, n = 0, i = req.offset();
  for (; i < rows.size() && n < limit; ++i, ++n) {
    ws::v1::LibraryInfo* li = l->add_libraries();
    li->set_library_id(rows[i].library_id);
    li->set_name(clean_utf8(rows[i].name, 512));
    li->set_source_kind(rows[i].source_kind);
    li->set_source_path(clean_utf8(rows[i].source_path));
    li->set_bundle_id(rows[i].bundle_id);
    li->set_instrument_count(rows[i].instrument_count());
    li->set_imported_seq(static_cast<std::uint64_t>(rows[i].imported_seq));
    bytes += li->ByteSizeLong() + 2;
    if (bytes > opts_.reply_budget_bytes && l->libraries_size() > 1) {  // keep the reply a frame the peer accepts (at least one entry: progress); `more` says ask again
      l->mutable_libraries()->RemoveLast();
      break;
    }
  }
  l->set_more(i < rows.size());
  send(c, m);
}

void LibraryService::handle_list_instruments(Client& c, std::uint64_t rid, const ws::v1::ListInstruments& req) {
  std::vector<InstrumentRecord> rows;
  std::uint32_t total = 0;
  bool found = false;
  const std::uint32_t limit = req.limit() == 0 ? 500 : std::min<std::uint32_t>(req.limit(), 2000);
  const CatalogStatus st = catalog_->list_instruments(req.library_id(), req.offset(), limit, rows, total, found);
  if (st != CatalogStatus::kOk) {
    auto e = make_error(rid, ws::v1::LIBRARY_ERROR_CODE_STORAGE_FAILED, "the catalog could not be read");
    (*e.mutable_error()->mutable_details())["reason"] = to_string(st);
    send(c, e);
    return;
  }
  if (!found) {
    auto e = make_error(rid, ws::v1::LIBRARY_ERROR_CODE_UNKNOWN_LIBRARY, "no such library");
    (*e.mutable_error()->mutable_details())["library_id"] = req.library_id().substr(0, 64);
    send(c, e);
    return;
  }
  ws::v1::LibraryServerMessage m;
  m.set_request_id(rid);
  ws::v1::InstrumentList* l = m.mutable_instrument_list();
  l->set_library_id(req.library_id());
  l->set_total(total);
  l->set_offset(req.offset());
  std::size_t bytes = 0, used = 0;
  for (const InstrumentRecord& r : rows) {
    ws::v1::InstrumentInfo* ii = l->add_instruments();
    ii->set_instrument_key(clean_utf8(r.key, 512));
    ii->set_name(clean_utf8(r.name, 512));
    ii->set_category(r.category);
    ii->set_sample_count(r.sample_count);
    ii->set_region_count(r.region_count);
    ii->set_bytes(r.bytes);
    bytes += ii->ByteSizeLong() + 2;
    if (bytes > opts_.reply_budget_bytes && l->instruments_size() > 1) {
      l->mutable_instruments()->RemoveLast();
      break;
    }
    ++used;
  }
  l->set_more(static_cast<std::uint64_t>(req.offset()) + used < total);
  send(c, m);
}

void LibraryService::push_job_event(detail::ImportJob& job, const ws::v1::ImportItemResult* item) {
  ws::v1::ImportProgress p;
  p.set_job_id(job.id);
  p.set_event_seq(++job.event_seq);
  p.set_state(job.state);
  p.set_items_total(static_cast<std::uint32_t>(job.items.size()));
  p.set_items_done(job.done);
  p.set_items_imported(job.imported);
  p.set_items_unchanged(job.unchanged);
  p.set_items_skipped(job.skipped);
  p.set_items_failed(job.failed);
  p.set_items_cancelled(job.cancelled);
  p.set_current_item(clean_utf8(job.current_item));
  p.set_bytes_total(job.bytes_total);
  p.set_bytes_done(job.bytes_done);
  job.last_event = p;  // the status answer carries no item result
  if (item) *p.mutable_item() = *item;
  ws::v1::LibraryServerMessage m;
  *m.mutable_import_progress() = std::move(p);
  for (Client& c : clients_)
    if (!c.dead && !c.closing && (c.id == job.owner || c.subscribed)) send(c, m);
}

void LibraryService::process_worker() {
  if (!worker_) return;
  for (detail::WorkerMsg& m : worker_->drain()) on_worker_msg(m);
}

void LibraryService::finish_job(detail::ImportJob& job, bool all_items_processed) {
  release_reservation(job);
  job.current_item.clear();
  if (!all_items_processed || job.cancelled > 0) job.state = ws::v1::IMPORT_STATE_CANCELLED;
  else job.state = (job.imported + job.unchanged) > 0 ? ws::v1::IMPORT_STATE_PUBLISHED : ws::v1::IMPORT_STATE_FAILED;
  push_job_event(job, nullptr);
  point("job-finished", job.id, 0);
}

void LibraryService::on_worker_msg(detail::WorkerMsg& m) {
  if (m.kind == detail::WorkerMsg::Kind::kPreflightDone) return on_preflight_done(*m.pre, m.pre);
  detail::ImportJob& job = *m.job;
  switch (m.kind) {
    case detail::WorkerMsg::Kind::kJobStarted:
      job.state = ws::v1::IMPORT_STATE_RUNNING;
      push_job_event(job, nullptr);
      return;
    case detail::WorkerMsg::Kind::kItemStarted:
      job.state = ws::v1::IMPORT_STATE_RUNNING;
      job.current_item = job.items[m.index].path;
      push_job_event(job, nullptr);
      return;
    case detail::WorkerMsg::Kind::kJobDone:
      finish_job(job, job.done >= job.items.size());
      return;
    case detail::WorkerMsg::Kind::kPreflightDone:
      return;  // handled above
    case detail::WorkerMsg::Kind::kItemDone:
      break;
  }
  // An item the worker has finished. Order of the story: the bundle is already published (verified, fsynced, renamed, `current` flipped); only now
  // do the catalog rows go in, in one transaction. A failure here leaves a published library that the next start's reconcile pass lists.
  const detail::JobItem& it = job.items[m.index];
  const ImportResult& r = m.result;
  ws::v1::ImportItemResult ir;
  ir.set_path(clean_utf8(it.path));
  ir.set_library_id(r.library_id);
  ir.set_bundle_id(r.bundle_id);
  ir.set_samples(static_cast<std::uint32_t>(r.sample_count));
  ir.set_regions(static_cast<std::uint32_t>(r.region_count));
  ir.set_warning_count(static_cast<std::uint32_t>(r.warnings.size()));
  ir.set_allowlisted_count(static_cast<std::uint32_t>(r.allowlisted.size()));
  ir.set_suggested_category(static_cast<std::uint32_t>(fmtlib::id(r.suggested_category)));
  // Bounded, valid UTF-8: a file with thousands of offenders must not make the ImportProgress frame exceed the frame limit.
  for (std::size_t i = 0; i < r.reasons.size() && i < kMaxReportEntries; ++i) ir.add_messages(clean_utf8(r.reasons[i], kMaxReportString));
  if (r.reasons.size() > kMaxReportEntries) ir.add_messages("... " + std::to_string(r.reasons.size() - kMaxReportEntries) + " more");
  for (std::size_t i = 0; i < r.offenders.size() && i < kMaxReportEntries; ++i) {
    const Offender& o = r.offenders[i];
    ws::v1::ImportOffender* po = ir.add_offenders();
    po->set_opcode(clean_utf8(o.opcode, kMaxReportString));
    po->set_file(clean_utf8(o.file, kMaxReportString));
    po->set_line(static_cast<std::uint32_t>(o.line));
    po->set_message(clean_utf8(o.message, kMaxReportString));
  }
  if (r.ok()) {
    if (!m.have_record) {
      ir.set_status(ws::v1::IMPORT_ITEM_FAILED);
      ir.set_detail(clean_utf8("the published bundle could not be described: " + m.describe_error, 1024));
    } else {
      cur_job_ = job.id;
      cur_item_ = m.index;
      bool changed = false;
      const CatalogStatus st = catalog_->publish_library(m.record, changed);
      sync_counters();
      if (st != CatalogStatus::kOk) {
        ir.set_status(ws::v1::IMPORT_ITEM_FAILED);
        ir.set_detail(std::string("the library is published but the catalog write failed (") + to_string(st) + "); the next start lists it");
      } else {
        point("catalog-committed", job.id, m.index);
        ir.set_status(r.bundle_created || r.current_flipped ? ws::v1::IMPORT_ITEM_IMPORTED : ws::v1::IMPORT_ITEM_UNCHANGED);
        ir.set_instruments(m.record.instrument_count());
      }
    }
  } else if (r.status == Status::kSkipped) {
    ir.set_status(ws::v1::IMPORT_ITEM_SKIPPED);
    ir.set_reason(r.reason_code == ReasonCode::kDamaged ? ws::v1::IMPORT_REASON_DAMAGED : ws::v1::IMPORT_REASON_UNSUPPORTED_FEATURES);
    ir.set_failing_path(clean_utf8(r.failing_path));
  } else if (r.status == Status::kAborted) {
    ir.set_status(ws::v1::IMPORT_ITEM_CANCELLED);
  } else {
    ir.set_status(ws::v1::IMPORT_ITEM_FAILED);
    ir.set_detail(clean_utf8(std::string(to_string(r.status)) + (r.reasons.empty() ? std::string() : ": " + r.reasons.front()), 1024));
  }
  switch (ir.status()) {
    case ws::v1::IMPORT_ITEM_IMPORTED: ++job.imported; break;
    case ws::v1::IMPORT_ITEM_UNCHANGED: ++job.unchanged; break;
    case ws::v1::IMPORT_ITEM_SKIPPED: ++job.skipped; break;
    case ws::v1::IMPORT_ITEM_CANCELLED: ++job.cancelled; break;
    default: ++job.failed; break;
  }
  ++job.done;
  job.bytes_done += it.bytes;
  job.state = ws::v1::IMPORT_STATE_RUNNING;
  push_job_event(job, &ir);
  if (m.ticket) {
    std::lock_guard<std::mutex> lk(m.ticket->m);
    m.ticket->done = true;
    m.ticket->cv.notify_all();
  }
}

// --- Story 5.7: import preflight ---------------------------------------------------------------------------------------------------------------

bool LibraryService::free_bytes(std::uint64_t& out) const {
  if (opts_.free_bytes_fn) return opts_.free_bytes_fn(opts_.data_dir, out);
  struct statvfs sv{};
  if (::statvfs(opts_.data_dir.c_str(), &sv) != 0) return false;
  out = static_cast<std::uint64_t>(sv.f_bavail) * static_cast<std::uint64_t>(sv.f_frsize);
  return true;
}

void LibraryService::handle_preflight_request(Client& c, std::uint64_t rid, const ws::v1::PreflightRequest& req) {
  if (stopping_.load()) {
    send(c, typed_error(rid, ws::v1::LIBRARY_ERROR_CODE_SHUTTING_DOWN, "the service is stopping and accepts no new preflight", {}, {}));
    return;
  }
  auto refuse = [&](ws::v1::LibraryErrorCode code, const std::string& message, const std::string& path, const std::string& reason) {
    send(c, typed_error(rid, code, message, path, reason));
  };
  if (req.source().empty() || req.source().size() > 4096 || req.source().find('\0') != std::string::npos)
    return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "the preflight source is empty or too long", {}, "source_missing");
  if (!valid_display_name(req.volume_label()))
    return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND, "invalid volume label", {}, "invalid_name");
  std::error_code ec;
  const fs::path source = fs::canonical(fs::path(req.source()), ec);
  if (ec) return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "the preflight source cannot be read: " + ec.message(), req.source(), "source_unreadable");
  if (!fs::is_directory(source, ec))
    return refuse(ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, "the preflight source is not a folder", req.source(), "not_a_folder");
  {
    std::size_t mine = 0;
    for (const auto& kv : pending_preflights_) mine += kv.second->owner == c.id ? 1 : 0;
    if (mine >= opts_.max_preflights_per_client) {
      auto e = typed_error(rid, ws::v1::LIBRARY_ERROR_CODE_QUEUE_FULL, "this connection already has too many preflight scans waiting", {}, "per_client");
      (*e.mutable_error()->mutable_details())["limit"] = std::to_string(opts_.max_preflights_per_client);
      send(c, e);
      return;
    }
  }
  auto task = std::make_shared<detail::PreflightTask>();
  task->id = next_preflight_id_;
  task->owner = c.id;
  task->request_id = rid;
  task->source = source.string();
  task->label = req.volume_label();
  task->max_files = req.max_files();
  if (!worker_ || !worker_->enqueue_preflight(task, opts_.max_queued_preflights)) {
    if (stopping_.load() || (worker_ && worker_->stopping()))
      return refuse(ws::v1::LIBRARY_ERROR_CODE_SHUTTING_DOWN, "the service is stopping and accepts no new preflight", {}, {});
    auto e = typed_error(rid, ws::v1::LIBRARY_ERROR_CODE_QUEUE_FULL, "too many preflight scans are waiting", {}, {});
    (*e.mutable_error()->mutable_details())["limit"] = std::to_string(opts_.max_queued_preflights);
    send(c, e);
    return;
  }
  ++next_preflight_id_;
  pending_preflights_[task->id] = task;  // the answer (PreflightDone) is sent when the worker has finished the scan
}

void LibraryService::handle_cancel_preflight(Client& c, std::uint64_t rid, std::uint64_t request_id) {
  bool found = false;
  for (auto& kv : pending_preflights_)
    if (kv.second->owner == c.id && kv.second->request_id == request_id) {
      kv.second->cancel.store(true);
      found = true;
    }
  reply_ack(c, rid, found);
}

void LibraryService::cancel_preflights_of(std::uint64_t client_id) {
  for (auto& kv : pending_preflights_)
    if (kv.second->owner == client_id) kv.second->cancel.store(true);
}

void LibraryService::on_preflight_done(detail::PreflightTask& t, const std::shared_ptr<detail::PreflightTask>& sp) {
  pending_preflights_.erase(t.id);
  Client* owner = nullptr;
  for (Client& c : clients_)
    if (c.id == t.owner && !c.dead && !c.closing) owner = &c;
  if (!owner) return;  // the requester is gone: nothing to answer, nothing kept
  PreflightResult& r = t.result;
  if (!r.ok) {
    send(*owner, typed_error(t.request_id, ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE, r.error, t.source, "source_unreadable"));
    return;
  }
  ws::v1::LibraryServerMessage m;
  m.set_request_id(t.request_id);
  ws::v1::PreflightDone* d = m.mutable_preflight_done();
  d->set_source(clean_utf8(r.source));
  d->set_volume_label(clean_utf8(r.volume_label, 255));
  d->set_files_scanned(static_cast<std::uint32_t>(r.files_scanned));
  d->set_symlinks_skipped(static_cast<std::uint32_t>(r.symlinks_skipped));
  d->set_bytes_read(r.bytes_read);
  d->set_duration_ms(r.duration_ms);
  d->set_truncated(r.truncated);
  if (r.cancelled) {
    d->set_cancelled(true);
    send(*owner, m);
    return;
  }
  // `already imported`: the catalog holds the library_id AND the instrument key (the importer's key: the SFZ file name, or the SF2 "bank:preset"). One query
  // reads every library with its instruments (bounded by the size of the catalog, on the service thread; the per-row work below is a set lookup).
  std::vector<LibraryRecord> libs;
  if (const CatalogStatus st = catalog_->list_libraries(libs, true); st != CatalogStatus::kOk) {
    auto e = make_error(t.request_id, ws::v1::LIBRARY_ERROR_CODE_STORAGE_FAILED, "the catalog could not be read");
    (*e.mutable_error()->mutable_details())["reason"] = to_string(st);
    send(*owner, e);
    return;
  }
  std::map<std::string, std::set<std::string>> keys;
  {
    std::set<std::string> wanted;
    for (const PreflightRow& row : r.rows) wanted.insert(row.library_id);
    for (const LibraryRecord& l : libs)
      if (wanted.count(l.library_id))
        for (const InstrumentRecord& ir : l.instruments) keys[l.library_id].insert(ir.key);
  }
  libs.clear();
  for (PreflightRow& row : r.rows) {
    if ((row.status != RowStatus::kReady && row.status != RowStatus::kNeedsCategory) || row.instrument_key.empty()) continue;
    const auto it = keys.find(row.library_id);
    if (it != keys.end() && it->second.count(row.instrument_key)) row.status = RowStatus::kAlreadyImported;
  }
  std::vector<ws::v1::PreflightLibrary> sums(r.libraries.size());
  for (std::size_t i = 0; i < sums.size(); ++i) {
    sums[i].set_index(static_cast<std::uint32_t>(i));
    sums[i].set_name(clean_utf8(r.libraries[i].name, 255));
    sums[i].set_folder(clean_utf8(r.libraries[i].folder, 255));
  }
  std::uint64_t pcm = 0;
  std::set<std::string> import_files;
  std::map<std::string, std::uint64_t> file_pcm;
  std::set<std::string> skipped_files;
  std::uint32_t n_ready = 0, n_needs = 0, n_unsup = 0, n_dam = 0, n_imp = 0;
  for (const PreflightRow& row : r.rows) {
    ws::v1::PreflightLibrary& s = sums[row.library];
    s.set_rows(s.rows() + 1);
    switch (row.status) {
      case RowStatus::kReady: s.set_ready(s.ready() + 1); ++n_ready; break;
      case RowStatus::kNeedsCategory: s.set_needs_category(s.needs_category() + 1); ++n_needs; break;
      case RowStatus::kSkippedUnsupportedFeatures: s.set_skipped_unsupported_features(s.skipped_unsupported_features() + 1); ++n_unsup; break;
      case RowStatus::kSkippedDamaged: s.set_skipped_damaged(s.skipped_damaged() + 1); ++n_dam; break;
      case RowStatus::kAlreadyImported: s.set_already_imported(s.already_imported() + 1); ++n_imp; break;
    }
    if (row.status == RowStatus::kReady || row.status == RowStatus::kNeedsCategory) {
      pcm += row.pcm_bytes;
      import_files.insert(row.path);
    }
    if (row.status == RowStatus::kSkippedDamaged || row.status == RowStatus::kSkippedUnsupportedFeatures) skipped_files.insert(row.path);
    file_pcm[row.path] += row.pcm_bytes;
  }
  // Remember the decoded size per importable file for the space check of the ImportRequest that follows (the worker already read the size and mtime of every
  // file the figure depends on, so nothing is stat'ed here). Skipped files are not remembered.
  if (estimates_.size() + file_pcm.size() > kMaxEstimates) estimates_.clear();
  for (const auto& kv : file_pcm) {
    if (skipped_files.count(kv.first)) continue;
    const auto dep = r.deps.find(kv.first);
    if (dep == r.deps.end()) continue;
    estimates_[kv.first] = Estimate{kv.second, dep->second};
  }
  for (std::size_t i = 0; i < sums.size() && i < kMaxLibrariesInDone; ++i) *d->add_libraries() = sums[i];
  d->set_libraries_total(static_cast<std::uint32_t>(sums.size()));
  d->set_rows_total(static_cast<std::uint32_t>(r.rows.size()));
  d->set_ready(n_ready);
  d->set_needs_category(n_needs);
  d->set_skipped_unsupported_features(n_unsup);
  d->set_skipped_damaged(n_dam);
  d->set_already_imported(n_imp);
  d->set_pcm_bytes(pcm);
  d->set_reserve_bytes(kImportReserveBytes);
  const std::uint64_t required = required_import_bytes(pcm, import_files.size());
  d->set_required_bytes(required);
  std::uint64_t free_now = 0;
  const bool known_free = free_bytes(free_now);
  d->set_free_bytes(known_free ? free_now : 0);
  d->set_free_known(known_free);
  d->set_fits(known_free && required <= free_now);
  d->set_preflight_id(t.id);
  preflights_[t.id] = sp;
  while (preflights_.size() > kMaxStoredPreflights) preflights_.erase(preflights_.begin());
  send(*owner, m);
}

void LibraryService::handle_list_preflight_rows(Client& c, std::uint64_t rid, const ws::v1::ListPreflightRows& req) {
  const auto it = preflights_.find(req.preflight_id());
  if (it == preflights_.end()) {
    auto e = make_error(rid, ws::v1::LIBRARY_ERROR_CODE_UNKNOWN_PREFLIGHT, "no such preflight result");
    (*e.mutable_error()->mutable_details())["preflight_id"] = std::to_string(req.preflight_id());
    send(c, e);
    return;
  }
  const PreflightResult& r = it->second->result;
  // The rows of one library are contiguous; without a filter the whole list is paged.
  std::size_t first = 0, count = r.rows.size();
  if (req.has_library()) {
    if (req.library() >= r.libraries.size()) {
      count = 0;
    } else {
      first = r.libraries[req.library()].first_row;
      count = r.libraries[req.library()].row_count;
    }
  }
  ws::v1::LibraryServerMessage m;
  m.set_request_id(rid);
  ws::v1::PreflightRowList* l = m.mutable_preflight_rows();
  l->set_preflight_id(req.preflight_id());
  l->set_total(static_cast<std::uint32_t>(count));
  l->set_offset(req.offset());
  const std::size_t limit = req.limit() == 0 ? 500 : std::min<std::size_t>(req.limit(), 2000);
  std::size_t bytes = 0, used = 0;
  for (std::size_t i = req.offset(); i < count && used < limit; ++i) {
    const PreflightRow& row = r.rows[first + i];
    ws::v1::PreflightRow* pr = l->add_rows();
    pr->set_path(clean_utf8(row.path));
    pr->set_rel_path(clean_utf8(row.rel_path));
    pr->set_name(clean_utf8(row.name, 512));
    pr->set_format(row.format);
    pr->set_library_id(row.library_id);
    pr->set_instrument_key(clean_utf8(row.instrument_key, 512));
    pr->set_size_bytes(row.size_bytes);
    pr->set_pcm_bytes(row.pcm_bytes);
    pr->set_suggested_category(static_cast<std::uint32_t>(fmtlib::id(row.suggested)));
    ws::v1::PreflightStatus st = ws::v1::PREFLIGHT_STATUS_READY;
    switch (row.status) {
      case RowStatus::kReady: st = ws::v1::PREFLIGHT_STATUS_READY; break;
      case RowStatus::kNeedsCategory: st = ws::v1::PREFLIGHT_STATUS_NEEDS_CATEGORY; break;
      case RowStatus::kSkippedUnsupportedFeatures: st = ws::v1::PREFLIGHT_STATUS_SKIPPED_UNSUPPORTED_FEATURES; break;
      case RowStatus::kSkippedDamaged: st = ws::v1::PREFLIGHT_STATUS_SKIPPED_DAMAGED; break;
      case RowStatus::kAlreadyImported: st = ws::v1::PREFLIGHT_STATUS_ALREADY_IMPORTED; break;
    }
    pr->set_status(st);
    pr->set_action(row.status == RowStatus::kReady || row.status == RowStatus::kNeedsCategory ? ws::v1::IMPORT_ACTION_IMPORT : ws::v1::IMPORT_ACTION_SKIP);
    pr->set_library(static_cast<std::uint32_t>(row.library));
    pr->set_detail(clean_utf8(row.detail, 1024));
    bytes += pr->ByteSizeLong() + 2;
    if (bytes > opts_.reply_budget_bytes && l->rows_size() > 1) {
      l->mutable_rows()->RemoveLast();
      break;
    }
    ++used;
  }
  l->set_more(static_cast<std::uint64_t>(req.offset()) + used < count);
  send(c, m);
}

}  // namespace velora::librarian
