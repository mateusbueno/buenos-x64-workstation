#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace velora::ports {

// Story 7.7: which kind of physical input a packet arrived on. The adapter knows its source (CoreMIDI: USB or virtual sources are kUsb, a source named with
// --din-source is kDin; the board's DIN UART adapter will tag kDin the same way, see engine/host/README.md). Only the clock source setting cares.
enum class MidiPortKind : std::uint8_t { kUsb = 0, kDin = 1 };

// Receives raw Universal MIDI Packet words from a MIDI input adapter, stamped with the host
// clock (nanoseconds) on arrival. Called on the adapter's own thread; single producer.
class UmpSink {
 public:
  virtual ~UmpSink() = default;
  // `words` holds `count` 32-bit UMP words (one or more whole packets, any message type).
  virtual void on_ump(const std::uint32_t* words, std::size_t count, std::uint64_t host_ns) noexcept = 0;
  // The same, tagged with the input port kind. The default drops the tag (a sink that does not care about ports); adapters that know their source call this.
  virtual void on_ump_port(MidiPortKind port, const std::uint32_t* words, std::size_t count, std::uint64_t host_ns) noexcept {
    (void)port;
    on_ump(words, count, host_ns);
  }
};

class MidiInput {
 public:
  virtual ~MidiInput() = default;
  virtual bool open(UmpSink& sink, std::string& error) = 0;
  // Connect sources that appeared since the last call; returns the number newly connected.
  virtual std::size_t rescan() = 0;
  virtual std::size_t connected_sources() const = 0;
  virtual void close() = 0;
};

}  // namespace velora::ports
