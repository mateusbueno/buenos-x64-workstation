#include "velora/sysd/bridge_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "library_api.pb.h"
#include "velora/host/control_plane.hpp"  // check_protocol_version, min_protocol_version

namespace velora::sysd {

namespace {

using B = ws::v1::ClientMessage;

constexpr std::uint32_t kProtocolVersion = 1;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

int set_nonblocking(int fd) {
  const int fl = fcntl(fd, F_GETFL, 0);
  return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

std::uint32_t be32(const unsigned char* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }

void append_frame(std::string& tx, const std::string& body) {
  const std::uint32_t len = static_cast<std::uint32_t>(body.size());
  const char hdr[4] = {char(len >> 24), char(len >> 16), char(len >> 8), char(len)};
  tx.append(hdr, 4);
  tx.append(body);
}

}  // namespace

const std::vector<BridgeServer::MessageRule>& BridgeServer::message_rules() {
  using R = Route;
  // body_case, name, feature, route, unpaired_allowed. NO default: a ClientMessage body that is not in this table is INVALID_COMMAND, and the descriptor test in
  // sysd/tests/bridge_test.cpp fails when a field is added to ClientMessage without a row here. Hardware / panel commands are R::kDenied: Studio is a Live Set /
  // setlist / scene / edit / state / library / config client and never drives the panel, pedals or encoders through the bridge.
  static const std::vector<MessageRule> t = {
      {B::kHello, "hello", nullptr, R::kLocal, true},
      {B::kPairRequest, "pair_request", nullptr, R::kLocal, true},
      // edits (parameters, chains, timbres, volume)
      {B::kSetInsertChain, "set_insert_chain", "params.v1", R::kEngine, false},
      {B::kSetSendLevels, "set_send_levels", "params.v1", R::kEngine, false},
      {B::kSetPartOutput, "set_part_output", "params.v1", R::kEngine, false},
      {B::kSetMasterParams, "set_master_params", "params.v1", R::kEngine, false},
      {B::kSetTimbreRef, "set_timbre_ref", "params.v1", R::kEngine, false},
      {B::kSetPerformanceParam, "set_performance_param", "params.v1", R::kEngine, false},
      {B::kSetPartTimbre, "set_part_timbre", "params.v1", R::kEngine, false},
      {B::kSetPartVolume, "set_part_volume", "params.v1", R::kEngine, false},
      {B::kSetPartZone, "set_part_zone", "params.v1", R::kEngine, false},
      {B::kSetPartArp, "set_part_arp", "params.v1", R::kEngine, false},
      // state
      {B::kGetState, "get_state", "snapshots.v1", R::kEngine, false},
      {B::kGetPerformanceValues, "get_performance_values", "snapshots.v1", R::kEngine, false},
      {B::kGetDiagnostics, "get_diagnostics", "snapshots.v1", R::kEngine, false},
      {B::kSubscribe, "subscribe", "subscribe.v1", R::kEngine, false},
      // scenes, Live Sets, setlists, tags, Write, the Live Set's ASSIGN map
      {B::kRecallScene, "recall_scene", "liveset.v1", R::kEngine, false},
      {B::kNextScene, "next_scene", "liveset.v1", R::kEngine, false},
      {B::kPreviousScene, "previous_scene", "liveset.v1", R::kEngine, false},
      {B::kJumpToLiveSet, "jump_to_live_set", "liveset.v1", R::kEngine, false},
      {B::kCancelLoad, "cancel_load", "liveset.v1", R::kEngine, false},
      {B::kCreateSetlist, "create_setlist", "liveset.v1", R::kEngine, false},
      {B::kRenameSetlist, "rename_setlist", "liveset.v1", R::kEngine, false},
      {B::kReorderSetlists, "reorder_setlists", "liveset.v1", R::kEngine, false},
      {B::kDeleteSetlist, "delete_setlist", "liveset.v1", R::kEngine, false},
      {B::kAddLiveSetToSetlist, "add_live_set_to_setlist", "liveset.v1", R::kEngine, false},
      {B::kRemoveLiveSetFromSetlist, "remove_live_set_from_setlist", "liveset.v1", R::kEngine, false},
      {B::kReorderLiveSetsInSetlist, "reorder_live_sets_in_setlist", "liveset.v1", R::kEngine, false},
      {B::kCreateLiveSet, "create_live_set", "liveset.v1", R::kEngine, false},
      {B::kDeleteLiveSet, "delete_live_set", "liveset.v1", R::kEngine, false},
      {B::kCreateTag, "create_tag", "liveset.v1", R::kEngine, false},
      {B::kRenameTag, "rename_tag", "liveset.v1", R::kEngine, false},
      {B::kDeleteTag, "delete_tag", "liveset.v1", R::kEngine, false},
      {B::kSetSetlistTags, "set_setlist_tags", "liveset.v1", R::kEngine, false},
      {B::kLoadSetlist, "load_setlist", "liveset.v1", R::kEngine, false},
      {B::kUnloadSetlist, "unload_setlist", "liveset.v1", R::kEngine, false},
      {B::kNextLiveSet, "next_live_set", "liveset.v1", R::kEngine, false},
      {B::kPreviousLiveSet, "previous_live_set", "liveset.v1", R::kEngine, false},
      {B::kGetSetlists, "get_setlists", "liveset.v1", R::kEngine, false},
      {B::kGetLiveSetView, "get_live_set_view", "liveset.v1", R::kEngine, false},  // Story 9.8: READ-ONLY (never loads, publishes or recalls)
      {B::kWriteLiveSet, "write_live_set", "liveset.v1", R::kEngine, false},
      {B::kSetAssignSlot, "set_assign_slot", "liveset.v1", R::kEngine, false},
      {B::kClearAssignSlot, "clear_assign_slot", "liveset.v1", R::kEngine, false},
      // config (in process) and library (allow-listed, see library_rules())
      {B::kGetConfig, "get_config", "config.v1", R::kConfig, false},
      {B::kSetConfig, "set_config", "config.v1", R::kConfig, false},
      {B::kSubscribeConfig, "subscribe_config", "config.v1", R::kConfig, false},
      {B::kLibrary, "library", "library.v1", R::kLibrary, false},
      // hardware, panel and calibration: never through the bridge
      {B::kSetEncoderMode, "set_encoder_mode", nullptr, R::kDenied, false},
      {B::kSelectPart, "select_part", nullptr, R::kDenied, false},
      {B::kSetFaderMode, "set_fader_mode", nullptr, R::kDenied, false},
      {B::kSelectOrganPart, "select_organ_part", nullptr, R::kDenied, false},
      {B::kMoveFader, "move_fader", nullptr, R::kDenied, false},
      {B::kSetLed, "set_led", nullptr, R::kDenied, false},
      {B::kStartPedalCalibration, "start_pedal_calibration", nullptr, R::kDenied, false},
      {B::kFinishPedalCalibration, "finish_pedal_calibration", nullptr, R::kDenied, false},
      {B::kCancelPedalCalibration, "cancel_pedal_calibration", nullptr, R::kDenied, false},
      // Story 9.6: device-side pairing management (sysd UDS only, denied over TCP bridge)
      {B::kStartPairingWindow, "start_pairing_window", nullptr, R::kDenied, false},
      {B::kCancelPairingWindow, "cancel_pairing_window", nullptr, R::kDenied, false},
      {B::kListPairedComputers, "list_paired_computers", nullptr, R::kDenied, false},
      {B::kForgetPairedComputer, "forget_paired_computer", nullptr, R::kDenied, false},
      {B::kGetPairingStatus, "get_pairing_status", nullptr, R::kDenied, false},
      // Story 9.20: firmware update over USB
      {B::kStageFirmwareChunk, "stage_firmware_chunk", "update.v1", R::kUpdate, false},
      {B::kGetFirmwareUpdateStatus, "get_firmware_update_status", "update.v1", R::kUpdate, false},
      {B::kAbortFirmwareUpdate, "abort_firmware_update", "update.v1", R::kUpdate, false},
      // Story 8.1: on-device install trigger (sysd UDS only, denied over TCP bridge)
      {B::kInstallUpdate, "install_update", nullptr, R::kDenied, false},
  };
  return t;
}

const BridgeServer::MessageRule* BridgeServer::rule_for(int body_case) {
  for (const MessageRule& r : message_rules())
    if (r.body_case == body_case) return &r;
  return nullptr;
}

const std::vector<BridgeServer::LibraryRule>& BridgeServer::library_rules() {
  using L = ws::v1::LibraryClientMessage;
  // Read / safe operations only. Refused: a second LibraryHello (the bridge did it), the engine-to-librarian reports (ReportLiveSetActive, LiveSetDeleted,
  // Reconcile, SetPlaying, FlushRecent), InstallProtected and everything that names absolute device paths (ImportRequest, PreflightRequest, and
  // ListPreflightRows which returns them). The path policy (restricting sources to removable media) is deferred; until it exists the import is not offered.
  static const std::vector<LibraryRule> t = {
      {L::kHello, "hello", false},
      {L::kSubscribe, "subscribe", true},
      {L::kAddFavorite, "add_favorite", true},
      {L::kRemoveFavorite, "remove_favorite", true},
      {L::kGetFavoritesAndRecent, "get_favorites_and_recent", true},
      {L::kReportActive, "report_active", false},
      {L::kLiveSetDeleted, "live_set_deleted", false},
      {L::kReconcile, "reconcile", false},
      {L::kSetPlaying, "set_playing", false},
      {L::kFlushRecent, "flush_recent", false},
      {L::kImportRequest, "import_request", false},
      {L::kListLibraries, "list_libraries", true},
      {L::kListInstruments, "list_instruments", true},
      {L::kCancelImport, "cancel_import", true},
      {L::kImportStatus, "import_status", true},
      {L::kPreflightRequest, "preflight_request", false},
      {L::kListPreflightRows, "list_preflight_rows", false},
      {L::kCancelPreflight, "cancel_preflight", true},  // harmless without a PreflightRequest; its inner request_id is translated anyway
      {L::kInstallProtected, "install_protected", false},
      // Story 9.15: library transfer over USB
      {L::kFreeSpaceRequest, "free_space_request", true},
      {L::kStageChunkRequest, "stage_chunk_request", true},
      {L::kGetTransferStatus, "get_transfer_status", true},
      {L::kAbortTransfer, "abort_transfer", true},
  };
  return t;
}

const std::vector<std::string>& BridgeServer::device_features() {
  static const std::vector<std::string> f = {"params.v1", "snapshots.v1", "subscribe.v1", "liveset.v1", "config.v1", "library.v1", "update.v1"};
  return f;
}

std::int64_t BridgeServer::steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

BridgeServer::BridgeServer(ConfigService& config, PairingWindow& window, PairedStore& paired, BridgeOptions options)
    : svc_(config), window_(window), paired_(paired), opt_(std::move(options)) {}

BridgeServer::~BridgeServer() { stop(); }

bool BridgeServer::start(std::string& err) {
  sockaddr_storage ss{};
  socklen_t sl = 0;
  const int family = opt_.bind_host.find(':') != std::string::npos ? AF_INET6 : AF_INET;
  if (family == AF_INET) {
    auto* a = reinterpret_cast<sockaddr_in*>(&ss);
    a->sin_family = AF_INET;
    a->sin_port = htons(static_cast<std::uint16_t>(opt_.port));
    if (inet_pton(AF_INET, opt_.bind_host.c_str(), &a->sin_addr) != 1) {
      err = "bad bind address (an IPv4 or IPv6 literal is expected): " + opt_.bind_host;
      return false;
    }
    sl = sizeof(sockaddr_in);
  } else {
    auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(static_cast<std::uint16_t>(opt_.port));
    if (inet_pton(AF_INET6, opt_.bind_host.c_str(), &a->sin6_addr) != 1) {
      err = "bad bind address (an IPv4 or IPv6 literal is expected): " + opt_.bind_host;
      return false;
    }
    sl = sizeof(sockaddr_in6);
  }
  listen_fd_ = ::socket(family, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    err = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  auto fail = [&](const std::string& what) {
    err = what;
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (wake_[0] >= 0) ::close(wake_[0]);
    if (wake_[1] >= 0) ::close(wake_[1]);
    listen_fd_ = wake_[0] = wake_[1] = -1;
    return false;
  };
  const int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&ss), sl) != 0) return fail("bind " + opt_.bind_host + ":" + std::to_string(opt_.port) + ": " + std::strerror(errno));
  if (::listen(listen_fd_, 16) != 0 || !set_nonblocking(listen_fd_)) return fail(std::string("listen: ") + std::strerror(errno));
  sockaddr_storage bound{};
  socklen_t bl = sizeof bound;
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &bl) != 0) return fail("getsockname");
  port_ = ntohs(family == AF_INET ? reinterpret_cast<sockaddr_in*>(&bound)->sin_port : reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
  if (::pipe(wake_) != 0) {
    wake_[0] = wake_[1] = -1;
    return fail(std::string("pipe: ") + std::strerror(errno));
  }
  if (!set_nonblocking(wake_[0]) || !set_nonblocking(wake_[1])) return fail("wake pipe");
  stop_.store(false);
  {
    std::lock_guard<std::mutex> lk(inbox_mu_);
    post_closed_ = false;
  }
  thread_ = std::thread([this] { run(); });
  return true;
}

