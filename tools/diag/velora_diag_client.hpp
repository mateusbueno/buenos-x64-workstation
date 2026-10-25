// Story 1.21: the velora-diag client: connect to the engine's ws.v1 socket, Hello, GetDiagnostics.
#pragma once

#include <string>

#include "ws_v1.pb.h"

namespace velora::diag {

// Returns true and fills `out` on success; otherwise false with a message in `err`.
bool query(const std::string& socket_path, ws::v1::DiagnosticsReply& out, std::string& err, int timeout_ms = 3000);

// Human-readable report (counters, both histograms, uptime).
std::string format(const ws::v1::DiagnosticsReply& r);

}  // namespace velora::diag
