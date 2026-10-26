// velora-sysdctl: dev CLI of the sysd config service (Foundation: sysd config service).
//   velora-sysdctl [--socket PATH] get
//   velora-sysdctl [--socket PATH] set FIELD VALUE [FIELD VALUE ...]     (one SetConfig with only the fields given; nothing changes when any value is invalid)
//   velora-sysdctl [--socket PATH] watch [COUNT]                         (prints the config now and on every change, COUNT pushes then exits; default forever)
// Fields: clock_source (internal|din|usb), velocity_curve (normal|light|heavy|fixed), sustain / expression ("MIN,MAX[,open|closed|auto]", parts may be empty),
//         boot_into (last_sound|start_of_setlist|default_live_set), default_live_set_id (ID).
//   velora-sysdctl [--socket PATH] --engine-socket PATH calibrate-pedal sustain|expression [--seconds N]
//                                                       (Story 3.11: asks the ENGINE (its ws.v1 socket) to measure the pedal's raw range while you press it fully
//                                                        and release it, then stores min / max in sysd. Waits for Enter, or N seconds with --seconds)
// Exit: 0 ok (calibrate-pedal: stored), 1 sysd unreachable or the request was refused (typed error on stderr), 2 usage.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "sysdctl_client.hpp"

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool parse_enum(const std::string& v, std::initializer_list<std::pair<const char*, int>> names, int& out) {
  for (const auto& n : names)
    if (lower(v) == n.first) {
      out = n.second;
      return true;
    }
  return false;
}

