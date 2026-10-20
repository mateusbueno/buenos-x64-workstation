// Story 5.7 tests: import preflight (batch scan, free space, duplicates). The scanner is exercised directly (preflight_scan) and through the Library API
// over a real Unix socket. Fixtures are generated (import_fixtures.hpp); nothing is committed. There is no timing assertion: the 1,000-file volume test
// asserts counts and correctness and prints the duration the reply reports (a Mac figure, not representative of the board).
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <set>
#include <sys/stat.h>

#include "import_fixtures.hpp"
#include "lib_test_util.hpp"
#include "velora/format/hash.hpp"
#include "velora/librarian/category_suggest.hpp"
#include "velora/librarian/import.hpp"
#include "velora/librarian/library_service.hpp"
#include "velora/librarian/preflight.hpp"

using namespace velora::librarian;
namespace fs = std::filesystem;
namespace fmt = velora::format;
using ltest::Client;
using ltest::TempDir;

namespace {

constexpr unsigned kPiano = 1;

struct Svc {
  TempDir td;
  LibraryServiceOptions o;
  std::unique_ptr<LibraryService> s;
  std::string err;
  explicit Svc(const std::function<void(LibraryServiceOptions&)>& cfg = {}) {
    o.data_dir = td.sub("data");
    fs::create_directories(o.data_dir);
    o.socket_path = td.sub("s.sock");
    if (cfg) cfg(o);
  }
  bool start() {
    s = std::make_unique<LibraryService>(o);
    return s->start(err);
  }
  ~Svc() {
    if (s) s->stop();
  }
  bool connect(Client& c) { return c.connect(o.socket_path) && c.handshake("preflight-test"); }
};

ws::v1::LibraryClientMessage pre_msg(const std::string& source, const std::string& label = {}, std::uint32_t max_files = 0) {
  ws::v1::LibraryClientMessage m;
  m.mutable_preflight_request()->set_source(source);
  m.mutable_preflight_request()->set_volume_label(label);
  m.mutable_preflight_request()->set_max_files(max_files);
  return m;
}

// Sends a preflight request and waits for its PreflightDone.
std::optional<ws::v1::LibraryServerMessage> preflight(Client& c, const std::string& source, const std::string& label = {}, std::uint32_t max_files = 0) {
  return c.call(pre_msg(source, label, max_files), 120000);
}

// All rows of a stored result, paged with a small page size to exercise paging.
std::vector<ws::v1::PreflightRow> all_rows(Client& c, std::uint64_t id, std::uint32_t page = 7, bool* ok = nullptr) {
  std::vector<ws::v1::PreflightRow> rows;
  std::uint32_t off = 0;
  bool good = true;
  for (;;) {
    ws::v1::LibraryClientMessage m;
    m.mutable_list_preflight_rows()->set_preflight_id(id);
    m.mutable_list_preflight_rows()->set_offset(off);
    m.mutable_list_preflight_rows()->set_limit(page);
    auto r = c.call(m, 60000);
    if (!r || !r->has_preflight_rows()) {
      good = false;
      break;
    }
    for (const auto& pr : r->preflight_rows().rows()) rows.push_back(pr);
    off += static_cast<std::uint32_t>(r->preflight_rows().rows_size());
    if (!r->preflight_rows().more() || r->preflight_rows().rows_size() == 0) break;
  }
  if (ok) *ok = good;
  return rows;
}

const ws::v1::PreflightRow* find_row(const std::vector<ws::v1::PreflightRow>& rows, const std::string& rel, const std::string& key = {}) {
  for (const auto& r : rows)
    if (r.rel_path() == rel && (key.empty() || r.instrument_key() == key)) return &r;
  return nullptr;
}

bool is_error(const std::optional<ws::v1::LibraryServerMessage>& r, ws::v1::LibraryErrorCode code) {
  return r && r->has_error() && r->error().code() == code;
}

// A WAV whose data chunk claims more bytes than the file holds (a cut-off copy).
std::string truncated_wav() {
  std::string w = ifx::wav16(300, 3);
  return w.substr(0, 44 + 100);  // the header says 600 data bytes, 100 are there
}

// A big WAV without writing its data: the header is valid and the file is sparse (a read of the data would show in bytes_read).
void write_sparse_wav(const fs::path& p, std::uint32_t data_bytes) {
  std::string head = ifx::wav16(0, 1);  // 44-byte header with a data size of 0
  auto le32 = [](std::uint32_t v) { return std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; };
  head.replace(4, 4, le32(36 + data_bytes));
  head.replace(40, 4, le32(data_bytes));
  fs::create_directories(p.parent_path());
  const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    [[maybe_unused]] const ssize_t w = ::write(fd, head.data(), head.size());
    [[maybe_unused]] const int t = ::ftruncate(fd, static_cast<off_t>(44 + data_bytes));
    ::close(fd);
  }
}

// The volume used by several tests: root files, two folders, every status except already_imported.
struct Volume {
  TempDir td;
  std::string root() const { return td.sub("USB"); }
  Volume() {
    const fs::path r = root();
    ifx::write_sfz(r, "Piano");                    // ready: the name says piano
    ifx::write_sfz(r, "qzxv");                     // needs_category: nothing in the name suggests one
    ifx::write_sfz(r / "Strings", "Violin");       // ready
    ifx::write_damaged_sfz(r / "Strings", "Gone"); // skipped_damaged: missing sample
    ifx::write_unsupported_sfz(r / "Strings", "Odd");  // skipped_unsupported_features
    ifx::put(r / "Strings" / "Cut_s.wav", truncated_wav());  // skipped_damaged: truncated sample
    ifx::put(r / "Strings" / "Cut.sfz", "<region> sample=Cut_s.wav key=60\n");
    ifx::write_sf2(r / "Fonts" / "Gm.sf2");        // six rows
    ifx::put(r / "Fonts" / "Bad.sf2", "RIFF\x10\x00\x00\x00sfbkgarbage garbage");  // skipped_damaged: not a font
  }
};

void test_scan_statuses_and_libraries() {
  Volume v;
  PreflightOptions o;
  o.source = v.root();
  o.volume_label = "MY_USB";
  const PreflightResult r = preflight_scan(o);
  CHECK(r.ok && !r.truncated && !r.cancelled);
  CHECK(r.volume_label == "MY_USB");
  // The root files form the first library, named after the label; then the folders by name.
  CHECK(r.libraries.size() == 3);
  if (r.libraries.size() == 3) {
    CHECK(r.libraries[0].name == "MY_USB" && r.libraries[0].folder.empty() && r.libraries[0].row_count == 2);
    CHECK(r.libraries[1].name == "Fonts" && r.libraries[1].folder == "Fonts" && r.libraries[1].row_count == 7);  // 6 presets + the broken font
    CHECK(r.libraries[2].name == "Strings" && r.libraries[2].row_count == 4);
  }
  CHECK(r.files_scanned == 8);
  auto row = [&](const std::string& rel, const std::string& key = {}) -> const PreflightRow* {
    for (const auto& x : r.rows)
      if (x.rel_path == rel && (key.empty() || x.instrument_key == key)) return &x;
    return nullptr;
  };
  const PreflightRow* piano = row("Piano.sfz");
  CHECK(piano && piano->status == RowStatus::kReady && piano->suggested == fmt::Category::kPiano && piano->format == "sfz");
  CHECK(piano && piano->name == "Piano" && piano->instrument_key == "Piano.sfz" && piano->pcm_bytes == 600 && piano->size_bytes > 600);
  const PreflightRow* qz = row("qzxv.sfz");
  CHECK(qz && qz->status == RowStatus::kNeedsCategory && qz->suggested == fmt::Category::kNone);
  CHECK(row("Strings/Violin.sfz") && row("Strings/Violin.sfz")->status == RowStatus::kReady);
  const PreflightRow* gone = row("Strings/Gone.sfz");
  CHECK(gone && gone->status == RowStatus::kSkippedDamaged && gone->pcm_bytes == 0 && !gone->detail.empty() && gone->instrument_key.empty());
  const PreflightRow* odd = row("Strings/Odd.sfz");
  CHECK(odd && odd->status == RowStatus::kSkippedUnsupportedFeatures && odd->pcm_bytes == 0);
  const PreflightRow* cut = row("Strings/Cut.sfz");
  CHECK(cut && cut->status == RowStatus::kSkippedDamaged);
  // SF2: one row per preset, keyed "bank:preset", and a file that is not a font is one damaged row.
  std::set<std::string> keys;
  for (const auto& x : r.rows)
    if (x.rel_path == "Fonts/Gm.sf2") {
      keys.insert(x.instrument_key);
      CHECK(x.format == "sf2" && x.status != RowStatus::kSkippedDamaged && x.pcm_bytes > 0);
    }
  CHECK(keys == (std::set<std::string>{"0:0", "0:1", "0:48", "8:0", "128:0", "1:5"}));
  const PreflightRow* bad = row("Fonts/Bad.sf2");
  CHECK(bad && bad->status == RowStatus::kSkippedDamaged && bad->format == "sf2");
  // library_id is the importer's: UUIDv5 of the canonical absolute path through formatlib's one minting function.
  std::error_code ec;
  const std::string canon = fs::canonical(fs::path(v.root()) / "Piano.sfz", ec).generic_string();
  CHECK(piano && piano->library_id == fmt::uuid_to_string(fmt::library_id_from_source(canon)));
  CHECK(piano && piano->path == canon);
  // Without a label the folder's name is the library name.
  PreflightOptions o2;
  o2.source = v.root();
  const PreflightResult r2 = preflight_scan(o2);
  CHECK(r2.ok && r2.libraries.size() == 3 && r2.libraries[0].name == "USB");
  // Not a folder.
  PreflightOptions o3;
  o3.source = v.root() + "/Piano.sfz";
  CHECK(!preflight_scan(o3).ok);
}

