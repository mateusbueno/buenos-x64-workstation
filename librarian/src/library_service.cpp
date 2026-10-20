#include "velora/librarian/library_service.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <grp.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

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
  CatalogStatus st = Catalog::open(opts_.data_dir, catalog_);
  if (status) *status = st;
  if (st != CatalogStatus::kOk) {
    err = std::string("catalog: ") + to_string(st);
    return false;
  }
  if (fault_hook_) catalog_->set_fault_hook(fault_hook_);
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

  sockaddr_un addr{};
  if (opts_.socket_path.empty() || opts_.socket_path.size() >= sizeof(addr.sun_path)) {
    err = "socket path is empty or too long: " + opts_.socket_path;
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
  started_ = true;
  thread_ = std::thread([this] { run(); });
  return true;
}

void LibraryService::stop() {
  if (!started_) return;
  started_ = false;
  stop_.store(true);
  const char b = 1;
  [[maybe_unused]] const ssize_t w = ::write(wake_[1], &b, 1);
  if (thread_.joinable()) thread_.join();
  for (Client& c : clients_) ::close(c.fd);
  clients_.clear();
  if (recent_dirty_) flush_recent(nullptr);  // orderly shutdown: the last chance to persist Recent, playing or not
  sync_counters();
  catalog_.reset();  // checkpoints the WAL and releases the writer lock
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
    for (const char* f : {"library.v1", "favorites.v1", "recent.v1", "subscribe.v1"}) h->add_features(f);
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

}  // namespace velora::librarian
