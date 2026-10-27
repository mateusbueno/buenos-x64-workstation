#include "velora/core/sample_engine.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace velora::core {

namespace {

// --- constants shared with the sfizz reference (see engine/core/README.md for the sources) -----
constexpr float kReleaseThreshold = 1e-4f;        // exponential release hands over to the fade-out below this
constexpr float kTransitionTime = 50e-3f;         // linear fade-out time after the exponential release
constexpr float kFastReleaseTime = 6e-3f;         // minimum exponential time and the "off" time at sample end
constexpr float kDefaultRelease = 0.001f;         // ampeg_release when the region does not carry one (<= 0)
constexpr float kFreeRunningSustain = 0.0032f;    // sustain at or below this releases by itself
constexpr float kVirtuallyZero = 0.001f;
constexpr int kPanSize = 4095;
constexpr float kMonoPanCompensation = 1.4125375446227544f;    // +3 dB, one pan stage
constexpr float kStereoPanCompensation = 1.9952623149688797f;  // +6 dB, pan and position stages

constexpr unsigned kXfadeSmoothingMs = 5;  // sfizz Config::xfadeSmoothing
constexpr float kMaxRatio = 1024.0f, kMinRatio = 1.0f / 4096.0f;
constexpr float kMaxSeconds = 100.0f;

bool finite_time(float t) noexcept { return std::isfinite(t) && t >= 0.0f && t <= kMaxSeconds; }

// Story 5.2 fields: keyswitch notes, crossfade ranges and the filter must be well formed (a region that is not is unplayable).
bool extension_sane(const velora::ports::SampleRegionDesc& d) noexcept {
  auto note_ok = [](std::int16_t k) { return k >= -1 && k <= 127; };
  if (!note_ok(d.sw.lokey) || !note_ok(d.sw.hikey) || !note_ok(d.sw.last) || !note_ok(d.sw.down) || !note_ok(d.sw.up) ||
      !note_ok(d.sw.previous) || !note_ok(d.sw.default_key))
    return false;
  if (d.sw.lokey >= 0 && d.sw.hikey >= 0 && d.sw.lokey > d.sw.hikey) return false;  // an inverted keyswitch range
  if (d.xfin_count > velora::ports::kSampleMaxCcCrossfades || d.xfout_count > velora::ports::kSampleMaxCcCrossfades) return false;
  for (unsigned i = 0; i < d.xfin_count; ++i)
    if (d.xfin[i].cc > 127 || d.xfin[i].lo > d.xfin[i].hi || d.xfin[i].hi > 127) return false;
  for (unsigned i = 0; i < d.xfout_count; ++i)
    if (d.xfout[i].cc > 127 || d.xfout[i].lo > d.xfout[i].hi || d.xfout[i].hi > 127) return false;
  if (static_cast<unsigned>(d.filter_type) > static_cast<unsigned>(velora::ports::SampleFilterType::kBpf2p)) return false;
  if (d.filter_type != velora::ports::SampleFilterType::kNone &&
      !(std::isfinite(d.filter_cutoff_hz) && d.filter_cutoff_hz >= 0.0f && std::isfinite(d.filter_resonance_db)))
    return false;
  return true;
}

// Everything the audio thread later turns into counts, rates and gains must be finite and bounded.
bool region_params_sane(const velora::ports::SampleRegionDesc& d) noexcept {
  return extension_sane(d) && finite_time(d.ampeg_delay) && finite_time(d.ampeg_attack) && finite_time(d.ampeg_hold) &&
         finite_time(d.ampeg_decay) && finite_time(d.ampeg_release) && std::isfinite(d.ampeg_start) &&
         std::isfinite(d.ampeg_sustain) && std::isfinite(d.lorand) && std::isfinite(d.hirand) &&
         d.lorand < d.hirand && d.seq_length >= 1 && d.seq_position >= 1 && d.seq_position <= d.seq_length &&
         std::isfinite(d.tune_cents) && std::isfinite(d.volume_db) && d.volume_db >= -144.0f &&
         d.volume_db <= 48.0f && std::isfinite(d.pan);
}

constexpr double kHalfPi = 1.57079632679489661923;

// sfizz maps a SFZ time to samples/rates in single precision; so do we.
std::int32_t seconds_to_samples(float t, float sr) noexcept {
  return t <= 0.0f ? 0 : static_cast<std::int32_t>(t * sr);
}
float seconds_to_lin_rate(float t, float sr) noexcept { return t <= 0.0f ? 1.0f : 1.0f / (sr * t); }
float seconds_to_exp_rate(float t, float sr) noexcept {
  if (t <= 0.0f) return 0.0f;
  t = std::max(kFastReleaseTime, t);
  return std::exp(-9.0f / (t * sr));
}

float pan_lookup(const std::array<float, 4096>& table, float p) noexcept {
  const long idx = static_cast<long>(std::lround(p * static_cast<float>(kPanSize - 1)));
  return table[static_cast<std::size_t>(std::clamp(idx, 0L, static_cast<long>(kPanSize)))];
}

inline void hermite_weights(float c, float w[4]) noexcept {
  for (int i = -1; i < 3; ++i) {
    const float x = std::fabs(static_cast<float>(i) - c);
    const float x2 = x * x, x3 = x2 * x;
    const float q = 2.5f * x2;
    const float p1 = 1.0f - q + 1.5f * x3;
    const float p2 = 2.0f - 4.0f * x + q - 0.5f * x3;
    float y = 0.0f;
    y = (x < 2.0f) ? p2 : y;
    y = (x < 1.0f) ? p1 : y;
    w[i + 1] = y;
  }
}

}  // namespace

SampleEngine::SampleEngine(const Config& cfg) : cfg_(cfg) {
  if (cfg_.max_voices == 0) cfg_.max_voices = 1;
  if (cfg_.sample_rate == 0) cfg_.sample_rate = 48000;
  capacity_ = cfg_.max_voices;
  voices_.resize(capacity_);
  for (int i = 0; i < kPanSize; ++i)
    pan_table_[static_cast<std::size_t>(i)] = static_cast<float>(std::cos(i * (kHalfPi / (kPanSize - 1))));
  for (std::size_t i = kPanSize; i < pan_table_.size(); ++i) pan_table_[i] = pan_table_[kPanSize - 1];
  tr_release_rate_off_ = seconds_to_exp_rate(kFastReleaseTime, static_cast<float>(cfg_.sample_rate));
  set_seed(cfg_.seed);
  // CcState defaults are sfizz's (MidiState / Synth::resetDefaultCCValues): CC7 volume 100, CC10 pan centre, CC11 expression 127.
  for (auto& part : cc_) {
    part.fill(0.0f);
    part[7] = 100.0f;
    part[10] = 63.5f;
    part[11] = 127.0f;
  }
  // The CC crossfade smoother is sfizz's `Smoother` (= LinearSmoother) with xfadeSmoothing = 5 ms: int(5e-3 * fs) frames.
  xf_frames_ = static_cast<std::int32_t>(1e-3f * static_cast<float>(kXfadeSmoothingMs) * static_cast<float>(cfg_.sample_rate));
}

void SampleEngine::set_seed(std::uint64_t seed) noexcept {
  // splitmix64 scramble so that nearby seeds give unrelated streams; never zero for xorshift.
  std::uint64_t z = seed + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  rng_ = z ? z : 0x2545F4914F6CDD1Dull;
}

float SampleEngine::next_random() noexcept {
  // xorshift64*; the top 24 bits give a float uniformly distributed in [0, 1).
  std::uint64_t x = rng_;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rng_ = x;
  const std::uint64_t r = x * 0x2545F4914F6CDD1Dull;
  return static_cast<float>(r >> 40) * (1.0f / 16777216.0f);
}

