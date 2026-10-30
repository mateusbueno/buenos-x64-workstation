#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>

#include "ws_v1.pb.h"
#include "velora/ui/screens/surface_skeleton.hpp"
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

class WsClient;

enum class PartSection {
  Timbre = 0,
  ZoneKeyboard,
  LevelOutput,
  SoundEngine,
  EnvelopeEQ,
  EffectsSends,
  Arpeggiator,
  Count
};

struct ArpSettings {
  bool on{false};
  int32_t mode{0}; // 0: Up, 1: Down, 2: Up-Down, 3: Random, 4: As played
  int32_t rate{1}; // 0: 1/4, 1: 1/8, 2: 1/16, 3: 1/32, 4: 1/4T, 5: 1/8T, 6: 1/16T, 7: 1/32T
  int32_t octaves{1}; // 1..4
  float gate{0.50f}; // 0.05..1.00 (5% to 100%)
  float swing{0.50f}; // 0.50..0.75 (50% to 75%)
  bool latch{false};
};

enum class EngineType {
  Sample = 1,
  Organ = 2,
  Fm = 3,
  Va = 4,
  ModeledEp = 5
};

enum class EpSoundGroup {
  Model = 0,
  Hammer,
  Pickup,
  Damper,
  Count
};

struct EpSettings {
  int32_t model_type{0}; // 0: Tine, 1: Reed
  float hammer_hardness{0.50f};
  float pickup_position{0.50f};
  float pickup_level{0.70f};
  float damper_noise{0.30f};
  float attack_noise{0.30f};
  float decay_rate{0.50f};
  float tone_bar_coupling{0.50f};
  int32_t brightness_offset{64};
};

enum class FmSoundGroup {
  Algorithm = 0,
  Operators,
  PitchEnv,
  Lfo,
  Count
};

struct FmOperator {
  float ratio{1.0f};      // 0.50 .. 32.00
  int32_t level{99};      // 0 .. 99
  int32_t attack{20};     // 0 .. 99
  int32_t decay{60};      // 0 .. 99
  int32_t sustain{70};    // 0 .. 99
  int32_t release{50};    // 0 .. 99
};

struct FmSettings {
  int32_t algorithm{5};    // 1 .. 32
  int32_t feedback{4};     // 0 .. 7 (on OP 6)
  std::array<FmOperator, 6> operators;
  int32_t pitch_rates[4]{99, 99, 99, 60};
  int32_t pitch_levels[4]{50, 50, 50, 50};
  int32_t lfo_wave{0};     // 0: Sine, 1: Triangle, 2: Saw up, 3: Saw down, 4: Square, 5: S&H
  int32_t lfo_speed{35};   // 0 .. 99
  int32_t lfo_delay{12};   // 0 .. 99
  int32_t lfo_pitch_depth{8}; // 0 .. 99
  int32_t lfo_amp_depth{0};   // 0 .. 99
  bool lfo_key_sync{true};
  int32_t brightness_offset{64}; // 0 .. 127
};

enum class VaSoundGroup {
  Oscillators = 0,
  Mixer,
  Filter,
  Envelopes,
  Lfo,
  Voice,
  Count
};

struct VaSettings {
  int32_t osc1_wave{0};    // 0: Saw, 1: Pulse, 2: Triangle, 3: Sine
  int32_t osc1_octave{0};  // -2 .. +2
  int32_t osc1_detune{0};  // -50 .. +50 cents
  int32_t osc1_pulse_width{50}; // 0 .. 100 %
  int32_t osc2_wave{1};    // Pulse
  int32_t osc2_octave{0};
  int32_t osc2_detune{5};
  int32_t osc2_pulse_width{50};

  int32_t mix_osc1{80};    // 0 .. 100
  int32_t mix_osc2{65};
  int32_t mix_sub{20};
  int32_t mix_noise{0};

  int32_t filter_type{0};  // 0: LP 24, 1: LP 12, 2: BP, 3: HP
  int32_t cutoff{74};      // 0 .. 127
  int32_t resonance{22};   // 0 .. 100
  int32_t drive{10};       // 0 .. 100
  int32_t key_track{50};   // 0 .. 100 %
  int32_t env_amount{40};  // -100 .. +100
  int32_t velocity{35};    // 0 .. 100

  int32_t amp_attack{4};
  int32_t amp_decay{52};
  int32_t amp_sustain{70};
  int32_t amp_release{38};
  int32_t filter_attack{4};
  int32_t filter_decay{52};
  int32_t filter_sustain{70};
  int32_t filter_release{38};