void test_service_preflight_all_five_statuses_and_import_again() {
  Volume v;
  Svc svc;
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  auto r = preflight(c, v.root(), "MY_USB");
  CHECK(r && r->has_preflight_done());
  if (!r || !r->has_preflight_done()) return;
  const auto& d = r->preflight_done();
  CHECK(d.preflight_id() != 0 && d.volume_label() == "MY_USB" && d.libraries_total() == 3 && d.libraries_size() == 3);
  CHECK(d.rows_total() == 13 && d.files_scanned() == 8 && !d.truncated() && !d.cancelled());
  CHECK(d.skipped_damaged() == 3 && d.skipped_unsupported_features() == 1 && d.already_imported() == 0);
  CHECK(d.ready() + d.needs_category() == 9);
  CHECK(d.bytes_read() > 0);
  CHECK(d.libraries(0).name() == "MY_USB" && d.libraries(0).rows() == 2 && d.libraries(0).ready() == 1 && d.libraries(0).needs_category() == 1);
  bool ok = false;
  auto rows = all_rows(c, d.preflight_id(), 7, &ok);
  CHECK(ok && rows.size() == 13);
  const auto* piano = find_row(rows, "Piano.sfz");
  CHECK(piano && piano->status() == ws::v1::PREFLIGHT_STATUS_READY && piano->action() == ws::v1::IMPORT_ACTION_IMPORT && piano->suggested_category() == kPiano);
  CHECK(find_row(rows, "qzxv.sfz") && find_row(rows, "qzxv.sfz")->status() == ws::v1::PREFLIGHT_STATUS_NEEDS_CATEGORY);
  CHECK(find_row(rows, "Strings/Gone.sfz") && find_row(rows, "Strings/Gone.sfz")->status() == ws::v1::PREFLIGHT_STATUS_SKIPPED_DAMAGED &&
        find_row(rows, "Strings/Gone.sfz")->action() == ws::v1::IMPORT_ACTION_SKIP);
  CHECK(find_row(rows, "Strings/Odd.sfz") && find_row(rows, "Strings/Odd.sfz")->status() == ws::v1::PREFLIGHT_STATUS_SKIPPED_UNSUPPORTED_FEATURES);
  // Paging by library.
  {
    ws::v1::LibraryClientMessage m;
    m.mutable_list_preflight_rows()->set_preflight_id(d.preflight_id());
    m.mutable_list_preflight_rows()->set_library(2);
    auto pr = c.call(m);
    CHECK(pr && pr->has_preflight_rows() && pr->preflight_rows().total() == 4 && pr->preflight_rows().rows_size() == 4);
  }
  CHECK(is_error(c.call([] {
    ws::v1::LibraryClientMessage m;
    m.mutable_list_preflight_rows()->set_preflight_id(999);
    return m;
  }()), ws::v1::LIBRARY_ERROR_CODE_UNKNOWN_PREFLIGHT));

  // Import Piano.sfz (category given), then preflight again: it is `already imported`, default action Skip.
  const std::string piano_path = piano ? piano->path() : std::string();
  auto import_one = [&](ws::v1::ImportAction action, unsigned cat) {
    ws::v1::LibraryClientMessage m;
    auto* req = m.mutable_import_request();
    req->set_source(v.root());
    auto* it = req->add_items();
    it->set_path(piano_path);
    if (cat) it->set_category(cat);
    it->set_action(action);
    return c.call(m, 30000);
  };
  auto wait_terminal = [&](std::uint64_t job) {
    ws::v1::ImportProgress last;
    for (int i = 0; i < 2000; ++i) {
      ws::v1::LibraryClientMessage m;
      m.mutable_import_status()->set_job_id(job);
      auto s = c.call(m);
      if (s && s->has_import_progress()) {
        last = s->import_progress();
        if (last.state() == ws::v1::IMPORT_STATE_PUBLISHED || last.state() == ws::v1::IMPORT_STATE_FAILED || last.state() == ws::v1::IMPORT_STATE_CANCELLED) break;
      }
      ::usleep(5000);
    }
    return last;
  };
  auto a1 = import_one(ws::v1::IMPORT_ACTION_DEFAULT, kPiano);
  CHECK(a1 && a1->has_import_accepted() && a1->import_accepted().item_count() == 1 && a1->import_accepted().skipped_count() == 0);
  if (a1 && a1->has_import_accepted()) {
    CHECK(a1->import_accepted().required_bytes() == required_import_bytes(600, 1) && a1->import_accepted().free_bytes() > 0);
    const auto last = wait_terminal(a1->import_accepted().job_id());
    CHECK(last.state() == ws::v1::IMPORT_STATE_PUBLISHED && last.items_imported() == 1);
    // The library the importer catalogued has exactly the library_id the preflight minted.
    ws::v1::LibraryClientMessage lm;
    lm.mutable_list_libraries();
    auto ll = c.call(lm);
    CHECK(ll && ll->has_library_list() && ll->library_list().libraries_size() == 1 && piano &&
          ll->library_list().libraries(0).library_id() == piano->library_id());
  }
  auto r2 = preflight(c, v.root(), "MY_USB");
  CHECK(r2 && r2->has_preflight_done() && r2->preflight_done().already_imported() == 1 && r2->preflight_done().preflight_id() != d.preflight_id());
  if (r2 && r2->has_preflight_done()) {
    auto rows2 = all_rows(c, r2->preflight_done().preflight_id());
    const auto* p2 = find_row(rows2, "Piano.sfz");
    CHECK(p2 && p2->status() == ws::v1::PREFLIGHT_STATUS_ALREADY_IMPORTED && p2->action() == ws::v1::IMPORT_ACTION_SKIP);
    CHECK(p2 && piano && p2->library_id() == piano->library_id());
    CHECK(r2->preflight_done().libraries(0).already_imported() == 1 && r2->preflight_done().libraries(0).ready() == 0);
    // The already imported row is not part of the space total.
    CHECK(r2->preflight_done().pcm_bytes() < d.pcm_bytes());
  }
  // Skip leaves the item out: nothing to import is a typed refusal.
  auto skip = import_one(ws::v1::IMPORT_ACTION_SKIP, 0);
  CHECK(is_error(skip, ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND) && skip->error().details().at("reason") == "nothing_to_import");
  // Import again (explicit action): accepted, the identical bundle is "unchanged"; a missing category is still refused.
  CHECK(is_error(import_one(ws::v1::IMPORT_ACTION_IMPORT, 0), ws::v1::LIBRARY_ERROR_CODE_CATEGORY_REQUIRED));
  auto again = import_one(ws::v1::IMPORT_ACTION_IMPORT, kPiano);
  CHECK(again && again->has_import_accepted());
  if (again && again->has_import_accepted()) {
    const auto last = wait_terminal(again->import_accepted().job_id());
    CHECK(last.state() == ws::v1::IMPORT_STATE_PUBLISHED && last.items_unchanged() == 1);
  }
  // A mixed request: one item skipped by action (needs no category), one imported.
  {
    ws::v1::LibraryClientMessage m;
    auto* req = m.mutable_import_request();
    req->set_source(v.root());
    auto* s1 = req->add_items();
    s1->set_path("qzxv.sfz");
    s1->set_action(ws::v1::IMPORT_ACTION_SKIP);
    auto* s2 = req->add_items();
    s2->set_path("Strings/Violin.sfz");
    s2->set_category(8);
    auto mixed = c.call(m, 30000);
    CHECK(mixed && mixed->has_import_accepted() && mixed->import_accepted().item_count() == 1 && mixed->import_accepted().skipped_count() == 1);
    // The SKIP item (qzxv, 600 bytes) is not in the space figure: exactly one item's worth.
    CHECK(mixed && mixed->has_import_accepted() && mixed->import_accepted().required_bytes() == required_import_bytes(600, 1) && mixed->import_accepted().free_known());
    if (mixed && mixed->has_import_accepted()) wait_terminal(mixed->import_accepted().job_id());
  }
}

