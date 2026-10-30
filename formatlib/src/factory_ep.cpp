#include "velora/format/factory_ep.hpp"

#include <algorithm>
#include <cstring>
#include "velora/format/hash.hpp"

namespace velora::format {

namespace {

EpVoiceParameters make_ep_voice(std::string_view name, EpInstrumentType type,
                                float hardness, float pickup_pos, float pickup_lvl,
                                float damper, float attack, float decay, float coupling,
                                uint8_t brightness = 64) {
  EpVoiceParameters p{};
  p.name = std::string(name);
  p.instrument_type = type;
  p.hammer_hardness = hardness;
  p.pickup_position = pickup_pos;
  p.pickup_level = pickup_lvl;
  p.damper_noise = damper;
  p.attack_noise = attack;
  p.decay_rate = decay;
  p.tone_bar_coupling = coupling;
  p.brightness_offset = brightness;
  return p;
}

}  // namespace

ParametricBundle create_factory_ep_bundle() {
  ParametricBundle bundle;
  bundle.set_name(kFactoryEpLibraryName);

  Uuid lib_id{};
  uuid_from_string(kFactoryEpLibraryId, lib_id);
  bundle.set_library_id(lib_id);

  auto add_ep = [&](std::string_view key, std::string_view name, EpInstrumentType type,
                    float hardness, float pickup_pos, float pickup_lvl,
                    float damper, float attack, float decay, float coupling,
                    uint8_t brightness = 64) {
    ParametricTimbre t;
    t.key = std::string(key);
    t.name = std::string(name);
    t.category = Category::kEPiano;
    t.engine_type_id = kEngineTypeEp;
    t.ep_parameters = make_ep_voice(name, type, hardness, pickup_pos, pickup_lvl,
                                    damper, attack, decay, coupling, brightness);
    (void)bundle.add_timbre(std::move(t));
  };

  // 1. Tine electric pianos (7 timbres: 1 Init + 6 voicings)
  add_ep("ep_init_tine", "Init Tine", EpInstrumentType::kTine,
         0.50f, 0.50f, 0.70f, 0.30f, 0.30f, 0.50f, 0.50f);
  add_ep("ep_tine_stage_73_pure", "Stage 73 Pure", EpInstrumentType::kTine,
         0.45f, 0.50f, 0.70f, 0.28f, 0.25f, 0.60f, 0.55f);
  add_ep("ep_tine_stage_73_bright", "Stage 73 Bright", EpInstrumentType::kTine,
         0.70f, 0.65f, 0.80f, 0.25f, 0.40f, 0.55f, 0.50f);
  add_ep("ep_tine_suitcase_warm", "Suitcase 73 Warm", EpInstrumentType::kTine,
         0.40f, 0.35f, 0.65f, 0.35f, 0.20f, 0.70f, 0.65f);
  add_ep("ep_tine_suitcase_88_bell", "Suitcase 88 Bell", EpInstrumentType::kTine,
         0.65f, 0.75f, 0.75f, 0.22f, 0.35f, 0.65f, 0.60f);
  add_ep("ep_tine_studio_vintage", "Studio Vintage Tine", EpInstrumentType::kTine,
         0.55f, 0.48f, 0.72f, 0.30f, 0.30f, 0.58f, 0.52f);
  add_ep("ep_tine_dyno_mod", "Dyno Mod Tine", EpInstrumentType::kTine,
         0.85f, 0.85f, 0.90f, 0.20f, 0.50f, 0.50f, 0.45f);

  // 2. Reed electric pianos (5 timbres: 1 Init + 4 voicings)
  add_ep("ep_init_reed", "Init Reed", EpInstrumentType::kReed,
         0.60f, 0.40f, 0.75f, 0.25f, 0.35f, 0.55f, 0.40f);
  add_ep("ep_reed_200a_classic", "200A Classic Reed", EpInstrumentType::kReed,
         0.58f, 0.42f, 0.78f, 0.22f, 0.32f, 0.52f, 0.42f);
  add_ep("ep_reed_200a_bark", "200A Bark Reed", EpInstrumentType::kReed,
         0.82f, 0.55f, 0.85f, 0.18f, 0.45f, 0.48f, 0.38f);
  add_ep("ep_reed_140b_early", "140B Early Reed", EpInstrumentType::kReed,
         0.48f, 0.32f, 0.68f, 0.30f, 0.28f, 0.62f, 0.48f);
  add_ep("ep_reed_vintage_bell", "Vintage Reed Bell", EpInstrumentType::kReed,
         0.72f, 0.60f, 0.80f, 0.20f, 0.38f, 0.56f, 0.44f);

  return bundle;
}

}  // namespace velora::format
