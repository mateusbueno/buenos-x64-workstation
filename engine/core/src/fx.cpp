#include "velora/core/fx.hpp"

#include "velora/core/block_renderer.hpp"
#include "velora/core/compressor.hpp"
#include "velora/core/eq.hpp"

namespace velora::core {

static_assert(kFxBlockFrames == kBlockFrames, "the effect block size is the engine block size");

std::unique_ptr<Effect> make_effect(EffectType type) {
  if (static_cast<std::uint8_t>(type) == 0 || static_cast<std::uint8_t>(type) >= params::kInsertTypeCount) return nullptr;
  std::unique_ptr<Effect> fx;
  switch (type) {
    case EffectType::kEq: fx = std::make_unique<EqEffect>(); break;
    case EffectType::kCompressor: fx = std::make_unique<CompressorEffect>(); break;
    default: fx = std::make_unique<PassThroughEffect>(type, effect_cost_units(type)); break;
  }
  fx->set_params(kDefaultInsertParams);
  fx->prepare(kSampleRate, kFxBlockFrames);
  return fx;
}

}  // namespace velora::core