void test_sf2_row_already_imported() {
  TempDir td;
  const fs::path src = fs::path(td.path) / "fonts";
  ifx::write_sf2(src / "Gm.sf2");
  Svc svc;
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  auto before = preflight(c, src.string());
  CHECK(before && before->has_preflight_done() && before->preflight_done().rows_total() == 6 && before->preflight_done().already_imported() == 0);
  ws::v1::LibraryClientMessage m;
  auto* req = m.mutable_import_request();
  req->set_source(src.string());
  auto* it = req->add_items();
  it->set_path("Gm.sf2");
  it->set_category(kPiano);
  auto acc = c.call(m, 30000);
  CHECK(acc && acc->has_import_accepted());
  for (int i = 0; i < 2000 && acc && acc->has_import_accepted(); ++i) {
    ws::v1::LibraryClientMessage q;
    q.mutable_import_status()->set_job_id(acc->import_accepted().job_id());
    auto s = c.call(q);
    if (s && s->has_import_progress() && s->import_progress().state() == ws::v1::IMPORT_STATE_PUBLISHED) break;
    ::usleep(5000);
  }
  auto after = preflight(c, src.string());
  CHECK(after && after->has_preflight_done() && after->preflight_done().already_imported() == 6 && after->preflight_done().ready() + after->preflight_done().needs_category() == 0);
  CHECK(after && after->has_preflight_done() && after->preflight_done().required_bytes() == 0);
}

void test_space() {
  Volume v;
  // Free space below what the import needs (the reserve alone is 256 MiB): refused, both numbers in the error, nothing queued or written.
  std::atomic<std::uint64_t> free_now{1000};
  Svc svc([&](LibraryServiceOptions& o) {
    o.free_bytes_fn = [&](const std::string&, std::uint64_t& out) {
      out = free_now.load();
      return true;
    };
  });
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  auto pre = preflight(c, v.root());
  CHECK(pre && pre->has_preflight_done());
  if (!pre || !pre->has_preflight_done()) return;
  const auto& d = pre->preflight_done();
  CHECK(d.free_bytes() == 1000 && !d.fits() && d.reserve_bytes() == kImportReserveBytes);
  CHECK(d.required_bytes() == required_import_bytes(d.pcm_bytes(), 8) && d.required_bytes() == 2 * d.pcm_bytes() + kImportReserveBytes);
  ws::v1::LibraryClientMessage m;
  auto* req = m.mutable_import_request();
  req->set_source(v.root());
  auto* a = req->add_items();
  a->set_path("Piano.sfz");
  a->set_category(kPiano);
  auto refused = c.call(m, 30000);
  CHECK(is_error(refused, ws::v1::LIBRARY_ERROR_CODE_INSUFFICIENT_SPACE));
  if (refused && refused->has_error()) {
    const auto& det = refused->error().details();
    CHECK(det.at("required_bytes") == std::to_string(required_import_bytes(600, 1)));
    CHECK(det.at("free_bytes") == "1000");
  }
  ws::v1::LibraryClientMessage l;
  l.mutable_list_libraries();
  auto libs = c.call(l);
  CHECK(libs && libs->has_library_list() && libs->library_list().libraries_size() == 0);
  CHECK(!fs::exists(fs::path(svc.o.data_dir) / "lib" / ".staging") || fs::is_empty(fs::path(svc.o.data_dir) / "lib" / ".staging") ||
        fs::directory_iterator(fs::path(svc.o.data_dir) / "lib" / ".staging")->path().filename() == ".lock");
  // Exactly enough is accepted; one byte less is refused.
  const std::uint64_t need = required_import_bytes(600, 1);
  free_now = need - 1;
  CHECK(is_error(c.call(m, 30000), ws::v1::LIBRARY_ERROR_CODE_INSUFFICIENT_SPACE));
  free_now = need;
  auto ok = c.call(m, 30000);
  CHECK(ok && ok->has_import_accepted() && ok->import_accepted().required_bytes() == need && ok->import_accepted().free_bytes() == need);
  // Unknown free space (the probe fails): the check is skipped.
  Svc svc2([&](LibraryServiceOptions& o) { o.free_bytes_fn = [](const std::string&, std::uint64_t&) { return false; }; });
  CHECK(svc2.start());
  Client c2;
  CHECK(svc2.connect(c2));
  auto pre2 = preflight(c2, v.root());
  CHECK(pre2 && pre2->has_preflight_done() && !pre2->preflight_done().fits() && !pre2->preflight_done().free_known() && pre2->preflight_done().free_bytes() == 0);
  // The real statvfs of the data directory gives a positive number.
  Svc svc3;
  CHECK(svc3.start());
  Client c3;
  CHECK(svc3.connect(c3));
  auto pre3 = preflight(c3, v.root());
  CHECK(pre3 && pre3->has_preflight_done() && pre3->preflight_done().free_bytes() > 0 && pre3->preflight_done().free_known());
}

void test_symlinks_never_escape() {
  TempDir td;
  const fs::path outside = fs::path(td.path) / "outside";
  const fs::path src = fs::path(td.path) / "vol";
  ifx::write_sfz(outside, "Secret");
  ifx::write_sfz(src, "Inside");
  fs::create_symlink(outside, src / "linkdir");                     // a linked folder: never followed
  fs::create_symlink(outside / "Secret.sfz", src / "Link.sfz");     // a linked file leaving the source: skipped
  fs::create_symlink(src / "Inside.sfz", src / "Alias.sfz");        // a linked file inside the source: used
  ifx::put(src / "Own.sfz", "<region> sample=esc.wav key=60\n");    // its sample is a link that leaves the source
  fs::create_symlink(outside / "Secret_s.wav", src / "esc.wav");
  const PreflightResult r = preflight_scan({src.string(), "", 0, nullptr});
  CHECK(r.ok);
  std::set<std::string> rels;
  for (const auto& x : r.rows) rels.insert(x.rel_path);
  CHECK(!rels.count("linkdir/Secret.sfz") && !rels.count("Link.sfz"));
  CHECK(rels.count("Inside.sfz") && rels.count("Own.sfz"));
  CHECK(!rels.count("Alias.sfz") && r.aliases_skipped == 1);  // a link to a file that is already listed is listed once, so its bytes are counted once
  CHECK(r.symlinks_skipped >= 2);
  for (const auto& x : r.rows) {
    if (x.rel_path == "Own.sfz") CHECK(x.status == RowStatus::kSkippedDamaged);
    CHECK(x.path.find("outside") == std::string::npos);
  }
}

void test_truncation_cap() {
  TempDir td;
  const fs::path src = fs::path(td.path) / "vol";
  for (int i = 0; i < 12; ++i) {
    char name[16];
    std::snprintf(name, sizeof name, "f%02d", i);
    ifx::write_sfz(src, name);
  }
  PreflightOptions o;
  o.source = src.string();
  o.max_files = 5;
  const PreflightResult r = preflight_scan(o);
  CHECK(r.ok && r.truncated && r.files_scanned == 5 && r.rows.size() == 5);
  o.max_files = 0;  // default cap, far above 12
  const PreflightResult r2 = preflight_scan(o);
  CHECK(r2.ok && !r2.truncated && r2.rows.size() == 12);
  CHECK(kPreflightMaxFiles == 20000);
  // Through the API: the flag is in the reply.
  Svc svc;
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  auto pr = preflight(c, src.string(), "", 5);
  CHECK(pr && pr->has_preflight_done() && pr->preflight_done().truncated() && pr->preflight_done().rows_total() == 5);
  // A cancelled flag stops the scan.
  std::atomic<bool> cancel{true};
  o.cancel = &cancel;
  const PreflightResult r3 = preflight_scan(o);
  CHECK(r3.ok && r3.cancelled && r3.rows.empty());
}

