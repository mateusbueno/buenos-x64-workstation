// Story 8.8: the real-time side publishes how many physical keys are held and whether the sustain pedal is down (LastValueTable::load_held_notes /
// load_pedal_down), once per block with relaxed atomic stores. The control thread reads them for the Boot-into pointer's idle-safe policy.
#include <cstdint>
#include <cstdio>
#include <memory>

#include "velora/core/block_renderer.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                               \
    }                                                         \
  } while (0)

namespace {
struct Rig {
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>(64);
  void send(const Ump& u) { CHECK(br->submit(TimedUmp{u, 0})); }
  void run() {
    float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
    OutputBuses ob{l, r, al, ar};
    br->process_block(ob);
  }
  std::uint32_t notes() const { return br->last_values().load_held_notes(); }
  bool pedal() const { return br->last_values().load_pedal_down(); }
};
}  // namespace

int main() {
  Rig r;
  r.run();
  CHECK(r.notes() == 0 && !r.pedal());
  r.send(make_note_on(0, 0, 60, 30000));
  r.send(make_note_on(0, 0, 64, 30000));
  r.send(make_note_on(0, 1, 60, 30000));  // same key number on another channel is another key
  r.run();
  CHECK(r.notes() == 3);
  r.send(make_note_on(0, 0, 60, 30000));  // a re-strike of a held key does not count twice
  r.run();
  CHECK(r.notes() == 3);
  r.send(make_note_off(0, 0, 60));
  r.send(make_note_off(0, 0, 99));  // never held: ignored
  r.run();
  CHECK(r.notes() == 2);
  r.send(make_control_change(0, 0, 64, 0xFFFFFFFFu));
  r.run();
  CHECK(r.pedal() && r.notes() == 2);
  r.send(make_note_off(0, 0, 64));
  r.send(make_note_off(0, 1, 60));
  r.run();
  CHECK(r.notes() == 0 && r.pedal());  // pedal alone: no key held, pedal down
  r.send(make_control_change(0, 0, 64, 0));
  r.run();
  CHECK(!r.pedal());
  // velocity 0 note on is a note off
  r.send(make_note_on(0, 2, 70, 30000));
  r.run();
  CHECK(r.notes() == 1);
  r.send(make_note_on(0, 2, 70, 0));
  r.run();
  CHECK(r.notes() == 0);
  // CC 123 / CC 120 clear that channel's keys only
  for (std::uint32_t n = 0; n < 5; ++n) r.send(make_note_on(0, 3, 50 + n, 30000));
  r.send(make_note_on(0, 4, 50, 30000));
  r.run();
  CHECK(r.notes() == 6);
  r.send(make_control_change(0, 3, 123, 0));
  r.run();
  CHECK(r.notes() == 1);
  for (std::uint32_t n = 0; n < 3; ++n) r.send(make_note_on(0, 5, 10 + n, 30000));
  r.run();
  CHECK(r.notes() == 4);
  r.send(make_control_change(0, 5, 120, 0));
  r.send(make_control_change(0, 4, 120, 0));
  r.run();
  CHECK(r.notes() == 0);
  r.send(make_note_off(0, 4, 50));  // late note off after a panic: no underflow
  r.run();
  CHECK(r.notes() == 0);
  // pedal boundary
  r.send(make_control_change(0, 0, 64, 0x7FFFFFFFu));
  r.run();
  CHECK(!r.pedal());
  r.send(make_control_change(0, 0, 64, 0x80000000u));
  r.run();
  CHECK(r.pedal());
  if (g_fail) return 1;
  std::printf("input_activity_tests: all passed\n");
  return 0;
}
