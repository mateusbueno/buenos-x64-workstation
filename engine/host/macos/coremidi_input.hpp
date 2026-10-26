#pragma once

#include <CoreMIDI/CoreMIDI.h>

#include <set>
#include <string>

#include "velora/ports/midi_input.hpp"

namespace velora::host::macos {

// CoreMIDI adapter. Opens a client and a UMP input port (MIDI 2.0 protocol; falls back to a
// MIDI 1.0 UMP port, whose type-2 packets the ingest layer up-converts), connects every source,
// and picks up sources that appear later via rescan(). Packets are stamped with the host clock
// (mach time -> ns) in the CoreMIDI receive callback and handed to the UmpSink; CoreMIDI delivers
// all of a client's input on one thread, so the sink has a single producer.
class CoreMidiInput final : public ports::MidiInput {
 public:
  CoreMidiInput() = default;
  ~CoreMidiInput() override { close(); }
  CoreMidiInput(const CoreMidiInput&) = delete;
  CoreMidiInput& operator=(const CoreMidiInput&) = delete;

  bool open(ports::UmpSink& sink, std::string& error) override;
  std::size_t rescan() override;
  std::size_t connected_sources() const override { return connected_.size(); }
  void close() override;

  MIDIProtocolID protocol() const noexcept { return protocol_; }

  // Story 7.7: a source whose name (CoreMIDI name or display name) contains `name` (case-insensitive) is tagged kDin; every other source (USB, virtual) is kUsb.
  // Lets DIN be exercised on a Mac. The board's DIN UART adapter (not implemented here) tags its packets kDin itself: it calls UmpSink::on_ump_port(kDin, ...)
  // with the arrival time it stamps in its own reader thread. Set before open() / rescan(); a source already connected keeps its tag.
  void set_din_source(std::string name) { din_name_ = std::move(name); }
  std::size_t din_sources() const noexcept { return din_count_; }

 private:
  ports::UmpSink* sink_ = nullptr;
  MIDIClientRef client_ = 0;
  MIDIPortRef port_ = 0;
  MIDIProtocolID protocol_ = kMIDIProtocol_2_0;
  std::set<MIDIEndpointRef> connected_;  // control thread only
  std::string din_name_;
  std::size_t din_count_ = 0;
};

}  // namespace velora::host::macos