void test_headers_only() {
  TempDir td;
  const fs::path src = fs::path(td.path) / "vol";
  // 64 MiB of WAV data that is never written (sparse) and an SF2 whose sample words are 2 MB: reading them would show in bytes_read.
  write_sparse_wav(src / "Big_s.wav", 64u << 20);
  ifx::put(src / "Big.sfz", "<region> sample=Big_s.wav key=60\n");
  {
    sf2b::Builder b;
    const int smp = sf2b::add_sample(b, "huge", 1000000, 1000, 48000, 60, 0);
    sf2b::Instrument ins;
    ins.name = "Huge";
    ins.zones = {sf2b::izone(smp)};
    b.instruments = {ins};
    sf2b::add_preset(b, "Huge", 0, 0, {sf2b::pzone(0)});
    const auto bytes = b.build();
    ifx::put(src / "Gm.sf2", std::string(bytes.begin(), bytes.end()));
  }
  const std::uint64_t sf2_size = fs::file_size(src / "Gm.sf2");
  const PreflightResult r = preflight_scan({src.string(), "", 0, nullptr});
  CHECK(r.ok && r.rows.size() == 2);
  const PreflightRow* big = nullptr;
  for (const auto& x : r.rows)
    if (x.rel_path == "Big.sfz") big = &x;
  CHECK(big && big->status != RowStatus::kSkippedDamaged && big->pcm_bytes == (64u << 20));
  // What the scan read is far below the sample data it describes: the WAV header and the SF2 tables only.
  CHECK(r.bytes_read < 64 * 1024);
  CHECK(sf2_size > 1900000 && r.bytes_read < sf2_size / 20);  // the SF2 tables and sample headers only, a small fraction of its 2 MB
  std::printf("preflight headers-only: %llu bytes read for %llu bytes of WAV data and a %llu byte SF2\n", static_cast<unsigned long long>(r.bytes_read),
              static_cast<unsigned long long>(64u << 20), static_cast<unsigned long long>(sf2_size));
  // The space estimate of an import is the same headers-only analysis.
  const ImportEstimate e = estimate_import((src / "Big.sfz").string());
  CHECK(!e.skipped && e.pcm_bytes == (64u << 20) && e.bytes_read < 4096);
}

void test_thousand_file_volume() {
  TempDir td;
  const fs::path src = fs::path(td.path) / "BIGVOL";
  for (int i = 0; i < 1000; ++i) {
    char dir[16], name[16];
    std::snprintf(dir, sizeof dir, "lib%02d", i % 10);
    std::snprintf(name, sizeof name, "t%04d", i);
    ifx::write_sfz(src / dir, name);
  }
  // A few that are not ready, to check the counts and not only the total.
  ifx::write_damaged_sfz(src / "lib00", "Zdamaged");
  ifx::write_unsupported_sfz(src / "lib01", "Zodd");
  Svc svc;
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  auto pr = preflight(c, src.string(), "BIGVOL");
  CHECK(pr && pr->has_preflight_done());
  if (!pr || !pr->has_preflight_done()) return;
  const auto& d = pr->preflight_done();
  CHECK(d.rows_total() == 1002 && d.files_scanned() == 1002 && !d.truncated());
  CHECK(d.libraries_total() == 10 && d.skipped_damaged() == 1 && d.skipped_unsupported_features() == 1);
  CHECK(d.ready() + d.needs_category() == 1000 && d.pcm_bytes() == 1000 * 600ull);
  CHECK(d.bytes_read() < 1002ull * 400);  // a few dozen bytes per timbre, never the 600 bytes of sample data each
  bool ok = false;
  const auto rows = all_rows(c, d.preflight_id(), 400, &ok);
  CHECK(ok && rows.size() == 1002);
  std::set<std::string> ids;
  for (const auto& r : rows) ids.insert(r.library_id());
  CHECK(ids.size() == 1002);
  std::printf("preflight 1,000-file volume: %llu ms on this host (not representative of the board), %llu bytes read\n",
              static_cast<unsigned long long>(d.duration_ms()), static_cast<unsigned long long>(d.bytes_read()));
}

void test_cancel_and_bad_sources() {
  Volume v;
  std::mutex m;
  std::condition_variable cv;
  bool entered = false, release = false;
  Svc svc([&](LibraryServiceOptions& o) {
    o.preflight_start_hook = [&] {
      std::unique_lock<std::mutex> lk(m);
      entered = true;
      cv.notify_all();
      cv.wait(lk, [&] { return release; });
    };
  });
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  // The scan runs on the worker, not on the service thread: while it is held, the service still answers.
  ws::v1::LibraryClientMessage req = pre_msg(v.root());
  req.set_request_id(5000);
  CHECK(c.send(req));
  {
    std::unique_lock<std::mutex> lk(m);
    CHECK(cv.wait_for(lk, std::chrono::seconds(20), [&] { return entered; }));
  }
  ws::v1::LibraryClientMessage gf;
  gf.mutable_get_favorites_and_recent();
  auto st = c.call(gf);
  CHECK(st && st->has_state());
  ws::v1::LibraryClientMessage cancel;
  cancel.mutable_cancel_preflight()->set_request_id(5000);
  auto ack = c.call(cancel);
  CHECK(ack && ack->has_ack() && ack->ack().changed());
  {
    std::lock_guard<std::mutex> lk(m);
    release = true;
    cv.notify_all();
  }
  std::optional<ws::v1::LibraryServerMessage> done;
  for (int i = 0; i < 10 && !done; ++i) {
    auto x = c.recv(10000);
    if (!x) break;
    if (x->request_id() == 5000) done = x;
  }
  CHECK(done && done->has_preflight_done() && done->preflight_done().cancelled() && done->preflight_done().preflight_id() == 0);
  // Cancelling something that is not running answers changed=false.
  ws::v1::LibraryClientMessage cancel2;
  cancel2.mutable_cancel_preflight()->set_request_id(424242);
  auto ack2 = c.call(cancel2);
  CHECK(ack2 && ack2->has_ack() && !ack2->ack().changed());
  // Bad sources are typed refusals.
  CHECK(is_error(preflight(c, ""), ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE));
  CHECK(is_error(preflight(c, v.root() + "/nope"), ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE));
  CHECK(is_error(preflight(c, v.root() + "/Piano.sfz"), ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE));
  // A client that disconnects while its scan is held cancels it; the service goes on.
  {
    Client c2;
    CHECK(svc.connect(c2));
    {
      std::lock_guard<std::mutex> lk(m);
      release = false;
      entered = false;
    }
    ws::v1::LibraryClientMessage r2 = pre_msg(v.root());
    r2.set_request_id(1);
    CHECK(c2.send(r2));
    {
      std::unique_lock<std::mutex> lk(m);
      CHECK(cv.wait_for(lk, std::chrono::seconds(20), [&] { return entered; }));
    }
    c2.close();
    ::usleep(100000);
    std::lock_guard<std::mutex> lk(m);
    release = true;
    cv.notify_all();
  }
  Client c3;
  CHECK(svc.connect(c3));
  auto okr = preflight(c3, v.root());
  CHECK(okr && okr->has_preflight_done() && okr->preflight_done().rows_total() == 13);
  // Neither the cancelled scan nor the one of the dropped client left a stored result: every id below the one just made is unknown.
  if (okr && okr->has_preflight_done()) {
    for (std::uint64_t id = 1; id < okr->preflight_done().preflight_id(); ++id) {
      ws::v1::LibraryClientMessage q;
      q.mutable_list_preflight_rows()->set_preflight_id(id);
      CHECK(is_error(c3.call(q), ws::v1::LIBRARY_ERROR_CODE_UNKNOWN_PREFLIGHT));
    }
  }
}

void test_non_utf8_names() {
  TempDir td;
  const fs::path src = fs::path(td.path) / "vol";
  fs::create_directories(src);
  const std::string odd = "\xff\xfe_Piano.sfz";
  bool created = false;
  {
    const int fd = ::open((src.string() + "/" + odd).c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) {
      const std::string body = "<region> sample=x.wav key=60\n";
      [[maybe_unused]] const ssize_t w = ::write(fd, body.data(), body.size());
      ::close(fd);
      created = true;
    }
  }
  if (!created) {
    std::printf("preflight: this file system refuses non-UTF-8 names; case skipped\n");
    return;
  }
  Svc svc;
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  auto pr = preflight(c, src.string());
  CHECK(pr && pr->has_preflight_done() && pr->preflight_done().rows_total() == 1);
  if (pr && pr->has_preflight_done()) {
    auto rows = all_rows(c, pr->preflight_done().preflight_id());
    CHECK(rows.size() == 1 && rows[0].rel_path().find("\xEF\xBF\xBD") != std::string::npos);  // the bad bytes became U+FFFD, the frame parsed
  }
}

// --- review round: parity with the real importer, space, limits ---------------------------------------------------------------------------------

std::string wav8() {  // 8-bit PCM: a sample format the importer does not have
  auto le32 = [](std::uint32_t v) { return std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; };
  auto le16 = [](std::uint32_t v) { return std::string{char(v), char(v >> 8)}; };
  const std::string pcm(300, '\x40');
  return "RIFF" + le32(static_cast<std::uint32_t>(36 + pcm.size())) + "WAVE" + "fmt " + le32(16) + le16(1) + le16(1) + le32(48000) + le32(48000) + le16(1) +
         le16(8) + "data" + le32(static_cast<std::uint32_t>(pcm.size())) + pcm;
}

