// Story 6.4: the Library API installs protected bundles (InstallProtectedRequest) as a job on the librarian's worker. Real Unix socket client; waits are bounded
// polls for an event, nothing is timed.
//
// Covered: the job (accepted, queued, running, one terminal state, the item result with its typed install_reason), Factory rows next to User rows with the kind in
// ListLibraries, idempotent re-install, every refusal before the job exists, rejections that leave the catalog and the library tree untouched, cancel, drive removal,
// space, the refusal of a user import of a Factory library (at the request and in the importer), the startup reconcile with and without trusted keys, the catalog
// schema 3 migration, and the hello features.
#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <thread>
#include <map>
#include <mutex>
#include <set>

#include "import_fixtures.hpp"
#include "lib_test_util.hpp"
#include "protected_fixtures.hpp"
#include "velora/librarian/import.hpp"
#include "velora/librarian/library_index.hpp"
#include "velora/librarian/library_service.hpp"

using namespace velora::librarian;
namespace fs = std::filesystem;
namespace fmt = velora::format;
using ltest::Client;
using ltest::TempDir;
using pfx::Bytes;

namespace {

const pfx::Keys& K1() {
  static const pfx::Keys k = pfx::make_keys(1);
  return k;
}

bool terminal(ws::v1::ImportState s) {
  return s == ws::v1::IMPORT_STATE_PUBLISHED || s == ws::v1::IMPORT_STATE_FAILED || s == ws::v1::IMPORT_STATE_CANCELLED || s == ws::v1::IMPORT_STATE_DRIVE_REMOVED;
}

ws::v1::LibraryClientMessage install_msg(const std::string& dir, const std::string& expected = {}) {
  ws::v1::LibraryClientMessage m;
  m.mutable_install_protected()->set_source_dir(dir);
  if (!expected.empty()) m.mutable_install_protected()->set_expected_library_id(expected);
  return m;
}

std::vector<ws::v1::ImportProgress> collect(Client& c, std::uint64_t job, int timeout_ms = 30000) {
  std::vector<ws::v1::ImportProgress> ev;
  std::deque<ws::v1::LibraryServerMessage> keep;
  auto take = [&](const ws::v1::LibraryServerMessage& m) {
    if (m.has_import_progress() && m.import_progress().job_id() == job) ev.push_back(m.import_progress());
    else keep.push_back(m);
  };
  for (const auto& m : c.pushes) take(m);
  c.pushes.clear();
  while (ev.empty() || !terminal(ev.back().state())) {
    auto r = c.recv(timeout_ms);
    if (!r) break;
    take(*r);
  }
  for (auto& m : keep) c.pushes.push_back(std::move(m));
  return ev;
}

std::optional<ws::v1::LibraryServerMessage> list_libs(Client& c) {
  ws::v1::LibraryClientMessage m;
  m.mutable_list_libraries();
  return c.call(m);
}

struct Svc {
  TempDir td;
  LibraryServiceOptions o;
  std::unique_ptr<LibraryService> s;
  std::string err;
  explicit Svc(const std::function<void(LibraryServiceOptions&)>& cfg = {}, const std::string& data = {}) {
    o.data_dir = data.empty() ? td.sub("data") : data;
    if (data.empty()) fs::create_directories(o.data_dir);
    o.socket_path = td.sub("s.sock");
    o.trusted_keys = K1().trusted;
    if (cfg) cfg(o);
  }
  bool start() {
    s = std::make_unique<LibraryService>(o);
    return s->start(err);
  }
  void stop() {
    if (s) s->stop();
    s.reset();
  }
  std::string data() const { return o.data_dir; }
  bool connect(Client& c) { return c.connect(o.socket_path) && c.handshake("install-test"); }
};

// Runs one install to its end; returns every event.
std::vector<ws::v1::ImportProgress> run_install(Client& c, const std::string& dir, std::uint64_t* job_out = nullptr, ws::v1::LibraryServerMessage* reply = nullptr) {
  auto r = c.call(install_msg(dir));
  if (reply && r) *reply = *r;
  if (!r || !r->has_import_accepted()) return {};
  if (job_out) *job_out = r->import_accepted().job_id();
  return collect(c, r->import_accepted().job_id());
}

const ws::v1::ImportItemResult* item_of(const std::vector<ws::v1::ImportProgress>& ev) {
  for (auto it = ev.rbegin(); it != ev.rend(); ++it)
    if (it->has_item()) return &it->item();
  return nullptr;
}

bool tree_has_no_library(const std::string& data) {
  std::error_code ec;
  for (fs::directory_iterator it(fs::path(data) / "lib", ec), end; !ec && it != end; it.increment(ec))
    if (it->path().filename().string().size() == 36) return false;
  return true;
}
bool staging_clean(const std::string& data) {
  std::error_code ec;
  for (fs::directory_iterator it(fs::path(data) / "lib" / ".staging", ec), end; !ec && it != end; it.increment(ec))
    if (it->path().filename().string() != ".lock") return false;
  return true;
}

struct FailingSource : fmt::ByteSource {
  int fd = -1;
  std::uint64_t sz = 0;
  mutable std::atomic<std::uint64_t> reads{0};
  std::uint64_t fail_after;
  FailingSource(const std::string& p, std::uint64_t fa) : fail_after(fa) {
    fd = ::open(p.c_str(), O_RDONLY);
    struct stat st;
    if (fd >= 0 && ::fstat(fd, &st) == 0) sz = static_cast<std::uint64_t>(st.st_size);
  }
  ~FailingSource() override {
    if (fd >= 0) ::close(fd);
  }
  std::uint64_t size() const override { return sz; }
  const std::uint8_t* direct(std::uint64_t, std::size_t) const override { return nullptr; }
  fmt::Error read_at(std::uint64_t off, void* dst, std::size_t n) const override {
    if (++reads > fail_after) return fmt::Error::kIo;
    return off + n <= sz && ::pread(fd, dst, n, static_cast<off_t>(off)) == static_cast<ssize_t>(n) ? fmt::Error::kNone : fmt::Error::kIo;
  }
};

void test_install_job_and_kinds() {
  Svc sv;
  CHECK(sv.start());
  Client c;
  CHECK(sv.connect(c));
  TempDir t;
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  // the user library next to it
  const fs::path sfz = ifx::write_sfz(t.sub("user"), "Piano");
  {
    ws::v1::LibraryClientMessage m;
    auto* r = m.mutable_import_request();
    r->set_source(sfz.string());
    r->add_items()->set_category(1);
    auto a = c.call(m);
    CHECK(a && a->has_import_accepted());
    const auto ev = collect(c, a->import_accepted().job_id());
    CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
  }
  std::uint64_t job = 0;
  ws::v1::LibraryServerMessage reply;
  const auto ev = run_install(c, p.dir.string(), &job, &reply);
  CHECK(reply.has_import_accepted() && reply.import_accepted().item_count() == 1 && reply.import_accepted().bytes_total() == p.bundle.size());
  CHECK(reply.import_accepted().required_bytes() >= p.bundle.size() + 256u * 1024 * 1024 && reply.import_accepted().free_known());
  CHECK(!ev.empty() && ev.front().state() == ws::v1::IMPORT_STATE_QUEUED && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
  CHECK(ev.back().items_total() == 1 && ev.back().items_imported() == 1 && ev.back().items_failed() == 0 && ev.back().current_item().empty());
  {  // a RUNNING event names the pack, the progress counts bytes of the bundle, and the last event has them all
    bool running = false;
    for (const auto& e : ev)
      if (e.state() == ws::v1::IMPORT_STATE_RUNNING && !e.current_item().empty()) {
        running = true;
        CHECK(e.current_item().find(p.library_id) != std::string::npos && e.bytes_total() == p.bundle.size() && e.bytes_done() <= e.bytes_total());
      }
    CHECK(running && ev.back().bytes_done() == p.bundle.size() && ev.back().bytes_total() == p.bundle.size() && ev.back().timbres_total() == 1 && ev.back().timbres_imported() == 1);
  }
  std::uint64_t last_seq = 0;
  for (const auto& e : ev) {  // monotone events, one terminal
    CHECK(e.event_seq() > last_seq);
    last_seq = e.event_seq();
    CHECK(e.state() == ws::v1::IMPORT_STATE_QUEUED || e.state() == ws::v1::IMPORT_STATE_RUNNING || &e == &ev.back());
  }
  const auto* it = item_of(ev);
  CHECK(it && it->status() == ws::v1::IMPORT_ITEM_IMPORTED && it->install_reason() == ws::v1::INSTALL_REASON_NONE);
  CHECK(it && it->library_id() == p.library_id && it->bundle_id() == p.bundle_id && it->instruments() == 2);
  // both libraries, with their kind
  auto l = list_libs(c);
  CHECK(l && l->has_library_list() && l->library_list().total() == 2);
  int factory = 0, user = 0;
  for (const auto& li : l->library_list().libraries()) {
    if (li.library_id() == p.library_id) {
      ++factory;
      CHECK(li.kind() == ws::v1::LIBRARY_KIND_FACTORY && li.name() == "Vendor Grand Piano" && li.instrument_count() == 2 && li.bundle_id() == p.bundle_id);
    } else {
      ++user;
      CHECK(li.kind() == ws::v1::LIBRARY_KIND_USER);
    }
  }
  CHECK(factory == 1 && user == 1);
  ws::v1::LibraryClientMessage li;
  li.mutable_list_instruments()->set_library_id(p.library_id);
  auto inst = c.call(li);
  CHECK(inst && inst->has_instrument_list() && inst->instrument_list().total() == 2 && inst->instrument_list().instruments(0).category() == 1);
  CHECK(inst && inst->instrument_list().instruments(0).name() == "Instrument 0" && inst->instrument_list().instruments(0).instrument_key() == "pack/inst0.sfz");
  // the same install again: the identical bundle is unchanged, idempotent
  const auto ev2 = run_install(c, p.dir.string());
  const auto* it2 = item_of(ev2);
  CHECK(!ev2.empty() && ev2.back().state() == ws::v1::IMPORT_STATE_PUBLISHED && ev2.back().items_unchanged() == 1 && ev2.back().items_imported() == 0);
  CHECK(it2 && it2->status() == ws::v1::IMPORT_ITEM_UNCHANGED && it2->install_reason() == ws::v1::INSTALL_REASON_ALREADY_INSTALLED);
  // the user import path leaves the Factory library alone: importing again does not touch it
  l = list_libs(c);
  CHECK(l && l->library_list().total() == 2);
  // the handshake advertises the feature
  Client c2;
  CHECK(c2.connect(sv.o.socket_path));
  ws::v1::LibraryClientMessage h;
  h.mutable_hello()->set_version(1);
  auto ack = c2.call(h);
  std::set<std::string> feats;
  if (ack && ack->has_hello_ack()) feats = {ack->hello_ack().features().begin(), ack->hello_ack().features().end()};
  CHECK(feats.count("install.protected.v1") == 1 && feats.count("library.kind.v1") == 1 && ack->hello_ack().catalog_schema_version() == kCatalogSchemaVersion);
  // restart: the reconcile pass keeps the Factory row (trusted keys given), and with no trusted keys the library is not examined but its row stays
  sv.stop();
  CHECK(sv.start());
  CHECK(sv.s->startup_reconcile().rows_unchanged == 2 && sv.s->startup_reconcile().skipped.empty());
  sv.stop();
  sv.o.trusted_keys = fmt::TrustedKeys{};
  CHECK(sv.start());
  CHECK(sv.s->startup_reconcile().rows_removed == 0 && sv.s->startup_reconcile().skipped.size() == 1 && sv.s->startup_reconcile().rows_unchanged == 1);
  Client c3;
  CHECK(sv.connect(c3));
  l = list_libs(c3);
  CHECK(l && l->library_list().total() == 2);
  // and with no trusted keys an install is refused untrusted_key
  const auto ev3 = run_install(c3, pfx::make_pack(t.sub("packs3"), [] {
                                      pfx::PackSpec s;
                                      s.library_id = "00000000-0000-4000-8000-0000000000aa";
                                      return s;
                                    }(), K1()).dir.string());
  const auto* it3 = item_of(ev3);
  CHECK(!ev3.empty() && ev3.back().state() == ws::v1::IMPORT_STATE_FAILED && it3 && it3->status() == ws::v1::IMPORT_ITEM_FAILED &&
        it3->install_reason() == ws::v1::INSTALL_REASON_UNTRUSTED_KEY);
}

void test_rejections_leave_nothing() {
  Svc sv;
  CHECK(sv.start());
  Client c;
  CHECK(sv.connect(c));
  TempDir t;
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  auto expect = [&](const std::string& dir, ws::v1::InstallReason reason, const char* what, bool expect_empty = true) {
    const auto ev = run_install(c, dir);
    const auto* it = item_of(ev);
    const bool ok = !ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_FAILED && ev.back().items_failed() == 1 && it && it->status() == ws::v1::IMPORT_ITEM_FAILED &&
                    it->install_reason() == reason && !it->detail().empty();
    if (!ok) {
      std::printf("FAIL rejection case %s (reason %d)\n", what, it ? static_cast<int>(it->install_reason()) : -1);
      ++g_fail;
    }
    CHECK(staging_clean(sv.data()) && (!expect_empty || tree_has_no_library(sv.data())));
    auto l = list_libs(c);
    CHECK(l && l->library_list().total() == (expect_empty ? 0u : 1u));
  };
  {  // one flipped bit in a block
    pfx::Pack q = p;
    q.dir = t.sub("bad_block") + "/" + p.library_id;
    fs::create_directories(q.dir.parent_path());
    fs::copy(p.dir, q.dir, fs::copy_options::recursive);
    Bytes b = p.bundle;
    b[b.size() - 7] ^= 0x01;
    pfx::write_bytes(q.dir / "bundle.vlrp", b);
    expect(q.dir.string(), ws::v1::INSTALL_REASON_BLOCK_HASH_MISMATCH, "flipped block");
  }
  {  // one flipped bit in the manifest
    const fs::path d = t.sub("bad_man") + "/" + p.library_id;
    fs::create_directories(d.parent_path());
    fs::copy(p.dir, d, fs::copy_options::recursive);
    Bytes b = p.bundle;
    b[200] ^= 0x01;
    pfx::write_bytes(d / "bundle.vlrp", b);
    expect(d.string(), ws::v1::INSTALL_REASON_CORRUPT_BUNDLE, "flipped manifest");
  }
  {  // signed by a key the service does not trust
    const pfx::Keys other = pfx::make_keys(9);
    const pfx::Pack q = pfx::make_pack(t.sub("other_key"), {}, other);
    expect(q.dir.string(), ws::v1::INSTALL_REASON_UNTRUSTED_KEY, "untrusted signer");
  }
  {  // a broken wrapped key
    const fs::path d = t.sub("bad_wk") + "/" + p.library_id;
    fs::create_directories(d.parent_path());
    fs::copy(p.dir, d, fs::copy_options::recursive);
    Bytes w = p.wrapped;
    w[1] ^= 1;
    pfx::write_bytes(d / p.wrapped_name, w);
    expect(d.string(), ws::v1::INSTALL_REASON_WRAPPED_KEY_INVALID, "wrapped key magic");
  }
  {  // an altered catalog.meta
    const fs::path d = t.sub("bad_meta") + "/" + p.library_id;
    fs::create_directories(d.parent_path());
    fs::copy(p.dir, d, fs::copy_options::recursive);
    Bytes m = p.meta;
    m[40] ^= 1;
    pfx::write_bytes(d / "catalog.meta", m);
    expect(d.string(), ws::v1::INSTALL_REASON_CATALOG_META_INVALID, "catalog.meta");
  }
  {  // validly signed but wrong: an inconsistent hash-list root; an unknown format minor; a replayed (lower) revision
    const fs::path d = t.sub("bad_hl") + "/" + p.library_id;
    fs::create_directories(d.parent_path());
    fs::copy(p.dir, d, fs::copy_options::recursive);
    pfx::write_bytes(d / "bundle.vlrp", pfx::resign(p.bundle, K1().seed, [](Bytes& manifest, Bytes&) { manifest[96 + 3] ^= 0x01; }));
    expect(d.string(), ws::v1::INSTALL_REASON_HASH_LIST_MISMATCH, "hash list root");
    pfx::write_bytes(d / "bundle.vlrp", pfx::resign(p.bundle, K1().seed, [](Bytes& manifest, Bytes& header) {
                       manifest[2] = 5;
                       header[6] = 5;
                     }));
    expect(d.string(), ws::v1::INSTALL_REASON_UNSUPPORTED_VERSION, "unknown minor");
    // an unprotected (format 2) bundle
    auto plain = pfx::make_writer({}).finalize();
    CHECK(plain.ok());
    pfx::write_bytes(d / "bundle.vlrp", plain.value());
    expect(d.string(), ws::v1::INSTALL_REASON_NOT_PROTECTED, "unprotected bundle");
  }
  {  // the wrong library
    const auto r = c.call(install_msg(p.dir.string(), "00000000-0000-4000-8000-000000000042"));
    CHECK(r && r->has_import_accepted());
    const auto ev = collect(c, r->import_accepted().job_id());
    const auto* it = item_of(ev);
    CHECK(it && it->install_reason() == ws::v1::INSTALL_REASON_LIBRARY_ID_MISMATCH && ev.back().state() == ws::v1::IMPORT_STATE_FAILED);
    CHECK(tree_has_no_library(sv.data()) && staging_clean(sv.data()));
  }
  // the good pack installs on the same service afterwards; then a replay of an OLDER signed revision is a typed downgrade
  const auto ev = run_install(c, p.dir.string());
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
  {
    pfx::PackSpec newer;
    newer.variant = 1;
    newer.revision = 2;
    const pfx::Pack pn = pfx::make_pack(t.sub("newer"), newer, K1());
    const auto evn = run_install(c, pn.dir.string());
    CHECK(!evn.empty() && evn.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
    expect(p.dir.string(), ws::v1::INSTALL_REASON_DOWNGRADE, "replayed older revision", false);
    auto l = list_libs(c);
    CHECK(l && l->library_list().total() == 1 && l->library_list().libraries(0).bundle_id() == pn.bundle_id);
  }
}

void test_request_refusals() {
  Svc sv([](LibraryServiceOptions& o) {
    o.free_bytes_fn = [](const std::string&, std::uint64_t& f) {
      f = 100ull << 20;  // below the reserve alone
      return true;
    };
  });
  CHECK(sv.start());
  Client c;
  CHECK(sv.connect(c));
  TempDir t;
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  auto code_of = [&](const ws::v1::LibraryClientMessage& m, const char* reason = nullptr) {
    auto r = c.call(m);
    if (!r || !r->has_error()) return ws::v1::LIBRARY_ERROR_CODE_UNSPECIFIED;
    if (reason) {
      const auto it = r->error().details().find("reason");
      if (it == r->error().details().end() || it->second != reason) {
        std::printf("FAIL refusal reason: want %s got %s\n", reason, it == r->error().details().end() ? "(none)" : it->second.c_str());
        ++g_fail;
      }
    }
    return r->error().code();
  };
  ws::v1::LibraryClientMessage m;
  m.mutable_install_protected();
  CHECK(code_of(m, "install_target") == ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND);  // neither
  m.mutable_install_protected()->set_source_dir(p.dir.string());
  m.mutable_install_protected()->set_bundle_path((p.dir / "bundle.vlrp").string());
  CHECK(code_of(m, "install_target") == ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND);  // both
  CHECK(code_of(install_msg("relative/dir"), "bad_path") == ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE);
  CHECK(code_of(install_msg(t.sub("nowhere")), "source_missing") == ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE);
  CHECK(code_of(install_msg((p.dir / "bundle.vlrp").string()), "not_a_directory") == ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE);
  fs::create_directories(t.sub("empty"));
  CHECK(code_of(install_msg(t.sub("empty")), "bundle_missing") == ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE);
  CHECK(code_of(install_msg(p.dir.string(), "not-a-uuid"), "invalid_library_id") == ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND);
  {
    ws::v1::LibraryClientMessage b;
    b.mutable_install_protected()->set_bundle_path(t.sub("missing.vlrp"));
    CHECK(code_of(b, "bundle_missing") == ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE);
  }
  // space: bundle + reserve does not fit
  auto r = c.call(install_msg(p.dir.string()));
  CHECK(r && r->has_error() && r->error().code() == ws::v1::LIBRARY_ERROR_CODE_INSUFFICIENT_SPACE);
  if (r && r->has_error()) {
    const auto& d = r->error().details();
    CHECK(d.count("required_bytes") == 1 && d.count("free_bytes") == 1 && std::stoull(d.at("required_bytes")) > std::stoull(d.at("free_bytes")));
    CHECK(d.at("free_bytes") == std::to_string(100ull << 20));
  }
  CHECK(tree_has_no_library(sv.data()));
}

void test_cancel_and_drive_removal() {
  TempDir t;
  pfx::PackSpec big;
  big.big = true;
  big.instruments = 1;
  big.frames = 200000;
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), big, K1());
  {  // cancel while the job is running (the worker is held in the test hook until the cancel is in)
    std::mutex m;
    std::condition_variable cv;
    bool at_hook = false, release = false;
    Svc sv([&](LibraryServiceOptions& o) {
      o.install_options_hook = [&](std::uint64_t, std::size_t, InstallOptions&) {
        std::unique_lock<std::mutex> lk(m);
        at_hook = true;
        cv.notify_all();
        cv.wait(lk, [&] { return release; });
      };
    });
    CHECK(sv.start());
    Client c;
    CHECK(sv.connect(c));
    auto r = c.call(install_msg(p.dir.string()));
    CHECK(r && r->has_import_accepted());
    const auto job = r->import_accepted().job_id();
    {
      std::unique_lock<std::mutex> lk(m);
      CHECK(cv.wait_for(lk, std::chrono::seconds(10), [&] { return at_hook; }));
    }
    ws::v1::LibraryClientMessage cm;
    cm.mutable_cancel_import()->set_job_id(job);
    auto ca = c.call(cm);
    CHECK(ca && ca->has_ack() && ca->ack().changed());
    {
      std::lock_guard<std::mutex> lk(m);
      release = true;
      cv.notify_all();
    }
    const auto ev = collect(c, job);
    CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_CANCELLED);
    const auto* it = item_of(ev);
    CHECK(it && it->status() == ws::v1::IMPORT_ITEM_CANCELLED);
    CHECK(tree_has_no_library(sv.data()) && staging_clean(sv.data()));
    auto l = list_libs(c);
    CHECK(l && l->library_list().total() == 0);
    // a queued install cancelled before it starts: ack changed, CANCELLED, nothing written
  }
  {  // the volume goes away while the bundle is copied: DRIVE_REMOVED, nothing published, nothing catalogued
    Svc sv([&](LibraryServiceOptions& o) {
      o.install_options_hook = [&](std::uint64_t, std::size_t, InstallOptions& io) {
        auto opened = std::make_shared<int>(0);
        io.open_source = [opened](const std::string& path) -> std::unique_ptr<fmt::ByteSource> {
          const bool is_bundle = path.size() > 11 && path.compare(path.size() - 11, 11, "bundle.vlrp") == 0;
          return std::make_unique<FailingSource>(path, is_bundle && ++*opened == 2 ? 3 : ~0ull);
        };
        io.source_gone = [] { return true; };
      };
    });
    CHECK(sv.start());
    Client c;
    CHECK(sv.connect(c));
    const auto ev = run_install(c, p.dir.string());
    const auto* it = item_of(ev);
    CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_DRIVE_REMOVED);
    CHECK(it && it->status() == ws::v1::IMPORT_ITEM_DRIVE_REMOVED && !it->detail().empty());
    CHECK(tree_has_no_library(sv.data()) && staging_clean(sv.data()));
    auto l = list_libs(c);
    CHECK(l && l->library_list().total() == 0);
    // a client that did not list import.checkpoints.v1 sees FAILED with message drive_removed, as for imports
    Client old;
    CHECK(old.connect(sv.o.socket_path));
    ws::v1::LibraryClientMessage h;
    h.mutable_hello()->set_version(1);
    CHECK(old.call(h).has_value());
    auto r = old.call(install_msg(p.dir.string()));
    CHECK(r && r->has_import_accepted());
    const auto ev2 = collect(old, r->import_accepted().job_id());
    CHECK(!ev2.empty() && ev2.back().state() == ws::v1::IMPORT_STATE_FAILED && ev2.back().message() == "drive_removed");
  }
}

