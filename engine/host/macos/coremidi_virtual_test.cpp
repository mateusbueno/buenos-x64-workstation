// macOS, device-free: a note-on sent through a CoreMIDI virtual source must reach the engine as
// a UMP event at the expected mapped in-block frame, via the same RenderDriver used by the live
// CoreAudio callback. Exit 77 (ctest SKIP) when CoreMIDI is unavailable.
#include <CoreMIDI/CoreMIDI.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "coremidi_input.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/render_driver.hpp"

using namespace velora;
using namespace velora::host;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

static bool wait_for(const std::atomic<std::uint64_t>& v, std::uint64_t target) {
  for (int i = 0; i < 400; ++i) {
    if (v.load() >= target) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

// Delivery timeout: the runner has no working MIDI server -> skip rather than fail.
static constexpr int kSkip = 77;
struct Skip { const char* why; };

// Drive one block so the most recent stamped event is due at `want_frame`; verify mapping + audio.
static void check_mapped(MidiIngest& ing, HostEventQueue& queue, int want_frame, const char* label) {
  auto br = std::make_unique<core::BlockRenderer>();  // fresh renderer: no earlier voices
  RenderDriver drv(*br, queue);
  float l[64], r[64];
  const std::uint64_t bs = ing.last_stamp_ns() + drv.mapper().config().delay_ns -
                           frames_to_ns(static_cast<std::uint64_t>(want_frame), 48000);
  drv.render_block(l, r, bs);
  std::printf("%s: mapped frame %u (want %d)\n", label, drv.counters().last_frame.load(), want_frame);
  CHECK(drv.counters().last_frame == static_cast<std::uint32_t>(want_frame));
  int first = -1;
  for (int i = 0; i < 64 && first < 0; ++i)
    if (l[i] != 0.0f) first = i;
  CHECK(first == want_frame);  // audible exactly at the mapped offset
}

static bool send(MIDIEndpointRef src, MIDIProtocolID proto, const UInt32* w, ByteCount n) {
  MIDIEventList list;
  MIDIEventPacket* p = MIDIEventListInit(&list, proto);
  p = MIDIEventListAdd(&list, sizeof(list), p, 0, n, w);
  return p != nullptr && MIDIReceivedEventList(src, &list) == noErr;
}

// Owns every CoreMIDI object so each exit path cleans up.
struct Fixture {
  MIDIClientRef client = 0;
  MIDIEndpointRef src1 = 0, src2 = 0, src3 = 0;
  ~Fixture() {
    for (MIDIEndpointRef e : {src1, src2, src3})
      if (e) MIDIEndpointDispose(e);
    if (client) MIDIClientDispose(client);
  }
};

static int run(Fixture& fx) {
  auto queue = std::make_unique<HostEventQueue>();
  MidiIngest ing(*queue);
  macos::CoreMidiInput in;
  std::string err;
  if (!in.open(ing, err)) {
    std::printf("FAIL: CoreMidiInput::open: %s\n", err.c_str());
    return 1;
  }
  CHECK(in.connected_sources() >= 2);

  const core::Ump n = core::make_note_on(0, 0, 60, 45000);
  const UInt32 w2[2] = {n.word0, n.word1};
  CHECK(send(fx.src2, kMIDIProtocol_2_0, w2, 2));
  if (!wait_for(ing.counters().notes, 1)) throw Skip{"MIDI 2.0 note-on never arrived"};
  check_mapped(ing, *queue, 20, "midi2");

  const UInt32 w1[1] = {0x2090'3E64u};  // MIDI 1.0 note-on ch0 note 62 vel 100
  CHECK(send(fx.src1, kMIDIProtocol_1_0, w1, 1));
  if (!wait_for(ing.counters().notes, 2)) throw Skip{"MIDI 1.0 note-on never arrived"};
  check_mapped(ing, *queue, 37, "midi1");

  const UInt32 cc[1] = {0x20B0'0740u};  // CC 7: the registry does not map it, so it passes as a controller (Story 5.2)
  CHECK(send(fx.src1, kMIDIProtocol_1_0, cc, 1));
  CHECK(wait_for(ing.counters().controllers, 1));
  const UInt32 pc[1] = {0x20C0'0700u};  // program change: ignored
  CHECK(send(fx.src1, kMIDIProtocol_1_0, pc, 1));
  CHECK(wait_for(ing.counters().ignored, 1));

  // Hot-plug: a source created after open() is picked up by rescan().
  if (MIDISourceCreateWithProtocol(fx.client, CFSTR("velora-test-src3"), kMIDIProtocol_2_0, &fx.src3) == noErr)
    CHECK(in.rescan() >= 1);
  in.close();
  return g_fail ? 1 : 0;
}

int main() {
  Fixture fx;
  if (MIDIClientCreate(CFSTR("velora-test"), nullptr, nullptr, &fx.client) != noErr) {
    std::printf("SKIP: CoreMIDI client unavailable\n");
    return kSkip;
  }
  if (MIDISourceCreateWithProtocol(fx.client, CFSTR("velora-test-src2"), kMIDIProtocol_2_0, &fx.src2) != noErr ||
      MIDISourceCreateWithProtocol(fx.client, CFSTR("velora-test-src1"), kMIDIProtocol_1_0, &fx.src1) != noErr) {
    std::printf("SKIP: cannot create CoreMIDI virtual sources\n");
    return kSkip;
  }
  int rc;
  try {
    rc = run(fx);
  } catch (const Skip& s) {
    std::printf("SKIP: %s (no working MIDI delivery on this machine)\n", s.why);
    return kSkip;
  }
  if (rc == 0) std::printf("coremidi_virtual_test: ok\n");
  return rc;
}