enum class Expect { kOk, kDamaged, kUnsupported };

struct ParityCase {
  const char* name;
  std::function<fs::path(const fs::path& dir)> make;  // returns the file to examine
  Expect importer;
  Expect preflight;  // differs from `importer` only where the story's confinement rule deliberately applies
};

Expect importer_verdict(const fs::path& file) {
  TempDir root;
  fs::create_directories(fs::path(root.path) / "lib");
  ImportOptions o;
  o.sfz_path = file.string();
  o.root = root.path;
  o.category = "category.piano";
  const ImportResult r = import_file(o);
  if (r.ok()) return Expect::kOk;
  if (r.status == Status::kSkipped) return r.reason_code == ReasonCode::kDamaged ? Expect::kDamaged : Expect::kUnsupported;
  std::printf("importer failed unexpectedly (%s): %s\n", to_string(r.status), r.reasons.empty() ? "" : r.reasons[0].c_str());
  return Expect::kDamaged;
}

Expect preflight_verdict(const fs::path& dir, const fs::path& file) {
  const PreflightResult r = preflight_scan({dir.string(), "", 0, nullptr, {}});
  std::error_code ec;
  const std::string want = fs::canonical(file, ec).string();
  Expect e = Expect::kOk;
  bool seen = false;
  for (const auto& x : r.rows) {
    if (x.path != want) continue;
    seen = true;
    if (x.status == RowStatus::kSkippedDamaged) e = Expect::kDamaged;
    else if (x.status == RowStatus::kSkippedUnsupportedFeatures) e = Expect::kUnsupported;
  }
  if (!seen) std::printf("parity: no row for %s\n", want.c_str());
  return seen ? e : Expect::kDamaged;
}

void test_parity_with_the_importer() {
  const std::vector<ParityCase> cases = {
      {"plain sfz", [](const fs::path& d) { return ifx::write_sfz(d, "A"); }, Expect::kOk, Expect::kOk},
      {"plain sf2", [](const fs::path& d) { return ifx::write_sf2(d / "A.sf2"); }, Expect::kOk, Expect::kOk},
      {"unsupported wav format (8-bit)", [](const fs::path& d) {
         ifx::put(d / "a_s.wav", wav8());
         ifx::put(d / "A.sfz", "<region> sample=a_s.wav key=60\n");
         return d / "A.sfz";
       }, Expect::kUnsupported, Expect::kUnsupported},
      {"loop beyond the sample", [](const fs::path& d) {
         ifx::put(d / "a_s.wav", ifx::wav16(300, 1));
         ifx::put(d / "A.sfz", "<region> sample=a_s.wav key=60 loop_mode=loop_continuous loop_start=0 loop_end=99999\n");
         return d / "A.sfz";
       }, Expect::kDamaged, Expect::kDamaged},
      {"missing include", [](const fs::path& d) {
         ifx::put(d / "a_s.wav", ifx::wav16(300, 1));
         ifx::put(d / "A.sfz", "#include \"nope.sfz\"\n<region> sample=a_s.wav key=60\n");
         return d / "A.sfz";
       }, Expect::kDamaged, Expect::kDamaged},
      {"unsupported opcode", [](const fs::path& d) { return ifx::write_unsupported_sfz(d, "A"); }, Expect::kUnsupported, Expect::kUnsupported},
      {"missing sample", [](const fs::path& d) { return ifx::write_damaged_sfz(d, "A"); }, Expect::kDamaged, Expect::kDamaged},
      {"truncated wav", [](const fs::path& d) {
         ifx::put(d / "a_s.wav", truncated_wav());
         ifx::put(d / "A.sfz", "<region> sample=a_s.wav key=60\n");
         return d / "A.sfz";
       }, Expect::kDamaged, Expect::kDamaged},
      {"sf2 with an unsupported sample rate", [](const fs::path& d) {
         sf2b::Builder b;
         const int smp = sf2b::add_sample(b, "low", 1000, 10, 22050, 60, 0);
         sf2b::Instrument ins;
         ins.name = "I";
         ins.zones = {sf2b::izone(smp)};
         b.instruments = {ins};
         sf2b::add_preset(b, "P", 0, 0, {sf2b::pzone(0)});
         const auto bytes = b.build();
         ifx::put(d / "A.sf2", std::string(bytes.begin(), bytes.end()));
         return d / "A.sf2";
       }, Expect::kUnsupported, Expect::kUnsupported},
      {"sf2 that is not a font", [](const fs::path& d) {
         ifx::put(d / "A.sf2", "RIFFxxxxsfbk-not-a-font");
         return d / "A.sf2";
       }, Expect::kDamaged, Expect::kDamaged},
      // Deliberate divergence: the importer follows a sample link wherever it leads; the preflight (story rule: never follow symlinks outside the source
      // root) calls such an SFZ damaged, so nothing outside the volume is read by a scan.
      {"sample link leaving the source", [](const fs::path& d) {
         const fs::path outside = d.parent_path() / "outside";
         ifx::put(outside / "o_s.wav", ifx::wav16(300, 1));
         fs::create_directories(d);
         fs::create_symlink(outside / "o_s.wav", d / "a_s.wav");
         ifx::put(d / "A.sfz", "<region> sample=a_s.wav key=60\n");
         return d / "A.sfz";
       }, Expect::kOk, Expect::kDamaged},
  };
  for (const ParityCase& c : cases) {
    TempDir td;
    const fs::path dir = fs::path(td.path) / "vol";
    fs::create_directories(dir);
    const fs::path file = c.make(dir);
    const Expect imp = importer_verdict(file);
    const Expect pre = preflight_verdict(dir, file);
    if (imp != c.importer || pre != c.preflight) std::printf("parity case '%s': importer %d (want %d), preflight %d (want %d)\n", c.name, int(imp), int(c.importer), int(pre), int(c.preflight));
    CHECK(imp == c.importer);
    CHECK(pre == c.preflight);
  }
  // A FIFO as a sample never blocks the scan (opened non-blocking, not a regular file): damaged. The importer is not run on it (it would block).
  TempDir td;
  const fs::path dir = fs::path(td.path) / "vol";
  fs::create_directories(dir);
  ifx::put(dir / "A.sfz", "<region> sample=f.wav key=60\n");
  const bool fifo = ::mkfifo((dir / "f.wav").c_str(), 0644) == 0;
  CHECK(fifo);
  if (fifo) {
    const PreflightResult r = preflight_scan({dir.string(), "", 0, nullptr, {}});
    CHECK(r.ok && r.rows.size() == 1 && r.rows[0].status == RowStatus::kSkippedDamaged);
  }
}

void test_estimate_not_confined_and_never_zero() {
  TempDir td;
  const fs::path vol = fs::path(td.path) / "vol";
  ifx::put(vol / "Samples" / "s.wav", ifx::wav16(300, 1));
  ifx::put(vol / "Patches" / "P.sfz", "<region> sample=../Samples/s.wav key=60\n");
  const ImportEstimate e = estimate_import((vol / "Patches" / "P.sfz").string());
  const Expect imp = importer_verdict(vol / "Patches" / "P.sfz");
  std::printf("sibling samples: importer verdict %d, estimate skipped=%d pcm=%llu\n", int(imp), int(e.skipped), static_cast<unsigned long long>(e.pcm_bytes));
  CHECK(e.skipped == (imp != Expect::kOk));
  if (imp == Expect::kOk) CHECK(e.pcm_bytes == 600);
  // The SFZ parser itself refuses ".." in a sample path, so a sample that lies outside the import source is legal only as a link: the estimate follows it.
  ifx::put(vol / "Elsewhere" / "real.wav", ifx::wav16(300, 2));
  ifx::put(vol / "Linked" / "L.sfz", "<region> sample=l_s.wav key=60\n");
  fs::create_symlink(vol / "Elsewhere" / "real.wav", vol / "Linked" / "l_s.wav");
  const ImportEstimate el = estimate_import((vol / "Linked" / "L.sfz").string());
  CHECK(importer_verdict(vol / "Linked" / "L.sfz") == Expect::kOk && !el.skipped && el.pcm_bytes == 600);
  // Through the service, the import source is the Linked folder only: the sample lies outside it and the space figure must still count it.
  Svc svc([](LibraryServiceOptions& o) {
    o.free_bytes_fn = [](const std::string&, std::uint64_t& out) {
      out = 1000;
      return true;
    };
  });
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  ws::v1::LibraryClientMessage m;
  auto* req = m.mutable_import_request();
  req->set_source((vol / "Linked").string());
  auto* it = req->add_items();
  it->set_path("L.sfz");
  it->set_category(kPiano);
  auto r = c.call(m, 30000);
  CHECK(is_error(r, ws::v1::LIBRARY_ERROR_CODE_INSUFFICIENT_SPACE));
  if (r && r->has_error()) CHECK(r->error().details().at("required_bytes") == std::to_string(required_import_bytes(600, 1)));
  // A file the estimate calls skipped is charged its own size, never nothing: a damaged SFZ of 100 bytes still has a positive, exact figure.
  ifx::write_damaged_sfz(vol / "Patches", "Gone");
  ws::v1::LibraryClientMessage m2;
  auto* r2 = m2.mutable_import_request();
  r2->set_source((vol / "Patches").string());
  auto* i2 = r2->add_items();
  i2->set_path("Gone.sfz");
  i2->set_category(kPiano);
  auto rr = c.call(m2, 30000);
  CHECK(is_error(rr, ws::v1::LIBRARY_ERROR_CODE_INSUFFICIENT_SPACE) && rr->error().details().at("required_bytes") ==
        std::to_string(required_import_bytes(fs::file_size(vol / "Patches" / "Gone.sfz"), 1)));
}

