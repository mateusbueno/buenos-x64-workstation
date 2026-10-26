#include "coremidi_input.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>

#include "mach_time.hpp"

namespace velora::host::macos {

namespace {
std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}
bool property_contains(MIDIObjectRef obj, CFStringRef prop, const std::string& needle_lower) {
  CFStringRef v = nullptr;
  if (MIDIObjectGetStringProperty(obj, prop, &v) != noErr || !v) return false;
  char buf[256] = {0};
  const bool ok = CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingUTF8);
  CFRelease(v);
  return ok && lower(buf).find(needle_lower) != std::string::npos;
}
// A source is DIN when --din-source NAME is given and the name or the display name contains it.
bool is_din(MIDIEndpointRef src, const std::string& name) {
  if (name.empty()) return false;
  const std::string n = lower(name);
  return property_contains(src, kMIDIPropertyName, n) || property_contains(src, kMIDIPropertyDisplayName, n);
}
}  // namespace

bool CoreMidiInput::open(ports::UmpSink& sink, std::string& error) {
  close();  // repeated open(): drop any previous client
  sink_ = &sink;
  OSStatus s = MIDIClientCreate(CFSTR("velora-engine-host"), nullptr, nullptr, &client_);
  if (s != noErr) {
    error = "MIDIClientCreate failed (OSStatus " + std::to_string(s) + ")";
    client_ = 0;
    return false;
  }
  ports::UmpSink* target = sink_;
  auto receive = ^(const MIDIEventList* list, void* src_ref) {
    // The connection refCon (set in rescan) carries the source's port kind.
    const ports::MidiPortKind kind = src_ref ? ports::MidiPortKind::kDin : ports::MidiPortKind::kUsb;
    const std::uint64_t arrival_ns = now_ns();  // stamp on arrival, host clock
    const MIDIEventPacket* p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i) {
      // Prefer the packet's own host-time stamp (0 = "now"); never later than arrival.
      std::uint64_t t = arrival_ns;
      if (p->timeStamp != 0) t = std::min(arrival_ns, host_ticks_to_ns(p->timeStamp));
      target->on_ump_port(kind, p->words, p->wordCount, t);
      p = MIDIEventPacketNext(p);
    }
  };
  protocol_ = kMIDIProtocol_2_0;
  s = MIDIInputPortCreateWithProtocol(client_, CFSTR("velora-in"), protocol_, &port_, receive);
  if (s != noErr) {
    protocol_ = kMIDIProtocol_1_0;
    s = MIDIInputPortCreateWithProtocol(client_, CFSTR("velora-in"), protocol_, &port_, receive);
  }
  if (s != noErr) {
    error = "MIDIInputPortCreateWithProtocol failed (OSStatus " + std::to_string(s) + ")";
    MIDIClientDispose(client_);
    client_ = 0;
    port_ = 0;
    return false;
  }
  rescan();
  return true;
}

std::size_t CoreMidiInput::rescan() {
  if (!client_ || !port_) return 0;
  std::size_t added = 0;
  const ItemCount n = MIDIGetNumberOfSources();
  std::set<MIDIEndpointRef> current;
  for (ItemCount i = 0; i < n; ++i)
    if (MIDIEndpointRef src = MIDIGetSource(i)) current.insert(src);
  for (auto it = connected_.begin(); it != connected_.end();)  // forget vanished sources
    it = current.count(*it) ? std::next(it) : connected_.erase(it);
  for (MIDIEndpointRef src : current) {
    if (connected_.count(src)) continue;
    const bool din = is_din(src, din_name_);
    if (MIDIPortConnectSource(port_, src, din ? reinterpret_cast<void*>(static_cast<uintptr_t>(1)) : nullptr) == noErr) {
      connected_.insert(src);
      if (din) ++din_count_;
      ++added;
    }
  }
  return added;
}

void CoreMidiInput::close() {
  if (port_) MIDIPortDispose(port_);
  if (client_) MIDIClientDispose(client_);
  port_ = 0;
  client_ = 0;
  sink_ = nullptr;
  connected_.clear();
  din_count_ = 0;
}

}  // namespace velora::host::macos