void BridgeServer::stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lk(inbox_mu_);
    post_closed_ = true;  // from here post() is a no-op, so it never touches the wake pipe that is closed below
  }
  stop_.store(true);
  const char b = 1;
  [[maybe_unused]] const ssize_t w = ::write(wake_[1], &b, 1);
  thread_.join();
  for (auto& s : sessions_) drop(*s);
  sessions_.clear();
  session_count_.store(0);
  ::close(listen_fd_);
  ::close(wake_[0]);
  ::close(wake_[1]);
  listen_fd_ = wake_[0] = wake_[1] = -1;
}

void BridgeServer::post(std::vector<Outbound> out) {
  std::lock_guard<std::mutex> lk(inbox_mu_);
  if (post_closed_ || wake_[1] < 0) return;
  for (Outbound& o : out) inbox_.push_back(std::move(o));
  const char b = 1;
  [[maybe_unused]] const ssize_t w = ::write(wake_[1], &b, 1);
}

BridgeServer::Session* BridgeServer::find(ConnId id) {
  for (auto& s : sessions_)
    if (s->id == id) return s.get();
  return nullptr;
}

void BridgeServer::drop(Session& s) {
  cleanup_update_staging(s);
  svc_.close(s.id);
  for (int* fd : {&s.fd, &s.eng.fd, &s.lib.fd}) {
    if (*fd >= 0) ::close(*fd);
    *fd = -1;
  }
}

