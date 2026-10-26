// Story 1.18: the offline renderer renders the same events with the parallel voice render off, with 1 execution thread (the three fixed partitions
// inline) and with 3 worker threads. 1 vs 3 must be bit-identical (AC), and both must equal the serial render within a float tolerance (only the
// summation order differs). 280 sample voices across 8 channels with note offs (releases) in the middle. Correctness only, no timing.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "render.hpp"
#include "velora/ports/sample_source.hpp"

namespace off = velora::offline;
namespace ports = velora::ports;

static int g_fail = 0;
#define CHECK(c)                                                \
  do {                                                          \
    if (!(c)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
      ++g_fail;                                                 \
    }                                                           \
  } while (0)

namespace {

struct Instrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<std::vector<float>> data;
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }
};

bool render(const Instrument& ins, std::uint32_t threads, off::WavData& out) {
  std::vector<off::NoteEvent> ev;
  std::uint32_t k = 0;
  for (std::uint32_t i = 0; i < 280; ++i) {
    off::NoteEvent e;
    e.on = true;
    e.channel = static_cast<std::uint8_t>(i % 8);
    e.note = static_cast<std::uint8_t>(30 + (i * 7) % 60);
    e.velocity16 = static_cast<std::uint16_t>(8000 + (i * 977) % 50000);
    e.frame = 64ull * (1 + i / 4) + (i % 4) * 11;
    ev.push_back(e);
    ++k;
  }
  for (std::uint32_t i = 0; i < 280; i += 2) {  // release half of them later
    off::NoteEvent e;
    e.on = false;
    e.channel = static_cast<std::uint8_t>(i % 8);
    e.note = static_cast<std::uint8_t>(30 + (i * 7) % 60);
    e.frame = 64ull * 120 + i * 13;
    ev.push_back(e);
  }
  off::RenderOptions opt;
  opt.engine = off::EngineKind::kSample;
  opt.instrument = &ins;
  opt.voices = 320;
  opt.parallel_voice_threads = threads;
  opt.tail_seconds = 0.5;
  std::string error;
  const bool ok = off::render_events(ev, opt, out, error);
  if (!ok) std::printf("render failed: %s\n", error.c_str());
  return ok;
}

}  // namespace

int main() {
  Instrument ins;
  {
    std::vector<float> buf(20000);
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = 0.02f * std::sin(static_cast<float>(i) * 0.07f) + 0.01f * std::sin(static_cast<float>(i) * 0.31f);
    ins.data.push_back(std::move(buf));
    ports::SamplePcm p;
    p.sample_rate = 48000;
    p.channels = 1;
    p.frames = 20000;
    p.left = ins.data.back().data();
    ins.pcm.push_back(p);
    ports::SampleRegionDesc d;
    d.key_lo = 0;
    d.key_hi = 127;
    d.key_center = 60;
    d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    d.has_loop_points = true;
    d.loop_start = 2000;
    d.loop_end = 18000;
    d.ampeg_release = 0.05f;
    ins.regions.push_back(d);
  }
  off::WavData serial, p1, p3, p3b;
  CHECK(render(ins, 0, serial));
  CHECK(render(ins, 1, p1));
  CHECK(render(ins, 3, p3));
  CHECK(render(ins, 3, p3b));  // repeatable
  CHECK(!serial.samples.empty() && serial.samples.size() == p1.samples.size() && p1.samples.size() == p3.samples.size());
  CHECK(p1.samples.size() == p3.samples.size() && std::memcmp(p1.samples.data(), p3.samples.data(), p1.samples.size() * sizeof(float)) == 0);
  CHECK(p3.samples.size() == p3b.samples.size() && std::memcmp(p3.samples.data(), p3b.samples.data(), p3.samples.size() * sizeof(float)) == 0);
  double diff = 0.0, energy = 0.0;
  for (std::size_t i = 0; i < serial.samples.size() && i < p3.samples.size(); ++i) {
    diff = std::max(diff, std::fabs(static_cast<double>(serial.samples[i]) - p3.samples[i]));
    energy += static_cast<double>(p3.samples[i]) * p3.samples[i];
  }
  std::printf("parallel_voices_test: frames=%llu energy=%.3g serial-vs-parallel max |diff|=%.3g\n", (unsigned long long)p3.frames(), energy, diff);
  CHECK(energy > 1.0);
  CHECK(diff <= 1e-5);
  std::printf(g_fail ? "parallel_voices_test: FAILED (%d)\n" : "parallel_voices_test: ok\n", g_fail);
  return g_fail ? 1 : 0;
}