void test_user_import_cannot_touch_factory() {
  TempDir t;
  // a Factory library whose id is exactly the id the user import of this SFZ would get (UUIDv5 of its canonical path): the worst case for a collision
  const fs::path sfz = ifx::write_sfz(t.sub("user"), "Clash");
  std::error_code ec;
  const std::string canon = fs::weakly_canonical(sfz, ec).generic_string();
  pfx::PackSpec spec;
  spec.library_id = fmt::uuid_to_string(fmt::library_id_from_source(canon));
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), spec, K1());
  Svc sv;
  CHECK(sv.start());
  Client c;
  CHECK(sv.connect(c));
  const auto ev = run_install(c, p.dir.string());
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
  const auto before = pfx::list_tree(fs::path(sv.data()) / "lib");
  // the user import path refuses at the request, typed, before anything is queued or written
  ws::v1::LibraryClientMessage m;
  m.mutable_import_request()->set_source(sfz.string());
  m.mutable_import_request()->add_items()->set_category(1);
  auto r = c.call(m);
  CHECK(r && r->has_error() && r->error().code() == ws::v1::LIBRARY_ERROR_CODE_FACTORY_LIBRARY_PROTECTED);
  CHECK(r && r->error().details().at("reason") == "factory_library_protected" && r->error().details().at("library_id") == p.library_id);
  CHECK(pfx::list_tree(fs::path(sv.data()) / "lib") == before);
  // the importer itself refuses too (the CLI path and a request that raced the install): nothing is added to the library
  ImportOptions io;
  io.sfz_path = sfz.string();
  io.root = sv.data();
  io.category = "piano";
  const ImportResult ir = import_file(io);
  CHECK(ir.status == Status::kFactoryProtected && !ir.ok() && !ir.bundle_created && !ir.current_flipped);
  CHECK(std::string(to_string(ir.status)) == "factory_library_protected");
  CHECK(pfx::list_tree(fs::path(sv.data()) / "lib") == before);
  auto l = list_libs(c);
  CHECK(l && l->library_list().total() == 1 && l->library_list().libraries(0).kind() == ws::v1::LIBRARY_KIND_FACTORY);
  // the Factory library is still the installed bundle
  CHECK(fs::read_symlink(fs::path(sv.data()) / "lib" / p.library_id / "current", ec).string() == p.bundle_id);
}

