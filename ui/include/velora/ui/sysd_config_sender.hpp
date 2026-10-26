#pragma once
// Story 7.7: the ui's seam to the sysd config service (AR17, AR39). sysd owns the system configuration; the ui asks it to change a setting and shows what sysd
// reports. The seam is injectable (like the LED send hook): tests give a fake; the real one (make_socket_sysd_sender) speaks ws.v1 to velora-sysd on a worker
// thread with the blocking sysdctl client, so the UI loop never waits on a socket. Today the only setting that goes through it is the Clock source.
#include <cstdint>
#include <memory>
#include <string>

#include "velora/ui/system_config.hpp"

namespace velora::ui {

class SysdConfigSender {
 public:
  struct Event {
    enum class Kind : uint8_t {
      Config,  // sysd reported its config (at connect, after a change, or the reply to ours): `clock_source` is its value
      Failed,  // a request could not be delivered or sysd refused it: `error` says why; the ui reverts to its last known value
    };
    bool unreachable{false};  // Failed: sysd could not be reached (absent, connection lost, no reply) as opposed to answering with a refusal
    Kind kind{Kind::Config};
    ClockSourceMode clock_source{ClockSourceMode::Internal};
    std::string error;
  };

  virtual ~SysdConfigSender() = default;
  // Asks sysd to set the clock source (a SetConfig patch with only clock_source). Never blocks; the outcome comes back through poll(). UI thread.
  virtual void request_clock_source(ClockSourceMode mode) = 0;
  // The next outcome / report, oldest first. UI thread; false when there is none.
  virtual bool poll(Event& out) = 0;
};

// The real sender: connects to sysd's Unix socket (reconnecting with backoff while sysd is absent), subscribes to config pushes and sends patches, all on one
// background thread. A missing sysd never blocks or crashes the ui: requests made meanwhile fail with a Failed event.
std::unique_ptr<SysdConfigSender> make_socket_sysd_sender(const std::string& socket_path);

}  // namespace velora::ui
