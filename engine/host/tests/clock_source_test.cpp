// Story 7.7: clock source setting and DIN / USB clock inputs (Mac logic). Synthetic MIDI clock on two ports goes through MidiIngest (port tagging, selected-port
// filter, stamp on arrival) into the BlockRenderer's follower; an arpeggiator held on part 0 is the observer (its note-on spacing is the tempo it follows).
//   rows of the I/O matrix: select DIN / USB / Internal with both ports sending, Start / Stop / Continue on the unselected port, change at runtime while notes
//   sound and the arp runs (no cut, no double trigger, the new source locks), Internal clears clock_lost, an unknown sysd value keeps the last one.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/snapshot.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/render_driver.hpp"
#include "velora/host/sysd_config_client.hpp"

using namespace velora;
using core::ClockSource;
using ports::MidiPortKind;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

namespace {

constexpr std::uint32_t kBlock = core::kBlockFrames;
constexpr std::uint32_t kDinTau = 1200;  // frames per MIDI clock tick: 100 BPM (120000 / bpm)
constexpr std::uint32_t kUsbTau = 800;   // 150 BPM
// 1/16 arpeggio = 240 ticks of 960 per beat = 1/4 beat: 100 BPM -> 7200 frames, 150 BPM -> 4800, 120 BPM (internal default) -> 6000.

struct Trace {
  std::vector<core::EmittedNoteEvent> ev;
  static void cb(void* ctx, const core::EmittedNoteEvent& e) noexcept { static_cast<Trace*>(ctx)->ev.push_back(e); }
};

struct Rig {
  std::unique_ptr<host::HostEventQueue> q = std::make_unique<host::HostEventQueue>();
  host::MidiIngest ingest{*q};
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>(kBlock);
  core::Snapshot snap = core::Snapshot::defaults();
  Trace trace;
  std::uint64_t frame = 0;  // first frame of the next block
  std::uint64_t next_din = 0, next_usb = 0;
  bool din_on = false, usb_on = false;

  Rig() {
    br->set_note_observer(&Trace::cb, &trace);
    snap.revision = 1;
    auto& a = snap.parts[0].arp;
    a.enabled = true;
    a.mode = core::ArpMode::kUp;
    a.rate = core::ArpRate::k1_16;
    a.octaves = 1;
    a.gate_percent = 50.0f;
    br->snapshots().publish(&snap);
    br->submit({core::make_note_on(0, 0, 60, 40000), 0});
    br->submit({core::make_note_on(0, 0, 64, 40000), 0});
  }

  void select(ClockSource s) {  // what the sysd callback does (host::apply_clock_source)
    ws::v1::Config c;
    c.set_clock_source(s == ClockSource::kDin ? ws::v1::CLOCK_SOURCE_DIN : s == ClockSource::kUsb ? ws::v1::CLOCK_SOURCE_USB : ws::v1::CLOCK_SOURCE_INTERNAL);
    CHECK(host::apply_clock_source(c, ingest, *br));
  }

  void send(MidiPortKind p, core::Ump u, std::uint64_t at) { ingest.on_ump_port(p, &u.word0, 1, at); }

