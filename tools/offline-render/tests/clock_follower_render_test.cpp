#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "velora/core/arpeggiator.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/clock_follower.hpp"
#include "velora/core/transport_clock.hpp"
#include "velora/core/ump.hpp"

using namespace velora::core;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

namespace {

struct ClockTick {
  std::size_t index = 0;
  double ideal_frame = 0.0;
  double actual_frame = 0.0;
};

struct TestResult {
  std::string name;
  std::size_t note_count = 0;
  double max_error_frames = 0.0;
  double max_error_ms = 0.0;
  double mean_error_ms = 0.0;
  bool passed = false;
};

int run_follower_render_case(const std::string& case_name,
                             const std::vector<ClockTick>& clock_ticks,
                             std::size_t start_tick_idx,
                             TestResult& result,
                             std::ostream& report_out) {
  BlockRenderer br;
  br.set_clock_source(ClockSource::kDin);

  ArpConfig cfg;
  cfg.enabled = true;
  cfg.mode = ArpMode::kUp;
  cfg.rate = ArpRate::k1_16;  // 240 ticks per note = 6 MIDI clock ticks
  cfg.octaves = 1;
  cfg.gate_percent = 50.0f;
  cfg.swing_percent = 50.0f;
  br.set_part_arp(0, cfg);

  std::vector<EmittedNoteEvent> emitted;
  br.set_note_observer(
      [](void* ctx, const EmittedNoteEvent& ev) noexcept {
        static_cast<std::vector<EmittedNoteEvent>*>(ctx)->push_back(ev);
      },
      &emitted);

  const double last_tick_frame = clock_ticks.back().actual_frame;
  const std::uint64_t total_frames = static_cast<std::uint64_t>(std::ceil(last_tick_frame)) + 500u;
  const std::uint64_t total_blocks = (total_frames + kBlockFrames - 1u) / kBlockFrames;

  float left[kBlockFrames]{};
  float right[kBlockFrames]{};

  std::size_t tick_idx = 0;
  bool start_submitted = false;

  for (std::uint64_t b = 0; b < total_blocks; ++b) {
    const double block_start = static_cast<double>(b * kBlockFrames);
    const double block_end = static_cast<double>((b + 1) * kBlockFrames);

    while (tick_idx < clock_ticks.size() && clock_ticks[tick_idx].actual_frame < block_end) {
      const auto& ct = clock_ticks[tick_idx++];
      const std::uint32_t off = ct.actual_frame < block_start
                                    ? 0u
                                    : static_cast<std::uint32_t>(ct.actual_frame - block_start);
      if (ct.index == start_tick_idx && !start_submitted) {
        start_submitted = true;
        CHECK(br.submit({make_start(), off}));
        // Play chord note 60 (C4) on Part 0
        CHECK(br.submit({make_note_on(0, 0, 60, 0x7FFF), off}));
      }
      CHECK(br.submit({make_timing_clock(), off}));
    }

    br.process_block(left, right);
  }

  // Filter emitted Note Ons for Part 0
  std::vector<EmittedNoteEvent> note_ons;
  for (const auto& ev : emitted) {
    if (ev.part == 0 && ev.on) {
      note_ons.push_back(ev);
    }
  }

  CHECK(!note_ons.empty());

  double max_err_frames = 0.0;
  double sum_err_ms = 0.0;

  for (std::size_t s = 0; s < note_ons.size(); ++s) {
    // 1/16 note = 240 ticks = 6 clock ticks
    const std::size_t clock_idx = start_tick_idx + s * 6;
    if (clock_idx >= clock_ticks.size()) break;

    const double ideal_frame = clock_ticks[clock_idx].ideal_frame;
    const double actual_frame = static_cast<double>(note_ons[s].frame);
    const double err_frames = std::abs(actual_frame - ideal_frame);
    const double err_ms = err_frames / 48.0;

    max_err_frames = std::max(max_err_frames, err_frames);
    sum_err_ms += err_ms;

    // Strict acceptance criteria: every Note On lies within +-1 ms (+-48 frames at 48 kHz) of ideal position
    if (err_ms > 1.0) {
      std::fprintf(stderr, "FAIL: %s note %zu frame %llu vs ideal %.2f (error %.2f frames = %.3f ms > 1.0 ms)\n",
                   case_name.c_str(), s, static_cast<unsigned long long>(note_ons[s].frame), ideal_frame, err_frames,
                   err_ms);
      return 1;
    }
  }

  result.name = case_name;
  result.note_count = note_ons.size();
  result.max_error_frames = max_err_frames;
  result.max_error_ms = max_err_frames / 48.0;
  result.mean_error_ms = sum_err_ms / static_cast<double>(note_ons.size());
  result.passed = (result.max_error_ms <= 1.0);

  report_out << "| " << std::left << std::setw(25) << result.name << " | "
             << std::right << std::setw(8) << result.note_count << " | "
             << std::fixed << std::setprecision(2) << std::setw(10) << result.max_error_frames << " | "
             << std::fixed << std::setprecision(3) << std::setw(8) << result.max_error_ms << " ms | "
             << std::fixed << std::setprecision(3) << std::setw(8) << result.mean_error_ms << " ms | "
             << (result.passed ? "PASS" : "FAIL") << " |\n";

  return 0;
}

}  // namespace