// The catalog gains `kind` by an additive migration: every row of a schema 2 catalog becomes 'user' and nothing else changes.
void test_schema3_migration() {
  TempDir t;
  fs::create_directories(t.sub("data"));
  {
    sqlite3* h = nullptr;
    CHECK(sqlite3_open(t.sub("data/catalog.db").c_str(), &h) == SQLITE_OK);
    CHECK(sqlite3_exec(h,
                       "PRAGMA journal_mode=WAL;"
                       "CREATE TABLE meta(key TEXT PRIMARY KEY, value INTEGER NOT NULL);"
                       "CREATE TABLE live_set_favorites(live_set_id TEXT PRIMARY KEY, added_seq INTEGER NOT NULL);"
                       "CREATE TABLE live_set_recent(live_set_id TEXT PRIMARY KEY, last_played_seq INTEGER NOT NULL);"
                       "CREATE TABLE libraries(library_id TEXT PRIMARY KEY CHECK(length(library_id) BETWEEN 1 AND 64), name TEXT NOT NULL,"
                       " source_kind TEXT NOT NULL CHECK(source_kind IN ('sfz','sf2')), source_path TEXT NOT NULL DEFAULT '', bundle_id TEXT NOT NULL CHECK(length(bundle_id) > 0),"
                       " instrument_count INTEGER NOT NULL CHECK(instrument_count >= 0), imported_seq INTEGER NOT NULL);"
                       "CREATE TABLE instruments(library_id TEXT NOT NULL REFERENCES libraries(library_id) ON DELETE CASCADE, instrument_key TEXT NOT NULL CHECK(length(instrument_key) > 0),"
                       " name TEXT NOT NULL, category INTEGER NOT NULL CHECK(category BETWEEN 1 AND 17), sample_count INTEGER NOT NULL DEFAULT 0, region_count INTEGER NOT NULL DEFAULT 0,"
                       " bytes INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(library_id, instrument_key));"
                       "INSERT INTO meta(key, value) VALUES('schema_version', 2), ('next_seq', 5);"
                       "INSERT INTO libraries VALUES('lib-a', 'A', 'sfz', '/x/a.sfz', 'b1', 1, 3);"
                       "INSERT INTO instruments VALUES('lib-a', 'a.sfz', 'A', 1, 1, 1, 100);"
                       "INSERT INTO live_set_favorites VALUES('fav', 1);"
                       "PRAGMA user_version=2;",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(h);
  }
  std::unique_ptr<Catalog> cat;
  CHECK(Catalog::open(t.sub("data"), cat) == CatalogStatus::kOk && cat->migrated() && cat->schema_version() == kCatalogSchemaVersion);
  std::vector<LibraryRecord> rows;
  CHECK(cat->list_libraries(rows, true) == CatalogStatus::kOk && rows.size() == 1 && rows[0].kind == LibraryKind::kUser && rows[0].instruments.size() == 1);
  LibraryRecord got;
  bool found = false;
  CHECK(cat->get_library("lib-a", got, found) == CatalogStatus::kOk && found && got.kind == LibraryKind::kUser && got.name == "A");
  std::vector<std::string> favs;
  CHECK(cat->favorites(favs) == CatalogStatus::kOk && favs == std::vector<std::string>{"fav"});
  // a Factory row round-trips, and a changed kind is a changed row
  LibraryRecord f = got;
  f.library_id = "lib-f";
  f.kind = LibraryKind::kFactory;
  bool changed = false;
  CHECK(cat->publish_library(f, changed) == CatalogStatus::kOk && changed);
  CHECK(cat->get_library("lib-f", got, found) == CatalogStatus::kOk && got.kind == LibraryKind::kFactory);
  LibraryRecord g = got;
  g.kind = LibraryKind::kUser;
  CHECK(!same_content(got, g));
  CHECK(cat->publish_library(g, changed) == CatalogStatus::kOk && changed);
  CHECK(cat->get_library("lib-f", got, found) == CatalogStatus::kOk && got.kind == LibraryKind::kUser);
  // the version number edited back to 2 on a file that already has the column: the step is tolerated (no "duplicate column")
  {
    sqlite3* e = nullptr;
    CHECK(sqlite3_open(t.sub("data/catalog.db").c_str(), &e) == SQLITE_OK);
    CHECK(sqlite3_exec(e, "UPDATE meta SET value=2 WHERE key='schema_version'; PRAGMA user_version=2;", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(e);
    cat.reset();
    std::unique_ptr<Catalog> again;
    CHECK(Catalog::open(t.sub("data"), again) == CatalogStatus::kOk && again->migrated() && again->schema_version() == kCatalogSchemaVersion);
    CHECK(again->get_library("lib-a", got, found) == CatalogStatus::kOk && found && got.name == "A");
    cat = std::move(again);
  }
  // a failed 2 -> 3 step (the injected fault before its commit) rolls back: the catalog stays at schema 2, untouched, and migrates at the next open
  {
    TempDir u;
    fs::create_directories(u.sub("data"));
    sqlite3* e = nullptr;
    CHECK(sqlite3_open(u.sub("data/catalog.db").c_str(), &e) == SQLITE_OK);
    CHECK(sqlite3_exec(e,
                       "PRAGMA journal_mode=WAL;"
                       "CREATE TABLE meta(key TEXT PRIMARY KEY, value INTEGER NOT NULL);"
                       "CREATE TABLE live_set_favorites(live_set_id TEXT PRIMARY KEY, added_seq INTEGER NOT NULL);"
                       "CREATE TABLE live_set_recent(live_set_id TEXT PRIMARY KEY, last_played_seq INTEGER NOT NULL);"
                       "CREATE TABLE libraries(library_id TEXT PRIMARY KEY, name TEXT NOT NULL, source_kind TEXT NOT NULL, source_path TEXT NOT NULL DEFAULT '', bundle_id TEXT NOT NULL,"
                       " instrument_count INTEGER NOT NULL, imported_seq INTEGER NOT NULL);"
                       "CREATE TABLE instruments(library_id TEXT NOT NULL, instrument_key TEXT NOT NULL, name TEXT NOT NULL, category INTEGER NOT NULL, sample_count INTEGER NOT NULL DEFAULT 0,"
                       " region_count INTEGER NOT NULL DEFAULT 0, bytes INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(library_id, instrument_key));"
                       "INSERT INTO meta(key, value) VALUES('schema_version', 2), ('next_seq', 2);PRAGMA user_version=2;",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(e);
    std::unique_ptr<Catalog> c2;
    CHECK(Catalog::open(u.sub("data"), c2, 2000, [](const char* point) { return std::string(point) == "migrate.before_commit"; }) == CatalogStatus::kFaultInjected && !c2);
    CHECK(sqlite3_open(u.sub("data/catalog.db").c_str(), &e) == SQLITE_OK);
    sqlite3_stmt* q = nullptr;
    CHECK(sqlite3_prepare_v2(e, "SELECT value FROM meta WHERE key='schema_version'", -1, &q, nullptr) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW && sqlite3_column_int(q, 0) == 2);
    sqlite3_finalize(q);
    sqlite3_stmt* cols = nullptr;
    CHECK(sqlite3_prepare_v2(e, "SELECT count(*) FROM pragma_table_info('libraries') WHERE name='kind'", -1, &cols, nullptr) == SQLITE_OK && sqlite3_step(cols) == SQLITE_ROW &&
          sqlite3_column_int(cols, 0) == 0);
    sqlite3_finalize(cols);
    sqlite3_close(e);
    CHECK(Catalog::open(u.sub("data"), c2) == CatalogStatus::kOk && c2->schema_version() == kCatalogSchemaVersion);
  }
  // the CHECK constraint on the column
  sqlite3* h = nullptr;
  CHECK(sqlite3_open(t.sub("data/catalog.db").c_str(), &h) == SQLITE_OK);
  CHECK(sqlite3_exec(h, "UPDATE libraries SET kind='bogus' WHERE library_id='lib-a'", nullptr, nullptr, nullptr) != SQLITE_OK);
  sqlite3_close(h);
}


struct Gate {
  std::mutex m;
  std::condition_variable cv;
  bool arrived = false, open = false;
  void reach() {
    std::unique_lock<std::mutex> lk(m);
    arrived = true;
    cv.notify_all();
    cv.wait(lk, [&] { return open; });
  }
  bool wait_arrived() {
    std::unique_lock<std::mutex> lk(m);
    return cv.wait_for(lk, std::chrono::seconds(20), [&] { return arrived; });
  }
  void release() {
    std::lock_guard<std::mutex> lk(m);
    open = true;
    cv.notify_all();
  }
};

void test_queue_cancel_reservation_shutdown() {
  TempDir t;
  const pfx::Pack p1 = pfx::make_pack(t.sub("p1"), {}, K1());
  pfx::PackSpec sp2;
  sp2.library_id = "00000000-0000-4000-8000-0000000000b2";
  const pfx::Pack p2 = pfx::make_pack(t.sub("p2"), sp2, K1());
  pfx::PackSpec sp3;
  sp3.library_id = "00000000-0000-4000-8000-0000000000b3";
  const pfx::Pack p3 = pfx::make_pack(t.sub("p3"), sp3, K1());
  {  // QUEUE_FULL, and a QUEUED install cancelled before it starts
    Gate gate;
    Svc sv([&](LibraryServiceOptions& o) {
      o.max_queued_jobs = 1;
      o.install_options_hook = [&](std::uint64_t job, std::size_t, InstallOptions&) {
        if (job == 1) gate.reach();
      };
    });
    CHECK(sv.start());
    Client c;
    CHECK(sv.connect(c));
    auto a1 = c.call(install_msg(p1.dir.string()));
    CHECK(a1 && a1->has_import_accepted() && a1->import_accepted().queue_position() == 0);
    CHECK(gate.wait_arrived());  // job 1 is running, held in the hook
    auto a2 = c.call(install_msg(p2.dir.string()));
    CHECK(a2 && a2->has_import_accepted() && a2->import_accepted().queue_position() == 1);
    auto a3 = c.call(install_msg(p3.dir.string()));
    CHECK(a3 && a3->has_error() && a3->error().code() == ws::v1::LIBRARY_ERROR_CODE_QUEUE_FULL && a3->error().details().at("limit") == "1");
    ws::v1::LibraryClientMessage cm;
    cm.mutable_cancel_import()->set_job_id(a2->import_accepted().job_id());
    auto ca = c.call(cm);
    CHECK(ca && ca->has_ack() && ca->ack().changed());
    const auto ev2 = collect(c, a2->import_accepted().job_id());
    CHECK(!ev2.empty() && ev2.back().state() == ws::v1::IMPORT_STATE_CANCELLED && item_of(ev2) == nullptr);  // it never started
    gate.release();
    const auto ev1 = collect(c, a1->import_accepted().job_id());
    CHECK(!ev1.empty() && ev1.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
    auto l = list_libs(c);
    CHECK(l && l->library_list().total() == 1 && l->library_list().libraries(0).library_id() == p1.library_id);  // only the first installed
    CHECK(!fs::exists(fs::path(sv.data()) / "lib" / p2.library_id));
  }
  {  // the space of a running install is reserved: a second install and an import must fit BESIDE it
    Gate gate;
    const std::uint64_t need = required_install_bytes(p1.bundle.size());
    Svc sv([&](LibraryServiceOptions& o) {
      o.free_bytes_fn = [need](const std::string&, std::uint64_t& f) {
        f = need + (1ull << 20);  // one install fits, two do not
        return true;
      };
      o.install_options_hook = [&](std::uint64_t job, std::size_t, InstallOptions&) {
        if (job == 1) gate.reach();
      };
    });
    CHECK(sv.start());
    Client c;
    CHECK(sv.connect(c));
    auto a1 = c.call(install_msg(p1.dir.string()));
    CHECK(a1 && a1->has_import_accepted() && a1->import_accepted().required_bytes() == need);
    CHECK(gate.wait_arrived());
    auto a2 = c.call(install_msg(p2.dir.string()));
    CHECK(a2 && a2->has_error() && a2->error().code() == ws::v1::LIBRARY_ERROR_CODE_INSUFFICIENT_SPACE && a2->error().details().at("reserved_bytes") == std::to_string(need));
    const fs::path sfz = ifx::write_sfz(t.sub("u"), "Piano");
    ws::v1::LibraryClientMessage m;
    m.mutable_import_request()->set_source(sfz.string());
    m.mutable_import_request()->add_items()->set_category(1);
    auto ai = c.call(m);  // an import beside the install: the same reservation
    CHECK(ai && ai->has_error() && ai->error().code() == ws::v1::LIBRARY_ERROR_CODE_INSUFFICIENT_SPACE && ai->error().details().at("reserved_bytes") == std::to_string(need));
    gate.release();
    const auto ev1 = collect(c, a1->import_accepted().job_id());
    CHECK(!ev1.empty() && ev1.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
    auto a3 = c.call(install_msg(p2.dir.string()));  // the reservation is released at the end of the job
    CHECK(a3 && a3->has_import_accepted());
    CHECK(!collect(c, a3->import_accepted().job_id()).empty());
  }
  {  // SHUTTING_DOWN: once stop() has begun, a new install is refused; the running and the queued one end CANCELLED
    Gate gate;
    Svc sv([&](LibraryServiceOptions& o) {
      o.max_queued_jobs = 1000;
      o.install_options_hook = [&](std::uint64_t job, std::size_t, InstallOptions&) {
        if (job == 1) gate.reach();
      };
    });
    CHECK(sv.start());
    Client c;
    CHECK(sv.connect(c));
    auto a1 = c.call(install_msg(p1.dir.string()));
    CHECK(a1 && a1->has_import_accepted());
    CHECK(gate.wait_arrived());
    auto a2 = c.call(install_msg(p2.dir.string()));
    CHECK(a2 && a2->has_import_accepted());
    std::thread stopper([&] { sv.s->stop(); });
    bool refused = false;
    for (int i = 0; i < 4000 && !refused; ++i) {
      auto r = c.call(install_msg(p3.dir.string()));
      if (r && r->has_error() && r->error().code() == ws::v1::LIBRARY_ERROR_CODE_SHUTTING_DOWN) refused = true;
      else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(refused);
    gate.release();
    stopper.join();
    const auto ev1 = collect(c, a1->import_accepted().job_id(), 5000);
    CHECK(!ev1.empty() && (ev1.back().state() == ws::v1::IMPORT_STATE_CANCELLED || ev1.back().state() == ws::v1::IMPORT_STATE_PUBLISHED));
    sv.s.reset();
    CHECK(sv.start());  // restart: consistent
    CHECK(sv.s->startup_reconcile().errors.empty() && staging_clean(sv.data()));
  }
}

void test_fifo_in_request() {
  TempDir t;
  const pfx::Pack p = pfx::make_pack(t.sub("p"), {}, K1());
  Svc sv;
  CHECK(sv.start());
  Client c;
  CHECK(sv.connect(c));
  const std::string fifo = t.sub("a.fifo");
  CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
  for (int which = 0; which < 3; ++which) {
    ws::v1::LibraryClientMessage m;
    auto* r = m.mutable_install_protected();
    r->set_source_dir(p.dir.string());
    if (which == 0) r->set_wrapped_key_path(fifo);
    if (which == 1) r->set_catalog_meta_path(fifo);
    if (which == 2) r->set_catalog_meta_seal_path(fifo);
    auto a = c.call(m);
    CHECK(a && a->has_error() && a->error().code() == ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE && a->error().details().at("reason") == "not_a_file");
  }
  ws::v1::LibraryClientMessage b;
  b.mutable_install_protected()->set_bundle_path(fifo);
  auto a = c.call(b);
  CHECK(a && a->has_error() && a->error().details().at("reason") == "bundle_missing");
  // a FIFO inside the pack directory: the job ends FAILED with a typed result, no hang
  pfx::Pack q = p;
  q.dir = t.sub("fp") + "/" + p.library_id;
  fs::create_directories(q.dir.parent_path());
  fs::copy(p.dir, q.dir, fs::copy_options::recursive);
  fs::remove(q.dir / "catalog.meta");
  CHECK(::mkfifo((q.dir / "catalog.meta").c_str(), 0600) == 0);
  ::alarm(120);
  const auto ev = run_install(c, q.dir.string());
  ::alarm(0);
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_FAILED && tree_has_no_library(sv.data()));
}

std::string run_proc(const std::string& cmd, int& rc) {
  const std::string out = "/tmp/vl_isvc_" + std::to_string(::getpid()) + ".txt";
  const int r = std::system((cmd + " >/dev/null 2>'" + out + "'").c_str());
  rc = WIFEXITED(r) ? WEXITSTATUS(r) : -1;
  const std::string text = ltest::slurp(out);
  std::remove(out.c_str());
  return text;
}

// The CLI install is a writer: a running service refuses it (single writer); the user import refuses a Factory library through the REAL `librarian import` process,
// for an SFZ, for an SF2 (with checkpoints, before anything is converted) and for a request whose SECOND item clashes; and an install that has no catalog row yet
// is protected too.
void test_user_import_refusals_and_single_writer() {
  TempDir t;
  const fs::path sfz = ifx::write_sfz(t.sub("user"), "Clash");
  const fs::path ok_sfz = ifx::write_sfz(t.sub("user"), "Fine");
  const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("user")) / "multi.sf2", 5);
  auto id_of = [&](const fs::path& f) {
    std::error_code ec;
    return fmt::uuid_to_string(fmt::library_id_from_source(fs::weakly_canonical(f, ec).generic_string()));
  };
  pfx::PackSpec a, b;
  a.library_id = id_of(sfz);
  b.library_id = id_of(sf2);
  const pfx::Pack pa = pfx::make_pack(t.sub("pa"), a, K1()), pb = pfx::make_pack(t.sub("pb"), b, K1());
  char idh[17], pubh[65];
  for (int i = 0; i < 8; ++i) std::snprintf(idh + 2 * i, 3, "%02x", K1().id[i]);
  for (int i = 0; i < 32; ++i) std::snprintf(pubh + 2 * i, 3, "%02x", K1().pub[i]);
  const std::string tl = t.sub("trust.pub-list");
  pfx::write_bytes(tl, pfx::as_bytes(std::string("dev content-manifest ") + idh + " " + pubh + "\n"));
  Svc sv;
  CHECK(sv.start());
  int rc = 0;
  // the service holds the data root: the CLI install is refused, nothing is written
  std::string err = run_proc("'" LIBRARIAN_EXE "' install-protected '" + pa.dir.string() + "' --root '" + sv.data() + "' --trust-list '" + tl + "'", rc);
  CHECK(rc == 1 && err.find("service") != std::string::npos && tree_has_no_library(sv.data()));
  Client c;
  CHECK(sv.connect(c));
  // the install goes through the API; the window before its catalog row exists is covered by installing straight into the root as well
  CHECK(!run_install(c, pa.dir.string()).empty());
  {
    InstallOptions o;
    o.root = sv.data();
    o.source_dir = pb.dir.string();
    o.trusted = &K1().trusted;
    CHECK(install_protected(o).status == InstallStatus::kInstalled);  // no catalog row for pb yet: only the file system knows
  }
  auto l = list_libs(c);
  CHECK(l && l->library_list().total() == 1);
  const auto before = pfx::list_tree(fs::path(sv.data()) / "lib");
  // a request whose second item clashes (SF2 whose library has bundles on disk but no row): refused as a whole, nothing queued, nothing written
  {
    ws::v1::LibraryClientMessage m;
    m.mutable_import_request()->set_source(t.sub("user"));
    auto* i1 = m.mutable_import_request()->add_items();
    i1->set_path(ok_sfz.string());
    i1->set_category(1);
    auto* i2 = m.mutable_import_request()->add_items();
    i2->set_path(sf2.string());
    i2->set_category(1);
    auto r = c.call(m);
    CHECK(r && r->has_error() && r->error().code() == ws::v1::LIBRARY_ERROR_CODE_FACTORY_LIBRARY_PROTECTED && r->error().details().at("library_id") == pb.library_id);
    CHECK(pfx::list_tree(fs::path(sv.data()) / "lib") == before);
    // and with the clashing SFZ second
    ws::v1::LibraryClientMessage m2;
    m2.mutable_import_request()->set_source(t.sub("user"));
    auto* j1 = m2.mutable_import_request()->add_items();
    j1->set_path(ok_sfz.string());
    j1->set_category(1);
    auto* j2 = m2.mutable_import_request()->add_items();
    j2->set_path(sfz.string());
    j2->set_category(1);
    auto r2 = c.call(m2);
    CHECK(r2 && r2->has_error() && r2->error().code() == ws::v1::LIBRARY_ERROR_CODE_FACTORY_LIBRARY_PROTECTED && pfx::list_tree(fs::path(sv.data()) / "lib") == before);
  }
  // the importer itself, for an SF2 with checkpoints: refused before any preset is converted
  {
    ImportOptions io;
    io.sfz_path = sf2.string();
    io.root = sv.data();
    io.category = "piano";
    io.checkpoint_timbres = 2;
    bool converted = false;
    io.fault.on_timbre = [&](const char*, std::size_t) {
      converted = true;
      return HookAction::kContinue;
    };
    const ImportResult ir = import_file(io);
    CHECK(ir.status == Status::kFactoryProtected && !converted && ir.checkpoints == 0 && pfx::list_tree(fs::path(sv.data()) / "lib") == before);
  }
  sv.stop();
  // the real CLI process, with the service gone
  err = run_proc("'" LIBRARIAN_EXE "' import '" + sfz.string() + "' --root '" + sv.data() + "' --category piano", rc);
  CHECK(rc == 1 && err.find("factory_library_protected") != std::string::npos && pfx::list_tree(fs::path(sv.data()) / "lib") == before);
  err = run_proc("'" LIBRARIAN_EXE "' import '" + sf2.string() + "' --root '" + sv.data() + "' --category piano --checkpoint-timbres 2", rc);
  CHECK(rc == 1 && err.find("factory_library_protected") != std::string::npos && pfx::list_tree(fs::path(sv.data()) / "lib") == before);
  // an ordinary file is still imported by the same process (the refusal is about the Factory library only)
  err = run_proc("'" LIBRARIAN_EXE "' import '" + ok_sfz.string() + "' --root '" + sv.data() + "' --category piano", rc);
  CHECK(rc == 0);
  // the CLI install is the same refusal when it meets a Factory-held id: fine, it is the owner of that path
  err = run_proc("'" LIBRARIAN_EXE "' install-protected '" + pa.dir.string() + "' --root '" + sv.data() + "' --trust-list '" + tl + "'", rc);
  CHECK(rc == 0);
}

// The real `librarian-service` binary with --trust-list: the dev variant reads the list, an unreadable list is a usage error.
void test_service_binary_trust_list() {
  TempDir t;
  const pfx::Pack p = pfx::make_pack(t.sub("p"), {}, K1());
  char idh[17], pubh[65];
  for (int i = 0; i < 8; ++i) std::snprintf(idh + 2 * i, 3, "%02x", K1().id[i]);
  for (int i = 0; i < 32; ++i) std::snprintf(pubh + 2 * i, 3, "%02x", K1().pub[i]);
  const std::string tl = t.sub("trust.pub-list");
  pfx::write_bytes(tl, pfx::as_bytes(std::string("dev content-manifest ") + idh + " " + pubh + "\n"));
  fs::create_directories(t.sub("data"));
  int rc = 0;
  run_proc("'" LIBRARIAN_SERVICE_EXE "' --data '" + t.sub("data") + "' --socket '" + t.sub("s.sock") + "' --trust-list '" + t.sub("missing") + "'", rc);
  CHECK(rc == 2);
  const pid_t pid = ::fork();
  if (pid == 0) {
    const int dn = ::open("/dev/null", O_WRONLY);
    ::dup2(dn, 1);
    ::dup2(dn, 2);
    ::execl(LIBRARIAN_SERVICE_EXE, "librarian-service", "--data", t.sub("data").c_str(), "--socket", t.sub("s.sock").c_str(), "--trust-list", tl.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  Client c;
  bool up = ltest::wait_until([&] { return c.connect(t.sub("s.sock")); }, 15000);
  CHECK(up && c.handshake("smoke"));
  if (up) {
    const auto ev = run_install(c, p.dir.string());
    CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
    auto l = list_libs(c);
    CHECK(l && l->library_list().total() == 1 && l->library_list().libraries(0).kind() == ws::v1::LIBRARY_KIND_FACTORY);
  }
  c.close();
  ::kill(pid, SIGTERM);
  int st = 0;
  ::waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

}  // namespace

int main() {
  test_install_job_and_kinds();
  test_rejections_leave_nothing();
  test_request_refusals();
  test_cancel_and_drive_removal();
  test_user_import_cannot_touch_factory();
  test_queue_cancel_reservation_shutdown();
  test_fifo_in_request();
  test_user_import_refusals_and_single_writer();
  test_service_binary_trust_list();
  test_schema3_migration();
  if (g_fail == 0) std::printf("librarian_install_service_test: ok\n");
  return g_fail == 0 ? 0 : 1;
}