std::shared_ptr<SampleBank> SampleEngine::prepare(const velora::ports::SampleSource& src, std::shared_ptr<const void> owner) const {
  static std::atomic<std::uint64_t> next_bank_id{1};
  auto bank = std::make_shared<SampleBank>();
  bank->id = next_bank_id.fetch_add(1, std::memory_order_relaxed);
  bank->owner = std::move(owner);
  std::vector<SampleBankRegion>& regions = bank->regions;
  const velora::ports::InstrumentView view = src.instrument();
  const float sr = static_cast<float>(cfg_.sample_rate);
  regions.reserve(view.regions.size());
  for (const auto& d : view.regions) {
    RegionRt r;
    r.d = d;
    if (d.sample_index < view.samples.size()) {
      const velora::ports::SamplePcm& pcm = view.samples[d.sample_index];
      r.frames = pcm.frames;
      r.left = pcm.left;
      r.right = pcm.right;
      r.has_tail = pcm.total_frames > pcm.frames;
      r.total_frames = r.has_tail ? pcm.total_frames : pcm.frames;
      if (d.has_end && d.end > 0)  // Story 13.3: exclusive end, clamped to the stored length of the sample
        r.end_cut = static_cast<std::int64_t>(std::min<std::uint64_t>(d.end, r.total_frames));
      if (r.has_tail) {  // Story 1.17: the tail's reader and the resident loop segments travel with the region (borrowed; the bank's owner keeps them alive)
        r.tail_source = pcm.tail_source;
        r.tail_sample = pcm.tail_sample;
        r.segs = pcm.segments;
        r.seg_count = pcm.segment_count;
      }
      r.damage = pcm.damaged_blocks;
      r.damage_block_frames = pcm.damaged_block_frames;
      r.damage_block_count = pcm.damaged_block_count;
      if (r.damage != nullptr && (r.damage_block_frames == 0 || r.damage_block_count == 0)) r.damage = nullptr;  // a map that cannot be indexed is no map
      if (r.damage != nullptr && (r.damage_block_frames & (r.damage_block_frames - 1)) == 0) {
        r.damage_shift = 0;
        while ((std::uint32_t{1} << r.damage_shift) < r.damage_block_frames) ++r.damage_shift;
      }
      r.stereo = pcm.channels == 2;
      r.speed = pcm.sample_rate == 0 ? 0.0f
                                     : static_cast<float>(static_cast<double>(pcm.sample_rate) /
                                                          static_cast<double>(cfg_.sample_rate));
      r.playable = (pcm.channels == 1 || pcm.channels == 2) && pcm.frames > 0 && pcm.left != nullptr &&
                   (pcm.channels == 1 || pcm.right != nullptr) && pcm.sample_rate != 0 &&
                   d.offset < pcm.frames && (r.end_cut < 0 || static_cast<std::int64_t>(d.offset) < r.end_cut) && d.key_lo <= d.key_hi && d.vel_lo <= d.vel_hi &&
                   region_params_sane(d);
      if (r.playable) {
        // Pitch ratio at the extreme keys of the range must be finite and sane.
        const float spd = r.speed;
        const float base = d.tune_cents + 100.0f * static_cast<float>(d.transpose);
        const float up = 100.0f * static_cast<float>(static_cast<int>(d.key_hi) - static_cast<int>(d.key_center)) + base;
        const float dn = 100.0f * static_cast<float>(static_cast<int>(d.key_lo) - static_cast<int>(d.key_center)) + base;
        const float rmax = std::pow(2.0f, up / 1200.0f) * spd, rmin = std::pow(2.0f, dn / 1200.0f) * spd;
        r.playable = std::isfinite(rmax) && std::isfinite(rmin) && rmax <= kMaxRatio && rmin >= kMinRatio;
      }
    }
    r.disabled = d.has_end && d.end == 0;  // sfizz: end=0 switches the region off
    if (!r.playable && !r.disabled) ++bank->unplayable_regions;

    r.uses_sequence = d.seq_length != 1 || d.seq_position != 1;

    // Loops: only the two looping modes, with the range inside the sample (otherwise no loop, as sfizz).
    if (d.loop_mode == velora::ports::SampleLoopMode::kLoopContinuous ||
        d.loop_mode == velora::ports::SampleLoopMode::kLoopSustain) {
      std::uint64_t loop_frames = r.has_tail ? r.total_frames : r.frames;  // a loop may lie beyond the head; the voice then underruns at the head end
      if (r.end_cut >= 0) loop_frames = std::min<std::uint64_t>(loop_frames, static_cast<std::uint64_t>(r.end_cut));  // a loop must lie inside the played part
      std::int64_t ls = 0, le = static_cast<std::int64_t>(loop_frames) - 1;
      if (d.has_loop_points) {
        ls = static_cast<std::int64_t>(std::min<std::uint64_t>(d.loop_start, INT64_MAX));
        le = static_cast<std::int64_t>(std::min<std::uint64_t>(d.loop_end, INT64_MAX));
      }
      if (loop_frames > 0 && ls <= le && le < static_cast<std::int64_t>(loop_frames)) {
        r.loops = true;
        r.loop_start = ls;
        r.loop_end = le;
        r.loop_size = le + 1 - ls;
        if (r.has_tail) {
          // Where the stream re-seeks on a wrap: the first frame after the resident run (head, then the segments) that starts at the frame before the loop start (the left tap).
          std::int64_t c = std::max<std::int64_t>(ls - 1, 0);
          for (int it = 0; it < 8; ++it) {
            bool moved = false;
            if (c < static_cast<std::int64_t>(r.frames)) {
              c = static_cast<std::int64_t>(r.frames);
              moved = true;
            }
            for (std::uint32_t k = 0; k < r.seg_count; ++k) {
              const auto& sg = r.segs[k];
              if (sg.left != nullptr && c >= static_cast<std::int64_t>(sg.start) && c < static_cast<std::int64_t>(sg.start + sg.frames)) {
                c = static_cast<std::int64_t>(sg.start + sg.frames);
                moved = true;
              }
            }
            if (!moved) break;
          }
          r.loop_seek = c;
        }
      }
    }

    r.base_cents = d.tune_cents + 100.0f * static_cast<float>(d.transpose);
    r.vol_gain = std::pow(10.0f, d.volume_db * 0.05f);
    const float p = std::clamp((d.pan / 100.0f + 1.0f) * 0.5f, 0.0f, 1.0f);
    const float pl = pan_lookup(pan_table_, p), pr = pan_lookup(pan_table_, 1.0f - p);
    if (r.stereo) {
      const float pos = pan_lookup(pan_table_, 0.5f);  // position stage at 0
      r.chan_l = pl * pos * kStereoPanCompensation;
      r.chan_r = pr * pos * kStereoPanCompensation;
    } else {
      r.chan_l = pl * kMonoPanCompensation;
      r.chan_r = pr * kMonoPanCompensation;
    }

    r.env_delay = seconds_to_samples(d.ampeg_delay, sr);
    r.env_hold = seconds_to_samples(d.ampeg_hold, sr);
    r.env_start = std::clamp(d.ampeg_start / 100.0f, 0.0f, 1.0f);
    r.env_attack_step = seconds_to_lin_rate(d.ampeg_attack, sr);
    r.env_decay_rate = seconds_to_exp_rate(d.ampeg_decay, sr);
    r.env_sustain = std::clamp(d.ampeg_sustain / 100.0f, 0.0f, 1.0f);
    r.env_release_rate = seconds_to_exp_rate(d.ampeg_release > 0.0f ? d.ampeg_release : kDefaultRelease, sr);
    r.env_free_running = r.env_sustain <= kFreeRunningSustain;

    // Story 5.2: keyswitch gating, CC crossfades, filter.
    r.has_keyswitch = d.sw.last >= 0 || d.sw.down >= 0 || d.sw.up >= 0 || d.sw.previous >= 0;
    r.has_xfade = d.xfin_count != 0 || d.xfout_count != 0;
    if (d.filter_type != velora::ports::SampleFilterType::kNone && extension_sane(d)) {
      r.filter = make_sample_filter(d.filter_type, d.filter_cutoff_hz, d.filter_resonance_db, static_cast<double>(cfg_.sample_rate));
      r.filtered = r.filter.kind != SampleFilterCoeffs::Kind::kNone;
    }
    if (extension_sane(d)) {
      if (d.sw.last >= 0) bank->switch_key[static_cast<std::size_t>(d.sw.last)] = 1;
      if (d.sw.lokey >= 0 || d.sw.hikey >= 0) {  // the keyswitch range: a missing end is the end of the keyboard (SFZ defaults 0 / 127)
        const int lo = d.sw.lokey >= 0 ? d.sw.lokey : 0, hi = d.sw.hikey >= 0 ? d.sw.hikey : 127;
        for (int k = lo; k <= hi; ++k) bank->switch_key[static_cast<std::size_t>(k)] = 1;
        bank->uses_keyswitches = true;
      }
      if (d.sw.default_key >= 0) bank->default_switch = d.sw.default_key;
    }
    bank->uses_keyswitches = bank->uses_keyswitches || r.has_keyswitch;
    bank->uses_filters = bank->uses_filters || r.filtered;
    bank->uses_crossfades = bank->uses_crossfades || r.has_xfade;
    regions.push_back(r);
  }

  // Per-note lookup, regions in table order (a note's regions are visited in that order).
  std::array<std::uint32_t, 128> counts{};
  for (const auto& r : regions) {
    if (r.disabled) continue;
    for (unsigned n = r.d.key_lo; n <= std::min<unsigned>(r.d.key_hi, 127); ++n) ++counts[n];
  }
  bank->note_begin[0] = 0;
  for (unsigned n = 0; n < 128; ++n) bank->note_begin[n + 1] = bank->note_begin[n] + counts[n];
  bank->note_list.assign(bank->note_begin[128], 0);
  std::array<std::uint32_t, 128> fill{};
  for (std::uint32_t i = 0; i < regions.size(); ++i) {
    if (regions[i].disabled) continue;
    for (unsigned n = regions[i].d.key_lo; n <= std::min<unsigned>(regions[i].d.key_hi, 127); ++n)
      bank->note_list[bank->note_begin[n] + fill[n]++] = i;
  }

  bank->seq_counter.assign(regions.size(), 0);
  bank->seq_switched.assign(regions.size(), 0);
  for (std::size_t i = 0; i < regions.size(); ++i) bank->seq_switched[i] = regions[i].uses_sequence ? 0 : 1;
  return bank;
}

