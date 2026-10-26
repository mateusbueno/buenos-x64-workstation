// The player's selected velocity curve, held for the keybed readers (Story 3.6). The sysd config callback (control thread) calls set(); the keybed link wiring
// attaches its KeybedReader when it creates it (the engine-host has no keybed link yet) and gets the current curve at once. A reader's curve setter is an atomic
// store, so a change made while the reader runs on its own thread takes effect for the next Note On and never touches a sounding note.
// attach / detach / set are serialised by a mutex (control and setup threads only, never the RT plane or the reader thread).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "velora/host/keybed_reader.hpp"
#include "velora/host/velocity_curve.hpp"

namespace velora::host {

class VelocityCurveHolder {
 public:
  // The reader must outlive its attachment (detach it first). Applies the current curve to it.
  void attach(KeybedReader& r);
  void detach(KeybedReader& r);
  // Stores the curve and pushes it to every attached reader.
  void set(VelocityCurve c);
  VelocityCurve curve() const noexcept { return static_cast<VelocityCurve>(curve_.load(std::memory_order_relaxed)); }
  std::size_t attached() const;

 private:
  std::atomic<std::uint8_t> curve_{static_cast<std::uint8_t>(VelocityCurve::kNormal)};
  mutable std::mutex mu_;
  std::vector<KeybedReader*> readers_;
};

}  // namespace velora::host