  int32_t lfo_wave{1};     // 0: Sine, 1: Triangle, 2: Saw up, 3: Square, 4: S&H
  int32_t lfo_rate{42};
  int32_t lfo_delay{0};
  bool lfo_key_sync{false};
  int32_t lfo_to_pitch{6};
  int32_t lfo_to_filter{20};
  int32_t lfo_to_amp{0};

  int32_t voice_mode{0};   // 0: Poly, 1: Mono, 2: Legato
  int32_t unison_voices{4}; // 1 .. 8
  int32_t unison_detune{18}; // 0 .. 100
  int32_t glide_time{0};   // 0 .. 100
  int32_t glide_mode{1};   // 0: Glide always, 1: Glide legato only

  int32_t brightness_offset{64}; // 0 .. 127
};

struct TimbreItem {
  std::string id;
  std::string name;
  std::string category;
};

class PartView {
 public:
  using CloseCallback = std::function<void()>;
  using PartEditCallback = std::function<void(int32_t part_idx)>;
  using OpenInsertsCallback = std::function<void(int32_t part_idx)>;
  using TimbreChangeCallback = std::function<void(int32_t part_idx, const std::string& timbre_id)>;
  using OpenLibraryCallback = std::function<void(int32_t part_idx)>;
  using ArpEditCallback = std::function<void(int32_t part_idx, const ArpSettings& settings)>;
  using TempoEditCallback = std::function<void(float bpm)>;

  explicit PartView(WsClient* client = nullptr);
  ~PartView();

  bool create(lv_obj_t* parent = nullptr);

  void on_panel_event(const PanelEvent& ev);
  void update_from_state(const ws::v1::State& state);

  void set_on_close(CloseCallback cb) { on_close_ = std::move(cb); }
  void set_on_edit(PartEditCallback cb) { on_edit_ = std::move(cb); }
  void set_on_open_inserts(OpenInsertsCallback cb) { on_open_inserts_ = std::move(cb); }
  void set_on_timbre_change(TimbreChangeCallback cb) { on_timbre_change_ = std::move(cb); }
  void set_on_open_library(OpenLibraryCallback cb) { on_open_library_ = std::move(cb); }
  void set_on_arp_edit(ArpEditCallback cb) { on_arp_edit_ = std::move(cb); }
  void set_on_tempo_edit(TempoEditCallback cb) { on_tempo_edit_ = std::move(cb); }

  // Part data accessors & mutators
  void set_part_index(int32_t idx);
  int32_t part_index() const noexcept { return part_idx_; }

  void set_timbre(const std::string& name, const std::string& category);
  const std::string& timbre_name() const noexcept { return timbre_name_; }
  const std::string& category_name() const noexcept { return category_name_; }

  void set_missing_library(bool missing);
  bool is_missing_library() const noexcept { return is_missing_library_; }

  void set_favorite(bool fav);
  bool is_favorite() const noexcept { return is_favorite_; }
  void toggle_favorite();

  void set_zone(int32_t low_key, int32_t high_key, int32_t octave, int32_t transpose, bool kb_on);
  int32_t low_key() const noexcept { return low_key_; }
  int32_t high_key() const noexcept { return high_key_; }
  int32_t octave() const noexcept { return octave_; }
  int32_t transpose() const noexcept { return transpose_; }
  bool keyboard_on() const noexcept { return keyboard_on_; }

  void set_level_output(float volume, bool aux_output);
  float volume() const noexcept { return volume_; }
  bool is_aux_output() const noexcept { return aux_output_; }

  void set_engine_type(EngineType type);
  EngineType engine_type() const noexcept { return engine_type_; }
  bool is_organ() const noexcept { return engine_type_ == EngineType::Organ; }
  bool is_fm() const noexcept { return engine_type_ == EngineType::Fm; }
  bool is_va() const noexcept { return engine_type_ == EngineType::Va; }
  bool is_ep() const noexcept { return engine_type_ == EngineType::ModeledEp; }
  bool is_sample() const noexcept { return engine_type_ == EngineType::Sample; }

  void set_engine_unavailable(bool unavail, const std::string& required_version = "1.4");
  bool is_engine_unavailable() const noexcept { return is_engine_unavailable_; }
  const std::string& required_version() const noexcept { return required_version_; }

  // FM accessors & mutators (Story 12.11)
  void set_fm_group(FmSoundGroup group);
  FmSoundGroup fm_group() const noexcept { return fm_sound_group_; }
  const FmSettings& fm_settings() const noexcept { return fm_settings_; }
  FmSettings& fm_settings() noexcept { return fm_settings_; }
  std::string fm_modulators_string() const;

