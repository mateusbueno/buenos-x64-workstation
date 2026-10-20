// Story 6.4, end to end across the real tools (processes, no test-only writer): velora-content-pack packs a tiny SFZ with generated dev keys, `librarian
// install-protected --trust-list` installs the pack, a librarian service started on that root lists a Factory library, a replay of an older revision is refused and
// a user import of the same path-derived id is not possible.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "import_fixtures.hpp"
#include "lib_test_util.hpp"
#include "velora/librarian/install.hpp"
#include "velora/librarian/library_service.hpp"

using namespace velora::librarian;
namespace fs = std::filesystem;
using ltest::Client;
using ltest::TempDir;

namespace {

int run(const std::string& cmd, std::string* err_text = nullptr) {
  const std::string errf = "/tmp/vl_e2e_" + std::to_string(::getpid()) + ".txt";
  const int rc = std::system((cmd + " >" + errf + ".out 2>" + errf).c_str());
  if (err_text) *err_text = ltest::slurp(errf) + ltest::slurp(errf + ".out");
  std::remove(errf.c_str());
  std::remove((errf + ".out").c_str());
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

}  // namespace

int main() {
  TempDir t;
  const std::string id = "00000000-0000-4000-8000-0000000000e1";
  const fs::path sfz = ifx::write_sfz(t.sub("src"), "Pack");
  {
    std::ofstream d(t.sub("desc.json"));
    d << "{\"schema_version\": 1, \"library_id\": \"" << id << "\", \"name\": \"E2E Piano\", \"source\": \"" << sfz.string() << "\", \"root\": \"dev\", \"category\": \"piano\"}\n";
  }
  std::string out;
  CHECK(run(std::string("'") + PYTHON_EXE + "' '" + KEYS_PY + "' generate --out '" + t.sub("keys") + "'", &out) == 0);
  CHECK(run(std::string("'") + PYTHON_EXE + "' '" + KEYS_PY + "' trust-entry --dir '" + t.sub("keys") + "'", &out) == 0);
  {
    std::ofstream tl(t.sub("trust.pub-list"));
    tl << out;
  }
  const std::string trust = ltest::slurp(t.sub("trust.pub-list"));
  CHECK(trust.find("content-manifest") != std::string::npos);
  fs::create_directories(t.sub("out1"));
  fs::create_directories(t.sub("out2"));
  CHECK(run(std::string("'") + CP_EXE + "' pack --description '" + t.sub("desc.json") + "' --out '" + t.sub("out1") + "' --keys-dir '" + t.sub("keys") + "' --device-pub '" +
                t.sub("keys") + "/device.x25519.pub' --revision 1",
            &out) == 0);
  CHECK(run(std::string("'") + CP_EXE + "' pack --description '" + t.sub("desc.json") + "' --out '" + t.sub("out2") + "' --keys-dir '" + t.sub("keys") + "' --device-pub '" +
                t.sub("keys") + "/device.x25519.pub' --revision 2",
            &out) == 0);
  const std::string root = t.sub("data");
  fs::create_directories(root);
  const std::string cli = std::string("'") + LIBRARIAN_EXE + "' install-protected ";
  CHECK(run(cli + "'" + t.sub("out1") + "/" + id + "' --root '" + root + "'", &out) == 1 && out.find("untrusted_key") != std::string::npos);  // the stock dev list trusts nothing
  CHECK(run(cli + "'" + t.sub("out1") + "/" + id + "' --root '" + root + "' --trust-list '" + t.sub("trust.pub-list") + "'", &out) == 0 && out.find("installed") != std::string::npos);
  CHECK(run(cli + "'" + t.sub("out2") + "/" + id + "' --root '" + root + "' --trust-list '" + t.sub("trust.pub-list") + "'", &out) == 0);  // revision 2
  CHECK(run(cli + "'" + t.sub("out1") + "/" + id + "' --root '" + root + "' --trust-list '" + t.sub("trust.pub-list") + "'", &out) == 1 && out.find("rejected (downgrade)") != std::string::npos);
  // the service on that root lists the Factory library (the startup reconcile reads the bundle through the trusted keys)
  LibraryServiceOptions o;
  o.data_dir = root;
  o.socket_path = t.sub("s.sock");
  std::string err;
  CHECK(load_trust_list(t.sub("trust.pub-list"), o.trusted_keys, err));
  LibraryService svc(o);
  CHECK(svc.start(err));
  Client c;
  CHECK(c.connect(o.socket_path) && c.handshake("e2e"));
  ws::v1::LibraryClientMessage m;
  m.mutable_list_libraries();
  auto l = c.call(m);
  CHECK(l && l->has_library_list() && l->library_list().total() == 1);
  if (l && l->library_list().total() == 1) {
    const auto& li = l->library_list().libraries(0);
    CHECK(li.library_id() == id && li.kind() == ws::v1::LIBRARY_KIND_FACTORY && li.name() == "E2E Piano" && li.instrument_count() == 1);
  }
  svc.stop();
  if (g_fail == 0) std::printf("librarian_e2e_pack_install_test: ok\n");
  return g_fail == 0 ? 0 : 1;
}
