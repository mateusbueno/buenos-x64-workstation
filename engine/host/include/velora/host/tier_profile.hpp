#pragma once
// Tier profile (Story 4.1, FR17, AR24, NFR39, AD-27): ONE image runs v1 and Pro. The tier is read once at start (ports::TierSource: board strap / OTP, or the
// Mac's dev override) and selects one of two constant TierProfile instances. Everything tier-dependent -- part count, head budget, voice and cost budgets,
// enabled modules -- is a field of the profile; consumers read the profile object and NEVER branch on the tier ID (tools/ci/check_tier_branching.py gates it).
// No code is compiled per tier.
//
// The numbers live in tier_profile.cpp, in one place, with their sources:
//   * epics Story 4.1 / AD-27: v1 = 8 parts, head budget 5 GB (inside the 4-6 GB range of NFR39); Pro = 16 parts, sequencer module enabled;
//   * the v1 voice and cost budgets are the defaults the engine already shipped with (arbiter budget 1 << 20 cost units, the sample / FM / VA floors of Stories
//     1.19, 12.5, 12.7, the FX budget of every slot of every part holding the heaviest insert);
//   * Pro = 2x v1 -- the user's decision for this story (head budget 10 GB, arbiter budget, floors, voice capacity and FX budget scaled by two; the FX budget
//     follows from the 16 parts).
//   TODO(board): all budgets are placeholders to be recalibrated on the board (Stories 1.18 / 2.12); none was measured there.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "velora/ports/tier_source.hpp"

namespace velora::host {

struct TierProfile {
  ports::TierId id;
  const char* name;  // "v1" | "pro"
  std::uint32_t part_count;                 // parts a Live Set carries on this tier (the structural capacity is core::params::kPartCount = 16)
  std::uint64_t head_budget_bytes;          // decoded PCM of the heads that may be resident (switcher / loader budget)
  std::uint64_t arbiter_budget;             // cost units of the whole voice pool (VoiceArbiter LiveSetConfig::budget)
  std::uint64_t fx_budget;                  // cost units reserved for effects (ControlPlane::set_fx_budget)
  std::uint32_t sample_floor_voices;        // guaranteed sample voices
  std::uint32_t fm_floor_voices;            // guaranteed FM voices
  std::uint32_t va_floor_voices;            // guaranteed VA voices
  std::uint32_t sample_voice_capacity;      // the sample engine's voice slots (floor + fade headroom + slack)
  std::uint32_t fm_voice_capacity;          // the FM engine's voice slots: at least fm_floor_voices + VoiceArbiter::kFadeHeadroomVoices
  std::uint32_t va_voice_capacity;          // the VA engine's voice slots: at least va_floor_voices + VoiceArbiter::kFadeHeadroomVoices
  std::uint32_t organ_voice_capacity;       // the organ's voice slots; its generators run for part_count parts (OrganEngine(part_count, organ_voice_capacity))
  bool sequencer_enabled;                   // module flag: the sequencer is a Pro module (no sequencer code in this story)
};

// The two profiles. Immutable for the process lifetime.
const TierProfile& v1_profile() noexcept;
const TierProfile& pro_profile() noexcept;
// The profile of a tier ID: the ONLY place that maps a tier to its profile.
const TierProfile& profile_for(ports::TierId id) noexcept;

// "v1" / "pro" (case-sensitive) -> the tier; false for anything else.
bool parse_tier_name(std::string_view name, ports::TierId& out) noexcept;

// The Mac host has no strap: read() always answers "not readable", so the Mac runs v1 unless the dev override says otherwise.
class NoStrapTierSource final : public ports::TierSource {
 public:
  ports::TierRead read() noexcept override;
};

struct TierSelection {
  const TierProfile* profile = nullptr;  // never null after resolve_tier()
  std::string origin;                    // "override" | "strap" | "default"
  std::string reason;                    // origin == "default": why the strap was not used
  std::string log_line;                  // the line the engine logs (prominent for the dev override)
};

// Reads the tier ONCE and picks the profile: the dev override (when given) wins and the source is not even read; else the source; an unreadable source
// (or none) gives v1 with the reason. Control thread, at start. Callers keep the result and never call it again (the tier is not re-read).
TierSelection resolve_tier(ports::TierSource* source, std::optional<ports::TierId> dev_override);

}  // namespace velora::host