bool SampleEngine::set_instrument(const velora::ports::SampleSource& src) {
  clear_instrument();
  default_bank_ = prepare(src);
  unplayable_regions_ = default_bank_->unplayable_regions;
  for (std::uint32_t ch = 0; ch < 16; ++ch) reset_part_keys(ch);
  return true;
}

void SampleEngine::clear_instrument() {
  for (auto& v : voices_) {
    release_stream(v);
    v = Voice{};
  }
  default_bank_.reset();
  last_vel7_.fill(0);
  last_bank_.fill(0);
  unplayable_regions_ = 0;
  skipped_unplayable_ = 0;
  undeclared_extended_ = 0;
  for (std::uint32_t ch = 0; ch < 16; ++ch) reset_part_keys(ch);
}

// --- Story 5.2: keyswitch state, controllers ----------------------------------------------------------------------------------

void SampleEngine::reset_part_keys(std::uint32_t channel) noexcept {
  channel &= 15u;
  KeyState k;
  const SampleBank* bank = bank_for(channel);
  k.last = bank ? bank->default_switch : static_cast<std::int16_t>(-1);
  ks_[channel] = k;
}

void SampleEngine::set_part_sample_bank(std::uint32_t part, SampleBank* bank) noexcept {
  if (part >= part_bank_.size()) return;
  const std::uint64_t id = bank ? bank->id : 0;
  part_bank_[part] = bank;
  if (part_bound_[part] && part_bank_id_[part] == id) return;  // the same instrument again (by token, not by address): nothing changes
  part_bank_id_[part] = id;
  part_bound_[part] = true;
  reset_part_keys(part);
}

void SampleEngine::clear_part_banks() noexcept {
  for (std::uint32_t p = 0; p < part_bank_.size(); ++p) {
    const bool was = part_bound_[p];
    part_bank_[p] = nullptr;
    part_bank_id_[p] = 0;
    part_bound_[p] = false;
    if (was) reset_part_keys(p);
  }
}

void SampleEngine::reset_play_state() noexcept {
  for (std::uint32_t ch = 0; ch < 16; ++ch) {
    reset_part_keys(ch);
    part_engaged_[ch].clear();  // the new Live Set's stored part_attack / part_release arrive right after and engage again where they differ from the default
  }
}

void SampleEngine::set_part_param(std::uint32_t part, std::uint32_t param_id, float value) noexcept { put_part_param(part, param_id, value, false); }
void SampleEngine::set_part_param_edit(std::uint32_t part, std::uint32_t param_id, float value) noexcept { put_part_param(part, param_id, value, true); }

void SampleEngine::put_part_param(std::uint32_t part, std::uint32_t param_id, float value, bool edit) noexcept {
  if (part >= 16 || (param_id != params::kPartAttack && param_id != params::kPartRelease) || !std::isfinite(value)) return;
  if (edit) part_params::engage(part_engaged_[part], param_id - params::kPartAttack);
  else if (!part_params::should_apply(part_engaged_[part], param_id - params::kPartAttack, param_id, value)) return;
  const params::ParamDef* d = params::find(param_id);
  const float ms = d ? std::clamp(value, static_cast<float>(d->min), static_cast<float>(d->max)) : value;
  const float sr = static_cast<float>(cfg_.sample_rate);
  if (param_id == params::kPartAttack) part_attack_step_[part] = seconds_to_lin_rate(ms * 0.001f, sr);
  else part_release_rate_[part] = seconds_to_exp_rate(ms * 0.001f, sr);
}

bool SampleEngine::set_part_cc(std::uint32_t part, std::uint32_t cc, std::uint32_t value32) noexcept {
  if (cc > 127) return false;
  // 32-bit value -> CC units. A 7-bit MIDI 1.0 value v arrives as the bit repetition of v, which is v / 127 of full scale to
  // better than 1e-8: values within 1e-6 of a whole CC step snap to it, so a controller sitting exactly on a crossfade
  // boundary behaves as in sfizz (which sees the integer).
  double x = static_cast<double>(value32) * (127.0 / 4294967295.0);
  const double nearest = std::floor(x + 0.5);
  if (std::fabs(x - nearest) < 1e-6) x = nearest;
  cc_[part & 15u][cc] = static_cast<float>(x);
  return true;
}

bool SampleEngine::key_released(std::uint32_t channel, std::uint32_t note) noexcept {
  if (note > 127) return false;
  channel &= 15u;
  KeyState& ks = ks_[channel];
  const bool was_held = ks.is_held(static_cast<int>(note));
  ks.set_held(static_cast<int>(note), false);
  if (!was_held) return false;  // a key up for a key that was not down (or a repeat) fires nothing

  // Story 13.3, trigger=release_key: fires at the physical key up, whatever the sustain pedal does (trigger=release is started by stop_voice_impl, which the
  // block loop defers to the pedal up). Same gates as a release region: the note-on velocity, round robin, keyswitch, an attack voice still alive.
  SampleBank* bank = bank_for(channel);
  const std::uint64_t started_on = last_bank_[channel * 128u + note];
  if (!bank || (started_on && started_on != bank->id)) return false;
  bool started = false;
  bool drawn = false;
  float rnd = 0.0f;  // drawn at the first candidate only, so a bank without release_key regions never moves the random sequence
  const std::uint8_t vel7 = last_vel7_[channel * 128u + note];
  for (std::uint32_t i = bank->note_begin[note]; i < bank->note_begin[note + 1]; ++i) {
    const std::uint32_t ri = bank->note_list[i];
    const RegionRt& r = bank->regions[ri];
    if (r.d.trigger != velora::ports::SampleTrigger::kReleaseKey) continue;
    if (r.has_keyswitch && !region_switched(r, ks)) continue;
    if (!bank->seq_switched[ri]) continue;
    if (vel7 < r.d.vel_lo || vel7 > r.d.vel_hi) continue;
    if (!drawn) {
      rnd = next_random();
      drawn = true;
    }
    if (!(rnd >= r.d.lorand && rnd < r.d.hirand)) continue;
    if (!playing_attack_voice(r, channel, true)) continue;
    if (!r.playable) {
      ++skipped_unplayable_;
      continue;
    }
    if (start_region(*bank, ri, channel, note, vel7, true)) started = true;  // a full voice pool drops the release sample, as it would a note
  }
  return started;
}

bool SampleEngine::region_switched(const RegionRt& r, const KeyState& ks) const noexcept {
  const auto& sw = r.d.sw;
  if (sw.last >= 0 && ks.last != sw.last) return false;
  if (sw.down >= 0 && !ks.is_held(sw.down)) return false;
  if (sw.up >= 0 && ks.is_held(sw.up)) return false;
  if (sw.previous >= 0 && ks.prev != sw.previous) return false;
  return true;
}

// The product of the region's CC crossfade gains at the part's current controller values (sfizz Voice::applyCrossfades).
float SampleEngine::crossfade_target(const RegionRt& r, std::uint32_t channel) const noexcept {
  const auto& cc = cc_[channel & 15u];
  float g = 1.0f;
  for (unsigned i = 0; i < r.d.xfin_count; ++i) g *= sample_xf_in_gain(r.d.xfin[i].lo, r.d.xfin[i].hi, cc[r.d.xfin[i].cc], r.d.xf_curve);
  for (unsigned i = 0; i < r.d.xfout_count; ++i) g *= sample_xf_out_gain(r.d.xfout[i].lo, r.d.xfout[i].hi, cc[r.d.xfout[i].cc], r.d.xf_curve);
  return g;
}

