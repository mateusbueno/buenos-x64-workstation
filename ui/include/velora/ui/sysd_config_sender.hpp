#pragma once
// Story 7.7: the ui's seam to the sysd config service (AR17, AR39). sysd owns the system configuration; the ui asks it to change a setting and shows what sysd
// reports. The seam is injectable (like the LED send hook): tests give a fake; the real one (make_socket_sysd_sender) speaks ws.v1 to velora-sysd on a worker
// thread with the blocking sysdctl client, so the UI loop never waits on a socket. The settings that go through it are the Clock source (Story 7.7) and Boot into (Story 8.9).
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
      PairingWindow,   // Story 9.6: pairing window status / code / active computer
      PairedComputers, // Story 9.6: list of paired computers
      UpdateAvailable, // Story 8.3: verified firmware bundle announced by sysd
      UpdateProgress,  // Story 8.3: install progress phase + percent
      UpdateFinished,  // Story 8.3: install completed (unit restarting)
    };
    // Which request a Failed event is about (the ui reverts that control and says so).
    enum class Setting : uint8_t { ClockSource, BootInto, Pairing, Update };
    Setting setting{Setting::ClockSource};
    // Config: sysd's Boot into (Story 8.9), when it reported one this ui knows: the mode as SystemConfig::boot_into (0 last sound, 1 start of setlist, 2 default Live Set)
    // and the default Live Set id ("" = none).
    bool has_clock_source{true};  // false when sysd reported a clock source this ui does not know (it is not shown; a valid boot_into in the same event still is)
    bool has_boot_into{false};
    uint8_t boot_into{0};
    std::string default_live_set_id;
    bool unreachable{false};  // Failed: sysd could not be reached (absent, connection lost, no reply) as opposed to answering with a refusal
    Kind kind{Kind::Config};
    ClockSourceMode clock_source{ClockSourceMode::Internal};
    std::string error;

    // Story 9.6 pairing fields
    struct PairedComputer {
      std::string id;
      std::string name;
      int64_t last_connected_unix_s{0};
    };
    bool pairing_is_open{false};
    std::string pairing_code;
    int64_t pairing_remaining_ms{0};
    int32_t pairing_attempts_left{0};
    std::string pairing_active_computer_name;
    std::vector<PairedComputer> paired_computers;

    // Story 8.3 update fields
    std::string update_version;
    std::string update_bundle_path;
    std::string update_phase;
    uint32_t update_percent{0};
    std::string update_fallback_version;
  };

  virtual ~SysdConfigSender() = default;
  // Asks sysd to set the clock source (a SetConfig patch with only clock_source). Never blocks; the outcome comes back through poll(). UI thread.
  virtual void request_clock_source(ClockSourceMode mode) = 0;
  // Story 8.9: asks sysd to set Boot into (a SetConfig patch with only the boot_into fields: the mode, and the default Live Set id only when `default_live_set_id` is
  // not empty). `mode` is SystemConfig::boot_into. Never blocks; the outcome comes back through poll(). UI thread.
  virtual void request_boot_into(uint8_t mode, const std::string& default_live_set_id) = 0;
  // Story 9.6: pairing requests (device Settings UI)
  virtual void request_start_pairing() = 0;
  virtual void request_cancel_pairing() = 0;
  virtual void request_pairing_status() = 0;
  virtual void request_list_paired_computers() = 0;
  virtual void request_forget_computer(const std::string& id) = 0;
  // Story 8.3: request firmware install on the unit
  virtual void request_install_update(const std::string& bundle_path = "") {}
  // The next outcome / report, oldest first. UI thread; false when there is none.
  virtual bool poll(Event& out) = 0;
};

// The real sender: connects to sysd's Unix socket (reconnecting with backoff while sysd is absent), subscribes to config pushes and sends patches, all on one
// background thread. A missing sysd never blocks or crashes the ui: requests made meanwhile fail with a Failed event.
std::unique_ptr<SysdConfigSender> make_socket_sysd_sender(const std::string& socket_path);

}  // namespace velora::ui
