#include "coremidi_input.hpp"

#include <algorithm>
#include <iterator>

#include "mach_time.hpp"

namespace velora::host::macos {

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
  auto receive = ^(const MIDIEventList* list, void*) {
    const std::uint64_t arrival_ns = now_ns();  // stamp on arrival, host clock
    const MIDIEventPacket* p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i) {
      // Prefer the packet's own host-time stamp (0 = "now"); never later than arrival.
      std::uint64_t t = arrival_ns;
      if (p->timeStamp != 0) t = std::min(arrival_ns, host_ticks_to_ns(p->timeStamp));
      target->on_ump(p->words, p->wordCount, t);
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
    if (MIDIPortConnectSource(port_, src, nullptr) == noErr) {
      connected_.insert(src);
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
}

}  // namespace velora::host::macos