std::uint32_t SampleEngine::active_voices() const noexcept {
  std::uint32_t n = 0;
  for (const auto& v : voices_) n += v.active ? 1 : 0;
  return n;
}

// --- amplitude envelope: a sample-by-sample port of sfizz's ADSREnvelope --------------------------

void SampleEngine::init_env(Env& e, const RegionRt& r, std::uint32_t channel) const noexcept {
  e.s = EnvState::kDelay;
  e.v = r.env_start;
  e.delay_left = r.env_delay;
  e.hold_left = r.env_hold;
  e.transition_delta = 0.0f;
  e.release_rate = r.env_release_rate;
  e.attack_step = part_engaged_[channel & 15u].test(0) ? part_attack_step_[channel & 15u] : r.env_attack_step;
}

float SampleEngine::env_next(Env& e, const RegionRt& r) const noexcept {
  const float sr = static_cast<float>(cfg_.sample_rate);
  for (;;) {
    switch (e.s) {
      case EnvState::kDelay:
        if (e.delay_left-- > 0) {
          e.v = r.env_start;
          return e.v;
        }
        e.s = EnvState::kAttack;
        continue;
      case EnvState::kAttack:
        e.v += e.attack_step;
        if (e.v < 1.0f) return e.v;
        e.v = 1.0f;
        e.s = EnvState::kHold;
        continue;
      case EnvState::kHold:
        if (e.hold_left-- > 0) return e.v;
        e.s = EnvState::kDecay;
        continue;
      case EnvState::kDecay: {
        e.v *= r.env_decay_rate;
        if (e.v > r.env_sustain) {
          const float out = e.v;
          if (e.v <= r.env_sustain + kVirtuallyZero) {
            e.s = EnvState::kSustain;
            e.transition_delta = (r.env_sustain - e.v) / (sr * kTransitionTime);
          }
          return out;
        }
        e.v = std::max(r.env_sustain, e.v);
        e.s = EnvState::kSustain;
        e.transition_delta = (r.env_sustain - e.v) / (sr * kTransitionTime);
        continue;
      }
      case EnvState::kSustain:
        if (r.env_free_running) {  // a (near) zero sustain level releases by itself
          e.s = EnvState::kRelease;
          continue;
        }
        if (e.v > r.env_sustain) e.v += e.transition_delta;
        return e.v;
      case EnvState::kRelease: {
        const float nv = e.v * e.release_rate;
        if (nv > kReleaseThreshold) {
          e.v = nv;
          return nv;
        }
        // Exponential tail exhausted: linear fade-out from the last emitted value.
        e.s = EnvState::kFadeout;
        e.transition_delta = -std::max(kReleaseThreshold, e.v) / (sr * kTransitionTime);
        continue;
      }
      case EnvState::kFadeout: {
        const float nv = e.v + e.transition_delta;
        if (nv > 0.0f) {
          e.v = nv;
          return nv;
        }
        e.s = EnvState::kDone;
        e.v = 0.0f;
        continue;
      }
      case EnvState::kDone:
      default:
        e.v = 0.0f;
        return 0.0f;
    }
  }
}

bool SampleEngine::env_release(Env& e, const RegionRt&) const noexcept {
  if (e.s == EnvState::kDelay && e.delay_left > 0) return false;  // released before it ever sounded
  if (e.s < EnvState::kRelease) e.s = EnvState::kRelease;
  return true;
}

void SampleEngine::env_fast_release(Env& e) const noexcept {
  e.release_rate = tr_release_rate_off_;
  if (e.s < EnvState::kRelease) e.s = EnvState::kRelease;
}

// --- voices ---------------------------------------------------------------------------------------