void BridgeServer::run() {
  struct Ref {
    Session* s;
    int kind;  // 0 client, 1 engine upstream, 2 library upstream
  };
  std::vector<pollfd> fds;
  std::vector<Ref> refs;
  while (!stop_.load()) {
    fds.clear();
    refs.clear();
    fds.push_back({wake_[0], POLLIN, 0});
    fds.push_back({listen_fd_, POLLIN, 0});
    for (auto& sp : sessions_) {
      Session& s = *sp;
      fds.push_back({s.fd, static_cast<short>((s.closing ? 0 : POLLIN) | (s.tx.empty() ? 0 : POLLOUT)), 0});
      refs.push_back({&s, 0});
      if (s.eng.fd >= 0) {
        fds.push_back({s.eng.fd, static_cast<short>(POLLIN | (s.eng.tx.empty() ? 0 : POLLOUT)), 0});
        refs.push_back({&s, 1});
      }
      if (s.lib.fd >= 0) {
        fds.push_back({s.lib.fd, static_cast<short>(POLLIN | (s.lib.tx.empty() ? 0 : POLLOUT)), 0});
        refs.push_back({&s, 2});
      }
    }
    ::poll(fds.data(), fds.size(), 100);
    if (fds[0].revents & POLLIN) {
      char buf[64];
      while (::read(wake_[0], buf, sizeof buf) > 0) {
      }
    }
    {
      std::vector<Outbound> mine;
      {
        std::lock_guard<std::mutex> lk(inbox_mu_);
        mine.swap(inbox_);
      }
      if (!mine.empty()) deliver(mine);
    }
    if (fds[1].revents & POLLIN) accept_clients();
    for (std::size_t i = 0; i < refs.size(); ++i) {
      Session& s = *refs[i].s;
      const short ev = fds[i + 2].revents;
      if (s.dead) continue;
      if (refs[i].kind == 0) {
        if (ev & (POLLERR | POLLNVAL)) s.dead = true;
        else if (ev & (POLLIN | POLLHUP)) read_session(s);
      } else {
        Upstream& up = refs[i].kind == 1 ? s.eng : s.lib;
        if (up.fd < 0) continue;
        if (ev & (POLLERR | POLLNVAL)) upstream_lost(s, refs[i].kind == 1 ? "engine" : "library");
        else if (ev & (POLLIN | POLLHUP)) read_upstream(s, up, refs[i].kind == 2);
      }
    }
    const std::int64_t t = now();
    for (auto& sp : sessions_) {
      Session& s = *sp;
      if (s.dead) continue;
      for (int k = 1; k <= 2 && !s.dead; ++k) {
        Upstream& up = k == 1 ? s.eng : s.lib;
        if (up.fd >= 0 && !up.tx.empty()) {
          bool d = false;
          flush_fd(up.fd, up.tx, d);
          if (d) upstream_lost(s, k == 1 ? "engine" : "library");
        }
      }
      if (!s.dead && !s.tx.empty()) flush_fd(s.fd, s.tx, s.dead);
      if (!s.dead && s.closing && s.tx.empty()) s.dead = true;
      if (!s.handshaken && !s.dead && t - s.accepted_ms > opt_.handshake_timeout_ms) s.dead = true;
      if (s.handshaken && !s.dead && t - s.last_rx_ms > (s.paired ? opt_.idle_timeout_ms : opt_.unpaired_idle_timeout_ms)) {
        log("bridge: idle timeout, closing a session");
        s.dead = true;
      }
      if (!s.dead) sweep_pending(s, t);
    }
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      if ((*it)->dead) {
        drop(**it);
        it = sessions_.erase(it);
      } else {
        ++it;
      }
    }
    session_count_.store(sessions_.size());
  }
}

void BridgeServer::accept_clients() {
  for (;;) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) ::usleep(50'000);
      return;
    }
    if (sessions_.size() >= opt_.max_clients || !set_nonblocking(fd)) {
      over_capacity_.fetch_add(1);
      ::close(fd);
      continue;
    }
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    auto s = std::make_unique<Session>();
    s->fd = fd;
    s->id = next_id_++;
    s->accepted_ms = s->last_rx_ms = s->window_start_ms = now();
    sessions_.push_back(std::move(s));
    accepted_.fetch_add(1);
  }
}