// "MIN,MAX[,open|closed|auto]": every part may be left empty (",,closed" sets only the polarity, "500," only the minimum): only the parts given are sent.
bool parse_pedal(const std::string& v, ws::v1::PedalPatch& p) {
  std::string parts[3];
  std::size_t n = 0, start = 0;
  for (;;) {
    const std::size_t comma = v.find(',', start);
    if (n >= 3) return false;
    parts[n++] = v.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  bool any = false;
  for (int i = 0; i < 2; ++i) {
    if (i >= static_cast<int>(n) || parts[i].empty()) continue;
    char* end = nullptr;
    const unsigned long x = std::strtoul(parts[i].c_str(), &end, 10);
    if (*end != '\0' || x > 0xFFFFFFFFul) return false;
    if (i == 0) p.set_min(static_cast<std::uint32_t>(x));
    else p.set_max(static_cast<std::uint32_t>(x));
    any = true;
  }
  if (n == 3 && !parts[2].empty()) {
    const std::string ps = lower(parts[2]);
    if (ps == "open") p.set_polarity(ws::v1::PEDAL_POLARITY_NORMALLY_OPEN);
    else if (ps == "closed") p.set_polarity(ws::v1::PEDAL_POLARITY_NORMALLY_CLOSED);
    else if (ps == "auto") p.set_polarity(ws::v1::PEDAL_POLARITY_UNSPECIFIED);
    else return false;
    any = true;
  }
  return any;
}

// Request timeout: 10 s, or VELORA_SYSDCTL_TIMEOUT_MS (a loaded machine or a slow fsync must not make the CLI fail spuriously).
int timeout_ms() {
  const char* e = std::getenv("VELORA_SYSDCTL_TIMEOUT_MS");
  const long v = e ? std::strtol(e, nullptr, 10) : 0;
  return v > 0 ? static_cast<int>(v) : 10000;
}

// Story 3.11: the calibration routine against the engine's socket. Exit 0 only when the result was stored.
int calibrate_pedal(const std::string& engine_socket, const std::string& pedal_name, long seconds) {
  ws::v1::PedalKind kind = ws::v1::PEDAL_KIND_UNSPECIFIED;
  if (lower(pedal_name) == "sustain") kind = ws::v1::PEDAL_KIND_SUSTAIN;
  else if (lower(pedal_name) == "expression") kind = ws::v1::PEDAL_KIND_EXPRESSION;
  else return 2;
  if (engine_socket.empty()) {
    std::fprintf(stderr, "velora-sysdctl: calibrate-pedal needs --engine-socket PATH (the engine's ws.v1 socket)\n");
    return 2;
  }
  velora::sysdctl::Connection e;
  std::string err;
  if (!e.open(engine_socket, err, timeout_ms())) {
    std::fprintf(stderr, "velora-sysdctl: engine: %s\n", err.c_str());
    return 1;
  }
  ws::v1::PedalCalibrationReply r;
  if (!e.start_pedal_calibration(kind, r, err)) {
    std::fprintf(stderr, "velora-sysdctl: %s\n", err.c_str());
    return 1;
  }
  const std::uint64_t token = r.run_token();
  std::printf("Press the %s pedal fully down, then release it.%s\n", pedal_name.c_str(), seconds > 0 ? "" : " Press Enter when done.");
  std::fflush(stdout);
  if (seconds > 0) {
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
  } else {
    std::string line;
    if (!std::getline(std::cin, line)) {  // no terminal: do not leave the engine collecting
      (void)e.cancel_pedal_calibration(token, r, err);
      std::fprintf(stderr, "velora-sysdctl: no input: %s\n", velora::sysdctl::format_pedal_result(r).c_str());
      return 1;
    }
  }
  if (!e.finish_pedal_calibration(token, r, err)) {
    std::fprintf(stderr, "velora-sysdctl: %s\n", err.c_str());
    return 1;
  }
  std::printf("%s\n", velora::sysdctl::format_pedal_result(r).c_str());
  return r.result() == ws::v1::PEDAL_CALIBRATION_RESULT_STORED ? 0 : 1;
}

int usage() {
  std::fprintf(stderr,
               "usage: velora-sysdctl [--socket PATH] get\n"
               "       velora-sysdctl [--socket PATH] set FIELD VALUE [FIELD VALUE ...]\n"
               "       velora-sysdctl [--socket PATH] watch [COUNT]\n"
               "       velora-sysdctl --engine-socket PATH calibrate-pedal sustain|expression [--seconds N]\n"
               "fields: clock_source internal|din|usb; velocity_curve normal|light|heavy|fixed; sustain|expression MIN,MAX[,open|closed|auto];\n"
               "        boot_into last_sound|start_of_setlist|default_live_set; default_live_set_id ID\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::string socket = "/tmp/velora-sysd.sock";
  int i = 1;
  if (i + 1 < argc && !std::strcmp(argv[i], "--socket")) {
    socket = argv[i + 1];
    i += 2;
  }
  std::string engine_socket;
  if (i + 1 < argc && !std::strcmp(argv[i], "--engine-socket")) {
    engine_socket = argv[i + 1];
    i += 2;
  }
  if (i + 1 < argc && !std::strcmp(argv[i], "--socket")) {  // either order
    socket = argv[i + 1];
    i += 2;
  }
  if (i >= argc) return usage();
  const std::string cmd = argv[i++];
  if (cmd == "calibrate-pedal") {
    if (i >= argc) return usage();
    const std::string pedal = argv[i++];
    long seconds = 0;
    if (i + 1 < argc && !std::strcmp(argv[i], "--seconds")) seconds = std::strtol(argv[i + 1], nullptr, 10);
    const int rc = calibrate_pedal(engine_socket, pedal, seconds);
    return rc == 2 ? usage() : rc;
  }
  velora::sysdctl::Connection c;
  std::string err;
  if (cmd != "get" && cmd != "set" && cmd != "watch") return usage();
  ws::v1::ConfigPatch patch;
  if (cmd == "set") {
    if (i >= argc || (argc - i) % 2 != 0) return usage();
    for (; i + 1 < argc; i += 2) {
      const std::string f = argv[i], v = argv[i + 1];
      int e = 0;
      if (f == "clock_source" && parse_enum(v, {{"internal", 1}, {"din", 2}, {"usb", 3}}, e)) patch.set_clock_source(static_cast<ws::v1::ClockSource>(e));
      else if (f == "velocity_curve" && parse_enum(v, {{"normal", 1}, {"light", 2}, {"heavy", 3}, {"fixed", 4}}, e)) patch.set_velocity_curve(static_cast<ws::v1::VelocityCurve>(e));
      else if (f == "sustain" && parse_pedal(v, *patch.mutable_sustain())) {}
      else if (f == "expression" && parse_pedal(v, *patch.mutable_expression())) {}
      else if (f == "boot_into" && parse_enum(v, {{"last_sound", 1}, {"start_of_setlist", 2}, {"default_live_set", 3}}, e)) patch.mutable_boot_into()->set_mode(static_cast<ws::v1::BootIntoMode>(e));
      else if (f == "default_live_set_id") patch.mutable_boot_into()->set_default_live_set_id(v);  // only the id: the stored mode is kept
      else {
        std::fprintf(stderr, "velora-sysdctl: bad field or value: %s %s\n", f.c_str(), v.c_str());
        return 2;
      }
    }
  }
  if (!c.open(socket, err, timeout_ms())) {
    std::fprintf(stderr, "velora-sysdctl: %s\n", err.c_str());
    return 1;
  }
  ws::v1::ConfigReply r;
  if (cmd == "get") {
    if (!c.get(r, err)) {
      std::fprintf(stderr, "velora-sysdctl: %s\n", err.c_str());
      return 1;
    }
    std::fputs(velora::sysdctl::format(r.config(), r.revision()).c_str(), stdout);
    return 0;
  }
  if (cmd == "set") {
    if (!c.set(patch, r, err)) {
      std::fprintf(stderr, "velora-sysdctl: %s\n", err.c_str());
      return 1;
    }
    std::fputs(velora::sysdctl::format(r.config(), r.revision()).c_str(), stdout);
    if (r.not_durable()) std::fprintf(stderr, "velora-sysdctl: applied, but not durable (directory sync failed)\n");
    return 0;
  }
  const long count = i < argc ? std::strtol(argv[i], nullptr, 10) : 0;
  if (!c.subscribe(true, err)) {
    std::fprintf(stderr, "velora-sysdctl: %s\n", err.c_str());
    return 1;
  }
  for (long n = 0; count <= 0 || n < count;) {
    ws::v1::ServerMessage m;
    if (!c.next_push(m, 3600 * 1000)) {
      std::fprintf(stderr, "velora-sysdctl: connection lost\n");
      return 1;
    }
    if (!m.has_config_reply()) continue;
    std::fputs(velora::sysdctl::format(m.config_reply().config(), m.config_reply().revision()).c_str(), stdout);
    std::fputs("--\n", stdout);
    std::fflush(stdout);
    ++n;
  }
  return 0;
}
