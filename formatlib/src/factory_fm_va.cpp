#include "velora/format/factory_fm_va.hpp"

#include <cstring>
#include "velora/format/hash.hpp"

namespace velora::format {

namespace {

FmVoiceParameters make_fm_voice(uint8_t algo, uint8_t fb, std::string_view name,
                                uint8_t op6_ratio, uint8_t op1_ratio = 1,
                                uint8_t op6_lvl = 85, uint8_t op1_lvl = 99) {
  FmVoiceParameters v{};
  v.algorithm = std::clamp<uint8_t>(algo, 1, 32);
  v.feedback = std::clamp<uint8_t>(fb, 0, 7);
  v.osc_key_sync = true;
  size_t n = std::min<size_t>(name.size(), 10);
  std::string s = sanitize_sysex_name(name.substr(0, n), 1);
  std::strncpy(v.name, s.c_str(), 10);

  for (int i = 0; i < 6; ++i) {
    auto& op = v.operators[i];
    op.rates = {99, 90, 70, 40};
    op.levels = {99, 80, 60, 0};
    op.kbd_breakpoint = 60;
    op.total_level = (i == 0) ? op1_lvl : 0;
    op.freq_coarse = 1;
  }
  // Carrier OP1
  v.operators[0].freq_coarse = op1_ratio;
  v.operators[0].total_level = op1_lvl;
  // Modulator OP6
  v.operators[5].freq_coarse = op6_ratio;
  v.operators[5].total_level = op6_lvl;
  v.operators[5].rates = {99, 85, 60, 30};

  return v;
}

VaVoiceParameters make_va_voice(std::string_view name, VaWaveform osc1, VaWaveform osc2,
                                float cutoff, float reso, float env_amt,
                                float att, float dec, float sus, float rel,
                                VaVoiceMode mode = VaVoiceMode::kPoly, uint8_t unison = 1) {
  VaVoiceParameters v{};
  v.name = std::string(name);
  v.osc1_wave = osc1;
  v.osc2_wave = osc2;
  v.cutoff_hz = cutoff;
  v.resonance = reso;
  v.filter_env_amount = env_amt;
  v.amp_attack_ms = att;
  v.amp_decay_ms = dec;
  v.amp_sustain_level = sus;
  v.amp_release_ms = rel;
  v.voice_mode = mode;
  v.unison_voices = unison;
  return v;
}

} // namespace

ParametricBundle create_factory_fm_va_bundle() {
  ParametricBundle bundle;
  bundle.set_name(kFactoryFmVaLibraryName);

  Uuid lib_id{};
  uuid_from_string(kFactoryFmVaLibraryId, lib_id);
  bundle.set_library_id(lib_id);

  auto add_fm = [&](std::string_view key, std::string_view name, Category cat,
                    uint8_t algo, uint8_t fb, uint8_t op6_ratio, uint8_t op1_ratio = 1,
                    uint8_t op6_lvl = 85, uint8_t op1_lvl = 99) {
    ParametricTimbre t;
    t.key = std::string(key);
    t.name = std::string(name);
    t.category = cat;
    t.engine_type_id = kEngineTypeFm;
    t.fm_parameters = make_fm_voice(algo, fb, name, op6_ratio, op1_ratio, op6_lvl, op1_lvl);
    (void)bundle.add_timbre(std::move(t));
  };

  auto add_va = [&](std::string_view key, std::string_view name, Category cat,
                    VaWaveform osc1, VaWaveform osc2, float cutoff, float reso,
                    float env_amt, float att, float dec, float sus, float rel,
                    VaVoiceMode mode = VaVoiceMode::kPoly, uint8_t unison = 1) {
    ParametricTimbre t;
    t.key = std::string(key);
    t.name = std::string(name);
    t.category = cat;
    t.engine_type_id = kEngineTypeVa;
    t.va_parameters = make_va_voice(name, osc1, osc2, cutoff, reso, env_amt, att, dec, sus, rel, mode, unison);
    (void)bundle.add_timbre(std::move(t));
  };

  // 1. Electric Pianos (Category::kEPiano) - 8 timbres
  add_fm("fm_ep_01", "Classic Stage Tine", Category::kEPiano, 5, 4, 14, 1, 86, 99);
  add_fm("fm_ep_02", "Crystal Bell EP", Category::kEPiano, 1, 3, 7, 1, 90, 99);
  add_fm("fm_ep_03", "Suitcase Vintage EP", Category::kEPiano, 8, 2, 1, 1, 75, 95);
  add_fm("fm_ep_04", "Dyno Bright EP", Category::kEPiano, 5, 6, 14, 1, 92, 99);
  add_fm("fm_ep_05", "Warm Chorus Tine", Category::kEPiano, 14, 3, 3, 1, 80, 99);
  add_fm("fm_ep_06", "Digital Stage EP", Category::kEPiano, 5, 5, 8, 1, 88, 99);
  add_va("va_ep_07", "Analog Warm EP", Category::kEPiano, VaWaveform::kPulse, VaWaveform::kTriangle, 2200.0f, 0.15f, 0.3f, 5.0f, 600.0f, 0.4f, 250.0f);
  add_va("va_ep_08", "Vintage Reed EP", Category::kEPiano, VaWaveform::kPulse, VaWaveform::kSaw, 1800.0f, 0.25f, 0.4f, 3.0f, 500.0f, 0.5f, 200.0f);

  // 2. Mallet & Bells (Category::kMalletBells) - 6 timbres
  add_fm("fm_bell_01", "Tubular Chime", Category::kMalletBells, 1, 4, 9, 1, 92, 99);
  add_fm("fm_bell_02", "Glockenspiel", Category::kMalletBells, 5, 2, 11, 2, 88, 99);
  add_fm("fm_bell_03", "Vibraphone Pure", Category::kMalletBells, 1, 1, 3, 1, 78, 99);
  add_fm("fm_bell_04", "Music Box Bells", Category::kMalletBells, 3, 3, 7, 2, 85, 99);
  add_fm("fm_bell_05", "Glass Marimba", Category::kMalletBells, 7, 2, 5, 1, 90, 99);
  add_va("va_bell_06", "Analog Kalimba", Category::kMalletBells, VaWaveform::kTriangle, VaWaveform::kSine, 2800.0f, 0.35f, 0.5f, 2.0f, 400.0f, 0.1f, 150.0f);

  // 3. Basses (Category::kBass) - 8 timbres
  add_fm("fm_bass_01", "Solid FM Punch Bass", Category::kBass, 1, 4, 1, 1, 95, 99);
  add_fm("fm_bass_02", "FM Slap Bass", Category::kBass, 5, 5, 3, 1, 92, 99);
  add_fm("fm_bass_03", "Wood Pick Bass", Category::kBass, 8, 3, 2, 1, 88, 99);
  add_va("va_bass_04", "Acid Reso 303 Bass", Category::kBass, VaWaveform::kSaw, VaWaveform::kPulse, 1200.0f, 0.75f, 0.6f, 5.0f, 250.0f, 0.1f, 100.0f, VaVoiceMode::kMono);
  add_va("va_bass_05", "Sub 808 Deep Bass", Category::kBass, VaWaveform::kSine, VaWaveform::kTriangle, 800.0f, 0.1f, 0.2f, 8.0f, 400.0f, 0.3f, 150.0f, VaVoiceMode::kMono);
  add_va("va_bass_06", "Dual Saw Octave Bass", Category::kBass, VaWaveform::kSaw, VaWaveform::kSaw, 1600.0f, 0.3f, 0.4f, 5.0f, 300.0f, 0.5f, 120.0f, VaVoiceMode::kMono);
  add_va("va_bass_07", "Fat Ladder Mini Bass", Category::kBass, VaWaveform::kSaw, VaWaveform::kPulse, 1400.0f, 0.4f, 0.5f, 4.0f, 350.0f, 0.4f, 150.0f, VaVoiceMode::kMono);
  add_va("va_bass_08", "Punchy Finger Bass", Category::kBass, VaWaveform::kPulse, VaWaveform::kTriangle, 1100.0f, 0.35f, 0.45f, 3.0f, 280.0f, 0.2f, 100.0f, VaVoiceMode::kMono);

  // 4. Brass (Category::kBrass) - 6 timbres
  add_va("va_brass_01", "Analog Brass Ensemble", Category::kBrass, VaWaveform::kSaw, VaWaveform::kSaw, 2400.0f, 0.25f, 0.5f, 25.0f, 450.0f, 0.7f, 300.0f);
  add_va("va_brass_02", "Power Synth Brass", Category::kBrass, VaWaveform::kSaw, VaWaveform::kPulse, 2800.0f, 0.3f, 0.6f, 15.0f, 400.0f, 0.65f, 250.0f);
  add_va("va_brass_03", "Fanfare Section", Category::kBrass, VaWaveform::kSaw, VaWaveform::kSaw, 3200.0f, 0.2f, 0.45f, 30.0f, 500.0f, 0.75f, 350.0f, VaVoiceMode::kPoly, 2);
  add_va("va_brass_04", "Warm Horn Swell", Category::kBrass, VaWaveform::kTriangle, VaWaveform::kSaw, 1600.0f, 0.15f, 0.4f, 60.0f, 600.0f, 0.8f, 400.0f);
  add_fm("fm_brass_05", "FM Metallic Brass", Category::kBrass, 22, 5, 2, 1, 90, 99);
  add_va("va_brass_06", "Bright Staccato Horns", Category::kBrass, VaWaveform::kPulse, VaWaveform::kSaw, 3500.0f, 0.35f, 0.7f, 5.0f, 200.0f, 0.2f, 100.0f);

  // 5. Strings (Category::kStrings) - 5 timbres
  add_va("va_str_01", "Analog String Ensemble", Category::kStrings, VaWaveform::kSaw, VaWaveform::kSaw, 3000.0f, 0.15f, 0.3f, 80.0f, 600.0f, 0.85f, 600.0f);
  add_va("va_str_02", "Vintage Solina Strings", Category::kStrings, VaWaveform::kPulse, VaWaveform::kSaw, 2800.0f, 0.2f, 0.35f, 60.0f, 500.0f, 0.8f, 500.0f);
  add_va("va_str_03", "Warm Chamber Strings", Category::kStrings, VaWaveform::kSaw, VaWaveform::kTriangle, 2200.0f, 0.1f, 0.25f, 100.0f, 700.0f, 0.9f, 700.0f);
  add_fm("fm_str_04", "FM Plucked Harp", Category::kStrings, 1, 2, 4, 1, 88, 99);
  add_va("va_str_05", "Analog Pizzicato", Category::kStrings, VaWaveform::kPulse, VaWaveform::kTriangle, 2600.0f, 0.4f, 0.6f, 3.0f, 180.0f, 0.05f, 80.0f);

  // 6. Synth Lead (Category::kSynthLead) - 7 timbres
  add_va("va_lead_01", "Sync Screamer Lead", Category::kSynthLead, VaWaveform::kSaw, VaWaveform::kPulse, 3800.0f, 0.45f, 0.65f, 5.0f, 350.0f, 0.7f, 200.0f, VaVoiceMode::kMono);
  add_va("va_lead_02", "Vintage Solo Saw Lead", Category::kSynthLead, VaWaveform::kSaw, VaWaveform::kSaw, 3200.0f, 0.3f, 0.4f, 8.0f, 400.0f, 0.8f, 250.0f, VaVoiceMode::kLegato);
  add_va("va_lead_03", "Square Pulse Lead", Category::kSynthLead, VaWaveform::kPulse, VaWaveform::kPulse, 2600.0f, 0.35f, 0.5f, 5.0f, 300.0f, 0.75f, 200.0f, VaVoiceMode::kMono);
  add_va("va_lead_04", "Hyper Searing Lead", Category::kSynthLead, VaWaveform::kSaw, VaWaveform::kSaw, 4500.0f, 0.4f, 0.55f, 4.0f, 350.0f, 0.85f, 300.0f, VaVoiceMode::kMono, 4);
  add_va("va_lead_05", "Pure Sine Whistle Lead", Category::kSynthLead, VaWaveform::kSine, VaWaveform::kTriangle, 5000.0f, 0.1f, 0.1f, 12.0f, 450.0f, 0.9f, 300.0f, VaVoiceMode::kLegato);
  add_fm("fm_lead_06", "FM Feedback Screamer", Category::kSynthLead, 6, 7, 1, 1, 95, 99);
  add_va("va_lead_07", "Fifth Interval Lead", Category::kSynthLead, VaWaveform::kSaw, VaWaveform::kPulse, 3400.0f, 0.3f, 0.5f, 6.0f, 380.0f, 0.75f, 250.0f, VaVoiceMode::kMono);

  // 7. Synth Pad (Category::kSynthPad) - 6 timbres
  add_va("va_pad_01", "Celestial Sweep Pad", Category::kSynthPad, VaWaveform::kSaw, VaWaveform::kSaw, 2200.0f, 0.35f, 0.4f, 120.0f, 800.0f, 0.85f, 900.0f, VaVoiceMode::kPoly, 2);
  add_va("va_pad_02", "Hyper Saw Warm Pad", Category::kSynthPad, VaWaveform::kSaw, VaWaveform::kSaw, 2600.0f, 0.2f, 0.3f, 90.0f, 700.0f, 0.8f, 800.0f, VaVoiceMode::kPoly, 4);
  add_fm("fm_pad_03", "Glass Ambient Pad", Category::kSynthPad, 16, 4, 3, 1, 82, 99);
  add_va("va_pad_04", "Analog Dream Pad", Category::kSynthPad, VaWaveform::kTriangle, VaWaveform::kPulse, 1800.0f, 0.15f, 0.25f, 150.0f, 900.0f, 0.9f, 1000.0f);
  add_fm("fm_pad_05", "Ethereal Breath Pad", Category::kSynthPad, 20, 3, 7, 1, 80, 99);
  add_va("va_pad_06", "Silk Atmosphere Pad", Category::kSynthPad, VaWaveform::kPulse, VaWaveform::kSaw, 2000.0f, 0.1f, 0.2f, 180.0f, 1000.0f, 0.95f, 1200.0f);

  // 8. Synth Poly (Category::kSynthPoly) - 6 timbres
  add_va("va_poly_01", "Super Saw Poly", Category::kSynthPoly, VaWaveform::kSaw, VaWaveform::kSaw, 3200.0f, 0.25f, 0.45f, 8.0f, 400.0f, 0.7f, 300.0f, VaVoiceMode::kPoly, 2);
  add_va("va_poly_02", "80s Poly Chords", Category::kSynthPoly, VaWaveform::kPulse, VaWaveform::kSaw, 2800.0f, 0.35f, 0.5f, 10.0f, 450.0f, 0.65f, 350.0f);
  add_va("va_poly_03", "Analog Fifth Poly", Category::kSynthPoly, VaWaveform::kSaw, VaWaveform::kPulse, 2900.0f, 0.2f, 0.4f, 12.0f, 420.0f, 0.7f, 320.0f);
  add_fm("fm_poly_04", "Bright Digital Poly", Category::kSynthPoly, 5, 5, 2, 1, 89, 99);
  add_fm("fm_poly_05", "Pluck Poly Synth", Category::kSynthPoly, 1, 3, 5, 1, 92, 99);
  add_va("va_poly_06", "Resonant Filter Comp", Category::kSynthPoly, VaWaveform::kSaw, VaWaveform::kTriangle, 2400.0f, 0.55f, 0.6f, 6.0f, 300.0f, 0.5f, 200.0f);

  return bundle;
}

} // namespace velora::format
