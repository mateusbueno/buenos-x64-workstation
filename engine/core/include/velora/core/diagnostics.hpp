// Story 1.21: in-RAM diagnostics (FR120, AR44): health counters and two fixed-bin histograms.
//
// Written on the RT / host render paths with relaxed atomic read-modify-write only: no syscall, lock or allocation. Read lock-free by the control
// thread (ws.v1 GetDiagnostics). Nothing here touches the disk. Pure std types (engine/core must not include protobuf).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "velora/core/rt.hpp"

namespace velora::core {

// A histogram of non-negative values in microseconds with fixed log-spaced bin edges.
// Bin i (i < N-1) counts values v with edge[i-1] <= v < edge[i] (edge[-1] = 0); the last bin is the overflow bin (v >= edge[N-2]).
template <std::size_t N>
class FixedHistogram {
 public:
  static_assert(N >= 2);
  static constexpr std::size_t kBins = N;
  using Edges = std::array<std::uint32_t, N - 1>;

  explicit constexpr FixedHistogram(const Edges& edges) noexcept : edges_(edges) {}

  void record_ns(std::uint64_t ns) noexcept VELORA_NONBLOCKING {
    const std::uint64_t us = ns / 1000;
    std::size_t i = 0;
    while (i < N - 1 && us >= edges_[i]) ++i;
    bins_[i].fetch_add(1, std::memory_order_relaxed);
  }
  std::uint64_t count(std::size_t bin) const noexcept { return bins_[bin].load(std::memory_order_relaxed); }
  std::uint64_t total() const noexcept {
    std::uint64_t t = 0;
    for (const auto& b : bins_) t += b.load(std::memory_order_relaxed);
    return t;
  }
  // Upper edges in microseconds (exclusive) of bins 0..N-2.
  const Edges& edges_us() const noexcept { return edges_; }

 private:
  Edges edges_;
  std::array<std::atomic<std::uint64_t>, N> bins_{};
};

class Diagnostics {
 public:
  // Render-time histogram: one block's render duration. 64 frames at 48 kHz = 1333 us deadline.
  static constexpr std::size_t kRenderBins = 13;
  // Event-to-onset latency: host timestamp of the event to the instant its first frame is heard.
  static constexpr std::size_t kLatencyBins = 12;

  Diagnostics() noexcept
      : render_(FixedHistogram<kRenderBins>::Edges{50, 100, 200, 400, 600, 800, 1000, 1333, 2000, 4000, 8000, 16000}),
        latency_(FixedHistogram<kLatencyBins>::Edges{250, 500, 1000, 2000, 3000, 4000, 5000, 7500, 10000, 20000, 50000}) {}

  // --- RT / host render path (atomic ops only) ---
  void record_xrun() noexcept VELORA_NONBLOCKING { xruns_.fetch_add(1, std::memory_order_relaxed); }
  void record_stream_underrun() noexcept VELORA_NONBLOCKING { stream_underruns_.fetch_add(1, std::memory_order_relaxed); }
  // A block render took `render_ns`; over `deadline_ns` it also counts an xrun.
  void record_render(std::uint64_t render_ns, std::uint64_t deadline_ns) noexcept VELORA_NONBLOCKING {
    render_.record_ns(render_ns);
    if (render_ns > deadline_ns) record_xrun();
  }
  void record_latency_ns(std::uint64_t ns) noexcept VELORA_NONBLOCKING { latency_.record_ns(ns); }
  // The voice arbiter's running totals (monotone), published by the block loop.
  void store_voice_totals(std::uint64_t steals, std::uint64_t exhausted) noexcept VELORA_NONBLOCKING {
    steals_.store(steals, std::memory_order_relaxed);
    exhausted_.store(exhausted, std::memory_order_relaxed);
  }

  // --- any thread ---
  std::uint64_t xruns() const noexcept { return xruns_.load(std::memory_order_relaxed); }
  std::uint64_t stream_underruns() const noexcept { return stream_underruns_.load(std::memory_order_relaxed); }
  std::uint64_t voice_steals() const noexcept { return steals_.load(std::memory_order_relaxed); }
  std::uint64_t voice_exhausted() const noexcept { return exhausted_.load(std::memory_order_relaxed); }
  const FixedHistogram<kRenderBins>& render_time() const noexcept { return render_; }
  const FixedHistogram<kLatencyBins>& latency() const noexcept { return latency_; }

 private:
  std::atomic<std::uint64_t> xruns_{0}, stream_underruns_{0}, steals_{0}, exhausted_{0};
  FixedHistogram<kRenderBins> render_;
  FixedHistogram<kLatencyBins> latency_;
};

}  // namespace velora::core
