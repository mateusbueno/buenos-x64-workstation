// Test-only reference oracle renderer for 6-operator FM synthesis (Story 12.2).
// Drives MSFA (Apache-2.0, unshipped test oracle) through its native SysEx parameter format
// and renders offline 48 kHz stereo WAV to null-test against Velora's FmEngine.
//
// Usage: fm_oracle_render <file.syx> <voice_slot> <file.mid> <out.wav>
//   voice_slot: 1..32 (for bulk dumps; for single voice dumps 1 is used)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "synth.h"
#include "controllers.h"
#include "dx7note.h"
#include "freqlut.h"
#include "exp2.h"
#include "sin.h"
#include "lfo.h"
#include "pitchenv.h"
#include "patch.h"

#include "smf.hpp"
#include "wav.hpp"

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::size_t kBlockFrames = 64;
constexpr double kScaleFactor = 1.0 / 134217728.0; // 1 / 2^27: aligns MSFA Q24/Q25 fixed-point to float [-1.0, 1.0]

struct VoiceSlot {
  int midi_note = -1;
  bool keydown = false;
  bool active = false;
  uint32_t silent_blocks = 0;
  std::unique_ptr<Dx7Note> note;
};

bool load_patch(const std::string& path, uint32_t slot, char unpacked_out[156], std::string& error) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) {
    error = "cannot open SysEx file: " + path;
    return false;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (data.empty()) {
    error = "empty SysEx file";
    return false;
  }

  // Check format: bulk dump (4104 bytes), single voice dump (163 bytes), or raw 128 / 155 bytes
  if (data.size() >= 4104 && data[0] == 0xF0 && data[1] == 0x43 && data[3] == 0x09 && data[4] == 0x20) {
    // 32-voice bulk dump
    if (slot < 1 || slot > 32) {
      error = "slot must be 1..32 for bulk dump";
      return false;
    }
    const size_t offset = 6 + (slot - 1) * 128;
    UnpackPatch(reinterpret_cast<const char*>(data.data() + offset), unpacked_out);
    return true;
  } else if (data.size() >= 163 && data[0] == 0xF0 && data[1] == 0x43 && data[3] == 0x00 && data[4] == 0x01) {
    // 1-voice single dump (155 unpacked bytes at offset 6)
    std::memcpy(unpacked_out, data.data() + 6, 155);
    unpacked_out[155] = 0;
    return true;
  } else if (data.size() == 128) {
    // Raw packed voice
    UnpackPatch(reinterpret_cast<const char*>(data.data()), unpacked_out);
    return true;
  } else if (data.size() >= 155) {
    // Raw unpacked voice
    std::memcpy(unpacked_out, data.data(), 155);
    unpacked_out[155] = 0;
    return true;
  }

  error = "unrecognized SysEx format in " + path;
  return false;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr, "usage: fm_oracle_render <file.syx> <voice_slot (1..32)> <file.mid> <out.wav>\n");
    return 2;
  }

  const std::string syx_path = argv[1];
  const uint32_t slot = static_cast<uint32_t>(std::atoi(argv[2]));
  const std::string mid_path = argv[3];
  const std::string out_path = argv[4];

  std::string err;
  char unpacked_patch[156] = {0};
  if (!load_patch(syx_path, slot, unpacked_patch, err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }

  // Load MIDI file
  auto smf = velora::offline::load_smf_file(mid_path);
  if (!smf.ok) {
    std::fprintf(stderr, "error loading %s: %s\n", mid_path.c_str(), smf.error.c_str());
    return 1;
  }

  // Init MSFA synth core
  Freqlut::init(static_cast<double>(kSampleRate));
  Exp2::init();
  Sin::init();
  Lfo::init(static_cast<double>(kSampleRate));
  PitchEnv::init(static_cast<double>(kSampleRate));

  Controllers ctrls;
  ctrls.values_[kControllerPitch] = 0x2000; // center pitch bend

  // Allocate voice pool
  constexpr size_t kPoolSize = 32;
  std::vector<VoiceSlot> pool(kPoolSize);
  for (auto& v : pool) {
    v.note = std::make_unique<Dx7Note>();
  }

  // Calculate total frames to render (last event frame + 1.0s tail to match velora-offline-render default)
  uint64_t total_frames = 0;
  for (const auto& e : smf.events) {
    if (e.frame > total_frames) total_frames = e.frame;
  }
  total_frames += static_cast<uint64_t>(kSampleRate * 1.0); // 1.0s tail
  const uint64_t blocks = (total_frames + kBlockFrames - 1) / kBlockFrames;

  velora::offline::WavData out_wav;
  out_wav.sample_rate = kSampleRate;
  out_wav.channels = 2;
  out_wav.samples.assign(static_cast<size_t>(blocks * kBlockFrames * 2), 0.0f);

  size_t event_idx = 0;
  std::vector<int32_t> block_buf(kBlockFrames);

  for (uint64_t b = 0; b < blocks; ++b) {
    const uint64_t block_start = b * kBlockFrames;
    const uint64_t block_end = block_start + kBlockFrames;

    // Dispatch MIDI events in this block
    while (event_idx < smf.events.size() && smf.events[event_idx].frame < block_end) {
      const auto& ev = smf.events[event_idx++];
      if (ev.on && ev.velocity16 > 0) {
        int vel7 = std::clamp<int>(ev.velocity16 >> 9, 1, 127);
        // Find inactive voice or steal oldest releasing
        int victim = -1;
        for (size_t i = 0; i < kPoolSize; ++i) {
          if (!pool[i].active) {
            victim = static_cast<int>(i);
            break;
          }
        }
        if (victim < 0) {
          for (size_t i = 0; i < kPoolSize; ++i) {
            if (!pool[i].keydown) {
              victim = static_cast<int>(i);
              break;
            }
          }
        }
        if (victim < 0) victim = 0;

        pool[victim].midi_note = ev.note;
        pool[victim].keydown = true;
        pool[victim].active = true;
        pool[victim].silent_blocks = 0;
        pool[victim].note->init(unpacked_patch, ev.note, vel7);
      } else if (!ev.on || ev.velocity16 == 0) {
        for (auto& v : pool) {
          if (v.active && v.keydown && v.midi_note == ev.note) {
            v.note->keyup();
            v.keydown = false;
          }
        }
      }
    }

    // Render block from all active voices
    std::fill(block_buf.begin(), block_buf.end(), 0);
    for (auto& v : pool) {
      if (v.active) {
        int32_t voice_buf[kBlockFrames] = {0};
        // Story 12.2 AC: with LFO off
        v.note->compute(voice_buf, 0, 0, &ctrls);

        int64_t energy = 0;
        for (size_t i = 0; i < kBlockFrames; ++i) {
          block_buf[i] += voice_buf[i];
          energy += std::abs(voice_buf[i]);
        }

        if (!v.keydown) {
          if (energy == 0) {
            ++v.silent_blocks;
            if (v.silent_blocks >= 4) { // silent for 4 blocks -> inactive
              v.active = false;
            }
          } else {
            v.silent_blocks = 0;
          }
        }
      }
    }

    // Accumulate into output stereo WAV
    float* dst = &out_wav.samples[b * kBlockFrames * 2];
    for (size_t i = 0; i < kBlockFrames; ++i) {
      float s = static_cast<float>(static_cast<double>(block_buf[i]) * kScaleFactor);
      dst[2 * i] = s;
      dst[2 * i + 1] = s;
    }
  }

  if (!velora::offline::write_wav(out_path, out_wav, err)) {
    std::fprintf(stderr, "error writing %s: %s\n", out_path.c_str(), err.c_str());
    return 1;
  }

  return 0;
}