  void block() {
    // Ticks of each running stream that fall in [frame, frame + kBlock), stamped on arrival with their frame.
    auto emit = [&](bool on, std::uint32_t tau, std::uint64_t& next, MidiPortKind p) {
      if (!on) return;
      while (next < frame + kBlock) {
        send(p, core::make_timing_clock(), next);
        next += tau;
      }
    };
    emit(din_on, kDinTau, next_din, MidiPortKind::kDin);
    emit(usb_on, kUsbTau, next_usb, MidiPortKind::kUsb);
    host::StampedUmp s;
    while (q->pop(s)) {
      const std::uint64_t off = s.host_ns > frame ? s.host_ns - frame : 0;
      br->submit({s.ump, static_cast<std::uint32_t>(off < kBlock ? off : kBlock - 1)});
    }
    float l[kBlock], r[kBlock], al[kBlock], ar[kBlock];
    core::OutputBuses ob{l, r, al, ar};
    br->process_block(ob);
    frame += kBlock;
  }
  void run(std::uint32_t blocks) {
    for (std::uint32_t i = 0; i < blocks; ++i) block();
  }
  void streams(bool din, bool usb) {
    if (din && !din_on) next_din = frame;
    if (usb && !usb_on) next_usb = frame;
    din_on = din;
    usb_on = usb;
  }
  // Frame gaps between the arpeggio note-ons that start at or after `from`.
  std::vector<std::uint64_t> onset_gaps(std::uint64_t from) const {
    std::vector<std::uint64_t> on, gaps;
    for (const auto& e : trace.ev)
      if (e.on && e.part == 0 && e.frame >= from) on.push_back(e.frame);
    for (std::size_t i = 1; i < on.size(); ++i) gaps.push_back(on[i] - on[i - 1]);
    return gaps;
  }
};

bool near(std::uint64_t v, std::uint64_t want) { return v + 40 >= want && v <= want + 40; }

void expect_gaps(const Rig& rg, std::uint64_t from, std::uint64_t want, const char* what) {
  const auto g = rg.onset_gaps(from);
  bool ok = g.size() >= 8;
  for (std::uint64_t x : g) ok = ok && near(x, want);
  if (!ok) {
    std::printf("FAIL %s: %zu gaps from frame %llu, wanted ~%llu; first gaps:", what, g.size(), static_cast<unsigned long long>(from), static_cast<unsigned long long>(want));
    for (std::size_t i = 0; i < g.size() && i < 6; ++i) std::printf(" %llu", static_cast<unsigned long long>(g[i]));
    std::printf("\n");
    ++g_fail;
  }
}

void test_select_din_both_ports_send() {
  Rig rg;
  rg.select(ClockSource::kDin);
  rg.streams(true, true);
  rg.run(3000);
  expect_gaps(rg, 120000, 7200, "DIN selected, both send");
  CHECK(rg.ingest.counters().clock_dropped.load() > 0 && rg.ingest.counters().clock_forwarded.load() > 0);
  CHECK(rg.ingest.counters().clock_dropped.load() > rg.ingest.counters().clock_forwarded.load());  // 150 BPM USB ticks dropped, 100 BPM DIN ticks forwarded
  CHECK(std::fabs(rg.br->tempo_bpm() - 100.0f) < 0.5f);
  CHECK(!rg.br->last_values().load_clock_lost());
}

void test_select_usb_clock_on_din_ignored() {
  Rig rg;
  rg.select(ClockSource::kUsb);
  rg.streams(true, false);  // only DIN sends: ignored, nothing follows it
  rg.run(600);
  CHECK(rg.ingest.counters().clock_forwarded.load() == 0 && rg.ingest.counters().clock_dropped.load() > 0);
  CHECK(!rg.br->clock_follower().locked());
  rg.streams(true, true);
  rg.run(3000);
  expect_gaps(rg, 600ull * kBlock + 120000, 4800, "USB selected, both send");
  CHECK(std::fabs(rg.br->tempo_bpm() - 150.0f) < 0.5f);
}

void test_internal_ignores_external_clock() {
  Rig rg;
  rg.select(ClockSource::kInternal);
  rg.streams(true, true);
  rg.run(1500);
  CHECK(rg.ingest.counters().clock_forwarded.load() == 0);
  CHECK(rg.ingest.counters().clock_dropped.load() > 0);
  CHECK(!rg.br->clock_follower().locked() && !rg.br->clock_follower().clock_lost());
  CHECK(!rg.br->last_values().load_clock_lost());
  expect_gaps(rg, 24000, 6000, "Internal (120 BPM default)");
}

void test_transport_messages_gated_by_port() {
  Rig rg;
  rg.select(ClockSource::kDin);
  rg.block();  // the source change (and the follower reset it asks for) is applied
  auto& f = rg.br->clock_follower();
  CHECK(!f.is_running());
  // Start and Continue on the unselected port are dropped: the follower does not start.
  rg.send(MidiPortKind::kUsb, core::make_start(), rg.frame + 1);
  rg.send(MidiPortKind::kUsb, core::make_continue(), rg.frame + 2);
  CHECK(rg.ingest.counters().clock_forwarded.load() == 0 && rg.ingest.counters().clock_dropped.load() == 2);
  rg.block();
  CHECK(!f.is_running());
  // A Start on the selected port does start it.
  rg.send(MidiPortKind::kDin, core::make_start(), rg.frame + 1);
  rg.block();
  CHECK(rg.ingest.counters().clock_forwarded.load() == 1 && f.is_running());
  // A Stop on the unselected port does not stop it; a Stop on the selected port does.
  rg.send(MidiPortKind::kUsb, core::make_stop(), rg.frame + 1);
  rg.block();
  CHECK(f.is_running());
  rg.send(MidiPortKind::kDin, core::make_stop(), rg.frame + 1);
  rg.block();
  CHECK(!f.is_running());
  // Continue on the selected port runs it again; Continue / Start on the other port after that change nothing.
  rg.send(MidiPortKind::kDin, core::make_continue(), rg.frame + 1);
  rg.block();
  CHECK(f.is_running());
  // Clock in a group other than 0 is dropped and counted (a multi-cable device would double the tick rate); group 0 passes.
  const std::uint64_t fwd = rg.ingest.counters().clock_forwarded.load();
  const core::Ump g3 = core::make_stop(3);
  rg.ingest.on_ump_port(MidiPortKind::kDin, &g3.word0, 1, rg.frame + 1);
  CHECK(rg.ingest.counters().clock_dropped_group.load() == 1 && rg.ingest.counters().clock_forwarded.load() == fwd);
  rg.block();
  CHECK(f.is_running());
  // Notes of both ports pass whatever the source is.
  const core::Ump n = core::make_note_on(0, 1, 70, 30000);
  const std::uint64_t before = rg.ingest.counters().notes.load();
  rg.ingest.on_ump_port(MidiPortKind::kDin, &n.word0, 2, rg.frame);
  rg.ingest.on_ump_port(MidiPortKind::kUsb, &n.word0, 2, rg.frame);
  CHECK(rg.ingest.counters().notes.load() == before + 2);
}

void test_clock_does_not_take_note_headroom() {
  Rig rg;
  rg.select(ClockSource::kUsb);
  while (rg.q->size() + host::kNoteHeadroom < host::HostEventQueue::capacity()) CHECK(rg.q->push(host::StampedUmp{core::make_note_on(0, 1, 70, 30000), 0}));
  rg.send(MidiPortKind::kUsb, core::make_timing_clock(), 1);
  CHECK(rg.ingest.counters().clock_dropped_full.load() == 1 && rg.ingest.counters().clock_forwarded.load() == 0);
  rg.send(MidiPortKind::kUsb, core::make_start(), 2);  // transport messages always get in
  rg.send(MidiPortKind::kUsb, core::make_stop(), 3);
  CHECK(rg.ingest.counters().clock_forwarded.load() == 2 && rg.ingest.counters().clock_dropped_full.load() == 1);
}

// Through the RenderDriver and the time mapper (no hand submit): stale clock of the old port is dropped, the selected port's clock locks the follower.
void test_render_driver_drops_stale_port_clock() {
  Rig rg;
  host::RenderDriver drv(*rg.br, *rg.q);
  const std::uint64_t base = 20'000'000'000ull;
  const std::uint64_t delay = drv.mapper().config().delay_ns;
  float l[kBlock], r[kBlock];
  std::uint64_t blk = 0;
  auto start_ns = [&] { return base + host::frames_to_ns(blk * kBlock, 48000); };
  auto host_ns_of_frame = [&](std::uint64_t f) { return base + host::frames_to_ns(f, 48000) - delay; };
  auto render = [&] {
    drv.render_block(l, r, start_ns());
    ++blk;
  };
  rg.select(ClockSource::kUsb);
  render();
  // Old-port (USB) clock sits in the queue when the source switches to DIN: ticks, Start and Stop just before the switch ...
  for (int i = 0; i < 5; ++i) rg.send(MidiPortKind::kUsb, core::make_timing_clock(), host_ns_of_frame(blk * kBlock + 10 + i));
  rg.send(MidiPortKind::kUsb, core::make_start(), host_ns_of_frame(blk * kBlock + 20));
  rg.send(MidiPortKind::kUsb, core::make_stop(), host_ns_of_frame(blk * kBlock + 21));
  // ... and one far in the future: the driver holds it (beyond the horizon) in lane.held.
  rg.send(MidiPortKind::kUsb, core::make_timing_clock(), host_ns_of_frame(blk * kBlock + 100000));
  rg.send(MidiPortKind::kUsb, core::make_timing_clock(), host_ns_of_frame(blk * kBlock + 100100));
  rg.select(ClockSource::kDin);  // the switch happens with all of those still in the queue
  render();
  CHECK(drv.counters().clock_stale.load() == 7 + 2);  // 5 ticks + Start + Stop, then the held far-future tick and the one behind it
  CHECK(!rg.br->clock_follower().is_running() && !rg.br->clock_follower().locked());
  // A held event across a switch: DIN selected, hold a DIN tick far in the future, switch to USB: dropped when the lane is drained again.
  rg.send(MidiPortKind::kDin, core::make_timing_clock(), host_ns_of_frame(blk * kBlock + 100000));
  render();
  CHECK(drv.counters().beyond_horizon.load() >= 1);
  const std::uint64_t stale = drv.counters().clock_stale.load();
  rg.select(ClockSource::kUsb);
  render();
  CHECK(drv.counters().clock_stale.load() == stale + 1);
  // Now DIN for real: the driver feeds the follower (100 BPM, 1200 frames per tick) and the tempo locks.
  rg.select(ClockSource::kDin);
  std::uint64_t next = blk * kBlock;
  for (int b = 0; b < 3000; ++b) {
    while (next < (blk + 1) * kBlock) {
      rg.send(MidiPortKind::kDin, core::make_timing_clock(), host_ns_of_frame(next));
      rg.send(MidiPortKind::kUsb, core::make_timing_clock(), host_ns_of_frame(next + 7));  // the other port keeps sending: never reaches the follower
      next += kDinTau;
    }
    render();
  }
  CHECK(std::fabs(rg.br->tempo_bpm() - 100.0f) < 1.0f);
  CHECK(rg.br->clock_follower().locked() && !rg.br->last_values().load_clock_lost());
}

// Rapid, repeated source changes (a sysd that flaps): each one is applied, the follower is reset per change on the RT thread, the last one wins and locks.
void test_rapid_switches() {
  Rig rg;
  const ClockSource seq[] = {ClockSource::kDin, ClockSource::kUsb, ClockSource::kInternal, ClockSource::kUsb, ClockSource::kDin, ClockSource::kDin};
  rg.streams(true, true);
  for (int i = 0; i < 600; ++i) {
    const ClockSource s = seq[i % 6];
    rg.br->set_clock_source(s);  // straight on the renderer, also repeating a value (a repeated external source still resets once)
    rg.ingest.set_clock_source(s);
    CHECK(rg.br->clock_source() == s);
    if (i % 3 == 0) rg.block();  // sometimes several changes land in one block
  }
  rg.select(ClockSource::kDin);
  rg.run(3000);
  CHECK(rg.br->clock_source() == ClockSource::kDin && std::fabs(rg.br->tempo_bpm() - 100.0f) < 0.5f);
  CHECK(rg.br->clock_follower().locked() && !rg.br->last_values().load_clock_lost());
}

void test_runtime_switch_no_cut_no_double_trigger() {
  Rig rg;
  rg.select(ClockSource::kUsb);
  rg.streams(true, true);
  rg.run(2000);
  expect_gaps(rg, 60000, 4800, "before the switch");
  const std::uint64_t switch_frame = rg.frame;
  const std::size_t ons_before = rg.trace.ev.size();
  rg.select(ClockSource::kDin);  // USB -> DIN while the arp runs and the keys are held
  rg.run(3000);
  // The new source locks: 100 BPM spacing after the settling window.
  expect_gaps(rg, switch_frame + 100000, 7200, "after the switch to DIN");
  // No glitch: every gap across the switch is a plausible step (no double trigger, no long hole), every note-on has its note-off, no stuck note.
  std::uint64_t prev_on = 0;
  bool have_prev = false;
  std::size_t ons = 0, offs = 0;
  for (std::size_t i = 0; i < rg.trace.ev.size(); ++i) {
    const auto& e = rg.trace.ev[i];
    if (!e.on) {
      ++offs;
      continue;
    }
    ++ons;
    if (have_prev && e.frame >= switch_frame - 20000) {
      CHECK(e.frame - prev_on >= 1500);   // never two triggers for one step
      CHECK(e.frame - prev_on <= 20000);  // the arp never stops
    }
    prev_on = e.frame;
    have_prev = true;
  }
  CHECK(rg.trace.ev.size() > ons_before);
  CHECK(offs + 1 >= ons);
  CHECK(!rg.br->last_values().load_clock_lost());
}

void test_clock_lost_cleared_by_internal() {
  Rig rg;
  rg.select(ClockSource::kDin);
  rg.streams(true, false);
  rg.run(400);
  rg.streams(false, false);  // the clock stops without a Stop
  rg.run(400);
  CHECK(rg.br->last_values().load_clock_lost());
  rg.select(ClockSource::kInternal);
  rg.run(2);
  CHECK(!rg.br->last_values().load_clock_lost());
  // And back to DIN: re-locks from a clean follower.
  rg.select(ClockSource::kDin);
  rg.streams(true, false);
  rg.run(900);
  CHECK(!rg.br->last_values().load_clock_lost() && std::fabs(rg.br->tempo_bpm() - 100.0f) < 0.5f);
}

void test_sysd_value_mapping() {
  core::ClockSource cs = ClockSource::kUsb;
  CHECK(host::clock_source_from_config(ws::v1::CLOCK_SOURCE_INTERNAL, cs) && cs == ClockSource::kInternal);
  CHECK(host::clock_source_from_config(ws::v1::CLOCK_SOURCE_DIN, cs) && cs == ClockSource::kDin);
  CHECK(host::clock_source_from_config(ws::v1::CLOCK_SOURCE_USB, cs) && cs == ClockSource::kUsb);
  cs = ClockSource::kDin;
  CHECK(!host::clock_source_from_config(ws::v1::CLOCK_SOURCE_UNSPECIFIED, cs) && cs == ClockSource::kDin);
  CHECK(!host::clock_source_from_config(static_cast<ws::v1::ClockSource>(99), cs) && cs == ClockSource::kDin);
  // Defaults: Internal until sysd answers; a bad value keeps the last applied one.
  Rig rg;
  CHECK(rg.ingest.clock_source() == ClockSource::kInternal && rg.br->clock_source() == ClockSource::kInternal);
  CHECK(host::default_config().clock_source() == ws::v1::CLOCK_SOURCE_INTERNAL);
  rg.select(ClockSource::kUsb);
  ws::v1::Config bad;
  bad.set_clock_source(static_cast<ws::v1::ClockSource>(77));
  CHECK(!host::apply_clock_source(bad, rg.ingest, *rg.br));
  CHECK(rg.ingest.clock_source() == ClockSource::kUsb && rg.br->clock_source() == ClockSource::kUsb);
}

}  // namespace

int main() {
  test_select_din_both_ports_send();
  test_select_usb_clock_on_din_ignored();
  test_internal_ignores_external_clock();
  test_transport_messages_gated_by_port();
  test_clock_does_not_take_note_headroom();
  test_render_driver_drops_stale_port_clock();
  test_rapid_switches();
  test_runtime_switch_no_cut_no_double_trigger();
  test_clock_lost_cleared_by_internal();
  test_sysd_value_mapping();
  if (g_fail == 0) std::printf("clock_source_tests: ok\n");
  return g_fail ? 1 : 0;
}
