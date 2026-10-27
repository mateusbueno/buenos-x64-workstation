#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace velora::offline {

struct WavData {
  std::uint32_t sample_rate = 48000;
  std::uint16_t channels = 2;
  std::vector<float> samples;  // interleaved
  bool from_pcm16 = false;     // decoded from a 16-bit integer PCM file (value = int16 / 32768)
  std::uint64_t frames() const { return channels ? samples.size() / channels : 0; }
};

// Written: 32-bit IEEE float PCM WAV (format tag 3). Read: that, or 16-bit integer PCM (format tag 1,
// the sfizz_render output), scaled by 1/32768. Little-endian on disk regardless of host.
bool write_wav(const std::string& path, const WavData& wav, std::string& error);
bool read_wav(const std::string& path, WavData& wav, std::string& error);
std::vector<std::uint8_t> encode_wav(const WavData& wav);
bool decode_wav(const std::vector<std::uint8_t>& bytes, WavData& wav, std::string& error);

// sfizz_render stores its float output with dr_wav's f32->s16 conversion, s = trunc((x + 1) * 32767.5) -
// 32768, which maps silence to -1 and scales by 32767.5. This restores the centre of each conversion
// step, x = (s + 1) / 32767.5, from samples read by decode_wav as s / 32768 (requires from_pcm16).
bool undo_sfizz_s16(WavData& wav, std::string& error);

// The forward model of the same conversion, applied to a float candidate: each sample goes through dr_wav's drwav_f32_to_s16
// exactly (float32 arithmetic: clamp to [-1, 1], c = x + 1, s = (int)(c * 32767.5f) - 32768) and is then decoded with the
// undo_sfizz_s16 step, x = (s + 1) / 32767.5. Quantising the candidate the way the oracle quantised its output puts both
// sides on the same 16-bit grid: where the two float renders agree to well below one LSB the integers are identical and the
// residual is zero instead of the oracle's rounding noise (about -101 dBFS per sample, which for a quiet instrument is the whole residual).
void quantize_like_sfizz_s16(WavData& wav);

}  // namespace velora::offline