void test_space_cache_miss_sf2_and_budget() {
  Volume v;
  Svc svc([](LibraryServiceOptions& o) {
    o.free_bytes_fn = [](const std::string&, std::uint64_t& out) {
      out = 1000;
      return true;
    };
  });
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  // No preflight was run: the figure comes from a fresh headers-only estimate; both numbers are in the refusal.
  ws::v1::LibraryClientMessage m;
  auto* req = m.mutable_import_request();
  req->set_source(v.root());
  auto* a = req->add_items();
  a->set_path("Piano.sfz");
  a->set_category(kPiano);
  auto r = c.call(m, 30000);
  CHECK(is_error(r, ws::v1::LIBRARY_ERROR_CODE_INSUFFICIENT_SPACE));
  if (r && r->has_error()) {
    CHECK(r->error().details().at("required_bytes") == std::to_string(required_import_bytes(600, 1)));
    CHECK(r->error().details().at("free_bytes") == "1000");
  }
  // An SF2 item: the sum of the decoded sizes of its presets.
  const PreflightResult scan = preflight_scan({v.root(), "", 0, nullptr, {}});
  std::uint64_t sf2_pcm = 0;
  for (const auto& x : scan.rows)
    if (x.rel_path == "Fonts/Gm.sf2") sf2_pcm += x.pcm_bytes;
  CHECK(sf2_pcm > 0);
  ws::v1::LibraryClientMessage m2;
  auto* r2 = m2.mutable_import_request();
  r2->set_source(v.root());
  auto* b = r2->add_items();
  b->set_path("Fonts/Gm.sf2");
  b->set_category(kPiano);
  auto rr = c.call(m2, 30000);
  CHECK(is_error(rr, ws::v1::LIBRARY_ERROR_CODE_INSUFFICIENT_SPACE) && rr->error().details().at("required_bytes") == std::to_string(required_import_bytes(sf2_pcm, 1)));

  // More than 50 items without a fresh preflight figure: refused before any work is done (LIMIT_REACHED estimate_budget); after a preflight they are cache hits.
  TempDir td;
  const fs::path src = fs::path(td.path) / "many";
  for (int i = 0; i < 60; ++i) {
    char name[16];
    std::snprintf(name, sizeof name, "m%02d", i);
    ifx::write_sfz(src, name);
  }
  Svc svc2;
  CHECK(svc2.start());
  Client c2;
  CHECK(svc2.connect(c2));
  auto many = [&] {
    ws::v1::LibraryClientMessage q;
    auto* rq = q.mutable_import_request();
    rq->set_source(src.string());
    for (int i = 0; i < 60; ++i) {
      char name[24];
      std::snprintf(name, sizeof name, "m%02d.sfz", i);
      auto* it = rq->add_items();
      it->set_path(name);
      it->set_category(kPiano);
    }
    return c2.call(q, 60000);
  };
  auto refused = many();
  CHECK(is_error(refused, ws::v1::LIBRARY_ERROR_CODE_LIMIT_REACHED) && refused->error().details().at("reason") == "estimate_budget");
  auto pre = preflight(c2, src.string());
  CHECK(pre && pre->has_preflight_done() && pre->preflight_done().rows_total() == 60);
  auto accepted = many();
  CHECK(accepted && accepted->has_import_accepted() && accepted->import_accepted().required_bytes() == required_import_bytes(60 * 600, 60));
  if (accepted && accepted->has_import_accepted()) {
    ws::v1::LibraryClientMessage cm;
    cm.mutable_cancel_import()->set_job_id(accepted->import_accepted().job_id());
    c2.call(cm);
  }
}

void test_estimate_cache_follows_the_samples() {
  Volume v;
  Svc svc;
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  auto pre = preflight(c, v.root());
  CHECK(pre && pre->has_preflight_done());
  auto request = [&] {
    ws::v1::LibraryClientMessage m;
    auto* req = m.mutable_import_request();
    req->set_source(v.root());
    auto* a = req->add_items();
    a->set_path("Piano.sfz");
    a->set_category(kPiano);
    return c.call(m, 30000);
  };
  // Unchanged: the preflight's figure is used.
  ws::v1::LibraryClientMessage cm;
  auto first = request();
  CHECK(first && first->has_import_accepted() && first->import_accepted().required_bytes() == required_import_bytes(600, 1));
  if (first && first->has_import_accepted()) {
    cm.mutable_cancel_import()->set_job_id(first->import_accepted().job_id());
    c.call(cm);
  }
  for (int i = 0; i < 400; ++i) {  // let the job end so its reservation is released
    ws::v1::LibraryClientMessage q;
    q.mutable_import_status()->set_job_id(first->import_accepted().job_id());
    auto st = c.call(q);
    if (st && st->has_import_progress() && st->import_progress().state() >= ws::v1::IMPORT_STATE_PUBLISHED) break;
    ::usleep(5000);
  }
  // The WAV grows after the preflight (the .sfz itself is untouched): the cached figure is stale and must be recomputed.
  ifx::put(fs::path(v.root()) / "Piano_s.wav", ifx::wav16(3000, 9));
  auto second = request();
  CHECK(second && second->has_import_accepted() && second->import_accepted().required_bytes() == required_import_bytes(6000, 1));
}

void test_reserved_space_across_jobs() {
  Volume v;
  const std::uint64_t need = required_import_bytes(600, 1);
  std::mutex m;
  std::condition_variable cv;
  bool release = false;
  Svc svc([&](LibraryServiceOptions& o) {
    o.free_bytes_fn = [need](const std::string&, std::uint64_t& out) {
      out = need + need / 2;  // room for one import, not for two
      return true;
    };
    o.import_point_hook = [&](const char* point, std::uint64_t, std::size_t) {
      if (std::string(point) != "stage-created") return;
      std::unique_lock<std::mutex> lk(m);
      cv.wait(lk, [&] { return release; });
    };
  });
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  auto request = [&](const char* file) {
    ws::v1::LibraryClientMessage q;
    auto* req = q.mutable_import_request();
    req->set_source(v.root());
    auto* a = req->add_items();
    a->set_path(file);
    a->set_category(kPiano);
    return c.call(q, 30000);
  };
  auto one = request("Piano.sfz");
  CHECK(one && one->has_import_accepted());
  auto two = request("qzxv.sfz");  // alone it would fit; beside the queued/running first job it does not
  CHECK(is_error(two, ws::v1::LIBRARY_ERROR_CODE_INSUFFICIENT_SPACE));
  if (two && two->has_error()) CHECK(two->error().details().at("reserved_bytes") == std::to_string(need) && two->error().details().at("required_bytes") == std::to_string(need));
  {
    std::lock_guard<std::mutex> lk(m);
    release = true;
    cv.notify_all();
  }
  for (int i = 0; i < 2000 && one && one->has_import_accepted(); ++i) {
    ws::v1::LibraryClientMessage q;
    q.mutable_import_status()->set_job_id(one->import_accepted().job_id());
    auto st = c.call(q);
    if (st && st->has_import_progress() && st->import_progress().state() >= ws::v1::IMPORT_STATE_PUBLISHED) break;
    ::usleep(5000);
  }
  auto three = request("qzxv.sfz");  // the first job finished: its reservation is gone
  CHECK(three && three->has_import_accepted());
}

void test_unknown_action() {
  Volume v;
  Svc svc;
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  ws::v1::LibraryClientMessage m;
  auto* req = m.mutable_import_request();
  req->set_source(v.root());
  auto* a = req->add_items();
  a->set_path("Piano.sfz");
  a->set_category(kPiano);
  a->set_action(static_cast<ws::v1::ImportAction>(77));
  auto r = c.call(m, 30000);
  CHECK(is_error(r, ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND) && r->error().details().at("reason") == "unknown_action");
}

