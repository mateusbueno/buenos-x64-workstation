#pragma once
// Damaged-library registry (Story 6.5, FR32). A protected library whose block fails its hash check while it plays (found by an I/O edge thread, never by the
// audio thread) is REPORTED here: the control plane shows it in State.content (additive) and the host API can ask for it. The report is sticky for the life of
// the process (a damaged library stays damaged until it is reinstalled and reloaded; the engine never plays a block it could not verify).
//
// Threading: report() may be called from any non-real-time thread; every reader may run on any non-real-time thread. It takes a mutex, so it must never be
// called from an audio callback (the audio side only counts: SampleEngine::content_integrity_events()).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace velora::host {

struct DamageEvent {
  std::string library_id;
  std::string instrument_key;
  std::size_t sample = 0;    // instrument-local sample index
  std::uint64_t block = 0;   // block index inside the sample
  std::string reason;        // "content_integrity" (hash mismatch, or a block that does not decode)
};

class DamageRegistry {
 public:
  static constexpr std::size_t kMaxEvents = 64;  // events kept for display; counts are exact

  // Records the event, marks the library damaged, raises the sequence number and calls the listener (outside the lock).
  void report(DamageEvent e);
  // Story 6.5: a read that failed for a reason that says nothing about the block (an I/O error): counted and shown, never marked damaged.
  void report_io_error(std::string_view library_id);
  std::uint64_t io_errors() const;
  // A library that was reloaded or reinstalled is no longer reported damaged (its events go too); the monotone counters stay. Calls the listener when it changed.
  void clear_library(std::string_view library_id);
  bool library_damaged(std::string_view library_id) const;
  std::vector<std::string> damaged_libraries() const;  // sorted, each once
  std::vector<DamageEvent> events() const;             // the first kMaxEvents events
  std::uint64_t reports() const;                       // every report() ever made
  std::uint64_t seq() const { return seq_snapshot(); }  // reports + io errors + clears: moves on every visible change
  // Called after each report on the reporting thread (the control plane bumps its revision so subscribers see the change). One listener.
  void set_listener(std::function<void()> fn);

 private:
  std::uint64_t seq_snapshot() const;
  std::uint64_t clears_ = 0;
  mutable std::mutex mu_;
  std::vector<DamageEvent> events_;
  std::vector<std::string> libraries_;
  std::uint64_t reports_ = 0, io_errors_ = 0;
  std::function<void()> listener_;
};

}  // namespace velora::host
