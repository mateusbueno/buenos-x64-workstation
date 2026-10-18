#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace velora::ports {

// Receives raw Universal MIDI Packet words from a MIDI input adapter, stamped with the host
// clock (nanoseconds) on arrival. Called on the adapter's own thread; single producer.
class UmpSink {
 public:
  virtual ~UmpSink() = default;
  // `words` holds `count` 32-bit UMP words (one or more whole packets, any message type).
  virtual void on_ump(const std::uint32_t* words, std::size_t count, std::uint64_t host_ns) noexcept = 0;
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