void test_tree_limits_and_rows() {
  TempDir td;
  const fs::path src = fs::path(td.path) / "vol";
  // Nested folders within the depth cap are listed (relative path, top-level folder as the library); one deeper than kPreflightMaxDepth is cut.
  fs::path deep = src / "Top";
  for (int i = 0; i < 5; ++i) deep /= "n";
  ifx::write_sfz(deep, "Deep");
  fs::path too_deep = src / "Cap";
  for (std::size_t i = 0; i < kPreflightMaxDepth + 2; ++i) too_deep /= "d";
  ifx::write_sfz(too_deep, "TooDeep");
  ifx::write_sfz(src / "._Hidden", "X");  // AppleDouble folder and file names are ignored
  ifx::put(src / "._Piano.sfz", "junk");
  const PreflightResult r = preflight_scan({src.string(), "", 0, nullptr, {}});
  CHECK(r.ok && r.truncated);
  bool deep_found = false, too_deep_found = false;
  for (const auto& x : r.rows) {
    if (x.rel_path == "Top/n/n/n/n/n/Deep.sfz" && r.libraries[x.library].name == "Top") deep_found = true;
    if (x.name == "TooDeep") too_deep_found = true;
    CHECK(x.name != "X");
  }
  CHECK(deep_found && !too_deep_found);
  // An unreadable folder is counted and skipped, its siblings still listed (not when running as root: permissions do not apply).
  if (::geteuid() != 0) {
    const fs::path src2 = fs::path(td.path) / "vol2";
    ifx::write_sfz(src2 / "Open", "Ok");
    ifx::write_sfz(src2 / "Closed", "Hidden");
    ::chmod((src2 / "Closed").c_str(), 0);
    const PreflightResult r2 = preflight_scan({src2.string(), "", 0, nullptr, {}});
    ::chmod((src2 / "Closed").c_str(), 0755);
    CHECK(r2.ok && r2.unreadable_dirs >= 1 && r2.rows.size() == 1 && r2.rows[0].name == "Ok");
  }
  // Cancel in the middle of a scan: the hook sets the flag after the 3rd file.
  const fs::path src3 = fs::path(td.path) / "vol3";
  for (int i = 0; i < 10; ++i) {
    char name[16];
    std::snprintf(name, sizeof name, "g%02d", i);
    ifx::write_sfz(src3, name);
  }
  std::atomic<bool> cancel{false};
  PreflightOptions o;
  o.source = src3.string();
  o.cancel = &cancel;
  o.on_file = [&](std::size_t done) {
    if (done == 3) cancel = true;
  };
  const PreflightResult r3 = preflight_scan(o);
  CHECK(r3.ok && r3.cancelled && r3.files_scanned == 3 && r3.rows.size() == 3);
  CHECK(kPreflightMaxRows == 50000);
}

void test_row_columns() {
  TempDir td;
  const fs::path src = fs::path(td.path) / "vol";
  const fs::path sfz = ifx::write_sfz(src, "Piano");
  const fs::path sf2 = ifx::write_sf2(src / "Gm.sf2");
  const PreflightResult r = preflight_scan({src.string(), "", 0, nullptr, {}});
  std::error_code ec;
  for (const auto& x : r.rows) {
    if (x.format == "sfz") {
      // The exact on-disk size of the timbre: the .sfz plus its one sample file.
      CHECK(x.size_bytes == fs::file_size(sfz) + fs::file_size(src / "Piano_s.wav") && x.pcm_bytes == 600);
    } else {
      CHECK(x.size_bytes == x.pcm_bytes && x.pcm_bytes > 0);
      const auto want = suggest_category(suggest_input_for_path(fs::canonical(sf2, ec).string(), {x.name}));
      CHECK(x.suggested == want.value_or(fmt::Category::kNone));
      CHECK(x.status == (want ? RowStatus::kReady : RowStatus::kNeedsCategory));
      if (x.instrument_key == "0:0") CHECK(x.name == "Grand");
      if (x.instrument_key == "128:0") CHECK(x.name == "Standard kit");
    }
  }
  int ready = 0, needs = 0;
  for (const auto& x : r.rows) (x.status == RowStatus::kReady ? ready : needs) += x.format == "sf2" ? 1 : 0;
  CHECK(ready + needs == 6);
}

void test_service_limits() {
  Volume v;
  std::mutex m;
  std::condition_variable cv;
  bool release = false;
  int entered = 0;
  Svc svc([&](LibraryServiceOptions& o) {
    o.max_queued_preflights = 1;
    o.max_preflights_per_client = 5;
    o.preflight_start_hook = [&] {
      std::unique_lock<std::mutex> lk(m);
      ++entered;
      cv.notify_all();
      cv.wait(lk, [&] { return release; });
    };
  });
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  ws::v1::LibraryClientMessage r1 = pre_msg(v.root());
  r1.set_request_id(11);
  CHECK(c.send(r1));  // running (held in the hook)
  {
    std::unique_lock<std::mutex> lk(m);
    CHECK(cv.wait_for(lk, std::chrono::seconds(20), [&] { return entered == 1; }));
  }
  ws::v1::LibraryClientMessage r2 = pre_msg(v.root());
  r2.set_request_id(12);
  CHECK(c.send(r2));  // queued
  ::usleep(100000);
  auto full = c.call(pre_msg(v.root()));  // the global queue (1) is full
  CHECK(is_error(full, ws::v1::LIBRARY_ERROR_CODE_QUEUE_FULL) && full->error().details().at("limit") == "1");
  {
    std::lock_guard<std::mutex> lk(m);
    release = true;
    cv.notify_all();
  }
  int dones = 0;
  for (int i = 0; i < 4 && dones < 2; ++i) {
    auto x = c.recv(20000);
    if (x && x->has_preflight_done() && !x->preflight_done().cancelled()) ++dones;
  }
  CHECK(dones == 2);

  // Per client (default 2): the third scan of one connection is refused even though the global queue has room.
  bool release2 = false;
  int entered2 = 0;
  Svc svc2([&](LibraryServiceOptions& o) {
    o.max_queued_preflights = 10;
    o.preflight_start_hook = [&] {
      std::unique_lock<std::mutex> lk(m);
      ++entered2;
      cv.notify_all();
      cv.wait(lk, [&] { return release2; });
    };
  });
  CHECK(svc2.start());
  Client b;
  CHECK(svc2.connect(b));
  for (std::uint64_t id : {21, 22}) {
    ws::v1::LibraryClientMessage q = pre_msg(v.root());
    q.set_request_id(id);
    CHECK(b.send(q));
  }
  {
    std::unique_lock<std::mutex> lk(m);
    CHECK(cv.wait_for(lk, std::chrono::seconds(20), [&] { return entered2 == 1; }));
  }
  ::usleep(100000);
  auto third = b.call(pre_msg(v.root()));
  CHECK(is_error(third, ws::v1::LIBRARY_ERROR_CODE_QUEUE_FULL) && third->error().details().at("reason") == "per_client");
  {
    std::lock_guard<std::mutex> lk(m);
    release2 = true;
    cv.notify_all();
  }
}

void test_summary_cap_paging_and_budget() {
  TempDir td;
  const fs::path src = fs::path(td.path) / "vol";
  for (int i = 0; i < 205; ++i) {
    char dir[16];
    std::snprintf(dir, sizeof dir, "f%03d", i);
    ifx::write_sfz(src / dir, "T");
  }
  Svc svc([](LibraryServiceOptions& o) { o.reply_budget_bytes = 3000; });
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  auto pr = preflight(c, src.string());
  CHECK(pr && pr->has_preflight_done());
  if (!pr || !pr->has_preflight_done()) return;
  const auto& d = pr->preflight_done();
  CHECK(d.libraries_total() == 205 && d.libraries_size() == 200 && d.rows_total() == 205);
  // A library beyond the summary cap is still reachable by its index.
  {
    ws::v1::LibraryClientMessage q;
    q.mutable_list_preflight_rows()->set_preflight_id(d.preflight_id());
    q.mutable_list_preflight_rows()->set_library(204);
    auto x = c.call(q);
    CHECK(x && x->has_preflight_rows() && x->preflight_rows().total() == 1 && x->preflight_rows().rows(0).library() == 204);
  }
  auto page = [&](std::uint32_t offset, std::uint32_t limit) {
    ws::v1::LibraryClientMessage q;
    q.mutable_list_preflight_rows()->set_preflight_id(d.preflight_id());
    q.mutable_list_preflight_rows()->set_offset(offset);
    q.mutable_list_preflight_rows()->set_limit(limit);
    return c.call(q);
  };
  // limit 0 = 500 and a huge limit = 2000, both cut at the reply budget (3000 bytes here) with at least one row and `more` set.
  for (std::uint32_t limit : {0u, 100000u}) {
    auto x = page(0, limit);
    CHECK(x && x->has_preflight_rows() && x->preflight_rows().rows_size() >= 1 && x->preflight_rows().rows_size() < 205 && x->preflight_rows().more());
    CHECK(x && x->preflight_rows().total() == 205);
  }
  bool ok = false;
  const auto all = all_rows(c, d.preflight_id(), 0, &ok);  // follows `more` until the end
  CHECK(ok && all.size() == 205);
  CHECK(page(205, 10) && page(205, 10)->preflight_rows().rows_size() == 0 && !page(205, 10)->preflight_rows().more());
}