  // VA accessors & mutators (Story 12.12)
  void set_va_group(VaSoundGroup group);
  VaSoundGroup va_group() const noexcept { return va_sound_group_; }
  const VaSettings& va_settings() const noexcept { return va_settings_; }
  VaSettings& va_settings() noexcept { return va_settings_; }

  // Modeled EP accessors & mutators (Story 14.9)
  void set_ep_group(EpSoundGroup group);
  EpSoundGroup ep_group() const noexcept { return ep_sound_group_; }
  const EpSettings& ep_settings() const noexcept { return ep_settings_; }
  EpSettings& ep_settings() noexcept { return ep_settings_; }

  bool sound_group_focus() const noexcept { return sound_group_focus_; }
  bool is_sound_group_focus() const noexcept { return sound_group_focus_; }
  bool is_editing_field() const noexcept { return is_editing_field_; }
  bool handle_exit_step();

  // Sound Engine parameters
  void set_sample_sound(float brightness, float cutoff_hz, float resonance, float vel_sense);
  float brightness() const noexcept { return brightness_; }
  float cutoff_hz() const noexcept { return cutoff_hz_; }
  float resonance() const noexcept { return resonance_; }
  float vel_sense() const noexcept { return vel_sense_; }

  void set_organ_drawbars(const uint8_t drawbars[9]);
  uint8_t drawbar(size_t idx) const { return (idx < 9) ? organ_drawbars_[idx] : 0; }
  std::string registration_string() const;
  void set_organ_percussion(bool on) { organ_percussion_on_ = on; mark_edits(); }
  bool organ_percussion() const noexcept { return organ_percussion_on_; }
  void set_organ_key_click(int32_t val) { organ_key_click_ = std::clamp(val, 0, 8); mark_edits(); }
  int32_t organ_key_click() const noexcept { return organ_key_click_; }
  void set_organ_vibrato(int32_t val) { organ_vibrato_ = std::clamp(val, 0, 6); mark_edits(); }
  int32_t organ_vibrato() const noexcept { return organ_vibrato_; }
  void set_organ_rotary(int32_t speed) { organ_rotary_speed_ = std::clamp(speed, 0, 2); mark_edits(); }
  int32_t organ_rotary() const noexcept { return organ_rotary_speed_; }

  // Envelope & EQ
  void set_envelope(float attack_ms, float decay_ms, float sustain, float release_ms);
  float attack_ms() const noexcept { return attack_ms_; }
  float decay_ms() const noexcept { return decay_ms_; }
  float sustain() const noexcept { return sustain_; }
  float release_ms() const noexcept { return release_ms_; }

  void set_eq(float low_db, float mid_db, float mid_freq_hz, float high_db);
  float eq_low_db() const noexcept { return eq_low_db_; }
  float eq_mid_db() const noexcept { return eq_mid_db_; }
  float eq_mid_freq_hz() const noexcept { return eq_mid_freq_hz_; }
  float eq_high_db() const noexcept { return eq_high_db_; }

  // Effects Sends
  void set_sends(float reverb_send, float delay_send);
  float reverb_send() const noexcept { return reverb_send_; }
  float delay_send() const noexcept { return delay_send_; }
  void open_inserts_chain();

  // Arpeggiator (Story 7.5)
  void set_arp(bool on, int32_t mode, int32_t rate, int32_t octaves, float gate, float swing, bool latch);
  const ArpSettings& arp() const noexcept { return arp_settings_[part_idx_]; }

  // Tempo (Story 7.5 & 7.8)
  void set_tempo(float bpm);
  float tempo() const noexcept { return tempo_bpm_; }
  void set_clock_source(const std::string& source, bool read_only);
  bool is_tempo_read_only() const noexcept { return tempo_read_only_; }
  const std::string& clock_source() const noexcept { return clock_source_; }

  void set_power(bool on);
  bool is_powered_on() const noexcept { return power_on_; }

  bool has_unsaved_edits() const noexcept { return has_unsaved_edits_; }

  void set_section(PartSection sec);
  PartSection current_section() const noexcept { return current_section_; }

  // Timbre picker modal
  void open_timbre_picker();
  void close_timbre_picker();
  bool is_timbre_picker_open() const noexcept { return timbre_picker_open_; }
  void select_timbre(size_t index);

  // Note event from keybed for setting low/high key
  void on_midi_note(uint8_t note_number);

