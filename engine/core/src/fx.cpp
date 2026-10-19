#include "velora/core/fx.hpp"

#include "velora/core/block_renderer.hpp"

namespace velora::core {

static_assert(kFxBlockFrames == kBlockFrames, "the effect block size is the engine block size");

std::unique_ptr<Effect> make_effect(EffectType type) {
  if (static_cast<std::uint8_t>(type) == 0 || static_cast<std::uint8_t>(type) >= params::kInsertTypeCount) return nullptr;
  auto fx = std::make_unique<PassThroughEffect>(type, effect_cost_units(type));
  fx->prepare(kSampleRate, kFxBlockFrames);
  return fx;
}

}  // namespace velora::core
