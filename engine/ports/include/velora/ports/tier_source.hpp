#pragma once
// Tier-source port (Story 4.1, FR17, AR24, AD-27): how the engine learns which product tier it runs on (v1 or Pro). Pure interface, standard library only.
//
// The tier ID is read ONCE, at engine start, and never again; the engine turns it into an immutable profile (engine/host/include/velora/host/tier_profile.hpp).
// On the board an implementation reads the board strap / OTP fuse (not implemented on the Mac, Story 4.1 is Mac logic). A source that cannot read the strap
// says so with a reason and the engine runs the v1 profile (the safe, smaller one) and logs the reason; the Mac host has no strap and always says so.
//
// Threading: read() runs once, on the control thread, before audio starts. It may block briefly (a register or fuse read); never from the audio thread.
#include <cstdint>
#include <string>

namespace velora::ports {

enum class TierId : std::uint8_t { kV1 = 1, kPro = 2 };

struct TierRead {
  bool readable = false;  // false: no strap / unreadable / unknown value; `tier` is then meaningless and the engine uses v1
  TierId tier = TierId::kV1;
  std::string reason;  // when !readable: why (logged)
};

class TierSource {
 public:
  virtual ~TierSource() = default;
  virtual TierRead read() noexcept = 0;
};

}  // namespace velora::ports