int main() {
  std::mt19937 prng(12345);
  // +-0.5 ms jitter = +-24 frames at 48 kHz
  std::uniform_real_distribution<double> jitter_dist(-24.0, 24.0);

  std::vector<TestResult> results;
  std::ostringstream report;

  report << "\n# Story 7.6: External MIDI Clock Follower Timing Verification Report\n\n";
  report << "| Scenario                  | Notes    | Max Error  | Max ms     | Mean ms    | Status |\n";
  report << "|:--------------------------|:---------|:-----------|:-----------|:-----------|:-------|\n";

  const std::size_t lead_in_ticks = 48;  // 2 beats lead-in to establish follower lock

  // Scenario 1: Constant 72 BPM with +-0.5 ms jitter
  {
    const double bpm = 72.0;
    const double tau = 120000.0 / bpm;
    std::vector<ClockTick> ticks;
    double ideal_f = 0.0;
    for (std::size_t i = 0; i < lead_in_ticks + 240; ++i) {  // 48 lead-in + 240 ticks (10 beats = 40 notes)
      ticks.push_back({i, ideal_f, std::max(0.0, ideal_f + jitter_dist(prng))});
      ideal_f += tau;
    }
    TestResult tr;
    if (run_follower_render_case("72 BPM (+-0.5ms jitter)", ticks, lead_in_ticks, tr, report) != 0) return 1;
    results.push_back(tr);
  }

  // Scenario 2: Constant 120 BPM with +-0.5 ms jitter
  {
    const double bpm = 120.0;
    const double tau = 120000.0 / bpm;
    std::vector<ClockTick> ticks;
    double ideal_f = 0.0;
    for (std::size_t i = 0; i < lead_in_ticks + 300; ++i) {
      ticks.push_back({i, ideal_f, std::max(0.0, ideal_f + jitter_dist(prng))});
      ideal_f += tau;
    }
    TestResult tr;
    if (run_follower_render_case("120 BPM (+-0.5ms jitter)", ticks, lead_in_ticks, tr, report) != 0) return 1;
    results.push_back(tr);
  }

  // Scenario 3: Constant 174 BPM with +-0.5 ms jitter
  {
    const double bpm = 174.0;
    const double tau = 120000.0 / bpm;
    std::vector<ClockTick> ticks;
    double ideal_f = 0.0;
    for (std::size_t i = 0; i < lead_in_ticks + 360; ++i) {
      ticks.push_back({i, ideal_f, std::max(0.0, ideal_f + jitter_dist(prng))});
      ideal_f += tau;
    }
    TestResult tr;
    if (run_follower_render_case("174 BPM (+-0.5ms jitter)", ticks, lead_in_ticks, tr, report) != 0) return 1;
    results.push_back(tr);
  }

  // Scenario 4: Tempo ramp 90 -> 130 BPM with +-0.5 ms jitter over 16 beats (384 ticks)
  {
    std::vector<ClockTick> ticks;
    const std::size_t ramp_ticks = 384;
    double ideal_f = 0.0;

    // 48 lead-in ticks at 90 BPM
    const double tau_90 = 120000.0 / 90.0;
    for (std::size_t i = 0; i < lead_in_ticks; ++i) {
      ticks.push_back({i, ideal_f, std::max(0.0, ideal_f + jitter_dist(prng))});
      ideal_f += tau_90;
    }

    // 384 ramp ticks 90 -> 130 BPM
    for (std::size_t i = 0; i < ramp_ticks; ++i) {
      const double frac = static_cast<double>(i) / static_cast<double>(ramp_ticks - 1);
      const double bpm = 90.0 + frac * (130.0 - 90.0);
      const double tau = 120000.0 / bpm;
      ticks.push_back({lead_in_ticks + i, ideal_f, std::max(0.0, ideal_f + jitter_dist(prng))});
      ideal_f += tau;
    }

    // 120 post-ramp ticks at 130 BPM
    const double tau_130 = 120000.0 / 130.0;
    for (std::size_t i = 0; i < 120; ++i) {
      ticks.push_back({lead_in_ticks + ramp_ticks + i, ideal_f, std::max(0.0, ideal_f + jitter_dist(prng))});
      ideal_f += tau_130;
    }

    TestResult tr;
    if (run_follower_render_case("90->130 Ramp (+-0.5ms)", ticks, lead_in_ticks, tr, report) != 0) return 1;
    results.push_back(tr);
  }

  report << "\nAll scenarios passed: every arpeggiated Note On onset within +-1 ms of ideal grid position.\n";

  std::cout << report.str() << std::endl;

  // Write report to _bmad-output/implementation-artifacts/clock-follower-timing-report.md
  const char* rep_paths[] = {
      "_bmad-output/implementation-artifacts/clock-follower-timing-report.md",
      "../../_bmad-output/implementation-artifacts/clock-follower-timing-report.md",
      "../../../_bmad-output/implementation-artifacts/clock-follower-timing-report.md"
  };
  for (const char* p : rep_paths) {
    std::ofstream rep_file(p);
    if (rep_file.is_open()) {
      rep_file << report.str();
      rep_file.close();
      break;
    }
  }

  std::printf("PASS clock_follower_render_test\n");
  return 0;
}