bool SampleEngine::start_region(const SampleBank& bank, std::uint32_t region, std::uint32_t channel, std::uint32_t note,
                                std::uint8_t vel7, bool release_trigger) noexcept {
  const std::uint32_t slot = acquire_slot(channel, note);
  if (slot == VoiceAllocator::kNoSlot || slot >= capacity_) return false;
  Voice* v = &voices_[slot];
  const RegionRt& r = bank.regions[region];

  // Pitch: key tracking (100 cents per key) from pitch_keycenter, tune, transpose; single precision.
  float cents = 100.0f * (static_cast<float>(note) - static_cast<float>(r.d.key_center));
  cents += r.base_cents;
  const float ratio = std::pow(2.0f, cents / 1200.0f) * r.speed;

  // Amplitude velocity tracking at the SFZ default amp_veltrack=100: gain = velocity squared.
  const float vf = static_cast<float>(vel7) / 127.0f;
  const float vgain = 1.0f - 1.0f * (1.0f - vf * vf);
  const float base = vgain * r.vol_gain;

  release_stream(*v);  // a free slot holds no ring (every end gives it back); defence in depth, so a ring can never leak through a reused slot
  *v = Voice{};
  v->active = true;
  v->release_trigger = release_trigger;
  v->channel = channel;
  v->note = note;
  v->region = region;
  v->bank = &bank;
  v->vel7 = vel7;
  v->start_time = clock_;
  v->pos = static_cast<double>(r.d.offset);
  v->ratio = ratio;
  v->gain_l = base * r.chan_l;
  v->gain_r = base * r.chan_r;
  v->base_gain = base;
  init_env(v->env, r, channel);
  if (r.has_xfade) {  // sfizz resetCrossfades(): the smoother starts at the current gain, so a new note does not ramp
    v->xf_tgt = crossfade_target(r, channel);
    v->xf_cur = v->xf_tgt;
    v->xf_step = 0.0f;
  }
  if ((r.filtered || r.has_xfade) && !cfg_.extended_voices) ++undeclared_extended_;
  if (stream_ != nullptr && r.has_tail && r.tail_source != nullptr) {
    // Story 1.17: register the stream (non-blocking). The ring starts at the head end: frames below it are read from the head. No ring = the Story 1.16 placeholder.
    velora::ports::StreamOpen req;
    req.source = r.tail_source;
    req.sample = r.tail_sample;
    req.total_frames = r.total_frames;
    req.channels = r.stereo ? 2 : 1;
    req.start = r.frames;
    if (velora::ports::StreamRing* ring = stream_->open(req)) {
      v->ring = ring;
      v->streamed = true;
      v->seg = 1;
      v->seg_start = r.frames;
    } else {
      stream_open_failures_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return true;
}

bool SampleEngine::playing_attack_voice(const RegionRt& r, std::uint32_t channel, bool match_velocity) const noexcept {
  // sfizz rule: a release region only fires while an attack voice of a compatible note is alive.
  for (const auto& v : voices_) {
    if (v.active && !v.stolen && !v.release_trigger && v.channel == channel && v.note >= r.d.key_lo && v.note <= r.d.key_hi &&
        (!match_velocity || (v.vel7 >= r.d.vel_lo && v.vel7 <= r.d.vel_hi)))
      return true;
  }
  return false;
}

EngineStatus SampleEngine::start_voice(std::uint32_t channel, std::uint32_t note,
                                       std::uint32_t velocity16) noexcept {
  channel &= 15u;
  SampleBank* bank = bank_for(channel);
  if (note > 127) return EngineStatus::kOk;
  KeyState& ks = ks_[channel];
  const bool others_held = ks.others_held(static_cast<int>(note));  // Story 13.3: before this key goes down (first / legato)
  ks.set_held(static_cast<int>(note), true);  // the key state follows the keyboard even when the part has no (or an empty) instrument
  if (!bank || bank->regions.empty()) {
    ks.prev = static_cast<std::int16_t>(note);
    return EngineStatus::kOk;
  }
  const std::uint32_t v16 = std::min<std::uint32_t>(velocity16, 65535u);
  // 7-bit velocity, as MIDI 1.0 and SFZ define it (the UMP value is rounded to the nearest step).
  std::uint8_t vel7 = static_cast<std::uint8_t>((v16 * 127u + 32767u) / 65535u);
  if (vel7 == 0) vel7 = 1;  // a note on is never velocity 0
  last_vel7_[(channel & 15u) * 128u + note] = vel7;
  last_bank_[(channel & 15u) * 128u + note] = bank->id;
  const float rnd = next_random();

  // Keyswitch state first (sfizz order): the key goes down, and a keyswitch key becomes the part's last switch, before any region
  // is looked at, so a region mapped to the keyswitch key itself already follows it.
  if (bank->switch_key[note]) ks.last = static_cast<std::int16_t>(note);

  bool exhausted = false;
  for (std::uint32_t i = bank->note_begin[note]; i < bank->note_begin[note + 1]; ++i) {
    const std::uint32_t ri = bank->note_list[i];
    const RegionRt& r = bank->regions[ri];
    // Round robin: every region whose key range holds the note advances its own counter.
    bank->seq_switched[ri] = (bank->seq_counter[ri]++ % r.d.seq_length) == r.d.seq_position - 1 ? 1 : 0;
    using Trig = velora::ports::SampleTrigger;
    if (r.d.trigger == Trig::kFirst ? others_held : r.d.trigger == Trig::kLegato ? !others_held : r.d.trigger != Trig::kAttack) continue;
    if (r.has_keyswitch && !region_switched(r, ks)) continue;
    if (!bank->seq_switched[ri]) continue;
    if (vel7 < r.d.vel_lo || vel7 > r.d.vel_hi) continue;
    if (!(rnd >= r.d.lorand && rnd < r.d.hirand)) continue;
    if (!r.playable) {
      ++skipped_unplayable_;
      continue;
    }
    if (!start_region(*bank, ri, channel, note, vel7, false)) exhausted = true;
  }
  ks.prev = static_cast<std::int16_t>(note);  // sw_previous: the note on before the next one
  return exhausted ? EngineStatus::kPoolExhausted : EngineStatus::kOk;
}

EngineStatus SampleEngine::stop_voice(std::uint32_t channel, std::uint32_t note) noexcept { return stop_voice_impl(channel, note, 0); }

EngineStatus SampleEngine::release_voice(std::uint32_t channel, std::uint32_t note, std::uint32_t release_velocity16) noexcept {
  return stop_voice_impl(channel, note, release_velocity16);
}

EngineStatus SampleEngine::stop_voice_impl(std::uint32_t channel, std::uint32_t note, std::uint32_t release_velocity16) noexcept {
  if (note > 127) return EngineStatus::kVoiceNotFound;
  channel &= 15u;
  // Only a physical note off clears a held key (key_released()): this call also comes from the sustain pedal's deferred release, when the key
  // may be down again.
  bool found = false;
  for (auto& v : voices_) {
    if (!v.active || v.stolen || v.release_trigger || v.channel != channel || v.note != note || v.key_up) continue;
    const RegionRt& r = v.bank->regions[v.region];
    if (v.env.s >= EnvState::kRelease) continue;  // already releasing
    found = true;
    if (r.d.loop_mode == velora::ports::SampleLoopMode::kOneShot) {
      v.key_up = true;  // one_shot ignores the note off
      continue;
    }
    if (!env_release(v.env, r)) {
      end_voice(v);
      continue;
    }
    if (part_engaged_[channel].test(1)) v.env.release_rate = part_release_rate_[channel];  // part_release replaces the region's release time
    if (const float scale = half_pedal_scale(cc_[channel][64]); scale < 1.0f) {
      v.env.release_rate = std::pow(v.env.release_rate, scale);  // Story 3.11: half-pedal damping (see half_pedal_scale)
    }
  }

  // Release-trigger regions: the note-on velocity, a fresh random draw.
  const float rnd = next_random();
  std::uint8_t vel7 = last_vel7_[(channel & 15u) * 128u + note];
  const bool by_release_velocity = cfg_.release_velocity_triggers && release_velocity16 != 0;
  if (by_release_velocity) {  // Story 3.6: the measured release velocity (same 16 -> 7 bit rounding as the note on)
    const std::uint32_t rv = std::min<std::uint32_t>(release_velocity16, 65535u);
    vel7 = static_cast<std::uint8_t>((rv * 127u + 32767u) / 65535u);
    if (vel7 == 0) vel7 = 1;
  }
  bool exhausted = false;
  SampleBank* bank = bank_for(channel);
  const std::uint64_t started_on = last_bank_[(channel & 15u) * 128u + note];
  if (started_on && (!bank || started_on != bank->id)) bank = nullptr;  // rebound while held: no release-trigger lookup on a bank the note never used
  for (std::uint32_t i = 0; bank && i < bank->note_begin[note + 1] - bank->note_begin[note]; ++i) {
    const std::uint32_t ri = bank->note_list[bank->note_begin[note] + i];
    const RegionRt& r = bank->regions[ri];
    if (r.d.trigger != velora::ports::SampleTrigger::kRelease) continue;
    if (r.has_keyswitch && !region_switched(r, ks_[channel])) continue;
    if (!bank->seq_switched[ri]) continue;
    if (vel7 < r.d.vel_lo || vel7 > r.d.vel_hi) continue;
    if (!(rnd >= r.d.lorand && rnd < r.d.hirand)) continue;
    if (!playing_attack_voice(r, channel, !by_release_velocity)) continue;  // by release velocity: the layer is chosen by it, the attack voice only has to be alive
    if (!r.playable) {
      ++skipped_unplayable_;
      continue;
    }
    if (start_region(*bank, ri, channel, note, vel7, true)) found = true;
    else exhausted = true;
  }
  if (exhausted) return EngineStatus::kPoolExhausted;
  return found ? EngineStatus::kOk : EngineStatus::kVoiceNotFound;
}

void SampleEngine::set_param(std::uint32_t param_id, float value) noexcept {
  if (!std::isfinite(value)) return;
  if (param_id == kParamGain) gain_ = std::clamp(value, 0.0f, kMaxGain);
  else if (param_id == kParamSeed && value >= 0.0f && value < 9.0e18f) set_seed(static_cast<std::uint64_t>(value));
}

bool SampleEngine::parallel_applicable(std::uint32_t frames) const noexcept {
  return pool_ != nullptr && frames != 0 && frames <= par_max_frames_;
}

void SampleEngine::render(float* left, float* right, std::uint32_t frames) noexcept {
  if (parallel_applicable(frames)) {
    float* l[1] = {left};
    float* r[1] = {right};
    render_parallel(l, r, frames, false);
  } else {
    for (auto& v : voices_)
      if (v.active) render_voice(v, left, right, frames);
  }
  clock_ += frames;
}

void SampleEngine::render_split(float* const* left, float* const* right, std::uint32_t frames) noexcept {
  if (parallel_applicable(frames)) {
    render_parallel(left, right, frames, true);
  } else {
    for (auto& v : voices_)
      if (v.active) render_voice(v, left[v.channel & (kSplitChannels - 1)], right[v.channel & (kSplitChannels - 1)], frames);
  }
  clock_ += frames;
}

void SampleEngine::set_parallel_voice_pool(WorkerPool* pool, std::uint32_t max_frames) {
  pool_ = nullptr;  // never render with a half-built scratch
  par_max_frames_ = 0;
  par_stride_ = 0;
  scratch_base_ = nullptr;
  scratch_.clear();
  scratch_.shrink_to_fit();
  if (pool == nullptr || max_frames == 0) return;
  // Every (partition, channel, bus) buffer starts on a cache line (buses of different partitions never share a line): the stride is rounded up to
  // 16 floats and the base is aligned inside a vector over-allocated by one line.
  par_stride_ = (max_frames + 15u) & ~15u;
  scratch_.assign(static_cast<std::size_t>(WorkerPool::kPartitions) * kSplitChannels * 2 * par_stride_ + 16u, 0.0f);
  const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(scratch_.data());
  scratch_base_ = scratch_.data() + (((64u - (addr & 63u)) & 63u) / sizeof(float));
  par_max_frames_ = max_frames;
  pool_ = pool;
}

void SampleEngine::partition_task(void* self, std::uint32_t partition) noexcept {
  static_cast<SampleEngine*>(self)->render_partition(partition);
}

// One partition's work: the voices of slots partition, partition + 3, ... rendered into this partition's own scratch buses.
//
// Shared-state audit (Story 1.18; render_partition runs on any pool thread, concurrently for the three partitions):
//  * Written: only the partition's own Voice objects (every Voice field, including `active = false` when a voice ends or its steal fade completes),
//    its own scratch buses, and its own partition_mask_[partition] element. Voices of different partitions are different slots (slot % 3).
//  * integrity_events_: std::atomic, fetch_add relaxed. The only engine-wide write.
//  * Read only during the phase (nothing writes them until the pool barrier has passed): cfg_, gain_, xf_frames_, tr_release_rate_off_,
//    pan_table_, cc_ (crossfade_target), clock_ (advanced after the barrier in render()/render_split()), par_frames_/par_split_ (published by the
//    pool's release/acquire), the banks' regions and PCM (const), and the damage flags (std::atomic, relaxed loads).
//  * Never touched here: the arbiter / VoiceAllocator (a freed voice only clears its `active` flag; the arbiter reads voice_info() in the serial phase,
//    after the barrier), the RNG (rng_, drawn only by start_voice/stop_voice), round-robin counters, keyswitch state (ks_), last_vel7_/last_bank_,
//    the skipped / undeclared counters. start_voice, stop_voice, begin_steal_fade, key_released and set_param all run in the serial phase.
//  * No end-of-voice notification exists, so nothing has to be deferred to the serial phase: voice end is observed through `active` there.
// No race was found. Adding any new engine-wide write to render_voice requires deferring it past the barrier.
void SampleEngine::render_partition(std::uint32_t partition) noexcept {
  const std::uint32_t frames = par_frames_;
  std::uint32_t mask = 0;
  for (std::uint32_t slot = partition; slot < voices_.size(); slot += WorkerPool::kPartitions) {
    const Voice& v = voices_[slot];
    if (v.active) mask |= std::uint32_t{1} << (par_split_ ? (v.channel & (kSplitChannels - 1)) : 0u);
  }
  partition_mask_[partition] = mask;
  for (std::uint32_t c = 0; c < kSplitChannels; ++c) {
    if (!(mask & (std::uint32_t{1} << c))) continue;
    std::fill_n(scratch(partition, 0, c), frames, 0.0f);
    std::fill_n(scratch(partition, 1, c), frames, 0.0f);
  }
  for (std::uint32_t slot = partition; slot < voices_.size(); slot += WorkerPool::kPartitions) {
    Voice& v = voices_[slot];
    if (!v.active) continue;
    const std::uint32_t c = par_split_ ? (v.channel & (kSplitChannels - 1)) : 0u;
    render_voice(v, scratch(partition, 0, c), scratch(partition, 1, c), frames);
  }
}

void SampleEngine::render_parallel(float* const* left, float* const* right, std::uint32_t frames, bool split) noexcept {
  bool any = false;
  for (const auto& v : voices_)
    if (v.active) {
      any = true;
      break;
    }
  if (!any) return;  // nothing to render: no need to wake the workers
  par_frames_ = frames;
  par_split_ = split;
  pool_->run(&SampleEngine::partition_task, this);  // fork, render the three partitions, barrier
  // Summation in the fixed order partition 0, 1, 2 (then ascending part), independent of the executing threads.
  for (std::uint32_t p = 0; p < WorkerPool::kPartitions; ++p) {
    for (std::uint32_t c = 0; c < kSplitChannels; ++c) {
      if (!(partition_mask_[p] & (std::uint32_t{1} << c))) continue;
      const float* sl = scratch(p, 0, c);
      const float* sr = scratch(p, 1, c);
      float* ol = split ? left[c] : left[0];
      float* orr = split ? right[c] : right[0];
      for (std::uint32_t f = 0; f < frames; ++f) {
        ol[f] += sl[f];
        orr[f] += sr[f];
      }
    }
  }
}

namespace {

// Story 1.17: where the frames of a streamed sample are: the resident head, the resident loop segments, or the ring buffer of the stream.
struct StreamView {
  const float* hl = nullptr;
  const float* hr = nullptr;
  std::int64_t head_n = 0;
  const velora::ports::SamplePcmSegment* segs = nullptr;
  std::uint32_t nsegs = 0;
  const float* rl = nullptr;  // the ring (nullptr: the voice no longer has one)
  const float* rr = nullptr;
  std::uint32_t cap = 1;
  std::uint64_t seg_start = 0, hi = 0;  // the ring holds frames [seg_start, hi) of the voice's segment (hi: a snapshot of the producer's filled_end)
  std::int64_t total = 0;
  bool stereo = false;
};

constexpr std::size_t kNoSlot = ~std::size_t{0};

// Frame `i` (left, right; mono repeats left in `r`). Outside [0, total) the sample is zero, as a fully resident render reads it. false = the frame is not available yet.
// `slot` is the ring slot of the PREVIOUS frame when the caller reads consecutive frames (kNoSlot otherwise): the slot of a ring frame is then that plus one with a wrap compare,
// instead of a 64-bit modulo per tap.
inline bool stream_frame(const StreamView& sv, std::int64_t i, float& l, float& r, std::size_t& slot) noexcept {
  if (i < 0 || i >= sv.total) {
    l = 0.0f;
    r = 0.0f;
    slot = kNoSlot;
    return true;
  }
  if (i < sv.head_n) {
    l = sv.hl[i];
    r = sv.stereo ? sv.hr[i] : l;
    slot = kNoSlot;
    return true;
  }
  for (std::uint32_t k = 0; k < sv.nsegs; ++k) {
    const auto& sg = sv.segs[k];
    if (sg.left != nullptr && i >= static_cast<std::int64_t>(sg.start) && i < static_cast<std::int64_t>(sg.start + sg.frames)) {
      const std::int64_t o = i - static_cast<std::int64_t>(sg.start);
      l = sg.left[o];
      r = sv.stereo && sg.right != nullptr ? sg.right[o] : l;
      slot = kNoSlot;
      return true;
    }
  }
  const auto u = static_cast<std::uint64_t>(i);
  if (sv.rl != nullptr && u >= sv.seg_start && u < sv.hi) {
    if (slot == kNoSlot) slot = static_cast<std::size_t>(u % sv.cap);
    else if (++slot == sv.cap) slot = 0;
    l = sv.rl[slot];
    r = sv.stereo ? sv.rr[slot] : l;
    return true;
  }
  slot = kNoSlot;
  return false;
}

}  // namespace

void SampleEngine::render_voice(Voice& v, float* left, float* right, std::uint32_t frames) noexcept {
  const RegionRt& r = v.bank->regions[v.region];
  const bool linear = cfg_.interpolation == SampleInterpolation::kLinear;
  const bool streamed = v.streamed;  // Story 1.17: the sample is `total_frames` long and the head is only its first part
  const std::int64_t nframes = static_cast<std::int64_t>(streamed ? r.total_frames : r.frames);
  // Story 13.3: `end` (exclusive) makes the played part [0, eff_n); without it eff_n == nframes and nothing below changes.
  const std::int64_t eff_n = (r.end_cut >= 0 && r.end_cut < nframes) ? r.end_cut : nframes;
  const std::int64_t end_idx = eff_n - 2;  // sfizz: positions at or past this index end the sample
  const bool head_cut = r.end_cut >= 0 && r.end_cut <= static_cast<std::int64_t>(r.frames);  // the sample ends inside the resident head: not an underrun
  const bool sustain_loop = r.d.loop_mode == velora::ports::SampleLoopMode::kLoopSustain;
  const float gl = v.gain_l * gain_, gr = v.gain_r * gain_;
  const bool has_xf = r.has_xfade, filtered = r.filtered;
  if (has_xf) {  // the controllers as they are for this render segment: a new target restarts the linear ramp (sfizz LinearSmoother)
    const float want = crossfade_target(r, v.channel);
    if (want != v.xf_tgt) {
      v.xf_tgt = want;
      v.xf_step = (want - v.xf_cur) / static_cast<float>(std::max<std::int32_t>(16, xf_frames_));
      if (v.xf_step == 0.0f || !std::isfinite(v.xf_step)) v.xf_cur = want;  // a step that rounds to nothing would never arrive: snap
    }
  }
  const float cl = r.chan_l * gain_, cr = r.chan_r * gain_;

  // Story 1.17: this block's view of the stream. The producer's progress is read ONCE (acquire) per block, and the lowest frame the voice can still read is published (release) so
  // the producer never overwrites it: positions only move forward within a segment, and the voice reads at most idx - 1 - ceil(ratio) (the oracle hold) below its start.
  StreamView sv;
  if (streamed) {
    sv.hl = r.left;
    sv.hr = r.right;
    sv.head_n = static_cast<std::int64_t>(r.frames);
    sv.segs = r.segs;
    sv.nsegs = r.seg_count;
    sv.total = nframes;
    sv.stereo = r.stereo;
    sv.seg_start = v.seg_start;
    if (v.ring != nullptr) {
      sv.rl = v.ring->left;
      sv.rr = v.ring->right;
      sv.cap = v.ring->capacity;
      const std::uint64_t a = v.ring->avail.load(std::memory_order_acquire);
      sv.hi = static_cast<std::uint32_t>(a >> 48) == v.seg ? (a & velora::ports::kStreamFrameMask) : 0;
      const std::int64_t i0 = static_cast<std::int64_t>(std::floor(v.pos));
      const std::int64_t lo = std::max<std::int64_t>(static_cast<std::int64_t>(v.seg_start), i0 - 2 - static_cast<std::int64_t>(std::ceil(v.ratio)));
      v.lo_pub = static_cast<std::uint64_t>(lo);
      v.ring->consumed_lo.store(v.lo_pub, std::memory_order_release);
    }
  }
  // Reads the producer's progress again (the data may have arrived since the block began). True when it moved.
  const auto refresh_stream = [&]() noexcept {
    if (v.ring == nullptr) return false;
    const std::uint64_t a = v.ring->avail.load(std::memory_order_acquire);
    const std::uint64_t nh = static_cast<std::uint32_t>(a >> 48) == v.seg ? (a & velora::ports::kStreamFrameMask) : 0;
    const bool moved = nh != sv.hi;
    sv.hi = nh;
    return moved;
  };

  const std::uint64_t oracle_n = cfg_.oracle_block_frames;
  // Story 6.5: how far ahead of the playhead (in sample frames) the integrity fade looks, so the fade has finished when a damaged block is reached.
  const std::int64_t integ_look = r.damage != nullptr ? static_cast<std::int64_t>(std::ceil(static_cast<double>(v.ratio) * kIntegrityFadeFrames)) + 1 : 0;
  for (std::uint32_t f = 0; f < frames; ++f) {
    float fade = 1.0f;
    if (v.stolen) {  // steal ramp: gain fade_left/fade_total, strictly falling; 0 frees the voice
      if (v.fade_left == 0) {
        end_voice(v);
        return;
      }
      fade = static_cast<float>(v.fade_left) / static_cast<float>(v.fade_total);
      --v.fade_left;
    }
    if (oracle_n != 0 && !v.stalled) {
      const std::uint64_t abs = clock_ + f;
      if (abs % oracle_n == 0 && abs / oracle_n > v.start_time / oracle_n) {
        v.pos = std::max(0.0, v.pos - static_cast<double>(v.ratio));  // repeat the previous frame's position once
        v.stalled = true;
      }
    }
    // Loop wrap: continuous loops always, sustain loops until the envelope is released.
    const bool looping = r.loops && !v.release_trigger && !v.ended && (!sustain_loop || v.env.s < EnvState::kRelease);
    std::int64_t idx = static_cast<std::int64_t>(std::floor(v.pos));
    if (looping) {
      if (idx > r.loop_end) {  // same as subtracting the loop size repeatedly, in bounded time
        v.pos = static_cast<double>(r.loop_start) +
                std::fmod(v.pos - static_cast<double>(r.loop_start), static_cast<double>(r.loop_size));
        idx = static_cast<std::int64_t>(std::floor(v.pos));
        if (streamed && v.ring != nullptr && r.loop_seek >= 0 && r.loop_seek < nframes) {
          // Story 1.17: the wrap sends the voice back to resident memory (the head and the loop segments); the stream re-seeks to the frame after that resident run. A ring
          // still at its segment start (nothing consumed, nothing skipped) already is where the seek would put it.
          const auto s0 = static_cast<std::uint64_t>(r.loop_seek);
          if (!(v.lo_pub <= v.seg_start && v.seg_start == s0)) {
            const std::uint32_t ns = velora::ports::stream_next_segment(v.seg);
            v.ring->consumed_lo.store(s0, std::memory_order_relaxed);
            v.ring->seek_start.store(s0, std::memory_order_relaxed);
            v.ring->seek_seq.store(ns, std::memory_order_release);  // publishes the two stores above to the producer
            v.seg = ns;
            v.seg_start = s0;
            v.lo_pub = s0;
            sv.seg_start = s0;
            sv.hi = 0;
          }
        }
      }
    } else if (idx >= end_idx && !v.ended) {
      // Sample exhausted: a 6 ms release starts on this very frame (sfizz turns the voice off at
      // the frame where the position reaches the end) while the last frame is held below.
      v.ended = true;
      env_fast_release(v.env);
      if (r.has_tail && !streamed && !head_cut) underrun_events_.fetch_add(1, std::memory_order_relaxed);  // Story 1.16: the head of a sample with a tail ran out (a streamed voice reads on)
    }
    if (looping && r.has_tail && !streamed && !head_cut && idx >= end_idx) {  // a loop that reaches beyond the resident head: same ending, same count (once ended, the voice no longer loops)
      v.ended = true;
      env_fast_release(v.env);
      underrun_events_.fetch_add(1, std::memory_order_relaxed);
    }

    const float env = env_next(v.env, r);
    if (v.env.s == EnvState::kDone) {
      end_voice(v);
      return;
    }

    float sl, sr_;
    if (streamed) {
      // Story 1.17: the same arithmetic as the resident path below on frames that come from the head, the loop segments or the ring. The frames the interpolation window needs
      // must all be there; otherwise the voice is STARVED: it never waits, it holds its last output under a short fade (silence), counts one underrun per episode and goes on
      // (the position advances). When the data arrives it fades back in. With the data on time the fade gain is exactly 1 and nothing differs from the resident render.
      bool ok = true;
      if (idx >= end_idx && !looping) {  // past the end: hold the last frame (read once, then the ring is given back)
        if (v.hold_valid) {
          sl = v.hold_l;
          sr_ = v.hold_r;
        } else {
          float a = 0.0f, b = 0.0f;
          std::size_t slot = kNoSlot;
          ok = stream_frame(sv, eff_n - 1, a, b, slot) || (refresh_stream() && stream_frame(sv, eff_n - 1, a, b, slot));
          sl = a;
          sr_ = r.stereo ? b : a;
          if (ok) {
            v.hold_l = sl;
            v.hold_r = sr_;
            v.hold_valid = true;
            release_stream(v);
            sv.rl = nullptr;
          }
        }
      } else {
        const float c = static_cast<float>(v.pos - static_cast<double>(idx));
        float tl[4], tr[4];
        const auto gather = [&]() noexcept {
          if (idx >= 1 && idx + 2 < sv.head_n) {  // all four taps in the head: the common case at the start of a note
            for (int k = 0; k < 4; ++k) {
              tl[k] = sv.hl[idx - 1 + k];
              tr[k] = sv.stereo ? sv.hr[idx - 1 + k] : tl[k];
            }
            return true;
          }
          std::size_t slot = kNoSlot;
          for (int k = 0; k < 4; ++k)
            if (!stream_frame(sv, idx - 1 + k, tl[k], tr[k], slot)) return false;
          return true;
        };
        ok = gather() || (refresh_stream() && gather());
        if (ok) {
          if (linear) {
            const float a = 1.0f - c;
            sl = tl[1] * a + tl[2] * c;
            sr_ = r.stereo ? tr[1] * a + tr[2] * c : sl;
          } else {
            float w[4];
            hermite_weights(c, w);
            sl = w[0] * tl[0] + w[1] * tl[1] + w[2] * tl[2] + w[3] * tl[3];
            sr_ = r.stereo ? w[0] * tr[0] + w[1] * tr[1] + w[2] * tr[2] + w[3] * tr[3] : sl;
          }
        } else {
          sl = 0.0f;
          sr_ = 0.0f;
        }
      }
      constexpr float kSgStep = 1.0f / static_cast<float>(kStreamFadeFrames);
      if (ok) {
        if (v.starved) {
          // Re-entry: the output was the held sample under the fade; crossfade from it (not from the pre-fade value) to the real samples over kStreamFadeFrames, so a starve of any
          // length, even a few frames, ends without a step. (Only after a starve: with the data on time none of this runs and nothing is multiplied.)
          v.starved = false;
          v.rx_l = v.last_l * v.sgain;
          v.rx_r = v.last_r * v.sgain;
          v.rx_n = 1;
          v.sgain = 1.0f;
        }
        if (v.rx_n != 0) {
          const float t = static_cast<float>(v.rx_n) * kSgStep;
          sl = v.rx_l * (1.0f - t) + sl * t;
          sr_ = v.rx_r * (1.0f - t) + sr_ * t;
          v.rx_n = v.rx_n >= kStreamFadeFrames ? 0u : v.rx_n + 1u;
        }
        v.last_l = sl;
        v.last_r = sr_;
      } else {
        if (!v.starved) {  // one underrun per episode (a stream that flaps starves, recovers and starves again counts each episode)
          v.starved = true;
          v.rx_n = 0;
          underrun_events_.fetch_add(1, std::memory_order_relaxed);
        }
        v.sgain = std::max(0.0f, v.sgain - kSgStep);
        sl = v.last_l * v.sgain;
        sr_ = v.last_r * v.sgain;
      }
    } else if (idx >= end_idx && (!looping || r.has_tail)) {  // past the end (or, for a looping voice with a tail, past the resident head): hold the last frame
      sl = r.left[eff_n - 1];
      sr_ = r.stereo ? r.right[eff_n - 1] : sl;
    } else {
      const float c = static_cast<float>(v.pos - static_cast<double>(idx));
      if (linear) {
        const float a = 1.0f - c;
        const float l0 = (idx >= 0 && idx < nframes) ? r.left[idx] : 0.0f, l1 = (idx >= -1 && idx + 1 < nframes) ? r.left[idx + 1] : 0.0f;
        sl = l0 * a + l1 * c;
        if (r.stereo) {
          const float r0 = (idx >= 0 && idx < nframes) ? r.right[idx] : 0.0f, r1 = (idx >= -1 && idx + 1 < nframes) ? r.right[idx + 1] : 0.0f;
          sr_ = r0 * a + r1 * c;
        } else {
          sr_ = sl;
        }
      } else {
        float w[4];
        hermite_weights(c, w);
        if (idx >= 1 && idx + 2 < nframes) {
          const float* p = r.left + idx;
          sl = w[0] * p[-1] + w[1] * p[0] + w[2] * p[1] + w[3] * p[2];
          if (r.stereo) {
            const float* q = r.right + idx;
            sr_ = w[0] * q[-1] + w[1] * q[0] + w[2] * q[1] + w[3] * q[2];
          } else {
            sr_ = sl;
          }
        } else {
          auto tap = [&](const float* d, std::int64_t i) { return (i < 0 || i >= nframes) ? 0.0f : d[i]; };
          sl = w[0] * tap(r.left, idx - 1) + w[1] * tap(r.left, idx) + w[2] * tap(r.left, idx + 1) +
               w[3] * tap(r.left, idx + 2);
          if (r.stereo)
            sr_ = w[0] * tap(r.right, idx - 1) + w[1] * tap(r.right, idx) + w[2] * tap(r.right, idx + 1) +
                  w[3] * tap(r.right, idx + 2);
          else
            sr_ = sl;
        }
      }
    }

    if (r.damage != nullptr) {  // Story 6.5: a protected sample. Fade out before a damaged block is reached, back in after it; count each entry once.
      const auto block_of = [&r](std::int64_t i) noexcept { return r.damage_shift >= 0 ? (i >> r.damage_shift) : i / static_cast<std::int64_t>(r.damage_block_frames); };
      const auto damaged_block = [&r](std::int64_t b) noexcept {
        return b >= 0 && b < static_cast<std::int64_t>(r.damage_block_count) && r.damage[b].load(std::memory_order_relaxed) != 0;
      };
      // Frames from the read position to the first damaged block in the look-ahead window [idx, idx + integ_look] (0 = the playhead is inside one); integ_look + 1 =
      // none. Walked block by block, bounded by look / block size + 1 steps; the window wraps through the loop point of a looping voice.
      const auto first_damaged_start = [&](std::int64_t from, std::int64_t to, std::int64_t& start) noexcept {
        for (std::int64_t b = block_of(from) + 1; b <= block_of(to); ++b)
          if (damaged_block(b)) {
            start = r.damage_shift >= 0 ? (b << r.damage_shift) : b * static_cast<std::int64_t>(r.damage_block_frames);
            return true;
          }
        return false;
      };
      std::int64_t dist = integ_look + 1;
      if (idx >= 0 && damaged_block(block_of(idx))) {
        dist = 0;
      } else {
        const std::int64_t from = idx < 0 ? 0 : idx;
        const std::int64_t hi = idx + integ_look;
        std::int64_t start = 0;
        if (first_damaged_start(from, hi, start)) dist = std::max<std::int64_t>(0, start - idx);
        if (looping && hi > r.loop_end) {
          const std::int64_t wrapped = r.loop_start + (hi - r.loop_start) % r.loop_size;
          if (damaged_block(block_of(r.loop_start))) dist = std::min(dist, std::max<std::int64_t>(0, r.loop_end + 1 - idx));
          else if (first_damaged_start(r.loop_start, wrapped, start)) dist = std::min(dist, std::max<std::int64_t>(0, (r.loop_end + 1 - idx) + (start - r.loop_start)));
        }
      }
      const bool bad = dist <= integ_look;
      // The gain the distance allows: a voice that is already inside the window (it started there, or the flag was set late) may not play the damaged block at
      // full gain, so the gain is capped at dist / look -- 0 when the block is reached.
      const float limit = bad ? std::min(1.0f, static_cast<float>(dist) / static_cast<float>(std::max<std::int64_t>(integ_look, 1))) : 1.0f;
      if (!v.integ_seen) {  // first look: start at the allowed gain (a voice that starts inside a damaged block starts silent)
        v.integ_seen = true;
        v.integ_gain = limit;
      }
      if (bad != v.integ_bad) {
        v.integ_bad = bad;
        if (bad) integrity_events_.fetch_add(1, std::memory_order_relaxed);
      }
      constexpr float kStep = 1.0f / static_cast<float>(kIntegrityFadeFrames);
      v.integ_gain = bad ? std::min(limit, std::max(0.0f, v.integ_gain - kStep)) : std::min(1.0f, v.integ_gain + kStep);
      sl *= v.integ_gain;
      sr_ *= v.integ_gain;
    }

    if (!has_xf && !filtered) {  // the plain voice: exactly the Story 1.15 arithmetic (bit-exact bypass)
      if (v.stolen) {
        left[f] += sl * env * gl * fade;
        right[f] += sr_ * env * gr * fade;
      } else {
        left[f] += sl * env * gl;
        right[f] += sr_ * env * gr;
      }
    } else {
      // CC crossfade gain: a linear ramp to the target the controllers ask for, 5 ms long (sfizz's LinearSmoother).
      float xf = 1.0f;
      if (has_xf) {
        if (v.xf_tgt > v.xf_cur) v.xf_cur = std::min(v.xf_tgt, v.xf_cur + v.xf_step);
        else if (v.xf_tgt < v.xf_cur) v.xf_cur = std::max(v.xf_tgt, v.xf_cur + v.xf_step);
        xf = v.xf_cur;
      }
      float ol, orr;
      if (filtered) {  // amplitude stage -> filter -> pan (mono) / pan -> filter (stereo; the same linear result)
        const float a = env * xf * v.base_gain;
        const float yl = static_cast<float>(sample_filter_tick(r.filter, v.fl, static_cast<double>(sl * a)));
        const float yr = r.stereo ? static_cast<float>(sample_filter_tick(r.filter, v.fr, static_cast<double>(sr_ * a))) : yl;
        ol = yl * cl;
        orr = yr * cr;
      } else {
        const float a = env * xf;
        ol = sl * a * gl;
        orr = sr_ * a * gr;
      }
      if (v.stolen) {
        ol *= fade;
        orr *= fade;
      }
      left[f] += ol;
      right[f] += orr;
    }
    v.pos += static_cast<double>(v.ratio);
  }
}

bool SampleEngine::voice_info(std::uint32_t slot, VoiceInfo& out) const noexcept {
  if (slot >= capacity_ || !voices_[slot].active) return false;
  const Voice& v = voices_[slot];
  out.start_time = v.start_time;
  out.level = v.env.v * std::max(v.gain_l, v.gain_r) * gain_;
  if (v.stolen || v.release_trigger || v.env.s >= EnvState::kRelease) out.state = VoiceState::kReleasing;
  else if (v.key_up) out.state = VoiceState::kSustained;
  else out.state = VoiceState::kHeld;
  out.channel = v.channel;
  out.note = v.note;
  return true;
}

bool SampleEngine::begin_steal_fade(std::uint32_t slot, std::uint32_t ramp_frames) noexcept {
  if (slot >= capacity_ || !voices_[slot].active || voices_[slot].stolen) return false;
  Voice& v = voices_[slot];
  v.stolen = true;
  v.fade_total = ramp_frames == 0 ? 1 : ramp_frames;
  v.fade_left = v.fade_total;
  return true;
}

}  // namespace velora::core