  SurfaceSkeleton& skeleton() noexcept { return skeleton_; }
  const SurfaceSkeleton& skeleton() const noexcept { return skeleton_; }

  // Testing helpers
  int32_t highlighted_field() const noexcept { return field_focus_idx_; }
  lv_obj_t* change_timbre_button() const noexcept { return btn_change_timbre_; }
  lv_obj_t* timbre_picker_modal() const noexcept { return picker_modal_; }

 private:
  void rebuild_top_bar();
  void rebuild_panel();
  void update_nav_ui();
  void update_field_focus_ui();
  void mark_edits();

  static void on_nav_item_click(lv_event_t* e);
  static void on_field_click(lv_event_t* e);
  static void on_change_timbre_click(lv_event_t* e);
  static void on_picker_item_click(lv_event_t* e);
  static void on_power_click(lv_event_t* e);

  WsClient* client_{nullptr};
  SurfaceSkeleton skeleton_;
  CloseCallback on_close_;
  PartEditCallback on_edit_;
  OpenInsertsCallback on_open_inserts_;
  TimbreChangeCallback on_timbre_change_;
  OpenLibraryCallback on_open_library_;
  ArpEditCallback on_arp_edit_;
  TempoEditCallback on_tempo_edit_;

  int32_t part_idx_{0};
  std::string timbre_name_{"German Grand D-274"};
  std::string category_name_{"ACOUSTIC PIANO"};
  bool is_missing_library_{false};
  bool is_favorite_{false};
  int32_t low_key_{21};   // A0
  int32_t high_key_{108}; // C8
  int32_t octave_{0};
  int32_t transpose_{0};
  bool keyboard_on_{true};
  float volume_{0.85f};
  bool aux_output_{false};
  bool power_on_{true};
  bool has_unsaved_edits_{false};

  // Sound Engine (Sample)
  float brightness_{0.75f};
  float cutoff_hz_{12000.0f};
  float resonance_{0.20f};
  float vel_sense_{0.80f};

  // Sound Engine (Organ)
  uint8_t organ_drawbars_[9]{8, 8, 8, 0, 0, 0, 0, 0, 0};
  bool organ_percussion_on_{true};
  int32_t organ_key_click_{4};
  int32_t organ_vibrato_{3}; // C3
  int32_t organ_rotary_speed_{1}; // Slow

  // Sound Engine (FM, VA & Modeled EP)
  EngineType engine_type_{EngineType::Sample};
  bool is_engine_unavailable_{false};
  std::string required_version_{"1.4"};
  FmSettings fm_settings_{};
  VaSettings va_settings_{};
  EpSettings ep_settings_{};
  FmSoundGroup fm_sound_group_{FmSoundGroup::Algorithm};
  VaSoundGroup va_sound_group_{VaSoundGroup::Oscillators};
  EpSoundGroup ep_sound_group_{EpSoundGroup::Model};
  bool sound_group_focus_{true};
  bool is_editing_field_{false};

  // Envelope & EQ
  float attack_ms_{10.0f};
  float decay_ms_{400.0f};
  float sustain_{0.70f};
  float release_ms_{350.0f};
  float eq_low_db_{0.0f};
  float eq_mid_db_{0.0f};
  float eq_mid_freq_hz_{1000.0f};
  float eq_high_db_{0.0f};

  // Effects Sends
  float reverb_send_{0.25f};
  float delay_send_{0.15f};

  // Arpeggiator (Story 7.5 & 7.8)
  std::array<ArpSettings, 8> arp_settings_{};
  float tempo_bpm_{120.0f};
  bool tempo_read_only_{false};
  std::string clock_source_{"Internal"};

  PartSection current_section_{PartSection::Timbre};
  int32_t field_focus_idx_{0}; // Focused field inside current section

  // UI objects
  lv_obj_t* btn_power_{nullptr};
  lv_obj_t* lbl_power_{nullptr};
  lv_obj_t* nav_col_{nullptr};
  std::vector<lv_obj_t*> nav_item_objs_;

  lv_obj_t* panel_col_{nullptr};
  lv_obj_t* btn_change_timbre_{nullptr};
  std::vector<lv_obj_t*> field_objs_;
  std::vector<lv_obj_t*> field_val_objs_;

  // Timbre picker modal
  lv_obj_t* picker_modal_{nullptr};
  lv_obj_t* picker_card_{nullptr};
  lv_obj_t* picker_list_{nullptr};
  std::vector<TimbreItem> timbres_catalog_;
  int32_t picker_focus_idx_{0};
  bool timbre_picker_open_{false};
};

}  // namespace velora::ui
