// velora-diag: an unreachable socket is an error (the CLI exits 1 on it) and the report lists counters, both histograms and uptime.
#include <cstdio>
#include <string>

#include "velora_diag_client.hpp"

#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main() {
  ws::v1::DiagnosticsReply r;
  std::string err;
  CHECK(!velora::diag::query("/nonexistent/velora-diag-test.sock", r, err));
  CHECK(!err.empty());

  r.set_xrun(3);
  r.set_voice_steal(7);
  r.set_uptime_ms(1234);
  r.mutable_render_time()->add_edges_us(100);
  r.mutable_render_time()->add_counts(5);
  r.mutable_render_time()->add_counts(2);
  const std::string s = velora::diag::format(r);
  CHECK(s.find("xrun: 3") != std::string::npos);
  CHECK(s.find("voice_steal: 7") != std::string::npos);
  CHECK(s.find("uptime_ms: 1234") != std::string::npos);
  CHECK(s.find("render_time") != std::string::npos);
  CHECK(s.find("event_to_onset_latency") != std::string::npos);
  CHECK(s.find("inf") != std::string::npos);
  std::printf("velora_diag_test: ok\n");
  return 0;
}
