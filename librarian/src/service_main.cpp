// librarian-service (Story 4.11): the librarian host process. Serves the Library API (proto/library_api.proto) on a Unix domain socket and owns
// the SQLite catalog `<data>/catalog.db`. On the board it runs as the systemd unit librarian/systemd/velora-librarian.service.
//
//   librarian-service --data DIR --socket PATH [--socket-group NAME]
//
// The socket is 0600 (dev). With --socket-group the socket gets that group and mode 0660 (the systemd unit passes `velora`, so ui/engine/studio can connect).
//
// Exit codes: 0 orderly shutdown (SIGTERM / SIGINT: unwritten Recent is flushed first), 1 could not start (reason on stderr: another service holds
// the catalog, the catalog is damaged or newer than this build, the socket is taken), 2 usage.
#include <signal.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "velora/librarian/library_service.hpp"

int main(int argc, char** argv) {
  std::string data, socket_path, socket_group;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--data") && i + 1 < argc) {
      data = argv[++i];
    } else if (!std::strcmp(argv[i], "--socket") && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (!std::strcmp(argv[i], "--socket-group") && i + 1 < argc) {
      socket_group = argv[++i];
    } else {
      std::fprintf(stderr, "usage: librarian-service --data DIR --socket PATH [--socket-group NAME]\n");
      return std::strcmp(argv[i], "--help") ? 2 : 0;
    }
  }
  if (data.empty() || socket_path.empty()) {
    std::fprintf(stderr, "usage: librarian-service --data DIR --socket PATH\n");
    return 2;
  }
  // Block the termination signals before any thread exists, so they are only ever taken by sigwait below.
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  signal(SIGPIPE, SIG_IGN);

  velora::librarian::LibraryServiceOptions opts;
  opts.data_dir = data;
  opts.socket_path = socket_path;
  opts.socket_group = socket_group;
  velora::librarian::LibraryService service(opts);
  std::string err;
  velora::librarian::CatalogStatus st = velora::librarian::CatalogStatus::kOk;
  if (!service.start(err, &st)) {
    if (st == velora::librarian::CatalogStatus::kBusy)
      std::fprintf(stderr, "librarian-service: another librarian service is already running on %s (the catalog is single-writer)\n", data.c_str());
    else
      std::fprintf(stderr, "librarian-service: %s\n", err.c_str());
    return 1;
  }
  std::printf("librarian-service: Library API (ws.v1) on %s, catalog %s/%s\n", socket_path.c_str(), data.c_str(),
              velora::librarian::kCatalogFile);
  std::fflush(stdout);
  int sig = 0;
  sigwait(&set, &sig);
  service.stop();
  std::printf("librarian-service: stopped (%llu recent writes, %llu favorite writes)\n", (unsigned long long)service.recent_writes(),
              (unsigned long long)service.favorite_writes());
  return 0;
}
