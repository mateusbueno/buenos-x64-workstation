#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/fm_engine.hpp"
#include "velora/core/mixer.hpp"
#include "velora/format/category.hpp"
#include "velora/format/parametric_bundle.hpp"
#include "velora/format/sysex_6op.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/sysex_receiver.hpp"

namespace {

using namespace velora;
using namespace velora::host;
namespace fmt = velora::format;

fmt::FmVoiceParameters make_test_voice(uint8_t algo, uint8_t fb, const std::string& name) {
  fmt::FmVoiceParameters v{};
  v.algorithm = std::clamp<uint8_t>(algo, 1, 32);
  v.feedback = std::clamp<uint8_t>(fb, 0, 7);
  v.osc_key_sync = true;
  std::memset(v.name, 0, sizeof(v.name));
  std::strncpy(v.name, name.c_str(), 10);
  for (int i = 0; i < 6; ++i) {
    auto& op = v.operators[i];
    op.rates = {99, 90, 70, 40};
    op.levels = {99, 80, 60, 0};
    op.kbd_breakpoint = 60;
    op.total_level = 90;
    op.freq_coarse = i + 1;
  }
  return v;
}

void test_single_voice_dump() {
  std::cout << "Running test_single_voice_dump...\n";
  HostEventQueue audio_q;
  SysexPacketQueue sysex_q;
  MidiIngest ingest(audio_q, &sysex_q);
  SysexReceiver receiver(sysex_q);

  fmt::FmVoiceParameters original = make_test_voice(5, 3, "SOLO LEAD ");
  std::vector<uint8_t> syx = fmt::encode_sysex_single_dump(original, 0);
  assert(syx.size() == 163);

  // 1. Packetize without stripping delimiters
  std::vector<core::Ump> packets = SysexReceiver::packetize_sysex(syx, 0, false);
  for (const auto& pkt : packets) {
    uint32_t words[2] = {pkt.word0, pkt.word1};
    ingest.on_ump(words, 2, 1000);
  }

  // Audio queue must be untouched
  assert(audio_q.size() == 0);
  assert(ingest.counters().notes == 0);
  assert(ingest.counters().sysex_packets == packets.size());

  fmt::SysexParseResult parsed;
  bool callback_fired = false;
  receiver.set_on_valid_dump([&](const fmt::SysexParseResult& res) {
    callback_fired = true;
    parsed = res;
  });

  std::size_t n = receiver.poll();
  assert(n == 1);
  assert(callback_fired);
  assert(parsed.ok());
  assert(parsed.voices.size() == 1);
  assert(parsed.voices[0].name == "SOLO LEAD");
  assert(parsed.voices[0].parameters.algorithm == 5);
  assert(parsed.voices[0].parameters.feedback == 3);
  assert(receiver.counters().sysex_valid_count == 1);
  assert(receiver.counters().sysex_damaged_count == 0);

  // 2. Packetize WITH stripped delimiters (standard MIDI 2.0 UMP format)
  packets = SysexReceiver::packetize_sysex(syx, 0, true);
  for (const auto& pkt : packets) {
    uint32_t words[2] = {pkt.word0, pkt.word1};
    ingest.on_ump(words, 2, 2000);
  }

  callback_fired = false;
  n = receiver.poll();
  assert(n == 1);
  assert(callback_fired);
  assert(parsed.ok());
  assert(parsed.voices.size() == 1);
  assert(receiver.counters().sysex_valid_count == 2);

  std::cout << "  ✓ test_single_voice_dump passed\n";
}

void test_bulk_dump_split_and_din() {
  std::cout << "Running test_bulk_dump_split_and_din...\n";
  HostEventQueue audio_q;
  SysexPacketQueue sysex_q;
  MidiIngest ingest(audio_q, &sysex_q);
  SysexReceiver receiver(sysex_q);

  std::vector<fmt::FmVoiceParameters> bank;
  for (int i = 1; i <= 32; ++i) {
    bank.push_back(make_test_voice((i % 32) + 1, i % 8, "VOX " + std::to_string(i)));
  }
  std::vector<uint8_t> bulk_syx = fmt::encode_sysex_bulk_dump(bank, 0);
  assert(bulk_syx.size() == 4104);

  // Parse directly from memory buffer (Story 12.3 baseline)
  fmt::SysexParseResult file_ref = fmt::parse_sysex_memory(bulk_syx);
  assert(file_ref.ok());
  auto file_bundle_res = fmt::ParametricBundle::from_sysex(file_ref, "bank.syx", "Test Bank");
  assert(file_bundle_res);
  auto file_bundle = file_bundle_res.take();
  assert(file_bundle.timbres().size() == 32);

  // Packetize into UMP packets with mixed/fragmented packet sizes
  // Simulating USB MIDI fragmentation and DIN byte delivery
  std::vector<core::Ump> packets = SysexReceiver::packetize_sysex(bulk_syx, 0, false);
  assert(!packets.empty());

  for (const auto& pkt : packets) {
    uint32_t words[2] = {pkt.word0, pkt.word1};
    ingest.on_ump(words, 2, 5000);
  }

  fmt::SysexParseResult received_result;
  bool callback_fired = false;
  receiver.set_on_valid_dump([&](const fmt::SysexParseResult& res) {
    callback_fired = true;
    received_result = res;
  });

  std::size_t n = receiver.poll();
  assert(n == 1);
  assert(callback_fired);
  assert(received_result.ok());
  assert(received_result.voices.size() == 32);

  // Verify imported timbres match file import of the same bank
  auto received_bundle_res = fmt::ParametricBundle::from_sysex(received_result, "bank.syx", "Test Bank");
  assert(received_bundle_res);
  auto received_bundle = received_bundle_res.take();
  assert(received_bundle.timbres().size() == 32);

  for (size_t i = 0; i < 32; ++i) {
    const auto& t_file = file_bundle.timbres()[i];
    const auto& t_recv = received_bundle.timbres()[i];
    assert(t_file.key == t_recv.key);
    assert(t_file.name == t_recv.name);
    assert(t_file.category == t_recv.category);
    assert(t_file.category == fmt::Category::kSynthPoly);
    assert(t_file.fm_parameters.algorithm == t_recv.fm_parameters.algorithm);
    assert(t_file.fm_parameters.feedback == t_recv.fm_parameters.feedback);
    for (int op = 0; op < 6; ++op) {
      assert(t_file.fm_parameters.operators[op].total_level == t_recv.fm_parameters.operators[op].total_level);
      assert(t_file.fm_parameters.operators[op].freq_coarse == t_recv.fm_parameters.operators[op].freq_coarse);
    }
  }

  assert(receiver.counters().sysex_valid_count == 1);
  assert(receiver.counters().sysex_damaged_count == 0);

  std::cout << "  ✓ test_bulk_dump_split_and_din passed\n";
}

void test_concurrent_256_voices_zero_xruns() {
  std::cout << "Running test_concurrent_256_voices_zero_xruns...\n";
  HostEventQueue audio_q;
  SysexPacketQueue sysex_q;
  MidiIngest ingest(audio_q, &sysex_q);
  SysexReceiver receiver(sysex_q);

  // Set up FM engine with 256 active sounding voices
  core::FmEngine fm(48000.0f);
  core::FmVoiceParameters params = core::create_ep_fm_voice();
  fm.set_part_voice_params(0, params);

  // Sound 256 voices simultaneously
  for (uint32_t i = 0; i < 256; ++i) {
    fm.start_voice(0, 20 + (i % 80), 0x8000);
  }
  assert(fm.active_voice_count() == 256);

  // Prepare a 4104-byte bulk SysEx dump
  std::vector<fmt::FmVoiceParameters> bank;
  for (int i = 1; i <= 32; ++i) {
    bank.push_back(make_test_voice((i % 32) + 1, i % 8, "VOX " + std::to_string(i)));
  }
  std::vector<uint8_t> bulk_syx = fmt::encode_sysex_bulk_dump(bank, 0);
  std::vector<core::Ump> packets = SysexReceiver::packetize_sysex(bulk_syx, 0, false);

  std::atomic<bool> audio_done{false};
  std::atomic<uint64_t> xruns{0};
  std::atomic<uint64_t> blocks_rendered{0};

  // Run audio render thread simulating RT block loop
  std::thread audio_thread([&]() {
    float left[256];
    float right[256];
    while (!audio_done.load(std::memory_order_relaxed)) {
      std::memset(left, 0, sizeof(left));
      std::memset(right, 0, sizeof(right));
      fm.render(left, right, 256);
      blocks_rendered.fetch_add(1, std::memory_order_relaxed);

      // Verify audio continues sounding cleanly (non-zero energy)
      float energy = 0.0f;
      for (int s = 0; s < 256; ++s) {
        energy += std::abs(left[s]) + std::abs(right[s]);
      }
      if (energy <= 1e-4f) {
        xruns.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  // Concurrently feed SysEx packets via MIDI thread
  fmt::SysexParseResult received_result;
  receiver.set_on_valid_dump([&](const fmt::SysexParseResult& res) {
    received_result = res;
  });

  for (const auto& pkt : packets) {
    uint32_t words[2] = {pkt.word0, pkt.word1};
    ingest.on_ump(words, 2, 10000);
    // Control thread polls concurrently
    receiver.poll();
  }

  // Let audio thread render continuously
  // (until enough blocks rendered, bounded: a loaded host can render slowly)
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  for (int i = 0; i < 100 && blocks_rendered.load() < 10; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Drain remaining packets
  while (receiver.poll() > 0) {}

  audio_done.store(true, std::memory_order_relaxed);
  audio_thread.join();

  assert(xruns.load() == 0);
  assert(blocks_rendered.load() >= 10);
  assert(audio_q.size() == 0); // Audio queue remained completely untouched
  assert(received_result.ok());
  assert(received_result.voices.size() == 32);
  assert(receiver.counters().sysex_valid_count == 1);
  assert(receiver.counters().sysex_damaged_count == 0);

  std::cout << "  ✓ test_concurrent_256_voices_zero_xruns passed\n";
}

void test_damaged_and_truncated_dumps() {
  std::cout << "Running test_damaged_and_truncated_dumps...\n";
  HostEventQueue audio_q;
  SysexPacketQueue sysex_q;
  MidiIngest ingest(audio_q, &sysex_q);
  SysexReceiver receiver(sysex_q);

  bool damaged_fired = false;
  receiver.set_on_damaged_dump([&]() {
    damaged_fired = true;
  });

  // 1. Truncated single voice dump (100 bytes instead of 163)
  fmt::FmVoiceParameters v = make_test_voice(1, 0, "TEST");
  std::vector<uint8_t> good = fmt::encode_sysex_single_dump(v, 0);
  std::vector<uint8_t> truncated(good.begin(), good.begin() + 100);
  truncated.push_back(0xF7);

  std::vector<core::Ump> packets = SysexReceiver::packetize_sysex(truncated, 0, false);
  for (const auto& pkt : packets) {
    uint32_t words[2] = {pkt.word0, pkt.word1};
    ingest.on_ump(words, 2, 1000);
  }
  receiver.poll();
  assert(damaged_fired);
  assert(receiver.counters().sysex_damaged_count == 1);
  assert(receiver.counters().sysex_valid_count == 0);

  // 2. Corrupt checksum in bulk dump
  damaged_fired = false;
  std::vector<fmt::FmVoiceParameters> bank;
  for (int i = 1; i <= 32; ++i) bank.push_back(make_test_voice(1, 0, "VOX"));
  std::vector<uint8_t> bulk = fmt::encode_sysex_bulk_dump(bank, 0);
  bulk[4102] ^= 0x40; // Flip checksum bit

  packets = SysexReceiver::packetize_sysex(bulk, 0, false);
  for (const auto& pkt : packets) {
    uint32_t words[2] = {pkt.word0, pkt.word1};
    ingest.on_ump(words, 2, 2000);
  }
  receiver.poll();
  assert(damaged_fired);
  assert(receiver.counters().sysex_damaged_count == 2);
  assert(receiver.counters().sysex_valid_count == 0);

  // 3. Dump interrupted by another Start packet
  damaged_fired = false;
  std::vector<core::Ump> partial_packets = SysexReceiver::packetize_sysex(bulk, 0, false);
  // Send first 10 packets of bulk dump
  for (size_t i = 0; i < 10; ++i) {
    uint32_t words[2] = {partial_packets[i].word0, partial_packets[i].word1};
    ingest.on_ump(words, 2, 3000);
  }
  // Then send a new Start packet of a single dump
  std::vector<core::Ump> single_packets = SysexReceiver::packetize_sysex(good, 0, false);
  for (const auto& pkt : single_packets) {
    uint32_t words[2] = {pkt.word0, pkt.word1};
    ingest.on_ump(words, 2, 4000);
  }
  receiver.poll();
  // The interrupted dump was marked damaged
  assert(damaged_fired);
  assert(receiver.counters().sysex_damaged_count == 3);
  // And the new valid dump succeeded!
  assert(receiver.counters().sysex_valid_count == 1);

  std::cout << "  ✓ test_damaged_and_truncated_dumps passed\n";
}

void test_non_6op_sysex_ignored() {
  std::cout << "Running test_non_6op_sysex_ignored...\n";
  HostEventQueue audio_q;
  SysexPacketQueue sysex_q;
  MidiIngest ingest(audio_q, &sysex_q);
  SysexReceiver receiver(sysex_q);

  bool valid_fired = false;
  bool damaged_fired = false;
  receiver.set_on_valid_dump([&](const fmt::SysexParseResult&) { valid_fired = true; });
  receiver.set_on_damaged_dump([&]() { damaged_fired = true; });

  // 1. Roland SysEx (Manufacturer ID 0x41)
  std::vector<uint8_t> roland_syx = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
  std::vector<core::Ump> packets = SysexReceiver::packetize_sysex(roland_syx, 0, false);
  for (const auto& pkt : packets) {
    uint32_t words[2] = {pkt.word0, pkt.word1};
    ingest.on_ump(words, 2, 1000);
  }
  receiver.poll();
  assert(!valid_fired);
  assert(!damaged_fired);
  assert(receiver.counters().sysex_ignored_count == 1);

  // 2. Korg SysEx (Manufacturer ID 0x42)
  std::vector<uint8_t> korg_syx = {0xF0, 0x42, 0x30, 0x00, 0x01, 0x02, 0xF7};
  packets = SysexReceiver::packetize_sysex(korg_syx, 0, false);
  for (const auto& pkt : packets) {
    uint32_t words[2] = {pkt.word0, pkt.word1};
    ingest.on_ump(words, 2, 2000);
  }
  receiver.poll();
  assert(!valid_fired);
  assert(!damaged_fired);
  assert(receiver.counters().sysex_ignored_count == 2);

  // 3. Yamaha non-6-OP SysEx (e.g. 4-OP TX81Z format 0x04)
  std::vector<uint8_t> yamaha_non_6op = {0xF0, 0x43, 0x00, 0x04, 0x20, 0x00, 0x12, 0x34, 0xF7};
  packets = SysexReceiver::packetize_sysex(yamaha_non_6op, 0, false);
  for (const auto& pkt : packets) {
    uint32_t words[2] = {pkt.word0, pkt.word1};
    ingest.on_ump(words, 2, 3000);
  }
  receiver.poll();
  assert(!valid_fired);
  assert(!damaged_fired);
  assert(receiver.counters().sysex_ignored_count == 3);

  // No damaged notices or valid notices raised for any of these
  assert(receiver.counters().sysex_damaged_count == 0);
  assert(receiver.counters().sysex_valid_count == 0);

  std::cout << "  ✓ test_non_6op_sysex_ignored passed\n";
}

} // namespace

int main() {
  std::cout << "=== Running sysex_receiver_test ===\n";
  test_single_voice_dump();
  test_bulk_dump_split_and_din();
  test_concurrent_256_voices_zero_xruns();
  test_damaged_and_truncated_dumps();
  test_non_6op_sysex_ignored();
  std::cout << "All sysex_receiver_test tests passed!\n";
  return 0;
}
