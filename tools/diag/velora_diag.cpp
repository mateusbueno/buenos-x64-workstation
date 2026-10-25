// velora-diag --socket PATH: print the engine's in-RAM diagnostics (counters, histograms, uptime). Exit 0, or 1 when the socket is unreachable.
#include <cstdio>
#include <cstring>
#include <string>

#include "velora_diag_client.hpp"

int main(int argc, char** argv) {
  std::string socket = "/tmp/velora-engine.sock";
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--socket") && i + 1 < argc) {
      socket = argv[++i];
    } else {
      std::fprintf(stderr, "usage: velora-diag [--socket PATH]\n");
      return std::strcmp(argv[i], "--help") ? 2 : 0;
    }
  }
  ws::v1::DiagnosticsReply r;
  std::string err;
  if (!velora::diag::query(socket, r, err)) {
    std::fprintf(stderr, "velora-diag: %s\n", err.c_str());
    return 1;
  }
  std::fputs(velora::diag::format(r).c_str(), stdout);
  return 0;
}
