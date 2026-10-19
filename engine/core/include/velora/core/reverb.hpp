#pragma once
// Reverb send (Story 2.10): the processor behind the Reverb send bus of the Mixer. Clean-room, public-fact design (no reverb source
// consulted): a feedback delay network (FDN; Stautner & Puckette 1982, Jot & Chaigne 1991) with a Householder feedback matrix,
// per-line one-pole damping filters whose gain follows Jot's rule (the loop gain of a delay line of d samples for a decay time T60 is
// 10^(-3 d / (T60 fs))), preceded by a pre-delay and a chain of Schroeder all-pass diffusers (Schroeder 1962). The README
// ("Reverb send") documents the structure, the three parameter sets, the level normalisation and the measurement method.
//
//   send sum (stereo) --> mono (L+R)/2 --> DC blocker --> pre-delay --> 4 all-pass diffusers --> FDN (8 lines, Householder matrix,
//   one-pole damping per line, slowly modulated read positions) --> two decorrelated taps (L, R) --> x return level --> wet (stereo)
//
// Types: Room, Hall, Plate are three parameter sets of the same engine (delay-line lengths, pre-delay, diffuser lengths and gain,
// damping, modulation). `time` sets RT60 in seconds (0.2 .. 10) at 1 kHz: the damping gain correction is computed at 1 kHz, so
// the measured RT60 there follows the parameter; above it the damping shortens the decay, below it the decay is the same.
//
// Type change: two engines run during a short cross-fade (kTypeFadeFrames, linear weights summing to 1); a change that arrives
// mid-fade waits (the latest wins). Time and level changes apply at once to every running engine (the gains sit inside the loop,
// so the state, and with it the output, stays continuous); the level ramps linearly over one block.
//
// Idle: the work stops when nothing can be heard. quiet() is true when the state was cleared (the tail decayed below
// kFlushLevel for kFlushBlocks blocks after kFlushAfterFrames frames without input, or the return level is 0) and no input has
// arrived since. A quiet reverb with silent input costs a scan of the input block, nothing else; the Mixer treats quiet() as
// transparent for its trivial-routing detection. Fixed storage (the engines are members), nothing allocates in process().
#include <array>
#include <cstdint>

#include "velora/core/fx.hpp"
#include "velora/core/rt.hpp"

namespace velora::core {

enum class ReverbType : std::uint8_t { kRoom = 0, kHall = 1, kPlate = 2 };
constexpr std::uint32_t kReverbTypeCount = 3;

namespace reverbmap {
constexpr float kMinTimeS = 0.2f, kMaxTimeS = 10.0f, kDefaultTimeS = 2.0f;
// Registry values to engine values: NaN -> the registry default (type hall, time 2 s, level 1), out of range -> clamped; the type is the nearest enum index.
ReverbType type(float v) noexcept;
float time_s(float v) noexcept;
float level(float v) noexcept;
}  // namespace reverbmap

class ReverbSend final : public Effect {
 public:
  static constexpr std::uint32_t kLines = 8;
  static constexpr std::uint32_t kLineSize = 4096;       // per line (power of two) > the longest delay + modulation
  static constexpr std::uint32_t kPreDelaySize = 1024;   // (power of two) > the longest pre-delay
  static constexpr std::uint32_t kDiffusers = 4;
  static constexpr std::uint32_t kDiffuserSize = 512;    // > the longest all-pass delay
  static constexpr std::uint32_t kTypeFadeFrames = 2400; // 50 ms at 48 kHz
  static constexpr std::uint32_t kLevelRampFrames = 64;  // one block
  static constexpr std::uint32_t kFlushAfterFrames = 8192;  // without input: longer than pre-delay + diffusers + the longest line
  static constexpr std::uint32_t kFlushBlocks = 8;          // consecutive quiet blocks before the state is cleared
  static constexpr float kFlushLevel = 1e-8f;               // -160 dBFS

  ReverbSend() noexcept;

  EffectType type() const noexcept override { return EffectType::kNone; }  // not an insert type: the send bus has no registry insert_type
  std::uint32_t cost_units() const noexcept override { return kReverbSendCost; }
  void reset() noexcept VELORA_NONBLOCKING override;

  // Reverb settings (registry reverb_type / reverb_time / reverb_level, already clamped through reverbmap). RT or control thread.
  // `instant`: the type applies at once without a cross-fade and the level without a ramp (first generation).
  void configure(ReverbType type, float time_s, float level, bool instant) noexcept VELORA_NONBLOCKING;

  // In place: the block holds the send sum (stereo) on entry and the wet signal (stereo, already scaled by the return level) on exit.
  void process(StereoBlock& block) noexcept VELORA_NONBLOCKING override;

  bool quiet() const noexcept { return quiet_; }
  // Blocks in which the engine actually ran (tests: an idle reverb does no work).
  std::uint64_t engine_blocks() const noexcept { return engine_blocks_; }
  bool fading() const noexcept { return fade_left_ > 0 || has_pending_; }
  ReverbType current_type() const noexcept { return engines_[cur_].type; }
  float time_s() const noexcept { return time_; }
  float level() const noexcept { return lvl_tgt_; }

  // One FDN engine. Public so that tests can inspect the parameter sets; not used outside the reverb.
  struct Engine {
    ReverbType type = ReverbType::kHall;
    alignas(16) std::array<std::array<float, kLineSize>, kLines> line{};
    std::array<float, kPreDelaySize> pre{};
    std::array<std::array<float, kDiffuserSize>, kDiffusers> ap{};
    std::array<std::uint32_t, kDiffusers> ap_pos{};
    std::array<float, kLines> damp_state{}, lfo_phase{}, gain{}, out_gain{}, lfo_inc{};
    std::uint32_t w = 0, pre_w = 0;
    float dc_x1 = 0.0f, dc_y1 = 0.0f;
    float damping = 0.0f, ap_gain = 0.5f, mod_depth = 0.0f;
    std::uint32_t pre_delay = 0;
    std::array<std::uint32_t, kLines> delay{};
    std::array<std::uint32_t, kDiffusers> ap_delay{};

    void clear() noexcept VELORA_NONBLOCKING;
    void configure(ReverbType t, float time_s) noexcept VELORA_NONBLOCKING;  // type (resets nothing but the parameters) and RT60
    void set_time(float time_s) noexcept VELORA_NONBLOCKING;
    // n <= 64 mono input samples -> n stereo wet samples (unit-power normalised, before the return level).
    void run(const float* in, float* out_l, float* out_r, std::uint32_t n) noexcept VELORA_NONBLOCKING;
  };

 private:
  void process_slice(StereoBlock& block) noexcept VELORA_NONBLOCKING;  // <= kFxBlockFrames
  void start_fade(ReverbType t) noexcept VELORA_NONBLOCKING;
  void clear_all() noexcept VELORA_NONBLOCKING;

  std::array<Engine, 2> engines_{};
  std::uint32_t cur_ = 0;          // the engine that is heard (the incoming one is 1 - cur_ while a fade runs)
  std::uint32_t fade_left_ = 0;
  bool has_pending_ = false;
  ReverbType pending_ = ReverbType::kHall;
  float time_ = reverbmap::kDefaultTimeS;
  float lvl_cur_ = 0.0f, lvl_tgt_ = 0.0f, lvl_step_ = 0.0f;
  std::uint32_t lvl_left_ = 0;
  bool quiet_ = true;
  std::uint32_t silent_frames_ = 0, quiet_blocks_ = 0;
  std::uint64_t engine_blocks_ = 0;
};

}  // namespace velora::core