void test_nothing_stored_for_a_dropped_client_and_preflight_between_items() {
  Volume v;
  std::mutex m;
  std::condition_variable cv;
  bool release = false, armed = true;
  std::vector<std::string> log;
  Svc svc([&](LibraryServiceOptions& o) {
    o.import_point_hook = [&](const char* point, std::uint64_t, std::size_t item) {
      if (std::string(point) != "stage-created") return;
      std::unique_lock<std::mutex> lk(m);
      log.push_back("stage:" + std::to_string(item));
      cv.notify_all();
      if (armed && item == 0) cv.wait(lk, [&] { return release; });
    };
    o.preflight_start_hook = [&] {
      std::lock_guard<std::mutex> lk(m);
      log.push_back("pre");
      cv.notify_all();
    };
  });
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  ws::v1::LibraryClientMessage q;
  auto* req = q.mutable_import_request();
  req->set_source(v.root());
  for (const char* f : {"Piano.sfz", "qzxv.sfz"}) {
    auto* a = req->add_items();
    a->set_path(f);
    a->set_category(kPiano);
  }
  auto acc = c.call(q, 30000);
  CHECK(acc && acc->has_import_accepted());
  {
    std::unique_lock<std::mutex> lk(m);
    CHECK(cv.wait_for(lk, std::chrono::seconds(20), [&] { return !log.empty(); }));
  }
  // The worker is inside item 0 of a two item job. A scan is queued now and must run before item 1 starts.
  ws::v1::LibraryClientMessage pm = pre_msg(v.root());
  pm.set_request_id(77);
  CHECK(c.send(pm));
  ::usleep(100000);
  {
    std::lock_guard<std::mutex> lk(m);
    release = true;
    cv.notify_all();
  }
  std::optional<ws::v1::LibraryServerMessage> done;
  for (int i = 0; i < 40 && !done; ++i) {
    auto x = c.recv(20000);
    if (!x) break;
    if (x->request_id() == 77) done = x;
  }
  CHECK(done && done->has_preflight_done() && done->preflight_done().rows_total() == 13);
  for (int i = 0; i < 2000; ++i) {
    ws::v1::LibraryClientMessage st;
    st.mutable_import_status()->set_job_id(acc->import_accepted().job_id());
    auto s = c.call(st);
    if (s && s->has_import_progress() && s->import_progress().state() >= ws::v1::IMPORT_STATE_PUBLISHED) break;
    ::usleep(5000);
  }
  {
    std::lock_guard<std::mutex> lk(m);
    int pre_at = -1, stage1_at = -1;
    for (std::size_t i = 0; i < log.size(); ++i) {
      if (log[i] == "pre" && pre_at < 0) pre_at = static_cast<int>(i);
      if (log[i] == "stage:1") stage1_at = static_cast<int>(i);
    }
    CHECK(pre_at >= 0 && stage1_at >= 0 && pre_at < stage1_at);
  }
}

void test_partial_presence_sf2_rows() {
  TempDir td;
  const fs::path src = fs::path(td.path) / "fonts";
  ifx::write_sf2(src / "Gm.sf2");
  Svc svc;
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  ws::v1::LibraryClientMessage m;
  auto* req = m.mutable_import_request();
  req->set_source(src.string());
  auto* it = req->add_items();
  it->set_path("Gm.sf2");
  it->set_category(kPiano);
  auto acc = c.call(m, 30000);
  CHECK(acc && acc->has_import_accepted());
  for (int i = 0; i < 2000 && acc && acc->has_import_accepted(); ++i) {
    ws::v1::LibraryClientMessage q;
    q.mutable_import_status()->set_job_id(acc->import_accepted().job_id());
    auto s = c.call(q);
    if (s && s->has_import_progress() && s->import_progress().state() == ws::v1::IMPORT_STATE_PUBLISHED) break;
    ::usleep(5000);
  }
  // The same font file gains a preset: the library is known (same path), six keys are in the catalog and the new one is not.
  {
    sf2b::Fixture f = sf2b::make_gm(false);
    sf2b::add_preset(f.b, "Extra", 5, 5, {sf2b::pzone(0)});
    const auto bytes = f.b.build();
    ifx::put(src / "Gm.sf2", std::string(bytes.begin(), bytes.end()));
  }
  auto pr = preflight(c, src.string());
  CHECK(pr && pr->has_preflight_done() && pr->preflight_done().rows_total() == 7 && pr->preflight_done().already_imported() == 6);
  if (!pr || !pr->has_preflight_done()) return;
  bool ok = false;
  const auto rows = all_rows(c, pr->preflight_done().preflight_id(), 100, &ok);
  CHECK(ok && rows.size() == 7);
  int skip = 0, imp = 0;
  for (const auto& r : rows) {
    if (r.instrument_key() == "5:5") {
      CHECK(r.status() != ws::v1::PREFLIGHT_STATUS_ALREADY_IMPORTED && r.action() == ws::v1::IMPORT_ACTION_IMPORT);
      ++imp;
    } else {
      CHECK(r.status() == ws::v1::PREFLIGHT_STATUS_ALREADY_IMPORTED && r.action() == ws::v1::IMPORT_ACTION_SKIP);
      ++skip;
    }
  }
  CHECK(skip == 6 && imp == 1);
  // Only the new preset counts for space.
  CHECK(pr->preflight_done().pcm_bytes() > 0 && pr->preflight_done().pcm_bytes() < 20000);
}

}  // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("-- %s\n", "test_scan_statuses_and_libraries");
  test_scan_statuses_and_libraries();
  std::printf("-- %s\n", "test_service_preflight_all_five_statuses_and_import_again");
  test_service_preflight_all_five_statuses_and_import_again();
  std::printf("-- %s\n", "test_sf2_row_already_imported");
  test_sf2_row_already_imported();
  std::printf("-- %s\n", "test_space");
  test_space();
  std::printf("-- %s\n", "test_symlinks_never_escape");
  test_symlinks_never_escape();
  std::printf("-- %s\n", "test_truncation_cap");
  test_truncation_cap();
  std::printf("-- %s\n", "test_headers_only");
  test_headers_only();
  std::printf("-- %s\n", "test_thousand_file_volume");
  test_thousand_file_volume();
  std::printf("-- %s\n", "test_cancel_and_bad_sources");
  test_cancel_and_bad_sources();
  std::printf("-- %s\n", "test_non_utf8_names");
  test_non_utf8_names();
  std::printf("-- %s\n", "test_parity_with_the_importer");
  test_parity_with_the_importer();
  std::printf("-- %s\n", "test_estimate_not_confined_and_never_zero");
  test_estimate_not_confined_and_never_zero();
  std::printf("-- %s\n", "test_space_cache_miss_sf2_and_budget");
  test_space_cache_miss_sf2_and_budget();
  std::printf("-- %s\n", "test_estimate_cache_follows_the_samples");
  test_estimate_cache_follows_the_samples();
  std::printf("-- %s\n", "test_reserved_space_across_jobs");
  test_reserved_space_across_jobs();
  std::printf("-- %s\n", "test_unknown_action");
  test_unknown_action();
  std::printf("-- %s\n", "test_tree_limits_and_rows");
  test_tree_limits_and_rows();
  std::printf("-- %s\n", "test_row_columns");
  test_row_columns();
  std::printf("-- %s\n", "test_service_limits");
  test_service_limits();
  std::printf("-- %s\n", "test_summary_cap_paging_and_budget");
  test_summary_cap_paging_and_budget();
  std::printf("-- %s\n", "test_nothing_stored_for_a_dropped_client_and_preflight_between_items");
  test_nothing_stored_for_a_dropped_client_and_preflight_between_items();
  std::printf("-- %s\n", "test_partial_presence_sf2_rows");
  test_partial_presence_sf2_rows();
  std::printf(g_fail ? "librarian_preflight_test: %d FAILED\n" : "librarian_preflight_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