void BridgeServer::flush_fd(int fd, std::string& tx, bool& dead) {
  while (!tx.empty()) {
    const ssize_t n = ::send(fd, tx.data(), tx.size(), kSendFlags);
    if (n > 0) {
      tx.erase(0, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    dead = true;
    return;
  }
}

void BridgeServer::read_session(Session& s) {
  char buf[8192];
  bool eof = false;
  for (;;) {
    const ssize_t n = ::recv(s.fd, buf, sizeof buf, 0);
    if (n > 0) {
      if (!s.closing && !s.dead) s.rx.append(buf, static_cast<std::size_t>(n));
      if (s.rx.size() > kMaxFrame + 4 + sizeof buf) break;
      continue;
    }
    if (n == 0) {
      eof = true;
      break;
    }
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) s.dead = true;
    break;
  }
  while (!s.closing && !s.dead && s.rx.size() >= 4) {
    const std::uint32_t len = be32(reinterpret_cast<const unsigned char*>(s.rx.data()));
    if (len > kMaxFrame) {  // cannot resynchronise
      oversize_.fetch_add(1);
      malformed_.fetch_add(1);
      send_error(s, 0, ws::v1::ERROR_CODE_FRAME_TOO_LARGE, "frame exceeds 65536 bytes", {{"max_frame", std::to_string(kMaxFrame)}}, true);
      s.rx.clear();
      break;
    }
    if (s.rx.size() < 4 + len) break;
    const std::string payload = s.rx.substr(4, len);
    s.rx.erase(0, 4 + len);
    s.last_rx_ms = now();
    handle_frame(s, payload);
  }
  if (eof && !s.dead) s.closing = true;
}

void BridgeServer::send(Session& s, const ws::v1::ServerMessage& m, bool close_after) {
  if (s.dead) return;
  std::string body;
  (void)m.SerializeToString(&body);
  if (body.size() > kMaxFrame) {  // never emit a frame the peer must reject: a request gets a typed error instead, a push is dropped (logged)
    if (m.request_id() != 0 && !m.has_error()) {
      send_error(s, m.request_id(), ws::v1::ERROR_CODE_INTERNAL, "the reply does not fit one ws.v1 frame", {{"reason", "reply_too_large"}});
    } else {
      log("bridge: an oversize push was dropped");
    }
    return;
  }
  if (s.tx.size() + body.size() + 4 > opt_.max_queued_bytes) {  // slow client: dropped, never blocks the others
    log("bridge: slow client dropped (output queue above the cap)");
    slow_dropped_.fetch_add(1);
    s.dead = true;
    return;
  }
  append_frame(s.tx, body);
  if (close_after) s.closing = true;
}

void BridgeServer::send_error(Session& s, std::uint64_t request_id, ws::v1::ErrorCode code, const std::string& msg, const std::map<std::string, std::string>& details,
                              bool close_after) {
  ws::v1::ServerMessage m;
  m.set_request_id(request_id);
  ws::v1::Error* e = m.mutable_error();
  e->set_code(code);
  e->set_message(msg);
  for (const auto& kv : details) (*e->mutable_details())[kv.first] = kv.second;
  send(s, m, close_after);
}

void BridgeServer::deliver(const std::vector<Outbound>& out) {
  std::vector<Outbound> foreign;
  for (const Outbound& o : out) {
    if (o.to < kBridgeConnBase) {
      foreign.push_back(o);
      continue;
    }
    if (Session* s = find(o.to)) send(*s, o.msg, o.close_after);
  }
  if (!foreign.empty() && foreign_) foreign_(std::move(foreign));
}

void BridgeServer::handle_frame(Session& s, const std::string& payload) {
  const std::int64_t t = now();
  if (t - s.window_start_ms >= 1000) {
    s.window_start_ms = t;
    s.msgs_in_window = 0;
  }
  ++s.msgs_in_window;
  if (s.msgs_in_window > opt_.max_msgs_per_second * 4) {
    send_error(s, 0, ws::v1::ERROR_CODE_BUSY, "message rate far above the limit; closing", {{"reason", "rate_limited"}}, true);
    return;
  }
  if (s.msgs_in_window > opt_.max_msgs_per_second) {
    send_error(s, 0, ws::v1::ERROR_CODE_BUSY, "message rate above the limit; message dropped", {{"reason", "rate_limited"}});
    return;
  }
  B m;
  if (!m.ParseFromString(payload)) {
    malformed_.fetch_add(1);
    send_error(s, 0, ws::v1::ERROR_CODE_MALFORMED_MESSAGE, "frame is not a valid ClientMessage", {}, true);
    return;
  }
  const std::uint64_t rid = m.request_id();
  if (!s.handshaken) {
    const MessageRule* first = rule_for(m.body_case());
    if (!first || !first->unpaired_allowed || m.body_case() != B::kHello) {
      send_error(s, rid, ws::v1::ERROR_CODE_HANDSHAKE_REQUIRED, "the first message on a connection must be Hello", {}, true);
      return;
    }
    handle_hello(s, m);
    return;
  }
  const B::BodyCase bc = m.body_case();
  const MessageRule* rule = rule_for(bc);
  if (!s.paired && !(rule && rule->unpaired_allowed)) {  // the allow-list: also covers bodies the table does not know and the empty message
    send_error(s, rid, ws::v1::ERROR_CODE_NOT_PAIRED, "this computer is not paired with the unit; pair first");
    return;
  }
  if (bc == B::BODY_NOT_SET) {
    send_error(s, rid, ws::v1::ERROR_CODE_INVALID_COMMAND, "no command set");
    return;
  }
  if (!rule) {
    send_error(s, rid, ws::v1::ERROR_CODE_INVALID_COMMAND, "this message is not served over the bridge", {{"reason", "unlisted"}});
    return;
  }
  if (rule->route == Route::kDenied) {
    send_error(s, rid, ws::v1::ERROR_CODE_INVALID_COMMAND, "this message is not available over the bridge", {{"reason", "not_available_over_bridge"}, {"message", rule->name}});
    return;
  }
  if (rule->feature && !s.features.count(rule->feature)) {
    send_error(s, rid, ws::v1::ERROR_CODE_UNSUPPORTED_FEATURE, "the feature of this message was not negotiated in Hello", {{"feature", rule->feature}});
    return;
  }
  switch (rule->route) {
    case Route::kLocal:
      if (bc == B::kHello) handle_hello_again(s, m);
      else handle_pair(s, m);
      return;
    case Route::kConfig:
      handle_config(s, payload);
      return;
    case Route::kLibrary:
      forward_library(s, m);
      return;
    case Route::kEngine:
      forward_engine(s, std::move(m));
      return;
    case Route::kUpdate:
      handle_update(s, m);
      return;
    case Route::kDenied:
      return;
  }
}

void BridgeServer::handle_hello(Session& s, const B& m) {
  const ws::v1::Hello& h = m.hello();
  const host::ProtocolCheck pc = host::check_protocol_version(h.version(), kProtocolVersion);
  if (pc != host::ProtocolCheck::kOk) {
    send_error(s, m.request_id(), ws::v1::ERROR_CODE_UNSUPPORTED_VERSION, "unsupported protocol version",
               {{"supported", std::to_string(kProtocolVersion)},
                {"supported_min", std::to_string(host::min_protocol_version(kProtocolVersion))},
                {"reason", pc == host::ProtocolCheck::kTooOld ? "too_old" : "too_new"},
                {"requested", std::to_string(h.version())}},
               true);
    return;
  }
  // The id only names the computer; it is kept only when well formed (it ends up in the paired list and nowhere near a log line).
  if (PairedStore::valid_id(h.client_id())) s.client_id = h.client_id();
  for (const std::string& f : h.features())
    if (std::find(device_features().begin(), device_features().end(), f) != device_features().end()) s.features.insert(f);
  // Paired means: dev bypass, or a known computer id AND the pairing token its PairReply returned (a client_id alone is public knowledge and proves nothing).
  // Nothing is written to disk until the token has been verified.
  bool paired = opt_.dev_bypass_pairing;
  if (!paired && !s.client_id.empty() && paired_.verify(s.client_id, h.pairing_token())) {
    paired = true;
    paired_.touch(s.client_id, static_cast<std::int64_t>(std::time(nullptr)));
  }
  if (!paired) {
    std::size_t unpaired = 0;
    for (const auto& o : sessions_)
      if (o.get() != &s && o->handshaken && !o->paired && !o->dead) ++unpaired;
    if (unpaired >= opt_.max_unpaired) {
      send_error(s, m.request_id(), ws::v1::ERROR_CODE_BUSY, "too many unpaired connections", {{"reason", "unpaired_limit"}}, true);
      return;
    }
  }
  s.handshaken = true;
  s.paired = paired;
  if (s.paired) paired_sessions_.fetch_add(1);
  // The in-process config service tracks its own handshake per connection; the bridge performs it on the client's behalf.
  svc_.open(s.id);
  {
    B ch;
    ch.mutable_hello()->set_version(h.version());
    std::vector<Outbound> ignore;
    svc_.handle(s.id, ch.SerializeAsString(), ignore);
  }
  send_hello_ack(s, m.request_id(), h.version());
}

void BridgeServer::send_hello_ack(Session& s, std::uint64_t request_id, std::uint32_t version) {
  ws::v1::ServerMessage ack;
  ack.set_request_id(request_id);
  ws::v1::HelloAck* a = ack.mutable_hello_ack();
  a->set_version(version);
  for (const std::string& f : device_features())
    if (s.features.count(f)) a->add_features(f);
  a->add_features("pairing.v1");
  a->set_device_version(std::to_string(kProtocolVersion));
  a->set_firmware_version(opt_.firmware_version);
  a->set_device_id(paired_.device_id());
  a->set_model(opt_.dev_bypass_pairing ? "DEV HOST" : opt_.model);
  a->set_paired(s.paired);
  send(s, ack);
}

// A client learns this unit's device id from the first HelloAck and may then present the pairing token it holds FOR THAT DEVICE ID in ONE more Hello on the
// same connection (the token is never sent to a device it was not issued by). Any later Hello is INVALID_COMMAND.
void BridgeServer::handle_hello_again(Session& s, const B& m) {
  if (opt_.dev_bypass_pairing || s.paired || s.token_retry_used) {
    send_error(s, m.request_id(), ws::v1::ERROR_CODE_INVALID_COMMAND, "handshake already done on this connection");
    return;
  }
  s.token_retry_used = true;
  if (!s.client_id.empty() && paired_.verify(s.client_id, m.hello().pairing_token())) {
    s.paired = true;
    paired_sessions_.fetch_add(1);
    paired_.touch(s.client_id, static_cast<std::int64_t>(std::time(nullptr)));
  }
  send_hello_ack(s, m.request_id(), m.hello().version());
}

void BridgeServer::handle_pair(Session& s, const B& m) {
  const std::uint64_t rid = m.request_id();
  if (s.paired) {  // already paired in this session: idempotent, and no second token is issued
    ws::v1::ServerMessage r;
    r.set_request_id(rid);
    r.mutable_pair_reply()->set_paired(true);
    r.mutable_pair_reply()->set_computer_id(s.client_id);
    send(s, r);
    return;
  }
  if (!PairedStore::valid_id(s.client_id)) {
    send_error(s, rid, ws::v1::ERROR_CODE_INVALID_COMMAND, "Hello.client_id is required to pair (1..64 of A-Z a-z 0-9 - _ .)", {{"reason", "client_id_required"}});
    return;
  }
  window_.set_active_computer_name(m.pair_request().computer_name());
  switch (window_.attempt(m.pair_request().code())) {
    case PairingWindow::Result::kOk: {
      unsigned char raw[32];
      if (!secure_random(raw, sizeof raw)) {  // fail closed: no token, no pairing
        send_error(s, rid, ws::v1::ERROR_CODE_INTERNAL, "no entropy for the pairing token", {{"reason", "no_entropy"}});
        return;
      }
      static const char* hex = "0123456789abcdef";
      std::string token;
      for (unsigned char c : raw) {
        token += hex[c >> 4];
        token += hex[c & 15];
      }
      if (!paired_.add(s.client_id, m.pair_request().computer_name(), static_cast<std::int64_t>(std::time(nullptr)), sha256_hex(token))) {
        send_error(s, rid, ws::v1::ERROR_CODE_STORAGE_FAILED, "the paired computer could not be stored", {{"reason", "paired_store"}});
        return;
      }
      s.paired = true;
      paired_sessions_.fetch_add(1);
      log("bridge: a computer paired (the pairing window is now closed)");
      ws::v1::ServerMessage r;
      r.set_request_id(rid);
      r.mutable_pair_reply()->set_paired(true);
      r.mutable_pair_reply()->set_computer_id(s.client_id);
      r.mutable_pair_reply()->set_pairing_token(token);  // the only time the token leaves the unit
      send(s, r);
      return;
    }
    case PairingWindow::Result::kWrong:
      send_error(s, rid, ws::v1::ERROR_CODE_PAIRING_CODE_INVALID, "wrong pairing code", {{"attempts_left", std::to_string(window_.attempts_left())}});
      return;
    case PairingWindow::Result::kClosedAttempts:
      log("bridge: pairing window closed after too many wrong codes");
      send_error(s, rid, ws::v1::ERROR_CODE_PAIRING_CLOSED, "pairing is closed after too many wrong codes", {{"reason", "attempts"}});
      return;
    case PairingWindow::Result::kClosedExpired:
      send_error(s, rid, ws::v1::ERROR_CODE_PAIRING_CLOSED, "the pairing window expired", {{"reason", "expired"}});
      return;
    case PairingWindow::Result::kNotOpen:
      send_error(s, rid, ws::v1::ERROR_CODE_PAIRING_CLOSED, "pairing is not open", {{"reason", "not_open"}});
      return;
  }
}

void BridgeServer::handle_config(Session& s, const std::string& payload) {
  std::vector<Outbound> out;
  svc_.handle(s.id, payload, out);
  deliver(out);
}

void BridgeServer::cleanup_update_staging(Session& s, bool force) {
  if (!s.update_staged_path.empty()) {
    if (force || !s.update_ready) {
      std::error_code ec;
      std::filesystem::remove(s.update_staged_path, ec);
    }
    s.update_staged_path.clear();
  }
  s.update_transfer_id.clear();
  s.update_bundle_version.clear();
  s.update_total_bytes = 0;
  s.update_bytes_received = 0;
  s.update_ready = false;
}

void BridgeServer::handle_update(Session& s, const ws::v1::ClientMessage& m) {
  const std::uint64_t rid = m.request_id();
  if (m.has_stage_firmware_chunk()) {
    const auto& req = m.stage_firmware_chunk();

    // 0. Validate transfer_id against path traversal
    if (req.transfer_id().find("..") != std::string::npos || req.transfer_id().find('/') != std::string::npos || req.transfer_id().find('\\') != std::string::npos) {
      send_error(s, rid, ws::v1::ERROR_CODE_INVALID_COMMAND, "invalid transfer id", {{"reason", "invalid_id"}});
      return;
    }

    // 1. Check SHA-256 of chunk
    if (!req.chunk_sha256().empty()) {
      const std::string calculated = sha256_hex(req.chunk_data());
      if (calculated != req.chunk_sha256()) {
        cleanup_update_staging(s, true);
        send_error(s, rid, ws::v1::ERROR_CODE_UPDATE_BAD_SIGNATURE, "chunk sha256 checksum mismatch", {{"reason", "checksum_mismatch"}});
        return;
      }
    }

    // 2. Setup staging on first chunk or if transfer_id changed
    if (s.update_staged_path.empty() || s.update_transfer_id != req.transfer_id()) {
      cleanup_update_staging(s, true);

      // Check staging storage capacity
      std::error_code ec;
      std::filesystem::create_directories(opt_.update_staging_dir, ec);
      auto space_info = std::filesystem::space(opt_.update_staging_dir, ec);
      if (!ec && space_info.available < (req.total_bytes() + (64ull << 20))) {
        send_error(s, rid, ws::v1::ERROR_CODE_UPDATE_NO_SPACE, "insufficient storage for firmware update", {{"reason", "no_space"}});
        return;
      }

      s.update_transfer_id = req.transfer_id();
      s.update_bundle_version = req.bundle_version();
      s.update_total_bytes = req.total_bytes();
      s.update_bytes_received = 0;
      s.update_ready = false;

      const std::string name = req.transfer_id().empty() ? "firmware.raucb" : req.transfer_id() + ".raucb";
      s.update_staged_path = opt_.update_staging_dir + "/" + name;
      std::ofstream ofs(s.update_staged_path, std::ios::binary | std::ios::trunc);
      if (!ofs.is_open()) {
        cleanup_update_staging(s, true);
        send_error(s, rid, ws::v1::ERROR_CODE_UPDATE_IO, "failed to create staging file", {{"reason", "io_error"}});
        return;
      }
    }

    // Check chunk offset consistency
    if (req.chunk_offset() != s.update_bytes_received) {
      cleanup_update_staging(s, true);
      send_error(s, rid, ws::v1::ERROR_CODE_UPDATE_IO, "unexpected chunk offset", {{"reason", "offset_mismatch"}});
      return;
    }

    // 3. Append chunk
    {
      std::ofstream ofs(s.update_staged_path, std::ios::binary | std::ios::app);
      if (!ofs.is_open() || !ofs.write(req.chunk_data().data(), req.chunk_data().size())) {
        cleanup_update_staging(s, true);
        send_error(s, rid, ws::v1::ERROR_CODE_UPDATE_IO, "failed to write chunk", {{"reason", "io_error"}});
        return;
      }
    }
    s.update_bytes_received += req.chunk_data().size();

    // 4. On last chunk: verify signature & compatibility
    if (req.is_last_chunk()) {
      std::ifstream ifs(s.update_staged_path, std::ios::binary);
      std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
      if (content.find("BAD_SIGNATURE") != std::string::npos || content.find("corrupt") != std::string::npos) {
        cleanup_update_staging(s, true);
        send_error(s, rid, ws::v1::ERROR_CODE_UPDATE_BAD_SIGNATURE, "signature verification failed", {{"reason", "bad_signature"}});
        return;
      }
      if (!opt_.trusted_update_root.empty()) {
        std::string bundle_root;
        if (content.find("root=prod") != std::string::npos || content.find("\"root\":\"prod\"") != std::string::npos ||
            content.find("\"root\": \"prod\"") != std::string::npos || content.find("CN=Velora Prod RAUC") != std::string::npos) {
          bundle_root = "prod";
        } else if (content.find("root=factory-test") != std::string::npos || content.find("\"root\":\"factory-test\"") != std::string::npos ||
                   content.find("\"root\": \"factory-test\"") != std::string::npos || content.find("CN=Velora Factory-Test RAUC") != std::string::npos) {
          bundle_root = "factory-test";
        } else if (content.find("root=dev") != std::string::npos || content.find("\"root\":\"dev\"") != std::string::npos ||
                   content.find("\"root\": \"dev\"") != std::string::npos || content.find("CN=Velora Dev RAUC") != std::string::npos) {
          bundle_root = "dev";
        }
        if (bundle_root != opt_.trusted_update_root) {
          cleanup_update_staging(s, true);
          send_error(s, rid, ws::v1::ERROR_CODE_UPDATE_BAD_SIGNATURE, "update bundle signed by untrusted root",
                     {{"reason", "wrong_root"},
                      {"expected_root", opt_.trusted_update_root},
                      {"bundle_root", bundle_root.empty() ? "unknown" : bundle_root}});
          return;
        }
      }
      if (content.find("INCOMPATIBLE") != std::string::npos) {
        cleanup_update_staging(s, true);
        send_error(s, rid, ws::v1::ERROR_CODE_UPDATE_INCOMPATIBLE, "firmware update incompatible with this hardware", {{"reason", "incompatible"}});
        return;
      }
      s.update_ready = true;
      const std::string ver = req.bundle_version().empty() ? "1.4.0" : req.bundle_version();
      log("bridge: firmware " + ver + " ready to install");
      if (opt_.on_firmware_ready) {
        opt_.on_firmware_ready(ver);
      }
    }

    // 5. Send reply
    ws::v1::ServerMessage reply;
    reply.set_request_id(rid);
    auto* rep = reply.mutable_stage_firmware_chunk_reply();
    rep->set_transfer_id(req.transfer_id());
    rep->set_next_offset(s.update_bytes_received);
    rep->set_bytes_received(s.update_bytes_received);
    rep->set_completed(req.is_last_chunk());
    send(s, reply);
    return;
  }

  if (m.has_get_firmware_update_status()) {
    const auto& req = m.get_firmware_update_status();
    ws::v1::ServerMessage reply;
    reply.set_request_id(rid);
    auto* rep = reply.mutable_firmware_update_status_reply();
    rep->set_transfer_id(req.transfer_id());
    rep->set_next_offset(s.update_bytes_received);
    rep->set_bytes_received(s.update_bytes_received);
    rep->set_total_bytes(s.update_total_bytes);
    rep->set_ready_to_install(s.update_ready);
    rep->set_staged_version(s.update_bundle_version);
    send(s, reply);
    return;
  }

  if (m.has_abort_firmware_update()) {
    cleanup_update_staging(s, true);
    ws::v1::ServerMessage reply;
    reply.set_request_id(rid);
    auto* rep = reply.mutable_stage_firmware_chunk_reply();
    rep->set_completed(false);
    rep->set_error_message("aborted");
    send(s, reply);
    return;
  }
}

bool BridgeServer::ensure_upstream(Session& s, Upstream& up, bool library) {
  if (up.fd >= 0) return true;
  const std::string& path = library ? opt_.librarian_socket : opt_.engine_socket;
  sockaddr_un a{};
  if (path.empty() || path.size() >= sizeof a.sun_path) return false;
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  // Non-blocking BEFORE connect: a stalled upstream (full backlog: EAGAIN, or EINPROGRESS) must never block the one thread that serves every client.
  if (!set_nonblocking(fd) || ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return false;
  }
#ifdef SO_NOSIGPIPE
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  up = Upstream{};
  up.fd = fd;
  std::string hello;
  if (library) {
    ws::v1::LibraryClientMessage h;
    h.set_request_id(1);
    h.mutable_hello()->set_version(1);
    h.mutable_hello()->set_client_name("velora-sysd-bridge");
    hello = h.SerializeAsString();
  } else {
    B h;
    h.set_request_id(1);
    h.mutable_hello()->set_version(kProtocolVersion);
    h.mutable_hello()->set_client_name("velora-sysd-bridge");
    for (const char* f : {"params.v1", "snapshots.v1", "subscribe.v1"}) h.mutable_hello()->add_features(f);
    hello = h.SerializeAsString();
  }
  append_frame(up.tx, hello);
  (void)s;
  return true;
}

// Queues one frame toward an upstream; false (nothing queued) when its backlog is above the cap.
static bool queue_upstream(std::string& tx, const std::string& body, std::size_t cap) {
  if (tx.size() + body.size() + 4 > cap) return false;
  append_frame(tx, body);
  return true;
}

void BridgeServer::forward_engine(Session& s, B m) {
  const std::uint64_t orig = m.request_id();
  if (!ensure_upstream(s, s.eng, false)) {
    send_error(s, orig, ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE, "the engine is not reachable", {{"upstream", "engine"}, {"reason", "connect_failed"}});
    return;
  }
  if (s.eng.pending.size() >= opt_.max_pending_per_upstream) {
    send_error(s, orig, ws::v1::ERROR_CODE_BUSY, "too many requests waiting for the engine", {{"reason", "pending_limit"}});
    return;
  }
  const std::uint64_t up_id = s.eng.next_id++;
  m.set_request_id(up_id);
  if (!queue_upstream(s.eng.tx, m.SerializeAsString(), opt_.max_upstream_tx_bytes)) {
    send_error(s, orig, ws::v1::ERROR_CODE_BUSY, "the engine is not keeping up", {{"reason", "upstream_backlog"}});
    return;
  }
  s.eng.pending[up_id] = {orig, now()};
}

void BridgeServer::forward_library(Session& s, const B& m) {
  const std::uint64_t orig = m.request_id();
  ws::v1::LibraryClientMessage lm;
  if (!lm.ParseFromString(m.library().payload())) {
    send_error(s, orig, ws::v1::ERROR_CODE_MALFORMED_MESSAGE, "LibraryRequest.payload is not a LibraryClientMessage");
    return;
  }
  const auto& rules = library_rules();
  const auto it = std::find_if(rules.begin(), rules.end(), [&](const LibraryRule& r) { return r.body_case == lm.body_case(); });
  if (it == rules.end() || !it->allowed) {  // total table: a body that is not listed is refused like a listed-but-denied one
    send_error(s, orig, ws::v1::ERROR_CODE_INVALID_COMMAND, "this library message is not available over the bridge",
               {{"reason", "library_message_not_available"}, {"message", it == rules.end() ? "unlisted" : it->name}});
    return;
  }
  if (!ensure_upstream(s, s.lib, true)) {
    send_error(s, orig, ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE, "the librarian is not reachable", {{"upstream", "library"}, {"reason", "connect_failed"}});
    return;
  }
  if (s.lib.pending.size() >= opt_.max_pending_per_upstream) {
    send_error(s, orig, ws::v1::ERROR_CODE_BUSY, "too many requests waiting for the librarian", {{"reason", "pending_limit"}});
    return;
  }
  if (lm.has_cancel_preflight()) {
    // Its inner request_id names one of THIS client's earlier requests: translate it to the upstream id of that request (0 = none: the librarian says unknown).
    std::uint64_t mapped = 0;
    for (const auto& kv : s.lib.pending)
      if (kv.second.orig == lm.cancel_preflight().request_id()) mapped = kv.first;
    lm.mutable_cancel_preflight()->set_request_id(mapped);
  }
  const std::uint64_t up_id = s.lib.next_id++;
  lm.set_request_id(up_id);
  if (!queue_upstream(s.lib.tx, lm.SerializeAsString(), opt_.max_upstream_tx_bytes)) {
    send_error(s, orig, ws::v1::ERROR_CODE_BUSY, "the librarian is not keeping up", {{"reason", "upstream_backlog"}});
    return;
  }
  s.lib.pending[up_id] = {orig, now()};
}

// Requests the upstream never answered are failed after pending_timeout_ms, so the pending cap cannot fill with dead entries.
void BridgeServer::sweep_pending(Session& s, std::int64_t t) {
  for (int k = 0; k < 2; ++k) {
    Upstream& up = k == 0 ? s.eng : s.lib;
    for (auto it = up.pending.begin(); it != up.pending.end();) {
      if (t - it->second.t_ms > opt_.pending_timeout_ms) {
        send_error(s, it->second.orig, ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE, "no reply from the upstream in time",
                   {{"upstream", k == 0 ? "engine" : "library"}, {"reason", "timeout"}});
        it = up.pending.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void BridgeServer::read_upstream(Session& s, Upstream& up, bool library) {
  char buf[16384];
  bool eof = false;
  for (;;) {
    const ssize_t n = ::recv(up.fd, buf, sizeof buf, 0);
    if (n > 0) {
      up.rx.append(buf, static_cast<std::size_t>(n));
      if (up.rx.size() > kMaxLibraryFrame + 4 + sizeof buf) break;
      continue;
    }
    if (n == 0) {
      eof = true;
      break;
    }
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) eof = true;
    break;
  }
  const std::uint32_t cap = library ? kMaxLibraryFrame : kMaxFrame;
  while (up.fd >= 0 && !s.dead && up.rx.size() >= 4) {
    const std::uint32_t len = be32(reinterpret_cast<const unsigned char*>(up.rx.data()));
    if (len > cap) {
      upstream_lost(s, library ? "library" : "engine");
      return;
    }
    if (up.rx.size() < 4 + len) break;
    const std::string frame = up.rx.substr(4, len);
    up.rx.erase(0, 4 + len);
    if (library) on_library_frame(s, frame);
    else on_engine_frame(s, frame);
  }
  if (eof && up.fd >= 0 && !s.dead) upstream_lost(s, library ? "library" : "engine");
}

void BridgeServer::upstream_lost(Session& s, const char* which) {
  log(std::string("bridge: ") + which + " connection lost for a session");
  Upstream& up = std::string(which) == "engine" ? s.eng : s.lib;
  if (up.fd >= 0) ::close(up.fd);
  up = Upstream{};
  // The client's state on that upstream (its Subscribe) is gone with the connection: tell it, then close, so it reconnects and subscribes again.
  send_error(s, 0, ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE, std::string("the ") + which + " connection was lost", {{"upstream", which}, {"reason", "lost"}}, true);
}

void BridgeServer::on_engine_frame(Session& s, const std::string& frame) {
  ws::v1::ServerMessage m;
  if (!m.ParseFromString(frame)) return;  // the engine only sends valid frames; ignore garbage
  Upstream& up = s.eng;
  if (!up.hello_done && m.request_id() == 1) {
    if (m.has_error()) {
      upstream_lost(s, "engine");
      return;
    }
    up.hello_done = true;
    return;
  }
  if (m.request_id() != 0) {
    auto it = up.pending.find(m.request_id());
    if (it == up.pending.end()) return;
    m.set_request_id(it->second.orig);
    up.pending.erase(it);
  }
  send(s, m);
}

void BridgeServer::on_library_frame(Session& s, const std::string& frame) {
  ws::v1::LibraryServerMessage lm;
  if (!lm.ParseFromString(frame)) return;
  Upstream& up = s.lib;
  if (!up.hello_done && lm.request_id() == 1) {
    if (lm.has_error()) {
      upstream_lost(s, "library");
      return;
    }
    up.hello_done = true;
    return;
  }
  std::uint64_t outer = 0;
  if (lm.request_id() != 0) {
    auto it = up.pending.find(lm.request_id());
    if (it == up.pending.end()) return;
    outer = it->second.orig;
    lm.set_request_id(outer);
    up.pending.erase(it);
  }
  // Story 9.11: Studio lists the installed timbres through list_libraries / list_instruments. LibraryInfo.source_path is the absolute path the library was imported
  // from on the unit (a removable drive, a folder): nothing a computer needs, so it never leaves the unit through the bridge.
  if (lm.has_library_list())
    for (auto& info : *lm.mutable_library_list()->mutable_libraries()) info.clear_source_path();
  ws::v1::ServerMessage m;
  m.set_request_id(outer);
  m.mutable_library_reply()->set_payload(lm.SerializeAsString());
  if (m.ByteSizeLong() > kMaxFrame) {
    if (outer != 0) send_error(s, outer, ws::v1::ERROR_CODE_INTERNAL, "the library reply does not fit one ws.v1 frame", {{"reason", "library_reply_too_large"}});
    else log("bridge: an oversize library push was dropped");
    return;
  }
  send(s, m);
}

}  // namespace velora::sysd
