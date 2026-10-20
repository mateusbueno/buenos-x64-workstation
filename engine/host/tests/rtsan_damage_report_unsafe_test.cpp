// Story 6.5 negative control for the damage path: REPORTING a damaged block (DamageRegistry::report: a mutex and allocations) inside a [[clang::nonblocking]] function
// must be caught by RealtimeSanitizer. That is why the audio thread only counts (SampleEngine::content_integrity_events) and the report is made by the I/O edge thread.
// CTest passes it only when RTSan itself names the violation.
#include "velora/host/damage_registry.hpp"

static void report_in_rt(velora::host::DamageRegistry& r) [[clang::nonblocking]] {
  r.report({"3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f", "piano.sfz", 0, 1, "content_integrity"});  // takes a lock and allocates: RTSan violation
}

int main() {
  velora::host::DamageRegistry reg;
  report_in_rt(reg);
  return 0;
}
