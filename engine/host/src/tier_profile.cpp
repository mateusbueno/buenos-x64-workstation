#include "velora/host/tier_profile.hpp"

#include "velora/core/fx.hpp"
#include "velora/core/voice_arbiter.hpp"

namespace velora::host {

namespace {

constexpr std::uint64_t kGiB = 1ull << 30;
using core::VoiceArbiter;

// The v1 numbers: epics Story 4.1 / AD-27 (8 parts, head budget 5 GB inside the 4-6 GB range of NFR39) and the defaults the engine already shipped with.
constexpr std::uint32_t kV1Parts = 8;
constexpr std::uint64_t kV1HeadBudget = 5 * kGiB;
constexpr std::uint64_t kV1ArbiterBudget = 1u << 20;  // was LiveSetLoaderOptions::arbiter_budget (calibrated on the board: Story 1.18)
constexpr std::uint32_t kV1SampleFloor = VoiceArbiter::kSampleFloorVoices;  // 256 (Story 1.19, NFR2)
constexpr std::uint32_t kV1FmFloor = VoiceArbiter::kFmFloorVoices;          // 128 (Story 12.5)
constexpr std::uint32_t kV1VaFloor = VoiceArbiter::kVaFloorVoices;          // 64 (Story 12.7)
constexpr std::uint32_t kV1FmCapacity = 256;     // FmEngine::kVoiceCapacity (floor 128 + 64 headroom + slack)
constexpr std::uint32_t kV1VaCapacity = 128;     // VaEngine::kVoiceCapacity
constexpr std::uint32_t kV1OrganCapacity = 640;  // OrganEngine::kVoiceCapacity: 8 parts x 73 keys + tails
constexpr std::uint32_t kV1SampleCapacity = 340;  // the 256 voice floor plus the 64 voice fade headroom, with slack (was hard-coded in engine-host)

// Pro = 2x v1: the user's decision for Story 4.1 (scale 2x). 16 parts and the sequencer module come from the epics (Story 4.1, AD-27).
constexpr std::uint32_t kProScale = 2;

constexpr TierProfile kV1{ports::TierId::kV1,
                          "v1",
                          kV1Parts,
                          kV1HeadBudget,
                          kV1ArbiterBudget,
                          core::max_fx_cost_for_parts(kV1Parts),
                          kV1SampleFloor,
                          kV1FmFloor,
                          kV1VaFloor,
                          kV1SampleCapacity,
                          kV1FmCapacity,
                          kV1VaCapacity,
                          kV1OrganCapacity,
                          false};

constexpr TierProfile kPro{ports::TierId::kPro,
                           "pro",
                           kV1Parts * kProScale,  // 16
                           kV1HeadBudget * kProScale,  // 10 GB
                           kV1ArbiterBudget * kProScale,
                           core::max_fx_cost_for_parts(kV1Parts * kProScale),
                           kV1SampleFloor * kProScale,  // 512
                           kV1FmFloor * kProScale,
                           kV1VaFloor * kProScale,
                           kV1SampleFloor * kProScale + (kV1SampleCapacity - kV1SampleFloor),  // the same headroom and slack above the doubled floor
                           kV1FmCapacity * kProScale,
                           kV1VaCapacity * kProScale,
                           kV1OrganCapacity * kProScale,
                           true};

static_assert(kPro.part_count <= core::params::kPartCount && kV1.part_count <= core::params::kPartCount,
              "the structural part capacity of the build carries every tier's part count");

}  // namespace

const TierProfile& v1_profile() noexcept { return kV1; }
const TierProfile& pro_profile() noexcept { return kPro; }

const TierProfile& profile_for(ports::TierId id) noexcept {
  switch (id) {
    case ports::TierId::kPro: return kPro;
    case ports::TierId::kV1: break;
  }
  return kV1;
}

bool parse_tier_name(std::string_view name, ports::TierId& out) noexcept {
  if (name == "v1") {
    out = ports::TierId::kV1;
    return true;
  }
  if (name == "pro") {
    out = ports::TierId::kPro;
    return true;
  }
  return false;
}

ports::TierRead NoStrapTierSource::read() noexcept {
  ports::TierRead r;
  r.readable = false;
  r.reason = "no tier strap on this host";
  return r;
}

TierSelection resolve_tier(ports::TierSource* source, std::optional<ports::TierId> dev_override) {
  TierSelection sel;
  if (dev_override) {
    sel.profile = &profile_for(*dev_override);
    sel.origin = "override";
    sel.log_line = std::string("tier: *** DEV OVERRIDE: running the ") + sel.profile->name + " profile (--tier); not what the board's strap says ***";
    return sel;
  }
  ports::TierRead r;
  if (source != nullptr) {
    r = source->read();
  } else {
    r.reason = "no tier source";
  }
  if (source != nullptr && r.readable) {
    switch (r.tier) {  // an ID this build does not know is as good as unreadable
      case ports::TierId::kV1:
      case ports::TierId::kPro: break;
      default:
        r.readable = false;
        r.reason = "unknown tier id " + std::to_string(static_cast<int>(r.tier)) + " from the tier source";
    }
  }
  if (source != nullptr && r.readable) {
    sel.profile = &profile_for(r.tier);
    sel.origin = "strap";
    sel.log_line = std::string("tier: ") + sel.profile->name + " (read once from the tier source)";
    return sel;
  }
  sel.profile = &kV1;
  sel.origin = "default";
  sel.reason = r.reason.empty() ? "tier unreadable" : r.reason;
  sel.log_line = std::string("tier: v1 (") + sel.reason + ")";
  return sel;
}

}  // namespace velora::host
