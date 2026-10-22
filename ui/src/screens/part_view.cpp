#include "velora/ui/screens/part_view.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/ws_client.hpp"

#include <algorithm>
#include <cstdio>

namespace velora::ui {

namespace {

std::string midi_note_to_name(int32_t note) {
  static const char* note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (note < 0 || note > 127) return "—";
  int oct = (note / 12) - 1;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%s%d (%d)", note_names[note % 12], oct, note);
  return std::string(buf);
}

std::string format_cldr_percent(int pct) {
  const std::string& lang = Locale::instance().current_locale();
  if (lang == "es" || lang == "pt-BR") {
    return std::to_string(pct) + " %";
  }
  return std::to_string(pct) + "%";
}

const char* kArpRateNames[] = {
  "1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T", "1/32T"
};

const char* kArpModeKeys[] = {
  "part_view.arp.mode.up",
  "part_view.arp.mode.down",
  "part_view.arp.mode.up_down",
  "part_view.arp.mode.random",
  "part_view.arp.mode.as_played"
};

const char* kFmLfoWaveKeys[] = {
  "wave.sine", "wave.triangle", "wave.saw_up", "wave.saw_down", "wave.square", "wave.sh"
};

const char* kVaLfoWaveKeys[] = {
  "wave.sine", "wave.triangle", "wave.saw_up", "wave.square", "wave.sh"
};

const char* kVaOscWaveKeys[] = {
  "wave.saw_up", "wave.pulse", "wave.triangle", "wave.sine"
};

const char* kVaFilterTypeKeys[] = {
  "filter.lp24", "filter.lp12", "filter.bp", "filter.hp"
};

const char* kVaVoiceModeKeys[] = {
  "voice.poly", "voice.mono", "voice.legato"
};

const char* kVaGlideModeKeys[] = {
  "glide.always", "glide.legato_only"
};

int get_fm_field_count(FmSoundGroup group) {
  switch (group) {
    case FmSoundGroup::Algorithm: return 2;
    case FmSoundGroup::Operators: return 12;
    case FmSoundGroup::PitchEnv: return 8;
    case FmSoundGroup::Lfo: return 6;
    default: return 0;
  }
}

int get_va_field_count(VaSoundGroup group) {
  switch (group) {
    case VaSoundGroup::Oscillators: return 8;
    case VaSoundGroup::Mixer: return 4;
    case VaSoundGroup::Filter: return 7;
    case VaSoundGroup::Envelopes: return 8;
    case VaSoundGroup::Lfo: return 7;
    case VaSoundGroup::Voice: return 5;
    default: return 0;
  }
}

void modify_fm_param(FmSettings& s, FmSoundGroup group, int field_idx, int delta) {
  if (group == FmSoundGroup::Algorithm) {
    if (field_idx == 0) s.algorithm = std::clamp(s.algorithm + delta, 1, 32);
    else if (field_idx == 1) s.feedback = std::clamp(s.feedback + delta, 0, 7);
  } else if (group == FmSoundGroup::Operators) {
    int op = std::clamp(field_idx / 2, 0, 5);
    bool is_ratio = (field_idx % 2 == 0);
    if (is_ratio) {
      s.operators[op].ratio = std::clamp(s.operators[op].ratio + (delta * 0.25f), 0.50f, 32.0f);
    } else {
      s.operators[op].level = std::clamp(s.operators[op].level + delta, 0, 99);
    }
  } else if (group == FmSoundGroup::PitchEnv) {
    if (field_idx < 4) {
      s.pitch_rates[field_idx] = std::clamp(s.pitch_rates[field_idx] + delta, 0, 99);
    } else {
      int lev_idx = field_idx - 4;
      s.pitch_levels[lev_idx] = std::clamp(s.pitch_levels[lev_idx] + delta, 0, 99);
    }
  } else if (group == FmSoundGroup::Lfo) {
    switch (field_idx) {
      case 0: s.lfo_wave = std::clamp(s.lfo_wave + delta, 0, 5); break;
      case 1: s.lfo_speed = std::clamp(s.lfo_speed + delta, 0, 99); break;
      case 2: s.lfo_delay = std::clamp(s.lfo_delay + delta, 0, 99); break;
      case 3: s.lfo_pitch_depth = std::clamp(s.lfo_pitch_depth + delta, 0, 99); break;
      case 4: s.lfo_amp_depth = std::clamp(s.lfo_amp_depth + delta, 0, 99); break;
      case 5: s.lfo_key_sync = !s.lfo_key_sync; break;
    }
  }
}

void modify_va_param(VaSettings& s, VaSoundGroup group, int field_idx, int delta) {
  if (group == VaSoundGroup::Oscillators) {
    switch (field_idx) {
      case 0: s.osc1_wave = std::clamp(s.osc1_wave + delta, 0, 3); break;
      case 1: s.osc1_octave = std::clamp(s.osc1_octave + delta, -2, 2); break;
      case 2: s.osc1_detune = std::clamp(s.osc1_detune + delta, -50, 50); break;
      case 3: s.osc1_pulse_width = std::clamp(s.osc1_pulse_width + delta, 0, 100); break;
      case 4: s.osc2_wave = std::clamp(s.osc2_wave + delta, 0, 3); break;
      case 5: s.osc2_octave = std::clamp(s.osc2_octave + delta, -2, 2); break;
      case 6: s.osc2_detune = std::clamp(s.osc2_detune + delta, -50, 50); break;
      case 7: s.osc2_pulse_width = std::clamp(s.osc2_pulse_width + delta, 0, 100); break;
    }
  } else if (group == VaSoundGroup::Mixer) {
    switch (field_idx) {
      case 0: s.mix_osc1 = std::clamp(s.mix_osc1 + delta, 0, 100); break;
      case 1: s.mix_osc2 = std::clamp(s.mix_osc2 + delta, 0, 100); break;
      case 2: s.mix_sub = std::clamp(s.mix_sub + delta, 0, 100); break;
      case 3: s.mix_noise = std::clamp(s.mix_noise + delta, 0, 100); break;
    }
  } else if (group == VaSoundGroup::Filter) {
    switch (field_idx) {
      case 0: s.filter_type = std::clamp(s.filter_type + delta, 0, 3); break;
      case 1: s.cutoff = std::clamp(s.cutoff + delta, 0, 127); break;
      case 2: s.resonance = std::clamp(s.resonance + delta, 0, 100); break;
      case 3: s.drive = std::clamp(s.drive + delta, 0, 100); break;
      case 4: s.key_track = std::clamp(s.key_track + delta, 0, 100); break;
      case 5: s.env_amount = std::clamp(s.env_amount + delta, -100, 100); break;
      case 6: s.velocity = std::clamp(s.velocity + delta, 0, 100); break;
    }
  } else if (group == VaSoundGroup::Envelopes) {
    switch (field_idx) {
      case 0: s.amp_attack = std::clamp(s.amp_attack + delta, 0, 100); break;
      case 1: s.amp_decay = std::clamp(s.amp_decay + delta, 0, 100); break;
      case 2: s.amp_sustain = std::clamp(s.amp_sustain + delta, 0, 100); break;
      case 3: s.amp_release = std::clamp(s.amp_release + delta, 0, 100); break;
      case 4: s.filter_attack = std::clamp(s.filter_attack + delta, 0, 100); break;
      case 5: s.filter_decay = std::clamp(s.filter_decay + delta, 0, 100); break;
      case 6: s.filter_sustain = std::clamp(s.filter_sustain + delta, 0, 100); break;
      case 7: s.filter_release = std::clamp(s.filter_release + delta, 0, 100); break;
    }
  } else if (group == VaSoundGroup::Lfo) {
    switch (field_idx) {
      case 0: s.lfo_wave = std::clamp(s.lfo_wave + delta, 0, 4); break;
      case 1: s.lfo_rate = std::clamp(s.lfo_rate + delta, 0, 100); break;
      case 2: s.lfo_delay = std::clamp(s.lfo_delay + delta, 0, 100); break;
      case 3: s.lfo_key_sync = !s.lfo_key_sync; break;
      case 4: s.lfo_to_pitch = std::clamp(s.lfo_to_pitch + delta, 0, 100); break;
      case 5: s.lfo_to_filter = std::clamp(s.lfo_to_filter + delta, 0, 100); break;
      case 6: s.lfo_to_amp = std::clamp(s.lfo_to_amp + delta, 0, 100); break;
    }
  } else if (group == VaSoundGroup::Voice) {
    switch (field_idx) {
      case 0: s.voice_mode = std::clamp(s.voice_mode + delta, 0, 2); break;
      case 1: s.unison_voices = std::clamp(s.unison_voices + delta, 1, 8); break;
      case 2: s.unison_detune = std::clamp(s.unison_detune + delta, 0, 100); break;
      case 3: s.glide_time = std::clamp(s.glide_time + delta, 0, 100); break;
      case 4: s.glide_mode = (s.glide_mode == 0 ? 1 : 0); break;
    }
  }
}

}  // namespace

PartView::PartView(WsClient* client)
    : client_(client) {
  // FM default settings (Story 12.10 & 12.11)
  fm_settings_.algorithm = 5;
  fm_settings_.feedback = 4;
  fm_settings_.operators[0] = {1.00f, 99, 20, 60, 70, 50}; // OP 1 Carrier
  fm_settings_.operators[1] = {2.00f, 78, 10, 60, 40, 52}; // OP 2 Modulator
  fm_settings_.operators[2] = {1.00f, 99, 30, 80, 70, 55}; // OP 3 Carrier
  fm_settings_.operators[3] = {3.50f, 62, 8, 40, 44, 54};  // OP 4 Modulator
  fm_settings_.operators[4] = {0.50f, 90, 20, 80, 70, 55}; // OP 5 Carrier
  fm_settings_.operators[5] = {7.00f, 41, 6, 50, 46, 55};  // OP 6 Modulator

  timbres_catalog_ = {
      {"timbre-grand", "German Grand D-274", "ACOUSTIC PIANO"},
      {"timbre-rhodes", "Rhodes Mark I 1975", "ELECTRIC PIANO"},
      {"timbre-wurlitzer", "Wurlitzer 200A", "ELECTRIC PIANO"},
      {"timbre-clav", "Clavinet D6 Funk", "KEYBOARDS"},
      {"timbre-b3", "B3 Tonewheel Organ", "ORGAN"},
      {"timbre-pad", "Warm Analog Pad", "SYNTH PAD"},
      {"timbre-fm-epiano", "Glass Keys", "E.PIANO"},
      {"timbre-va-sawpad", "Warm Saw Pad", "SYNTH PAD"},
      {"timbre-strings", "Studio Strings Ensemble", "STRINGS"},
      {"timbre-brass", "Analog Synth Brass", "BRASS"}
  };
}

PartView::~PartView() {
  nav_item_objs_.clear();
  field_objs_.clear();
  field_val_objs_.clear();
}

bool PartView::create(lv_obj_t* parent) {
  if (!skeleton_.create(parent)) return false;

  lv_obj_t* top_bar = skeleton_.top_bar();
  lv_obj_t* content = skeleton_.content_area();
  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  // 1. Power icon on top bar (right side)
  btn_power_ = lv_obj_create(top_bar);
  lv_obj_set_size(btn_power_, sp.touch_target_min, sp.touch_target_min);
  lv_obj_align(btn_power_, LV_ALIGN_RIGHT_MID, -sp.screen_gutter, 0);
  lv_obj_set_scrollable(btn_power_, false);
  lv_obj_set_clickable(btn_power_, true);
  lv_obj_set_style_bg_opa(btn_power_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_power_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn_power_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn_power_, on_power_click, LV_EVENT_CLICKED, this);

  lbl_power_ = lv_label_create(btn_power_);
  lv_label_set_text(lbl_power_, tr("performance.part.power").c_str());
  lv_obj_set_style_text_color(lbl_power_, p.ok, LV_PART_MAIN);
  lv_obj_align(lbl_power_, LV_ALIGN_CENTER, 0, 0);

  // 2. Left Column: Navigation sections (260px wide, 488px high)
  nav_col_ = lv_obj_create(content);
  lv_obj_set_size(nav_col_, 260, 488);
  lv_obj_set_pos(nav_col_, 0, 0);
  lv_obj_set_scrollable(nav_col_, false);
  lv_obj_set_style_bg_color(nav_col_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(nav_col_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(nav_col_, p.line, LV_PART_MAIN);
  lv_obj_set_style_border_width(nav_col_, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_all(nav_col_, 0, LV_PART_MAIN);

  nav_item_objs_.resize(static_cast<size_t>(PartSection::Count));

  const std::string nav_keys[] = {
      "part_view.nav.timbre",
      "part_view.nav.zone_keyboard",
      "part_view.nav.level_output",
      "part_view.nav.sound",
      "part_view.nav.envelope_eq",
      "part_view.nav.effects_sends",
      "part_view.nav.arpeggiator"
  };

  for (size_t i = 0; i < nav_item_objs_.size(); ++i) {
    lv_obj_t* item = lv_obj_create(nav_col_);
    lv_obj_set_size(item, 260, sp.touch_target_min);
    lv_obj_set_pos(item, 0, static_cast<int32_t>(i * sp.touch_target_min));
    lv_obj_set_scrollable(item, false);
    lv_obj_set_clickable(item, true);
    lv_obj_set_style_radius(item, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(item, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(item, sp.space_4, LV_PART_MAIN);
    lv_obj_add_event_cb(item, on_nav_item_click, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl = lv_label_create(item);
    lv_label_set_text(lbl, tr(nav_keys[i]).c_str());
    lv_obj_add_style(lbl, Theme::style_text(), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    nav_item_objs_[i] = item;
  }

  // 3. Right Column: Editor Panel (764px wide, 488px high)
  panel_col_ = lv_obj_create(content);
  lv_obj_set_size(panel_col_, 764, 488);
  lv_obj_set_pos(panel_col_, 260, 0);
  lv_obj_set_scrollable(panel_col_, false);
  lv_obj_set_style_bg_opa(panel_col_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(panel_col_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(panel_col_, sp.space_4, LV_PART_MAIN);

  // 4. Timbre Picker Modal (Full screen overlay with centered card)
  picker_modal_ = lv_obj_create(skeleton_.root_container());
  lv_obj_set_size(picker_modal_, 1024, 600);
  lv_obj_set_pos(picker_modal_, 0, 0);
  lv_obj_set_scrollable(picker_modal_, false);
  lv_obj_set_style_bg_color(picker_modal_, p.bg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(picker_modal_, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_border_width(picker_modal_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(picker_modal_, 0, LV_PART_MAIN);
  lv_obj_set_clickable(picker_modal_, true);
  lv_obj_set_hidden(picker_modal_, true);

  picker_card_ = lv_obj_create(picker_modal_);
  lv_obj_set_size(picker_card_, 560, 480);
  lv_obj_align(picker_card_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_scrollable(picker_card_, false);
  lv_obj_set_style_bg_color(picker_card_, p.raised, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(picker_card_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(picker_card_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(picker_card_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(picker_card_, r.lg, LV_PART_MAIN);
  lv_obj_set_style_pad_all(picker_card_, sp.space_4, LV_PART_MAIN);

  lv_obj_t* lbl_pick_title = lv_label_create(picker_card_);
  char title_buf[128];
  std::snprintf(title_buf, sizeof(title_buf), tr("part_view.timbre.picker_title").c_str(), part_idx_ + 1);
  lv_label_set_text(lbl_pick_title, title_buf);
  lv_obj_add_style(lbl_pick_title, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_pick_title, LV_ALIGN_TOP_LEFT, 0, 0);

  picker_list_ = lv_obj_create(picker_card_);
  lv_obj_set_size(picker_list_, 528, 400);
  lv_obj_set_pos(picker_list_, 0, 36);
  lv_obj_set_scrollable(picker_list_, true);
  lv_obj_set_style_bg_opa(picker_list_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(picker_list_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(picker_list_, 0, LV_PART_MAIN);

  for (size_t i = 0; i < timbres_catalog_.size(); ++i) {
    lv_obj_t* row = lv_obj_create(picker_list_);
    lv_obj_set_size(row, 528, sp.touch_target_min);
    lv_obj_set_pos(row, 0, static_cast<int32_t>(i * (sp.touch_target_min + 4)));
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, true);
    lv_obj_set_style_bg_color(row, p.surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, sp.space_3, LV_PART_MAIN);
    lv_obj_add_event_cb(row, on_picker_item_click, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_n = lv_label_create(row);
    lv_label_set_text(lbl_n, timbres_catalog_[i].name.c_str());
    lv_obj_add_style(lbl_n, Theme::style_text(), LV_PART_MAIN);
    lv_obj_align(lbl_n, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* lbl_c = lv_label_create(row);
    lv_label_set_text(lbl_c, timbres_catalog_[i].category.c_str());
    lv_obj_add_style(lbl_c, Theme::style_text_muted(), LV_PART_MAIN);
    lv_obj_align(lbl_c, LV_ALIGN_RIGHT_MID, 0, 0);
  }

  // 5. Hint Bar
  std::vector<HintItem> hints = {
      {"VALUE", tr("part_view.hint.edit_field")},
      {"WRITE", tr("part_view.hint.change_timbre")},
      {"EXIT", tr("hint.verb.back")}
  };
  skeleton_.set_hints(hints, tr("part_view.reassurance"));

  rebuild_top_bar();
  update_nav_ui();
  rebuild_panel();
  return true;
}

void PartView::rebuild_top_bar() {
  if (is_missing_library_) {
    char buf[128];
    snprintf(buf, sizeof(buf), "Part %d · %s", part_idx_ + 1, tr("library.missing").c_str());
    skeleton_.set_title(buf);
  } else {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s · %s · %s",
                  tr("part_view.title_prefix").c_str(),
                  timbre_name_.c_str(),
                  category_name_.c_str());
    char title_formatted[128];
    std::snprintf(title_formatted, sizeof(title_formatted), buf, part_idx_ + 1);
    skeleton_.set_title(title_formatted);
  }
}

void PartView::set_missing_library(bool missing) {
  is_missing_library_ = missing;
  rebuild_top_bar();
  rebuild_panel();
}

void PartView::set_favorite(bool fav) {
  is_favorite_ = fav;
  rebuild_panel();
}

void PartView::toggle_favorite() {
  set_favorite(!is_favorite_);
}

void PartView::set_part_index(int32_t idx) {
  part_idx_ = std::clamp(idx, 0, 7);
  rebuild_top_bar();
  rebuild_panel();
}

void PartView::set_timbre(const std::string& name, const std::string& category) {
  timbre_name_ = name;
  category_name_ = category;
  if (category_name_ == "ORGAN" || timbre_name_.find("Organ") != std::string::npos || timbre_name_.find("B3") != std::string::npos) {
    engine_type_ = EngineType::Organ;
  } else if (category_name_ == "FM" || timbre_name_.find("FM") != std::string::npos || timbre_name_.find("Glass Keys") != std::string::npos || timbre_name_.find("DX") != std::string::npos) {
    engine_type_ = EngineType::Fm;
  } else if (category_name_ == "VA" || category_name_ == "SYNTH PAD" || category_name_ == "SYNTH LEAD" || timbre_name_.find("Warm Saw Pad") != std::string::npos || timbre_name_.find("Analog") != std::string::npos) {
    engine_type_ = EngineType::Va;
  } else {
    engine_type_ = EngineType::Sample;
  }
  rebuild_top_bar();
  rebuild_panel();
}

void PartView::set_engine_type(EngineType type) {
  engine_type_ = type;
  rebuild_panel();
}

void PartView::set_engine_unavailable(bool unavail, const std::string& required_version) {
  is_engine_unavailable_ = unavail;
  required_version_ = required_version;
  rebuild_top_bar();
  rebuild_panel();
}

void PartView::set_fm_group(FmSoundGroup group) {
  fm_sound_group_ = group;
  field_focus_idx_ = 0;
  is_editing_field_ = false;
  sound_group_focus_ = false;
  rebuild_panel();
}

void PartView::set_va_group(VaSoundGroup group) {
  va_sound_group_ = group;
  field_focus_idx_ = 0;
  is_editing_field_ = false;
  sound_group_focus_ = false;
  rebuild_panel();
}

std::string PartView::fm_modulators_string() const {
  if (fm_settings_.algorithm == 5) {
    return "OP 2 · 4 · 6";
  }
  return "OP 2 · 4 · 6";
}

bool PartView::handle_exit_step() {
  if (current_section_ == PartSection::SoundEngine && (is_fm() || is_va())) {
    if (is_editing_field_) {
      is_editing_field_ = false;
      rebuild_panel();
      return true;
    }
    if (!sound_group_focus_) {
      sound_group_focus_ = true;
      rebuild_panel();
      return true;
    }
  }
  return false;
}

void PartView::set_zone(int32_t low_key, int32_t high_key, int32_t octave, int32_t transpose, bool kb_on) {
  low_key_ = std::clamp(low_key, 0, 127);
  high_key_ = std::clamp(high_key, low_key_, 127);
  octave_ = std::clamp(octave, -3, 3);
  transpose_ = std::clamp(transpose, -12, 12);
  keyboard_on_ = kb_on;
  rebuild_panel();
}

void PartView::set_level_output(float volume, bool aux_output) {
  volume_ = std::clamp(volume, 0.0f, 1.0f);
  aux_output_ = aux_output;
  rebuild_panel();
}


std::string PartView::registration_string() const {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u%u %u%u%u%u %u%u%u",
                organ_drawbars_[0], organ_drawbars_[1], organ_drawbars_[2],
                organ_drawbars_[3], organ_drawbars_[4], organ_drawbars_[5],
                organ_drawbars_[6], organ_drawbars_[7], organ_drawbars_[8]);
  return std::string(buf);
}

void PartView::set_sample_sound(float brightness, float cutoff_hz, float resonance, float vel_sense) {
  brightness_ = std::clamp(brightness, 0.0f, 1.0f);
  cutoff_hz_ = std::clamp(cutoff_hz, 20.0f, 20000.0f);
  resonance_ = std::clamp(resonance, 0.0f, 1.0f);
  vel_sense_ = std::clamp(vel_sense, 0.0f, 1.0f);
  rebuild_panel();
}

void PartView::set_organ_drawbars(const uint8_t drawbars[9]) {
  if (drawbars) {
    for (size_t i = 0; i < 9; ++i) {
      organ_drawbars_[i] = std::clamp(static_cast<int>(drawbars[i]), 0, 8);
    }
  }
  rebuild_panel();
}

void PartView::set_envelope(float attack_ms, float decay_ms, float sustain, float release_ms) {
  attack_ms_ = std::clamp(attack_ms, 0.0f, 5000.0f);
  decay_ms_ = std::clamp(decay_ms, 0.0f, 5000.0f);
  sustain_ = std::clamp(sustain, 0.0f, 1.0f);
  release_ms_ = std::clamp(release_ms, 0.0f, 5000.0f);
  rebuild_panel();
}

void PartView::set_eq(float low_db, float mid_db, float mid_freq_hz, float high_db) {
  eq_low_db_ = std::clamp(low_db, -12.0f, 12.0f);
  eq_mid_db_ = std::clamp(mid_db, -12.0f, 12.0f);
  eq_mid_freq_hz_ = std::clamp(mid_freq_hz, 200.0f, 8000.0f);
  eq_high_db_ = std::clamp(high_db, -12.0f, 12.0f);
  rebuild_panel();
}

void PartView::set_sends(float reverb_send, float delay_send) {
  reverb_send_ = std::clamp(reverb_send, 0.0f, 1.0f);
  delay_send_ = std::clamp(delay_send, 0.0f, 1.0f);
  rebuild_panel();
}

void PartView::set_arp(bool on, int32_t mode, int32_t rate, int32_t octaves, float gate, float swing, bool latch) {
  auto& a = arp_settings_[part_idx_];
  a.on = on;
  a.mode = std::clamp(mode, 0, 4);
  a.rate = std::clamp(rate, 0, 7);
  a.octaves = std::clamp(octaves, 1, 4);
  a.gate = std::clamp(gate, 0.05f, 1.0f);
  a.swing = std::clamp(swing, 0.50f, 0.75f);
  a.latch = latch;
  rebuild_panel();
}

void PartView::set_tempo(float bpm) {
  tempo_bpm_ = std::clamp(bpm, 20.0f, 300.0f);
  rebuild_panel();
}

void PartView::set_clock_source(const std::string& source, bool read_only) {
  clock_source_ = source;
  tempo_read_only_ = read_only;
  rebuild_panel();
}

void PartView::open_inserts_chain() {
  if (on_open_inserts_) {
    on_open_inserts_(part_idx_);
  }
}

void PartView::set_power(bool on) {
  power_on_ = on;
  const auto& p = Theme::current();
  if (lbl_power_) {
    lv_obj_set_style_text_color(lbl_power_, power_on_ ? p.ok : p.muted, LV_PART_MAIN);
  }
}

void PartView::set_section(PartSection sec) {
  current_section_ = sec;
  field_focus_idx_ = 0;
  update_nav_ui();
  rebuild_panel();
}

void PartView::update_nav_ui() {
  const auto& p = Theme::current();
  for (size_t i = 0; i < nav_item_objs_.size(); ++i) {
    bool active = (static_cast<size_t>(current_section_) == i);
    lv_obj_t* item = nav_item_objs_[i];
    if (active) {
      lv_obj_set_style_border_color(item, p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(item, 3, LV_PART_MAIN);
      lv_obj_set_style_border_side(item, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
      lv_obj_set_style_bg_color(item, p.raised, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_PART_MAIN);
    } else {
      lv_obj_set_style_border_width(item, 0, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, LV_PART_MAIN);
    }
  }
}

void PartView::rebuild_panel() {
  if (!panel_col_) return;
  lv_obj_clean(panel_col_);
  field_objs_.clear();
  field_val_objs_.clear();

  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  if (current_section_ == PartSection::Timbre) {
    // Current Timbre Card (56px min height, 720px wide)
    lv_obj_t* card = lv_obj_create(panel_col_);
    lv_obj_set_size(card, 720, 100);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, r.md, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, sp.space_3, LV_PART_MAIN);

    lv_obj_t* lbl_hdr = lv_label_create(card);
    lv_label_set_text(lbl_hdr, tr("part_view.timbre.current").c_str());
    lv_obj_add_style(lbl_hdr, Theme::style_text_muted(), LV_PART_MAIN);
    lv_obj_align(lbl_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* lbl_nm = lv_label_create(card);
    lv_label_set_text(lbl_nm, timbre_name_.c_str());
    lv_obj_add_style(lbl_nm, Theme::style_text(), LV_PART_MAIN);
    lv_obj_align(lbl_nm, LV_ALIGN_LEFT_MID, 0, 6);

    // Change Timbre Button (56px high)
    btn_change_timbre_ = lv_obj_create(panel_col_);
    lv_obj_set_size(btn_change_timbre_, 720, sp.touch_target_min);
    lv_obj_set_scrollable(btn_change_timbre_, false);
    lv_obj_set_clickable(btn_change_timbre_, true);
    lv_obj_set_style_bg_color(btn_change_timbre_, p.accent, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_change_timbre_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_change_timbre_, p.accent, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_change_timbre_, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_change_timbre_, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_change_timbre_, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_change_timbre_, on_change_timbre_click, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_btn = lv_label_create(btn_change_timbre_);
    lv_label_set_text(lbl_btn, tr("part_view.timbre.change").c_str());
    lv_obj_set_style_text_color(lbl_btn, p.on_accent, LV_PART_MAIN);
    lv_obj_align(lbl_btn, LV_ALIGN_CENTER, 0, 0);

    field_objs_.push_back(btn_change_timbre_);
  } else if (current_section_ == PartSection::ZoneKeyboard) {
    // 5 Fields: Low Key, High Key, Octave, Transpose, Keyboard on/off
    struct ZoneFieldDef {
      std::string label_key;
      std::string val_text;
    };
    std::vector<ZoneFieldDef> defs = {
        {"part_view.zone.low_key", midi_note_to_name(low_key_)},
        {"part_view.zone.high_key", midi_note_to_name(high_key_)},
        {"part_view.zone.octave", std::to_string(octave_)},
        {"part_view.zone.transpose", std::to_string(transpose_) + " st"},
        {"part_view.zone.keyboard_active", keyboard_on_ ? tr("part_view.zone.on") : tr("part_view.zone.off")}
    };

    for (size_t i = 0; i < defs.size(); ++i) {
      lv_obj_t* card = lv_obj_create(panel_col_);
      lv_obj_set_size(card, 720, sp.touch_target_min);
      lv_obj_set_scrollable(card, false);
      lv_obj_set_clickable(card, true);
      lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_hor(card, sp.space_3, LV_PART_MAIN);
      lv_obj_add_event_cb(card, on_field_click, LV_EVENT_CLICKED, this);

      lv_obj_t* lbl_h = lv_label_create(card);
      lv_label_set_text(lbl_h, tr(defs[i].label_key).c_str());
      lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
      lv_obj_align(lbl_h, LV_ALIGN_LEFT_MID, 0, 0);

      lv_obj_t* lbl_v = lv_label_create(card);
      lv_label_set_text(lbl_v, defs[i].val_text.c_str());
      lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
      lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, 0, 0);

      field_objs_.push_back(card);
      field_val_objs_.push_back(lbl_v);
    }
  } else if (current_section_ == PartSection::SoundEngine) {
    if (is_engine_unavailable_) {
      // 1. Engine not-available state (Story 12.10, UX-DR98)
      lv_obj_t* msg_card = lv_obj_create(panel_col_);
      lv_obj_set_size(msg_card, 720, 200);
      lv_obj_set_scrollable(msg_card, false);
      lv_obj_set_style_bg_color(msg_card, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(msg_card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(msg_card, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(msg_card, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(msg_card, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_all(msg_card, sp.space_4, LV_PART_MAIN);

      lv_obj_t* lbl_badge = lv_label_create(msg_card);
      char tag_buf[64];
      std::snprintf(tag_buf, sizeof(tag_buf), tr("part.not_available.tag").c_str(), required_version_.c_str());
      lv_label_set_text(lbl_badge, tag_buf);
      lv_obj_set_style_text_color(lbl_badge, p.warn, LV_PART_MAIN);
      lv_obj_set_pos(lbl_badge, 0, 0);

      lv_obj_t* lbl_title = lv_label_create(msg_card);
      lv_label_set_text(lbl_title, tr("part.not_available.title").c_str());
      lv_obj_add_style(lbl_title, Theme::style_text(), LV_PART_MAIN);
      lv_obj_set_pos(lbl_title, 0, 28);

      lv_obj_t* lbl_msg = lv_label_create(msg_card);
      lv_label_set_text(lbl_msg, tr("part.not_available.msg").c_str());
      lv_obj_add_style(lbl_msg, Theme::style_text_muted(), LV_PART_MAIN);
      lv_label_set_long_mode(lbl_msg, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(lbl_msg, 680);
      lv_obj_set_pos(lbl_msg, 0, 60);

      // Action buttons: Change timbre and Install update
      lv_obj_t* row_actions = lv_obj_create(panel_col_);
      lv_obj_set_size(row_actions, 720, sp.touch_target_min);
      lv_obj_set_scrollable(row_actions, false);
      lv_obj_set_style_bg_opa(row_actions, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_width(row_actions, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(row_actions, 0, LV_PART_MAIN);

      lv_obj_t* btn_ct = lv_obj_create(row_actions);
      lv_obj_set_size(btn_ct, 350, sp.touch_target_min);
      lv_obj_set_pos(btn_ct, 0, 0);
      lv_obj_set_clickable(btn_ct, true);
      lv_obj_set_style_bg_color(btn_ct, p.raised, LV_PART_MAIN);
      lv_obj_set_style_border_color(btn_ct, p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(btn_ct, 2, LV_PART_MAIN);
      lv_obj_set_style_radius(btn_ct, r.sm, LV_PART_MAIN);
      lv_obj_add_event_cb(btn_ct, on_change_timbre_click, LV_EVENT_CLICKED, this);
      btn_change_timbre_ = btn_ct;
      field_objs_.push_back(btn_ct);

      lv_obj_t* lbl_ct_sub = lv_label_create(btn_ct);
      lv_label_set_text(lbl_ct_sub, tr("part_view.nav.timbre").c_str());
      lv_obj_add_style(lbl_ct_sub, Theme::style_text_muted(), LV_PART_MAIN);
      lv_obj_set_pos(lbl_ct_sub, 12, 6);

      lv_obj_t* lbl_ct_title = lv_label_create(btn_ct);
      lv_label_set_text(lbl_ct_title, tr("part.not_available.change_timbre").c_str());
      lv_obj_add_style(lbl_ct_title, Theme::style_text(), LV_PART_MAIN);
      lv_obj_set_pos(lbl_ct_title, 12, 26);
      field_val_objs_.push_back(lbl_ct_title);

      lv_obj_t* btn_iu = lv_obj_create(row_actions);
      lv_obj_set_size(btn_iu, 350, sp.touch_target_min);
      lv_obj_set_pos(btn_iu, 370, 0);
      lv_obj_set_clickable(btn_iu, true);
      lv_obj_set_style_bg_color(btn_iu, p.surface, LV_PART_MAIN);
      lv_obj_set_style_border_color(btn_iu, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(btn_iu, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(btn_iu, r.sm, LV_PART_MAIN);
      field_objs_.push_back(btn_iu);

      lv_obj_t* lbl_iu_sub = lv_label_create(btn_iu);
      lv_label_set_text(lbl_iu_sub, tr("part.not_available.update_path").c_str());
      lv_obj_add_style(lbl_iu_sub, Theme::style_text_muted(), LV_PART_MAIN);
      lv_obj_set_pos(lbl_iu_sub, 12, 6);

      lv_obj_t* lbl_iu_title = lv_label_create(btn_iu);
      lv_label_set_text(lbl_iu_title, tr("part.not_available.install_update").c_str());
      lv_obj_add_style(lbl_iu_title, Theme::style_text(), LV_PART_MAIN);
      lv_obj_set_pos(lbl_iu_title, 12, 26);
      field_val_objs_.push_back(lbl_iu_title);

      std::vector<HintItem> unavail_hints = {
          {"VALUE", tr("hint.verb.select")},
          {"WRITE", tr("part.not_available.change_timbre")},
          {"EXIT", tr("hint.verb.back")}
      };
      char stat_buf[128];
      std::snprintf(stat_buf, sizeof(stat_buf), "! PART %d · %s · NEEDS UPDATE %s · SILENT",
                    part_idx_ + 1, timbre_name_.c_str(), required_version_.c_str());
      skeleton_.set_hints(unavail_hints, stat_buf);
    } else if (is_organ()) {
      // Organ sound section
      lv_obj_t* hdr_card = lv_obj_create(panel_col_);
      lv_obj_set_size(hdr_card, 720, 60);
      lv_obj_set_scrollable(hdr_card, false);
      lv_obj_set_style_bg_color(hdr_card, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(hdr_card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(hdr_card, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(hdr_card, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(hdr_card, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_hor(hdr_card, sp.space_3, LV_PART_MAIN);

      lv_obj_t* lbl_reg_title = lv_label_create(hdr_card);
      lv_label_set_text(lbl_reg_title, tr("performance.mode.drawbars").c_str());
      lv_obj_add_style(lbl_reg_title, Theme::style_text_muted(), LV_PART_MAIN);
      lv_obj_align(lbl_reg_title, LV_ALIGN_LEFT_MID, 0, 0);

      lv_obj_t* lbl_reg_val = lv_label_create(hdr_card);
      std::string reg = registration_string();
      lv_label_set_text(lbl_reg_val, reg.c_str());
      lv_obj_add_style(lbl_reg_val, Theme::style_text(), LV_PART_MAIN);
      lv_obj_align(lbl_reg_val, LV_ALIGN_RIGHT_MID, 0, 0);

      static const char* vc_names[] = {"Off", "V1", "C1", "V2", "C2", "V3", "C3"};
      static const char* rot_names[] = {"Stop", "Slow", "Fast"};

      struct OrganFieldDef {
        std::string label_key;
        std::string val_text;
      };
      std::vector<OrganFieldDef> defs = {
          {"part_view.sound.percussion", organ_percussion_on_ ? tr("part_view.zone.on") : tr("part_view.zone.off")},
          {"part_view.sound.key_click", std::to_string(organ_key_click_)},
          {"part_view.sound.vibrato_chorus", vc_names[std::clamp(organ_vibrato_, 0, 6)]},
          {"part_view.sound.leakage", std::to_string(organ_key_click_)},
          {"part_view.sound.rotary", rot_names[std::clamp(organ_rotary_speed_, 0, 2)]}
      };

      for (size_t i = 0; i < defs.size(); ++i) {
        lv_obj_t* card = lv_obj_create(panel_col_);
        lv_obj_set_size(card, 720, sp.touch_target_min);
        lv_obj_set_scrollable(card, false);
        lv_obj_set_clickable(card, true);
        lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(card, sp.space_3, LV_PART_MAIN);
        lv_obj_add_event_cb(card, on_field_click, LV_EVENT_CLICKED, this);

        lv_obj_t* lbl_h = lv_label_create(card);
        lv_label_set_text(lbl_h, tr(defs[i].label_key).c_str());
        lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
        lv_obj_align(lbl_h, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* lbl_v = lv_label_create(card);
        lv_label_set_text(lbl_v, defs[i].val_text.c_str());
        lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
        lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, 0, 0);

        field_objs_.push_back(card);
        field_val_objs_.push_back(lbl_v);
      }
    } else if (is_fm()) {
      // FM Sound Section (Story 12.11)
      static const char* fm_grp_keys[] = {
        "fm.group.algorithm", "fm.group.operators", "fm.group.pitch_env", "fm.group.lfo"
      };
      lv_obj_t* seg_ctrl = lv_obj_create(panel_col_);
      lv_obj_set_size(seg_ctrl, 720, 48);
      lv_obj_set_scrollable(seg_ctrl, false);
      lv_obj_set_style_bg_color(seg_ctrl, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(seg_ctrl, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(seg_ctrl, sound_group_focus_ ? p.accent : p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(seg_ctrl, sound_group_focus_ ? 2 : 1, LV_PART_MAIN);
      lv_obj_set_style_radius(seg_ctrl, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_all(seg_ctrl, 2, LV_PART_MAIN);

      for (int k = 0; k < 4; ++k) {
        lv_obj_t* seg = lv_obj_create(seg_ctrl);
        lv_obj_set_size(seg, 178, 40);
        lv_obj_set_pos(seg, k * 179, 0);
        lv_obj_set_scrollable(seg, false);
        lv_obj_set_clickable(seg, true);
        bool is_active = (static_cast<int>(fm_sound_group_) == k);
        lv_obj_set_style_bg_color(seg, is_active ? p.accent : p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(seg, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(seg, r.sm, LV_PART_MAIN);

        lv_obj_t* lbl_seg = lv_label_create(seg);
        lv_label_set_text(lbl_seg, tr(fm_grp_keys[k]).c_str());
        lv_obj_add_style(lbl_seg, Theme::style_text(), LV_PART_MAIN);
        if (is_active) {
          lv_obj_set_style_text_color(lbl_seg, p.on_accent, LV_PART_MAIN);
        }
        lv_obj_align(lbl_seg, LV_ALIGN_CENTER, 0, 0);

        lv_obj_add_event_cb(seg, [](lv_event_t* e) {
          auto* self = static_cast<PartView*>(lv_event_get_user_data(e));
          intptr_t grp = reinterpret_cast<intptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_current_target(e))));
          self->set_fm_group(static_cast<FmSoundGroup>(grp));
        }, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(seg, reinterpret_cast<void*>(static_cast<intptr_t>(k)));
      }

      if (fm_sound_group_ == FmSoundGroup::Algorithm) {
        lv_obj_t* row_alg = lv_obj_create(panel_col_);
        lv_obj_set_size(row_alg, 720, 220);
        lv_obj_set_scrollable(row_alg, false);
        lv_obj_set_style_bg_opa(row_alg, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(row_alg, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row_alg, 0, LV_PART_MAIN);

        // Diagram Card (left)
        lv_obj_t* diag_card = lv_obj_create(row_alg);
        lv_obj_set_size(diag_card, 350, 220);
        lv_obj_set_pos(diag_card, 0, 0);
        lv_obj_set_scrollable(diag_card, false);
        lv_obj_set_style_bg_color(diag_card, p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(diag_card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(diag_card, p.line_strong, LV_PART_MAIN);
        lv_obj_set_style_border_width(diag_card, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(diag_card, r.sm, LV_PART_MAIN);

        for (int b = 0; b < 6; ++b) {
          int col = b / 2;
          int row = b % 2;
          int op_num = (col * 2) + (row == 0 ? 2 : 1);
          bool is_mod = (row == 0);
          lv_obj_t* op_box = lv_obj_create(diag_card);
          lv_obj_set_size(op_box, 70, 36);
          lv_obj_set_pos(op_box, 20 + col * 110, 20 + row * 80);
          lv_obj_set_scrollable(op_box, false);
          lv_obj_set_style_bg_color(op_box, p.raised, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(op_box, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_color(op_box, p.accent, LV_PART_MAIN);
          lv_obj_set_style_border_width(op_box, 2, LV_PART_MAIN);
          lv_obj_set_style_radius(op_box, r.sm, LV_PART_MAIN);

          lv_obj_t* lbl_op = lv_label_create(op_box);
          char op_txt[16];
          std::snprintf(op_txt, sizeof(op_txt), "OP %d%s", op_num, is_mod ? " ◐" : "");
          lv_label_set_text(lbl_op, op_txt);
          lv_obj_add_style(lbl_op, Theme::style_text(), LV_PART_MAIN);
          lv_obj_align(lbl_op, LV_ALIGN_CENTER, 0, 0);
        }

        lv_obj_t* lbl_diag_note = lv_label_create(diag_card);
        char note_buf[64];
        std::snprintf(note_buf, sizeof(note_buf), tr("fm.brightness.note").c_str(), "OP 2 · 4 · 6");
        lv_label_set_text(lbl_diag_note, note_buf);
        lv_obj_add_style(lbl_diag_note, Theme::style_text_muted(), LV_PART_MAIN);
        lv_obj_align(lbl_diag_note, LV_ALIGN_BOTTOM_LEFT, 10, -10);

        // Right side fields
        lv_obj_t* right_col = lv_obj_create(row_alg);
        lv_obj_set_size(right_col, 350, 220);
        lv_obj_set_pos(right_col, 370, 0);
        lv_obj_set_scrollable(right_col, false);
        lv_obj_set_style_bg_opa(right_col, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(right_col, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(right_col, 0, LV_PART_MAIN);

        // Field 0: Algorithm
        lv_obj_t* card_alg = lv_obj_create(right_col);
        lv_obj_set_size(card_alg, 350, sp.touch_target_min);
        lv_obj_set_pos(card_alg, 0, 0);
        lv_obj_set_clickable(card_alg, true);
        lv_obj_set_style_bg_color(card_alg, p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card_alg, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card_alg, p.line_strong, LV_PART_MAIN);
        lv_obj_set_style_border_width(card_alg, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card_alg, r.sm, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(card_alg, sp.space_3, LV_PART_MAIN);
        lv_obj_add_event_cb(card_alg, on_field_click, LV_EVENT_CLICKED, this);

        lv_obj_t* lbl_ah = lv_label_create(card_alg);
        lv_label_set_text(lbl_ah, tr("fm.param.algorithm").c_str());
        lv_obj_add_style(lbl_ah, Theme::style_text_muted(), LV_PART_MAIN);
        lv_obj_align(lbl_ah, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* lbl_av = lv_label_create(card_alg);
        char alg_buf[32];
        if (field_focus_idx_ == 0 && is_editing_field_) {
          std::snprintf(alg_buf, sizeof(alg_buf), "◂ %d / 32 ▸", fm_settings_.algorithm);
        } else {
          std::snprintf(alg_buf, sizeof(alg_buf), "%d / 32", fm_settings_.algorithm);
        }
        lv_label_set_text(lbl_av, alg_buf);
        lv_obj_add_style(lbl_av, Theme::style_text(), LV_PART_MAIN);
        lv_obj_align(lbl_av, LV_ALIGN_RIGHT_MID, 0, 0);
        field_objs_.push_back(card_alg);
        field_val_objs_.push_back(lbl_av);

        // Field 1: Feedback
        lv_obj_t* card_fb = lv_obj_create(right_col);
        lv_obj_set_size(card_fb, 350, sp.touch_target_min);
        lv_obj_set_pos(card_fb, 0, 64);
        lv_obj_set_clickable(card_fb, true);
        lv_obj_set_style_bg_color(card_fb, p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card_fb, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card_fb, p.line_strong, LV_PART_MAIN);
        lv_obj_set_style_border_width(card_fb, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card_fb, r.sm, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(card_fb, sp.space_3, LV_PART_MAIN);
        lv_obj_add_event_cb(card_fb, on_field_click, LV_EVENT_CLICKED, this);

        lv_obj_t* lbl_fh = lv_label_create(card_fb);
        lv_label_set_text(lbl_fh, tr("fm.param.feedback").c_str());
        lv_obj_add_style(lbl_fh, Theme::style_text_muted(), LV_PART_MAIN);
        lv_obj_align(lbl_fh, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* lbl_fv = lv_label_create(card_fb);
        char fb_buf[32];
        if (field_focus_idx_ == 1 && is_editing_field_) {
          std::snprintf(fb_buf, sizeof(fb_buf), "◂ %d ▸", fm_settings_.feedback);
        } else {
          std::snprintf(fb_buf, sizeof(fb_buf), "%d", fm_settings_.feedback);
        }
        lv_label_set_text(lbl_fv, fb_buf);
        lv_obj_add_style(lbl_fv, Theme::style_text(), LV_PART_MAIN);
        lv_obj_align(lbl_fv, LV_ALIGN_RIGHT_MID, 0, 0);
        field_objs_.push_back(card_fb);
        field_val_objs_.push_back(lbl_fv);

        // Info card
        lv_obj_t* info_card = lv_obj_create(right_col);
        lv_obj_set_size(info_card, 350, 85);
        lv_obj_set_pos(info_card, 0, 128);
        lv_obj_set_scrollable(info_card, false);
        lv_obj_set_style_bg_color(info_card, p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(info_card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(info_card, p.line_strong, LV_PART_MAIN);
        lv_obj_set_style_border_width(info_card, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(info_card, r.sm, LV_PART_MAIN);

        lv_obj_t* lbl_info = lv_label_create(info_card);
        lv_label_set_text(lbl_info, tr("fm.info.diagram").c_str());
        lv_obj_add_style(lbl_info, Theme::style_text_muted(), LV_PART_MAIN);
        lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl_info, 310);
        lv_obj_align(lbl_info, LV_ALIGN_CENTER, 0, 0);

      } else if (fm_sound_group_ == FmSoundGroup::Operators) {
        for (int op = 0; op < 6; ++op) {
          int col = op % 2;
          int row = op / 2;
          lv_obj_t* op_card = lv_obj_create(panel_col_);
          lv_obj_set_size(op_card, 350, 70);
          lv_obj_set_pos(op_card, col * 370, 56 + row * 76);
          lv_obj_set_scrollable(op_card, false);
          lv_obj_set_style_bg_color(op_card, p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(op_card, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_color(op_card, p.line_strong, LV_PART_MAIN);
          lv_obj_set_style_border_width(op_card, 1, LV_PART_MAIN);
          lv_obj_set_style_radius(op_card, r.sm, LV_PART_MAIN);
          lv_obj_set_style_pad_all(op_card, sp.space_2, LV_PART_MAIN);

          bool is_carrier = (op == 0 || op == 2 || op == 4);
          lv_obj_t* lbl_op_t = lv_label_create(op_card);
          char op_t_buf[32];
          std::snprintf(op_t_buf, sizeof(op_t_buf), "OP %d · %s", op + 1,
                        is_carrier ? tr("fm.param.carrier").c_str() : tr("fm.param.modulator").c_str());
          lv_label_set_text(lbl_op_t, op_t_buf);
          lv_obj_add_style(lbl_op_t, Theme::style_text_muted(), LV_PART_MAIN);
          lv_obj_set_pos(lbl_op_t, 8, 2);

          // Ratio Cell
          int f_ratio = op * 2;
          lv_obj_t* c_ratio = lv_obj_create(op_card);
          lv_obj_set_size(c_ratio, 100, 36);
          lv_obj_set_pos(c_ratio, 8, 22);
          lv_obj_set_scrollable(c_ratio, false);
          lv_obj_set_clickable(c_ratio, true);
          lv_obj_set_style_bg_color(c_ratio, p.raised, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(c_ratio, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_color(c_ratio, p.line_strong, LV_PART_MAIN);
          lv_obj_set_style_border_width(c_ratio, 1, LV_PART_MAIN);
          lv_obj_set_style_radius(c_ratio, r.sm, LV_PART_MAIN);
          lv_obj_add_event_cb(c_ratio, on_field_click, LV_EVENT_CLICKED, this);

          lv_obj_t* lbl_rv = lv_label_create(c_ratio);
          char r_buf[32];
          if (field_focus_idx_ == f_ratio && is_editing_field_) {
            std::snprintf(r_buf, sizeof(r_buf), "◂ %.2f ▸", fm_settings_.operators[op].ratio);
          } else {
            std::snprintf(r_buf, sizeof(r_buf), "%.2f", fm_settings_.operators[op].ratio);
          }
          lv_label_set_text(lbl_rv, r_buf);
          lv_obj_add_style(lbl_rv, Theme::style_text(), LV_PART_MAIN);
          lv_obj_align(lbl_rv, LV_ALIGN_CENTER, 0, 0);
          field_objs_.push_back(c_ratio);
          field_val_objs_.push_back(lbl_rv);

          // Level Cell
          int f_level = op * 2 + 1;
          lv_obj_t* c_level = lv_obj_create(op_card);
          lv_obj_set_size(c_level, 100, 36);
          lv_obj_set_pos(c_level, 116, 22);
          lv_obj_set_scrollable(c_level, false);
          lv_obj_set_clickable(c_level, true);
          lv_obj_set_style_bg_color(c_level, p.raised, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(c_level, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_color(c_level, p.line_strong, LV_PART_MAIN);
          lv_obj_set_style_border_width(c_level, 1, LV_PART_MAIN);
          lv_obj_set_style_radius(c_level, r.sm, LV_PART_MAIN);
          lv_obj_add_event_cb(c_level, on_field_click, LV_EVENT_CLICKED, this);

          lv_obj_t* lbl_lv = lv_label_create(c_level);
          char l_buf[32];
          if (field_focus_idx_ == f_level && is_editing_field_) {
            std::snprintf(l_buf, sizeof(l_buf), "◂ %d ▸", fm_settings_.operators[op].level);
          } else {
            std::snprintf(l_buf, sizeof(l_buf), "%d", fm_settings_.operators[op].level);
          }
          lv_label_set_text(lbl_lv, l_buf);
          lv_obj_add_style(lbl_lv, Theme::style_text(), LV_PART_MAIN);
          lv_obj_align(lbl_lv, LV_ALIGN_CENTER, 0, 0);
          field_objs_.push_back(c_level);
          field_val_objs_.push_back(lbl_lv);
        }
      } else if (fm_sound_group_ == FmSoundGroup::PitchEnv) {
        lv_obj_t* curve_card = lv_obj_create(panel_col_);
        lv_obj_set_size(curve_card, 720, 80);
        lv_obj_set_scrollable(curve_card, false);
        lv_obj_set_style_bg_color(curve_card, p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(curve_card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(curve_card, p.line_strong, LV_PART_MAIN);
        lv_obj_set_style_border_width(curve_card, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(curve_card, r.sm, LV_PART_MAIN);

        static const char* pe_labels[] = {
          "fm.param.rate1", "fm.param.rate2", "fm.param.rate3", "fm.param.rate4",
          "fm.param.level1", "fm.param.level2", "fm.param.level3", "fm.param.level4"
        };
        for (int k = 0; k < 8; ++k) {
          int col = k % 4;
          int row = k / 4;
          lv_obj_t* card = lv_obj_create(panel_col_);
          lv_obj_set_size(card, 172, 50);
          lv_obj_set_pos(card, col * 180, 140 + row * 56);
          lv_obj_set_scrollable(card, false);
          lv_obj_set_clickable(card, true);
          lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
          lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
          lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
          lv_obj_set_style_pad_hor(card, sp.space_2, LV_PART_MAIN);
          lv_obj_add_event_cb(card, on_field_click, LV_EVENT_CLICKED, this);

          lv_obj_t* lbl_h = lv_label_create(card);
          lv_label_set_text(lbl_h, tr(pe_labels[k]).c_str());
          lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
          lv_obj_align(lbl_h, LV_ALIGN_LEFT_MID, 0, 0);

          int val = (k < 4) ? fm_settings_.pitch_rates[k] : fm_settings_.pitch_levels[k - 4];
          lv_obj_t* lbl_v = lv_label_create(card);
          char v_buf[32];
          if (field_focus_idx_ == k && is_editing_field_) {
            std::snprintf(v_buf, sizeof(v_buf), "◂ %d ▸", val);
          } else {
            std::snprintf(v_buf, sizeof(v_buf), "%d", val);
          }
          lv_label_set_text(lbl_v, v_buf);
          lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
          lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, 0, 0);

          field_objs_.push_back(card);
          field_val_objs_.push_back(lbl_v);
        }
      } else if (fm_sound_group_ == FmSoundGroup::Lfo) {
        lv_obj_t* wave_seg = lv_obj_create(panel_col_);
        lv_obj_set_size(wave_seg, 720, 44);
        lv_obj_set_scrollable(wave_seg, false);
        lv_obj_set_style_bg_color(wave_seg, p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(wave_seg, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(wave_seg, p.line_strong, LV_PART_MAIN);
        lv_obj_set_style_border_width(wave_seg, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(wave_seg, r.sm, LV_PART_MAIN);
        lv_obj_set_style_pad_all(wave_seg, 2, LV_PART_MAIN);
        lv_obj_add_event_cb(wave_seg, on_field_click, LV_EVENT_CLICKED, this);
        field_objs_.push_back(wave_seg);

        for (int w = 0; w < 6; ++w) {
          lv_obj_t* seg = lv_obj_create(wave_seg);
          lv_obj_set_size(seg, 118, 36);
          lv_obj_set_pos(seg, w * 119, 0);
          lv_obj_set_scrollable(seg, false);
          bool is_w_active = (fm_settings_.lfo_wave == w);
          lv_obj_set_style_bg_color(seg, is_w_active ? p.accent : p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_width(seg, 0, LV_PART_MAIN);
          lv_obj_set_style_radius(seg, r.sm, LV_PART_MAIN);

          lv_obj_t* lbl_w = lv_label_create(seg);
          lv_label_set_text(lbl_w, tr(kFmLfoWaveKeys[w]).c_str());
          lv_obj_add_style(lbl_w, Theme::style_text(), LV_PART_MAIN);
          if (is_w_active) {
            lv_obj_set_style_text_color(lbl_w, p.on_accent, LV_PART_MAIN);
          }
          lv_obj_align(lbl_w, LV_ALIGN_CENTER, 0, 0);
          if (w == 0) field_val_objs_.push_back(lbl_w);
        }

        struct LfoFieldDef { std::string key; std::string val; };
        std::vector<LfoFieldDef> lfo_defs = {
          {"fm.param.speed", std::to_string(fm_settings_.lfo_speed)},
          {"fm.param.delay", std::to_string(fm_settings_.lfo_delay)},
          {"fm.param.pitch_depth", std::to_string(fm_settings_.lfo_pitch_depth)},
          {"fm.param.amp_depth", std::to_string(fm_settings_.lfo_amp_depth)},
          {"fm.param.key_sync", fm_settings_.lfo_key_sync ? tr("part_view.zone.on") : tr("part_view.zone.off")}
        };
        for (size_t f = 0; f < lfo_defs.size(); ++f) {
          int col = f % 3;
          int row = f / 3;
          lv_obj_t* card = lv_obj_create(panel_col_);
          lv_obj_set_size(card, 234, sp.touch_target_min);
          lv_obj_set_pos(card, col * 243, 108 + row * 60);
          lv_obj_set_scrollable(card, false);
          lv_obj_set_clickable(card, true);
          lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
          lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
          lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
          lv_obj_set_style_pad_hor(card, sp.space_3, LV_PART_MAIN);
          lv_obj_add_event_cb(card, on_field_click, LV_EVENT_CLICKED, this);

          lv_obj_t* lbl_h = lv_label_create(card);
          lv_label_set_text(lbl_h, tr(lfo_defs[f].key).c_str());
          lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
          lv_obj_align(lbl_h, LV_ALIGN_LEFT_MID, 0, 0);

          lv_obj_t* lbl_v = lv_label_create(card);
          if (field_focus_idx_ == static_cast<int32_t>(f + 1) && is_editing_field_) {
            char edit_buf[32];
            std::snprintf(edit_buf, sizeof(edit_buf), "◂ %s ▸", lfo_defs[f].val.c_str());
            lv_label_set_text(lbl_v, edit_buf);
          } else {
            lv_label_set_text(lbl_v, lfo_defs[f].val.c_str());
          }
          lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
          lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, 0, 0);

          field_objs_.push_back(card);
          field_val_objs_.push_back(lbl_v);
        }
      }

      // Bottom Row: Brightness (Knob 2)
      lv_obj_t* b_card = lv_obj_create(panel_col_);
      lv_obj_set_size(b_card, 720, sp.touch_target_min);
      lv_obj_set_scrollable(b_card, false);
      lv_obj_set_style_bg_color(b_card, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(b_card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(b_card, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(b_card, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(b_card, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_hor(b_card, sp.space_3, LV_PART_MAIN);

      lv_obj_t* lbl_bh = lv_label_create(b_card);
      lv_label_set_text(lbl_bh, tr("sound.brightness.knob").c_str());
      lv_obj_add_style(lbl_bh, Theme::style_text_muted(), LV_PART_MAIN);
      lv_obj_align(lbl_bh, LV_ALIGN_LEFT_MID, 0, -8);

      lv_obj_t* lbl_bv = lv_label_create(b_card);
      lv_label_set_text(lbl_bv, std::to_string(fm_settings_.brightness_offset).c_str());
      lv_obj_add_style(lbl_bv, Theme::style_text(), LV_PART_MAIN);
      lv_obj_align(lbl_bv, LV_ALIGN_LEFT_MID, 0, 10);

      lv_obj_t* lbl_bn = lv_label_create(b_card);
      char b_note_buf[64];
      std::snprintf(b_note_buf, sizeof(b_note_buf), tr("fm.brightness.note").c_str(), "OP 2 · 4 · 6");
      lv_label_set_text(lbl_bn, b_note_buf);
      lv_obj_add_style(lbl_bn, Theme::style_text_muted(), LV_PART_MAIN);
      lv_obj_align(lbl_bn, LV_ALIGN_RIGHT_MID, 0, 0);

      std::vector<HintItem> fm_hints = {
          {"VALUE", is_editing_field_ ? tr("hint.verb.change") : (sound_group_focus_ ? tr("hint.verb.select") : tr("hint.verb.select"))},
          {"WRITE", is_editing_field_ ? tr("hint.verb.confirm") : tr("hint.verb.change")},
          {"EXIT", tr("hint.verb.back")}
      };
      skeleton_.set_hints(fm_hints, tr("sound.brightness.always_works"));

    } else if (is_va()) {
      // VA Sound Section (Story 12.12)
      static const char* va_grp_keys[] = {
        "va.group.oscillators", "va.group.mixer", "va.group.filter",
        "va.group.envelopes", "va.group.lfo", "va.group.voice"
      };
      lv_obj_t* seg_ctrl = lv_obj_create(panel_col_);
      lv_obj_set_size(seg_ctrl, 720, 48);
      lv_obj_set_scrollable(seg_ctrl, false);
      lv_obj_set_style_bg_color(seg_ctrl, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(seg_ctrl, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(seg_ctrl, sound_group_focus_ ? p.accent : p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(seg_ctrl, sound_group_focus_ ? 2 : 1, LV_PART_MAIN);
      lv_obj_set_style_radius(seg_ctrl, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_all(seg_ctrl, 2, LV_PART_MAIN);

      for (int k = 0; k < 6; ++k) {
        lv_obj_t* seg = lv_obj_create(seg_ctrl);
        lv_obj_set_size(seg, 118, 40);
        lv_obj_set_pos(seg, k * 119, 0);
        lv_obj_set_scrollable(seg, false);
        lv_obj_set_clickable(seg, true);
        bool is_active = (static_cast<int>(va_sound_group_) == k);
        lv_obj_set_style_bg_color(seg, is_active ? p.accent : p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(seg, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(seg, r.sm, LV_PART_MAIN);

        lv_obj_t* lbl_seg = lv_label_create(seg);
        lv_label_set_text(lbl_seg, tr(va_grp_keys[k]).c_str());
        lv_obj_add_style(lbl_seg, Theme::style_text(), LV_PART_MAIN);
        if (is_active) {
          lv_obj_set_style_text_color(lbl_seg, p.on_accent, LV_PART_MAIN);
        }
        lv_obj_align(lbl_seg, LV_ALIGN_CENTER, 0, 0);

        lv_obj_add_event_cb(seg, [](lv_event_t* e) {
          auto* self = static_cast<PartView*>(lv_event_get_user_data(e));
          intptr_t grp = reinterpret_cast<intptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_current_target(e))));
          self->set_va_group(static_cast<VaSoundGroup>(grp));
        }, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(seg, reinterpret_cast<void*>(static_cast<intptr_t>(k)));
      }

      if (va_sound_group_ == VaSoundGroup::Oscillators) {
        for (int osc = 0; osc < 2; ++osc) {
          lv_obj_t* card = lv_obj_create(panel_col_);
          lv_obj_set_size(card, 720, 100);
          lv_obj_set_scrollable(card, false);
          lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
          lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
          lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
          lv_obj_set_style_pad_all(card, sp.space_2, LV_PART_MAIN);

          lv_obj_t* lbl_t = lv_label_create(card);
          lv_label_set_text(lbl_t, tr(osc == 0 ? "va.param.osc1" : "va.param.osc2").c_str());
          lv_obj_add_style(lbl_t, Theme::style_text_muted(), LV_PART_MAIN);
          lv_obj_set_pos(lbl_t, 8, 2);

          int wave_val = (osc == 0) ? va_settings_.osc1_wave : va_settings_.osc2_wave;
          lv_obj_t* c_w = lv_obj_create(card);
          lv_obj_set_size(c_w, 160, 48);
          lv_obj_set_pos(c_w, 8, 24);
          lv_obj_set_clickable(c_w, true);
          lv_obj_set_style_bg_color(c_w, p.raised, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(c_w, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_color(c_w, p.line_strong, LV_PART_MAIN);
          lv_obj_set_style_border_width(c_w, 1, LV_PART_MAIN);
          lv_obj_set_style_radius(c_w, r.sm, LV_PART_MAIN);
          lv_obj_add_event_cb(c_w, on_field_click, LV_EVENT_CLICKED, this);

          lv_obj_t* lbl_wv = lv_label_create(c_w);
          lv_label_set_text(lbl_wv, tr(kVaOscWaveKeys[std::clamp(wave_val, 0, 3)]).c_str());
          lv_obj_add_style(lbl_wv, Theme::style_text(), LV_PART_MAIN);
          lv_obj_align(lbl_wv, LV_ALIGN_CENTER, 0, 0);
          field_objs_.push_back(c_w);
          field_val_objs_.push_back(lbl_wv);

          int oct = (osc == 0) ? va_settings_.osc1_octave : va_settings_.osc2_octave;
          int det = (osc == 0) ? va_settings_.osc1_detune : va_settings_.osc2_detune;
          int pw = (osc == 0) ? va_settings_.osc1_pulse_width : va_settings_.osc2_pulse_width;

          struct SubDef { std::string key; std::string val; };
          std::vector<SubDef> s_defs = {
            {"va.param.octave", std::to_string(oct)},
            {"va.param.detune", std::to_string(det) + " ct"},
            {"va.param.pulse_width", format_cldr_percent(pw)}
          };
          for (size_t s = 0; s < s_defs.size(); ++s) {
            lv_obj_t* c = lv_obj_create(card);
            lv_obj_set_size(c, 160, 48);
            lv_obj_set_pos(c, 180 + s * 170, 24);
            lv_obj_set_clickable(c, true);
            lv_obj_set_style_bg_color(c, p.raised, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_color(c, p.line_strong, LV_PART_MAIN);
            lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
            lv_obj_set_style_radius(c, r.sm, LV_PART_MAIN);
            lv_obj_add_event_cb(c, on_field_click, LV_EVENT_CLICKED, this);

            lv_obj_t* lbl_h = lv_label_create(c);
            lv_label_set_text(lbl_h, tr(s_defs[s].key).c_str());
            lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
            lv_obj_set_pos(lbl_h, 6, 2);

            lv_obj_t* lbl_v = lv_label_create(c);
            lv_label_set_text(lbl_v, s_defs[s].val.c_str());
            lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
            lv_obj_set_pos(lbl_v, 6, 22);

            field_objs_.push_back(c);
            field_val_objs_.push_back(lbl_v);
          }
        }
      } else if (va_sound_group_ == VaSoundGroup::Mixer) {
        struct MixDef { std::string key; int val; };
        std::vector<MixDef> m_defs = {
          {"va.param.osc1", va_settings_.mix_osc1},
          {"va.param.osc2", va_settings_.mix_osc2},
          {"va.param.sub", va_settings_.mix_sub},
          {"va.param.noise", va_settings_.mix_noise}
        };
        for (size_t i = 0; i < m_defs.size(); ++i) {
          lv_obj_t* card = lv_obj_create(panel_col_);
          lv_obj_set_size(card, 720, sp.touch_target_min);
          lv_obj_set_scrollable(card, false);
          lv_obj_set_clickable(card, true);
          lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
          lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
          lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
          lv_obj_set_style_pad_hor(card, sp.space_3, LV_PART_MAIN);
          lv_obj_add_event_cb(card, on_field_click, LV_EVENT_CLICKED, this);

          lv_obj_t* lbl_h = lv_label_create(card);
          lv_label_set_text(lbl_h, tr(m_defs[i].key).c_str());
          lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
          lv_obj_align(lbl_h, LV_ALIGN_LEFT_MID, 0, 0);

          lv_obj_t* lbl_v = lv_label_create(card);
          char v_buf[32];
          if (field_focus_idx_ == static_cast<int32_t>(i) && is_editing_field_) {
            std::snprintf(v_buf, sizeof(v_buf), "◂ %d ▸", m_defs[i].val);
          } else {
            std::snprintf(v_buf, sizeof(v_buf), "%d", m_defs[i].val);
          }
          lv_label_set_text(lbl_v, v_buf);
          lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
          lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, 0, 0);

          field_objs_.push_back(card);
          field_val_objs_.push_back(lbl_v);
        }
      } else if (va_sound_group_ == VaSoundGroup::Filter) {
        lv_obj_t* ftype_seg = lv_obj_create(panel_col_);
        lv_obj_set_size(ftype_seg, 720, 44);
        lv_obj_set_scrollable(ftype_seg, false);
        lv_obj_set_style_bg_color(ftype_seg, p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ftype_seg, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(ftype_seg, p.line_strong, LV_PART_MAIN);
        lv_obj_set_style_border_width(ftype_seg, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(ftype_seg, r.sm, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ftype_seg, 2, LV_PART_MAIN);
        lv_obj_add_event_cb(ftype_seg, on_field_click, LV_EVENT_CLICKED, this);
        field_objs_.push_back(ftype_seg);

        for (int t = 0; t < 4; ++t) {
          lv_obj_t* seg = lv_obj_create(ftype_seg);
          lv_obj_set_size(seg, 178, 36);
          lv_obj_set_pos(seg, t * 179, 0);
          lv_obj_set_scrollable(seg, false);
          bool is_t_active = (va_settings_.filter_type == t);
          lv_obj_set_style_bg_color(seg, is_t_active ? p.accent : p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_width(seg, 0, LV_PART_MAIN);
          lv_obj_set_style_radius(seg, r.sm, LV_PART_MAIN);

          lv_obj_t* lbl_t = lv_label_create(seg);
          lv_label_set_text(lbl_t, tr(kVaFilterTypeKeys[t]).c_str());
          lv_obj_add_style(lbl_t, Theme::style_text(), LV_PART_MAIN);
          if (is_t_active) {
            lv_obj_set_style_text_color(lbl_t, p.on_accent, LV_PART_MAIN);
          }
          lv_obj_align(lbl_t, LV_ALIGN_CENTER, 0, 0);
          if (t == 0) field_val_objs_.push_back(lbl_t);
        }

        struct FiltDef { std::string key; std::string val; };
        std::vector<FiltDef> f_defs = {
          {"va.param.cutoff", std::to_string(va_settings_.cutoff)},
          {"va.param.resonance", std::to_string(va_settings_.resonance)},
          {"va.param.drive", std::to_string(va_settings_.drive)},
          {"va.param.key_track", format_cldr_percent(va_settings_.key_track)},
          {"va.param.env_amount", (va_settings_.env_amount > 0 ? "+" : "") + std::to_string(va_settings_.env_amount)},
          {"va.param.velocity", std::to_string(va_settings_.velocity)}
        };
        for (size_t f = 0; f < f_defs.size(); ++f) {
          int col = f % 3;
          int row = f / 3;
          lv_obj_t* card = lv_obj_create(panel_col_);
          lv_obj_set_size(card, 234, sp.touch_target_min);
          lv_obj_set_pos(card, col * 243, 108 + row * 60);
          lv_obj_set_scrollable(card, false);
          lv_obj_set_clickable(card, true);
          lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
          lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
          lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
          lv_obj_set_style_pad_hor(card, sp.space_3, LV_PART_MAIN);
          lv_obj_add_event_cb(card, on_field_click, LV_EVENT_CLICKED, this);

          lv_obj_t* lbl_h = lv_label_create(card);
          lv_label_set_text(lbl_h, tr(f_defs[f].key).c_str());
          lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
          lv_obj_align(lbl_h, LV_ALIGN_LEFT_MID, 0, 0);

          lv_obj_t* lbl_v = lv_label_create(card);
          if (field_focus_idx_ == static_cast<int32_t>(f + 1) && is_editing_field_) {
            char edit_buf[32];
            std::snprintf(edit_buf, sizeof(edit_buf), "◂ %s ▸", f_defs[f].val.c_str());
            lv_label_set_text(lbl_v, edit_buf);
          } else {
            lv_label_set_text(lbl_v, f_defs[f].val.c_str());
          }
          lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
          lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, 0, 0);

          field_objs_.push_back(card);
          field_val_objs_.push_back(lbl_v);
        }
      } else if (va_sound_group_ == VaSoundGroup::Envelopes) {
        for (int env = 0; env < 2; ++env) {
          lv_obj_t* card = lv_obj_create(panel_col_);
          lv_obj_set_size(card, 350, 200);
          lv_obj_set_pos(card, env * 370, 56);
          lv_obj_set_scrollable(card, false);
          lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
          lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
          lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
          lv_obj_set_style_pad_all(card, sp.space_2, LV_PART_MAIN);

          lv_obj_t* lbl_t = lv_label_create(card);
          lv_label_set_text(lbl_t, tr(env == 0 ? "va.param.amp_env" : "va.param.filter_env").c_str());
          lv_obj_add_style(lbl_t, Theme::style_text_muted(), LV_PART_MAIN);
          lv_obj_set_pos(lbl_t, 8, 2);

          int a = (env == 0) ? va_settings_.amp_attack : va_settings_.filter_attack;
          int d = (env == 0) ? va_settings_.amp_decay : va_settings_.filter_decay;
          int s = (env == 0) ? va_settings_.amp_sustain : va_settings_.filter_sustain;
          int r_val = (env == 0) ? va_settings_.amp_release : va_settings_.filter_release;

          struct EnvSubDef { std::string key; int val; };
          std::vector<EnvSubDef> e_defs = {
            {"va.param.attack", a}, {"va.param.decay", d},
            {"va.param.sustain", s}, {"va.param.release", r_val}
          };
          for (size_t k = 0; k < e_defs.size(); ++k) {
            int col = k % 2;
            int row = k / 2;
            int f_idx = env * 4 + k;
            lv_obj_t* c = lv_obj_create(card);
            lv_obj_set_size(c, 160, 48);
            lv_obj_set_pos(c, 8 + col * 170, 70 + row * 56);
            lv_obj_set_clickable(c, true);
            lv_obj_set_style_bg_color(c, p.raised, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_color(c, p.line_strong, LV_PART_MAIN);
            lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
            lv_obj_set_style_radius(c, r.sm, LV_PART_MAIN);
            lv_obj_add_event_cb(c, on_field_click, LV_EVENT_CLICKED, this);

            lv_obj_t* lbl_h = lv_label_create(c);
            lv_label_set_text(lbl_h, tr(e_defs[k].key).c_str());
            lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
            lv_obj_set_pos(lbl_h, 6, 2);

            lv_obj_t* lbl_v = lv_label_create(c);
            if (field_focus_idx_ == f_idx && is_editing_field_) {
              char edit_buf[32];
              std::snprintf(edit_buf, sizeof(edit_buf), "◂ %d ▸", e_defs[k].val);
              lv_label_set_text(lbl_v, edit_buf);
            } else {
              lv_label_set_text(lbl_v, std::to_string(e_defs[k].val).c_str());
            }
            lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
            lv_obj_set_pos(lbl_v, 6, 22);

            field_objs_.push_back(c);
            field_val_objs_.push_back(lbl_v);
          }
        }
      } else if (va_sound_group_ == VaSoundGroup::Lfo) {
        lv_obj_t* wave_seg = lv_obj_create(panel_col_);
        lv_obj_set_size(wave_seg, 720, 44);
        lv_obj_set_scrollable(wave_seg, false);
        lv_obj_set_style_bg_color(wave_seg, p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(wave_seg, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(wave_seg, p.line_strong, LV_PART_MAIN);
        lv_obj_set_style_border_width(wave_seg, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(wave_seg, r.sm, LV_PART_MAIN);
        lv_obj_set_style_pad_all(wave_seg, 2, LV_PART_MAIN);
        lv_obj_add_event_cb(wave_seg, on_field_click, LV_EVENT_CLICKED, this);
        field_objs_.push_back(wave_seg);

        for (int w = 0; w < 5; ++w) {
          lv_obj_t* seg = lv_obj_create(wave_seg);
          lv_obj_set_size(seg, 142, 36);
          lv_obj_set_pos(seg, w * 143, 0);
          lv_obj_set_scrollable(seg, false);
          bool is_w_active = (va_settings_.lfo_wave == w);
          lv_obj_set_style_bg_color(seg, is_w_active ? p.accent : p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_width(seg, 0, LV_PART_MAIN);
          lv_obj_set_style_radius(seg, r.sm, LV_PART_MAIN);

          lv_obj_t* lbl_w = lv_label_create(seg);
          lv_label_set_text(lbl_w, tr(kVaLfoWaveKeys[w]).c_str());
          lv_obj_add_style(lbl_w, Theme::style_text(), LV_PART_MAIN);
          if (is_w_active) {
            lv_obj_set_style_text_color(lbl_w, p.on_accent, LV_PART_MAIN);
          }
          lv_obj_align(lbl_w, LV_ALIGN_CENTER, 0, 0);
          if (w == 0) field_val_objs_.push_back(lbl_w);
        }

        struct VaLfoDef { std::string key; std::string val; };
        std::vector<VaLfoDef> lfo_defs = {
          {"va.param.rate", std::to_string(va_settings_.lfo_rate)},
          {"va.param.delay", std::to_string(va_settings_.lfo_delay)},
          {"fm.param.key_sync", va_settings_.lfo_key_sync ? tr("part_view.zone.on") : tr("part_view.zone.off")},
          {"va.param.to_pitch", std::to_string(va_settings_.lfo_to_pitch)},
          {"va.param.to_filter", std::to_string(va_settings_.lfo_to_filter)},
          {"va.param.to_amp", std::to_string(va_settings_.lfo_to_amp)}
        };
        for (size_t f = 0; f < lfo_defs.size(); ++f) {
          int col = f % 3;
          int row = f / 3;
          lv_obj_t* card = lv_obj_create(panel_col_);
          lv_obj_set_size(card, 234, sp.touch_target_min);
          lv_obj_set_pos(card, col * 243, 108 + row * 60);
          lv_obj_set_scrollable(card, false);
          lv_obj_set_clickable(card, true);
          lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
          lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
          lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
          lv_obj_set_style_pad_hor(card, sp.space_3, LV_PART_MAIN);
          lv_obj_add_event_cb(card, on_field_click, LV_EVENT_CLICKED, this);

          lv_obj_t* lbl_h = lv_label_create(card);
          lv_label_set_text(lbl_h, tr(lfo_defs[f].key).c_str());
          lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
          lv_obj_align(lbl_h, LV_ALIGN_LEFT_MID, 0, 0);

          lv_obj_t* lbl_v = lv_label_create(card);
          if (field_focus_idx_ == static_cast<int32_t>(f + 1) && is_editing_field_) {
            char edit_buf[32];
            std::snprintf(edit_buf, sizeof(edit_buf), "◂ %s ▸", lfo_defs[f].val.c_str());
            lv_label_set_text(lbl_v, edit_buf);
          } else {
            lv_label_set_text(lbl_v, lfo_defs[f].val.c_str());
          }
          lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
          lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, 0, 0);

          field_objs_.push_back(card);
          field_val_objs_.push_back(lbl_v);
        }
      } else if (va_sound_group_ == VaSoundGroup::Voice) {
        lv_obj_t* vmode_seg = lv_obj_create(panel_col_);
        lv_obj_set_size(vmode_seg, 720, 44);
        lv_obj_set_scrollable(vmode_seg, false);
        lv_obj_set_style_bg_color(vmode_seg, p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(vmode_seg, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(vmode_seg, p.line_strong, LV_PART_MAIN);
        lv_obj_set_style_border_width(vmode_seg, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(vmode_seg, r.sm, LV_PART_MAIN);
        lv_obj_set_style_pad_all(vmode_seg, 2, LV_PART_MAIN);
        lv_obj_add_event_cb(vmode_seg, on_field_click, LV_EVENT_CLICKED, this);
        field_objs_.push_back(vmode_seg);

        for (int m = 0; m < 3; ++m) {
          lv_obj_t* seg = lv_obj_create(vmode_seg);
          lv_obj_set_size(seg, 238, 36);
          lv_obj_set_pos(seg, m * 239, 0);
          lv_obj_set_scrollable(seg, false);
          bool is_m_active = (va_settings_.voice_mode == m);
          lv_obj_set_style_bg_color(seg, is_m_active ? p.accent : p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_width(seg, 0, LV_PART_MAIN);
          lv_obj_set_style_radius(seg, r.sm, LV_PART_MAIN);

          lv_obj_t* lbl_m = lv_label_create(seg);
          lv_label_set_text(lbl_m, tr(kVaVoiceModeKeys[m]).c_str());
          lv_obj_add_style(lbl_m, Theme::style_text(), LV_PART_MAIN);
          if (is_m_active) {
            lv_obj_set_style_text_color(lbl_m, p.on_accent, LV_PART_MAIN);
          }
          lv_obj_align(lbl_m, LV_ALIGN_CENTER, 0, 0);
          if (m == 0) field_val_objs_.push_back(lbl_m);
        }

        struct VoiceDef { std::string key; int val; };
        std::vector<VoiceDef> v_defs = {
          {"va.param.unison_voices", va_settings_.unison_voices},
          {"va.param.unison_detune", va_settings_.unison_detune},
          {"va.param.glide_time", va_settings_.glide_time}
        };
        for (size_t f = 0; f < v_defs.size(); ++f) {
          lv_obj_t* card = lv_obj_create(panel_col_);
          lv_obj_set_size(card, 234, sp.touch_target_min);
          lv_obj_set_pos(card, f * 243, 108);
          lv_obj_set_scrollable(card, false);
          lv_obj_set_clickable(card, true);
          lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
          lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
          lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
          lv_obj_set_style_pad_hor(card, sp.space_3, LV_PART_MAIN);
          lv_obj_add_event_cb(card, on_field_click, LV_EVENT_CLICKED, this);

          lv_obj_t* lbl_h = lv_label_create(card);
          lv_label_set_text(lbl_h, tr(v_defs[f].key).c_str());
          lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
          lv_obj_align(lbl_h, LV_ALIGN_LEFT_MID, 0, 0);

          lv_obj_t* lbl_v = lv_label_create(card);
          if (field_focus_idx_ == static_cast<int32_t>(f + 1) && is_editing_field_) {
            char edit_buf[32];
            std::snprintf(edit_buf, sizeof(edit_buf), "◂ %d ▸", v_defs[f].val);
            lv_label_set_text(lbl_v, edit_buf);
          } else {
            lv_label_set_text(lbl_v, std::to_string(v_defs[f].val).c_str());
          }
          lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
          lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, 0, 0);

          field_objs_.push_back(card);
          field_val_objs_.push_back(lbl_v);
        }

        lv_obj_t* gmode_seg = lv_obj_create(panel_col_);
        lv_obj_set_size(gmode_seg, 720, 44);
        lv_obj_set_scrollable(gmode_seg, false);
        lv_obj_set_style_bg_color(gmode_seg, p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(gmode_seg, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(gmode_seg, p.line_strong, LV_PART_MAIN);
        lv_obj_set_style_border_width(gmode_seg, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(gmode_seg, r.sm, LV_PART_MAIN);
        lv_obj_set_style_pad_all(gmode_seg, 2, LV_PART_MAIN);
        lv_obj_add_event_cb(gmode_seg, on_field_click, LV_EVENT_CLICKED, this);
        field_objs_.push_back(gmode_seg);

        for (int gm = 0; gm < 2; ++gm) {
          lv_obj_t* seg = lv_obj_create(gmode_seg);
          lv_obj_set_size(seg, 358, 36);
          lv_obj_set_pos(seg, gm * 359, 0);
          lv_obj_set_scrollable(seg, false);
          bool is_gm_active = (va_settings_.glide_mode == gm);
          lv_obj_set_style_bg_color(seg, is_gm_active ? p.accent : p.surface, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_border_width(seg, 0, LV_PART_MAIN);
          lv_obj_set_style_radius(seg, r.sm, LV_PART_MAIN);

          lv_obj_t* lbl_gm = lv_label_create(seg);
          lv_label_set_text(lbl_gm, tr(kVaGlideModeKeys[gm]).c_str());
          lv_obj_add_style(lbl_gm, Theme::style_text(), LV_PART_MAIN);
          if (is_gm_active) {
            lv_obj_set_style_text_color(lbl_gm, p.on_accent, LV_PART_MAIN);
          }
          lv_obj_align(lbl_gm, LV_ALIGN_CENTER, 0, 0);
          if (gm == 0) field_val_objs_.push_back(lbl_gm);
        }
      }

      // Bottom Row: Brightness (Knob 2)
      lv_obj_t* b_card = lv_obj_create(panel_col_);
      lv_obj_set_size(b_card, 720, sp.touch_target_min);
      lv_obj_set_scrollable(b_card, false);
      lv_obj_set_style_bg_color(b_card, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(b_card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(b_card, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(b_card, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(b_card, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_hor(b_card, sp.space_3, LV_PART_MAIN);

      lv_obj_t* lbl_bh = lv_label_create(b_card);
      lv_label_set_text(lbl_bh, tr("sound.brightness.knob").c_str());
      lv_obj_add_style(lbl_bh, Theme::style_text_muted(), LV_PART_MAIN);
      lv_obj_align(lbl_bh, LV_ALIGN_LEFT_MID, 0, -8);

      lv_obj_t* lbl_bv = lv_label_create(b_card);
      lv_label_set_text(lbl_bv, std::to_string(va_settings_.brightness_offset).c_str());
      lv_obj_add_style(lbl_bv, Theme::style_text(), LV_PART_MAIN);
      lv_obj_align(lbl_bv, LV_ALIGN_LEFT_MID, 0, 10);

      lv_obj_t* lbl_bn = lv_label_create(b_card);
      lv_label_set_text(lbl_bn, tr("va.brightness.note").c_str());
      lv_obj_add_style(lbl_bn, Theme::style_text_muted(), LV_PART_MAIN);
      lv_obj_align(lbl_bn, LV_ALIGN_RIGHT_MID, 0, 0);

      std::vector<HintItem> va_hints = {
          {"VALUE", is_editing_field_ ? tr("hint.verb.change") : (sound_group_focus_ ? tr("hint.verb.select") : tr("hint.verb.select"))},
          {"WRITE", is_editing_field_ ? tr("hint.verb.confirm") : tr("hint.verb.change")},
          {"EXIT", tr("hint.verb.back")}
      };
      skeleton_.set_hints(va_hints, tr("sound.brightness.always_works"));

    } else {
      // Sample sound section
      char b_buf[32], c_buf[32], r_buf[32], v_buf[32];
      std::snprintf(b_buf, sizeof(b_buf), "%d%%", static_cast<int>(brightness_ * 100.0f));
      std::snprintf(c_buf, sizeof(c_buf), "%d Hz", static_cast<int>(cutoff_hz_));
      std::snprintf(r_buf, sizeof(r_buf), "%d%%", static_cast<int>(resonance_ * 100.0f));
      std::snprintf(v_buf, sizeof(v_buf), "%d%%", static_cast<int>(vel_sense_ * 100.0f));

      struct SampleFieldDef {
        std::string label_key;
        std::string val_text;
      };
      std::vector<SampleFieldDef> defs = {
          {"part_view.sound.brightness", b_buf},
          {"part_view.sound.cutoff", c_buf},
          {"part_view.sound.resonance", r_buf},
          {"part_view.sound.vel_sense", v_buf}
      };

      for (size_t i = 0; i < defs.size(); ++i) {
        lv_obj_t* card = lv_obj_create(panel_col_);
        lv_obj_set_size(card, 720, sp.touch_target_min);
        lv_obj_set_scrollable(card, false);
        lv_obj_set_clickable(card, true);
        lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(card, sp.space_3, LV_PART_MAIN);
        lv_obj_add_event_cb(card, on_field_click, LV_EVENT_CLICKED, this);

        lv_obj_t* lbl_h = lv_label_create(card);
        lv_label_set_text(lbl_h, tr(defs[i].label_key).c_str());
        lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
        lv_obj_align(lbl_h, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* lbl_v = lv_label_create(card);
        lv_label_set_text(lbl_v, defs[i].val_text.c_str());
        lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
        lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, 0, 0);

        field_objs_.push_back(card);
        field_val_objs_.push_back(lbl_v);
      }
    }
  } else if (current_section_ == PartSection::EnvelopeEQ) {
    char a_buf[32], d_buf[32], s_buf[32], rel_buf[32];
    char eq_l_buf[32], eq_m_buf[32], eq_mf_buf[32], eq_h_buf[32];
    std::snprintf(a_buf, sizeof(a_buf), "%d ms", static_cast<int>(attack_ms_));
    std::snprintf(d_buf, sizeof(d_buf), "%d ms", static_cast<int>(decay_ms_));
    std::snprintf(s_buf, sizeof(s_buf), "%d%%", static_cast<int>(sustain_ * 100.0f));
    std::snprintf(rel_buf, sizeof(rel_buf), "%d ms", static_cast<int>(release_ms_));

    std::snprintf(eq_l_buf, sizeof(eq_l_buf), "%+.1f dB", eq_low_db_);
    std::snprintf(eq_m_buf, sizeof(eq_m_buf), "%+.1f dB", eq_mid_db_);
    std::snprintf(eq_mf_buf, sizeof(eq_mf_buf), "%d Hz", static_cast<int>(eq_mid_freq_hz_));
    std::snprintf(eq_h_buf, sizeof(eq_h_buf), "%+.1f dB", eq_high_db_);

    struct EnvEqFieldDef {
      std::string label_key;
      std::string val_text;
    };
    std::vector<EnvEqFieldDef> defs = {
        {"part_view.env.attack", a_buf},
        {"part_view.env.decay", d_buf},
        {"part_view.env.sustain", s_buf},
        {"part_view.env.release", rel_buf},
        {"part_view.eq.low", eq_l_buf},
        {"part_view.eq.mid", eq_m_buf},
        {"part_view.eq.mid_freq", eq_mf_buf},
        {"part_view.eq.high", eq_h_buf}
    };

    for (size_t i = 0; i < defs.size(); ++i) {
      lv_obj_t* card = lv_obj_create(panel_col_);
      lv_obj_set_size(card, 720, sp.touch_target_min);
      lv_obj_set_scrollable(card, false);
      lv_obj_set_clickable(card, true);
      lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_hor(card, sp.space_3, LV_PART_MAIN);
      lv_obj_add_event_cb(card, on_field_click, LV_EVENT_CLICKED, this);

      lv_obj_t* lbl_h = lv_label_create(card);
      lv_label_set_text(lbl_h, tr(defs[i].label_key).c_str());
      lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
      lv_obj_align(lbl_h, LV_ALIGN_LEFT_MID, 0, 0);

      lv_obj_t* lbl_v = lv_label_create(card);
      lv_label_set_text(lbl_v, defs[i].val_text.c_str());
      lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
      lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, 0, 0);

      field_objs_.push_back(card);
      field_val_objs_.push_back(lbl_v);
    }
  } else if (current_section_ == PartSection::EffectsSends) {
    // 1. Open Inserts Button (56px touch target)
    lv_obj_t* btn_ins = lv_obj_create(panel_col_);
    lv_obj_set_size(btn_ins, 720, sp.touch_target_min);
    lv_obj_set_scrollable(btn_ins, false);
    lv_obj_set_clickable(btn_ins, true);
    lv_obj_set_style_bg_color(btn_ins, p.raised, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_ins, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_ins, p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_ins, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_ins, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn_ins, sp.space_3, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_ins, [](lv_event_t* e) {
      auto* self = static_cast<PartView*>(lv_event_get_user_data(e));
      if (self) self->open_inserts_chain();
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_ins = lv_label_create(btn_ins);
    lv_label_set_text(lbl_ins, tr("part_view.fx.open_inserts").c_str());
    lv_obj_add_style(lbl_ins, Theme::style_text(), LV_PART_MAIN);
    lv_obj_align(lbl_ins, LV_ALIGN_CENTER, 0, 0);

    field_objs_.push_back(btn_ins);

    // 2. Reverb & Delay Sends
    char rev_buf[32], dly_buf[32];
    std::snprintf(rev_buf, sizeof(rev_buf), "%d%%", static_cast<int>(reverb_send_ * 100.0f));
    std::snprintf(dly_buf, sizeof(dly_buf), "%d%%", static_cast<int>(delay_send_ * 100.0f));

    struct FxSendDef {
      std::string label_key;
      std::string val_text;
    };
    std::vector<FxSendDef> defs = {
        {"part_view.fx.reverb_send", rev_buf},
        {"part_view.fx.delay_send", dly_buf}
    };

    for (size_t i = 0; i < defs.size(); ++i) {
      lv_obj_t* card = lv_obj_create(panel_col_);
      lv_obj_set_size(card, 720, sp.touch_target_min);
      lv_obj_set_scrollable(card, false);
      lv_obj_set_clickable(card, true);
      lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_hor(card, sp.space_3, LV_PART_MAIN);
      lv_obj_add_event_cb(card, on_field_click, LV_EVENT_CLICKED, this);

      lv_obj_t* lbl_h = lv_label_create(card);
      lv_label_set_text(lbl_h, tr(defs[i].label_key).c_str());
      lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
      lv_obj_align(lbl_h, LV_ALIGN_LEFT_MID, 0, 0);

      lv_obj_t* lbl_v = lv_label_create(card);
      lv_label_set_text(lbl_v, defs[i].val_text.c_str());
      lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
      lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, 0, 0);

      field_objs_.push_back(card);
      field_val_objs_.push_back(lbl_v);
    }
  } else if (current_section_ == PartSection::Arpeggiator) {
    const auto& a = arp_settings_[part_idx_];

    char gate_buf[32], swing_buf[32], tempo_buf[64];
    std::snprintf(gate_buf, sizeof(gate_buf), "%s", format_cldr_percent(static_cast<int>(std::round(a.gate * 100.0f))).c_str());
    std::snprintf(swing_buf, sizeof(swing_buf), "%s", format_cldr_percent(static_cast<int>(std::round(a.swing * 100.0f))).c_str());

    if (tempo_read_only_) {
      std::snprintf(tempo_buf, sizeof(tempo_buf), "♩ %.0f · %s", tempo_bpm_, clock_source_.c_str());
    } else {
      std::snprintf(tempo_buf, sizeof(tempo_buf), "♩ %.1f", tempo_bpm_);
    }

    struct ArpFieldDef {
      std::string label_key;
      std::string val_text;
    };

    std::vector<ArpFieldDef> defs = {
        {"part_view.arp.on", a.on ? tr("part_view.zone.on") : tr("part_view.zone.off")},
        {"part_view.arp.mode", tr(kArpModeKeys[std::clamp(a.mode, 0, 4)])},
        {"part_view.arp.rate", kArpRateNames[std::clamp(a.rate, 0, 7)]},
        {"part_view.arp.octaves", std::to_string(a.octaves)},
        {"part_view.arp.gate", gate_buf},
        {"part_view.arp.swing", swing_buf},
        {"part_view.arp.latch", a.latch ? tr("part_view.zone.on") : tr("part_view.zone.off")},
        {"part_view.arp.tempo", tempo_buf}
    };

    for (size_t i = 0; i < defs.size(); ++i) {
      lv_obj_t* card = lv_obj_create(panel_col_);
      lv_obj_set_size(card, 720, 50);
      lv_obj_set_scrollable(card, false);
      lv_obj_set_clickable(card, true);
      lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(card, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_hor(card, sp.space_3, LV_PART_MAIN);
      lv_obj_add_event_cb(card, on_field_click, LV_EVENT_CLICKED, this);

      lv_obj_t* lbl_h = lv_label_create(card);
      lv_label_set_text(lbl_h, tr(defs[i].label_key).c_str());
      lv_obj_add_style(lbl_h, Theme::style_text_muted(), LV_PART_MAIN);
      lv_obj_align(lbl_h, LV_ALIGN_LEFT_MID, 0, 0);

      lv_obj_t* lbl_v = lv_label_create(card);
      lv_label_set_text(lbl_v, defs[i].val_text.c_str());
      lv_obj_add_style(lbl_v, Theme::style_text(), LV_PART_MAIN);
      lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, 0, 0);

      field_objs_.push_back(card);
      field_val_objs_.push_back(lbl_v);
    }
  }

  update_field_focus_ui();
}

void PartView::update_field_focus_ui() {
  const auto& p = Theme::current();
  for (size_t i = 0; i < field_objs_.size(); ++i) {
    bool focused = (!sound_group_focus_ && field_focus_idx_ == static_cast<int32_t>(i));
    lv_obj_t* obj = field_objs_[i];
    if (focused) {
      lv_obj_set_style_border_color(obj, p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN);
    } else {
      lv_obj_set_style_border_color(obj, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    }
  }
}

void PartView::mark_edits() {
  has_unsaved_edits_ = true;
  if (on_edit_) {
    on_edit_(part_idx_);
  }
}

void PartView::open_timbre_picker() {
  timbre_picker_open_ = true;
  picker_focus_idx_ = 0;
  if (picker_modal_) {
    lv_obj_set_hidden(picker_modal_, false);
  }
}

void PartView::close_timbre_picker() {
  timbre_picker_open_ = false;
  if (picker_modal_) {
    lv_obj_set_hidden(picker_modal_, true);
  }
}

void PartView::select_timbre(size_t index) {
  if (index >= timbres_catalog_.size()) return;
  timbre_name_ = timbres_catalog_[index].name;
  category_name_ = timbres_catalog_[index].category;
  has_unsaved_edits_ = true;
  if (on_timbre_change_) {
    on_timbre_change_(part_idx_, timbres_catalog_[index].id);
  } else if (on_edit_) {
    on_edit_(part_idx_);
  }
  close_timbre_picker();
  rebuild_top_bar();
  rebuild_panel();

  if (client_) {
    ws::v1::ClientMessage msg;
    msg.set_request_id(450);
    auto* cmd = msg.mutable_set_part_timbre();
    cmd->set_part(static_cast<uint32_t>(part_idx_));
    cmd->set_library_id("installed");
    cmd->set_instrument_key(timbres_catalog_[index].id);
    client_->send_command(msg);
  }
}

void PartView::on_midi_note(uint8_t note_number) {
  if (current_section_ == PartSection::ZoneKeyboard) {
    if (field_focus_idx_ == 0) {
      // Low Key
      low_key_ = std::min(static_cast<int32_t>(note_number), high_key_);
      mark_edits();
      rebuild_panel();
    } else if (field_focus_idx_ == 1) {
      // High Key
      high_key_ = std::max(static_cast<int32_t>(note_number), low_key_);
      mark_edits();
      rebuild_panel();
    }
  }
}

void PartView::on_panel_event(const PanelEvent& ev) {
  if (timbre_picker_open_) {
    if (ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
      picker_focus_idx_ = std::clamp(picker_focus_idx_ + ev.delta, 0, static_cast<int32_t>(timbres_catalog_.size() - 1));
      return;
    }
    if (ev.type == PanelEventType::Button && ev.pressed) {
      if (ev.id == MCU_BTN_EXIT) {
        close_timbre_picker();
        return;
      }
      if (ev.id == MCU_BTN_WRITE || ev.id == MCU_BTN_VALUE_PUSH) {
        select_timbre(static_cast<size_t>(picker_focus_idx_));
        return;
      }
    }
    return;
  }

  if (ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
    if (current_section_ == PartSection::Timbre) {
      // No numeric fields, nothing to edit directly
      return;
    } else if (current_section_ == PartSection::ZoneKeyboard) {
      switch (field_focus_idx_) {
        case 0:
          low_key_ = std::clamp(low_key_ + ev.delta, 0, high_key_);
          mark_edits();
          break;
        case 1:
          high_key_ = std::clamp(high_key_ + ev.delta, low_key_, 127);
          mark_edits();
          break;
        case 2:
          octave_ = std::clamp(octave_ + ev.delta, -3, 3);
          mark_edits();
          break;
        case 3:
          transpose_ = std::clamp(transpose_ + ev.delta, -12, 12);
          mark_edits();
          break;
        case 4:
          keyboard_on_ = !keyboard_on_;
          mark_edits();
          break;
        default:
          break;
      }
      rebuild_panel();
    } else if (current_section_ == PartSection::LevelOutput) {
      switch (field_focus_idx_) {
        case 0:
          volume_ = std::clamp(volume_ + (ev.delta * 0.01f), 0.0f, 1.0f);
          mark_edits();
          break;
        case 1:
          aux_output_ = !aux_output_;
          mark_edits();
          break;
        default:
          break;
      }
      rebuild_panel();
    } else if (current_section_ == PartSection::SoundEngine) {
      if (is_engine_unavailable_) {
        // No editable parameters when engine is unavailable
        return;
      }
      if (is_organ()) {
        switch (field_focus_idx_) {
          case 0:
            organ_percussion_on_ = !organ_percussion_on_;
            mark_edits();
            break;
          case 1:
            organ_key_click_ = std::clamp(organ_key_click_ + ev.delta, 0, 8);
            mark_edits();
            break;
          case 2:
            organ_vibrato_ = std::clamp(organ_vibrato_ + ev.delta, 0, 6);
            mark_edits();
            break;
          case 3:
            organ_rotary_speed_ = std::clamp(organ_rotary_speed_ + ev.delta, 0, 2);
            mark_edits();
            break;
          default:
            break;
        }
        rebuild_panel();
      } else if (is_fm()) {
        if (sound_group_focus_) {
          int cur = static_cast<int>(fm_sound_group_);
          cur = std::clamp(cur + ev.delta, 0, 3);
          fm_sound_group_ = static_cast<FmSoundGroup>(cur);
          field_focus_idx_ = 0;
          rebuild_panel();
        } else if (!is_editing_field_) {
          int count = get_fm_field_count(fm_sound_group_);
          if (count > 0) {
            field_focus_idx_ = std::clamp(field_focus_idx_ + ev.delta, 0, count - 1);
            rebuild_panel();
          }
        } else {
          modify_fm_param(fm_settings_, fm_sound_group_, field_focus_idx_, ev.delta);
          mark_edits();
          rebuild_panel();
        }
      } else if (is_va()) {
        if (sound_group_focus_) {
          int cur = static_cast<int>(va_sound_group_);
          cur = std::clamp(cur + ev.delta, 0, 5);
          va_sound_group_ = static_cast<VaSoundGroup>(cur);
          field_focus_idx_ = 0;
          rebuild_panel();
        } else if (!is_editing_field_) {
          int count = get_va_field_count(va_sound_group_);
          if (count > 0) {
            field_focus_idx_ = std::clamp(field_focus_idx_ + ev.delta, 0, count - 1);
            rebuild_panel();
          }
        } else {
          modify_va_param(va_settings_, va_sound_group_, field_focus_idx_, ev.delta);
          mark_edits();
          rebuild_panel();
        }
      } else {
        switch (field_focus_idx_) {
          case 0:
            brightness_ = std::clamp(brightness_ + (ev.delta * 0.02f), 0.0f, 1.0f);
            mark_edits();
            break;
          case 1:
            cutoff_hz_ = std::clamp(cutoff_hz_ + (ev.delta * 200.0f), 20.0f, 20000.0f);
            mark_edits();
            break;
          case 2:
            resonance_ = std::clamp(resonance_ + (ev.delta * 0.02f), 0.0f, 1.0f);
            mark_edits();
            break;
          case 3:
            vel_sense_ = std::clamp(vel_sense_ + (ev.delta * 0.02f), 0.0f, 1.0f);
            mark_edits();
            break;
          default:
            break;
        }
        rebuild_panel();
      }
    } else if (current_section_ == PartSection::EnvelopeEQ) {
      switch (field_focus_idx_) {
        case 0:
          attack_ms_ = std::clamp(attack_ms_ + (ev.delta * 10.0f), 0.0f, 5000.0f);
          mark_edits();
          break;
        case 1:
          decay_ms_ = std::clamp(decay_ms_ + (ev.delta * 10.0f), 0.0f, 5000.0f);
          mark_edits();
          break;
        case 2:
          sustain_ = std::clamp(sustain_ + (ev.delta * 0.02f), 0.0f, 1.0f);
          mark_edits();
          break;
        case 3:
          release_ms_ = std::clamp(release_ms_ + (ev.delta * 10.0f), 0.0f, 5000.0f);
          mark_edits();
          break;
        case 4:
          eq_low_db_ = std::clamp(eq_low_db_ + (ev.delta * 0.5f), -12.0f, 12.0f);
          mark_edits();
          break;
        case 5:
          eq_mid_db_ = std::clamp(eq_mid_db_ + (ev.delta * 0.5f), -12.0f, 12.0f);
          mark_edits();
          break;
        case 6:
          eq_mid_freq_hz_ = std::clamp(eq_mid_freq_hz_ + (ev.delta * 50.0f), 200.0f, 8000.0f);
          mark_edits();
          break;
        case 7:
          eq_high_db_ = std::clamp(eq_high_db_ + (ev.delta * 0.5f), -12.0f, 12.0f);
          mark_edits();
          break;
        default:
          break;
      }
      rebuild_panel();
    } else if (current_section_ == PartSection::EffectsSends) {
      switch (field_focus_idx_) {
        case 1:
          reverb_send_ = std::clamp(reverb_send_ + (ev.delta * 0.02f), 0.0f, 1.0f);
          mark_edits();
          rebuild_panel();
          break;
        case 2:
          delay_send_ = std::clamp(delay_send_ + (ev.delta * 0.02f), 0.0f, 1.0f);
          mark_edits();
          rebuild_panel();
          break;
        default:
          break;
      }
    } else if (current_section_ == PartSection::Arpeggiator) {
      auto& a = arp_settings_[part_idx_];
      switch (field_focus_idx_) {
        case 0:
          a.on = !a.on;
          mark_edits();
          if (on_arp_edit_) on_arp_edit_(part_idx_, a);
          break;
        case 1:
          a.mode = std::clamp(a.mode + ev.delta, 0, 4);
          mark_edits();
          if (on_arp_edit_) on_arp_edit_(part_idx_, a);
          break;
        case 2:
          a.rate = std::clamp(a.rate + ev.delta, 0, 7);
          mark_edits();
          if (on_arp_edit_) on_arp_edit_(part_idx_, a);
          break;
        case 3:
          a.octaves = std::clamp(a.octaves + ev.delta, 1, 4);
          mark_edits();
          if (on_arp_edit_) on_arp_edit_(part_idx_, a);
          break;
        case 4:
          a.gate = std::clamp(a.gate + (ev.delta * 0.05f), 0.05f, 1.0f);
          mark_edits();
          if (on_arp_edit_) on_arp_edit_(part_idx_, a);
          break;
        case 5:
          a.swing = std::clamp(a.swing + (ev.delta * 0.01f), 0.50f, 0.75f);
          mark_edits();
          if (on_arp_edit_) on_arp_edit_(part_idx_, a);
          break;
        case 6:
          a.latch = !a.latch;
          mark_edits();
          if (on_arp_edit_) on_arp_edit_(part_idx_, a);
          break;
        case 7:
          if (!tempo_read_only_) {
            tempo_bpm_ = std::clamp(tempo_bpm_ + (ev.delta * 1.0f), 20.0f, 300.0f);
            mark_edits();
            if (on_tempo_edit_) on_tempo_edit_(tempo_bpm_);
          }
          break;
        default:
          break;
      }
      rebuild_panel();
    }
    return;
  }

  // Physical Encoders 1..8
  if (ev.type == PanelEventType::Encoder && ev.id >= MCU_CTRL_ENCODER_1 && ev.id <= MCU_CTRL_ENCODER_8) {
    size_t enc_idx = static_cast<size_t>(ev.id - MCU_CTRL_ENCODER_1);
    if (current_section_ == PartSection::EnvelopeEQ) {
      switch (enc_idx) {
        case 0: attack_ms_ = std::clamp(attack_ms_ + (ev.delta * 10.0f), 0.0f, 5000.0f); break;
        case 1: decay_ms_ = std::clamp(decay_ms_ + (ev.delta * 10.0f), 0.0f, 5000.0f); break;
        case 2: sustain_ = std::clamp(sustain_ + (ev.delta * 0.02f), 0.0f, 1.0f); break;
        case 3: release_ms_ = std::clamp(release_ms_ + (ev.delta * 10.0f), 0.0f, 5000.0f); break;
        case 4: eq_low_db_ = std::clamp(eq_low_db_ + (ev.delta * 0.5f), -12.0f, 12.0f); break;
        case 5: eq_mid_db_ = std::clamp(eq_mid_db_ + (ev.delta * 0.5f), -12.0f, 12.0f); break;
        case 6: eq_mid_freq_hz_ = std::clamp(eq_mid_freq_hz_ + (ev.delta * 50.0f), 200.0f, 8000.0f); break;
        case 7: eq_high_db_ = std::clamp(eq_high_db_ + (ev.delta * 0.5f), -12.0f, 12.0f); break;
      }
      mark_edits();
      rebuild_panel();
      return;
    } else if (current_section_ == PartSection::Arpeggiator) {
      auto& a = arp_settings_[part_idx_];
      switch (enc_idx) {
        case 0: a.on = !a.on; if (on_arp_edit_) on_arp_edit_(part_idx_, a); break;
        case 1: a.mode = std::clamp(a.mode + ev.delta, 0, 4); if (on_arp_edit_) on_arp_edit_(part_idx_, a); break;
        case 2: a.rate = std::clamp(a.rate + ev.delta, 0, 7); if (on_arp_edit_) on_arp_edit_(part_idx_, a); break;
        case 3: a.octaves = std::clamp(a.octaves + ev.delta, 1, 4); if (on_arp_edit_) on_arp_edit_(part_idx_, a); break;
        case 4: a.gate = std::clamp(a.gate + (ev.delta * 0.05f), 0.05f, 1.0f); if (on_arp_edit_) on_arp_edit_(part_idx_, a); break;
        case 5: a.swing = std::clamp(a.swing + (ev.delta * 0.01f), 0.50f, 0.75f); if (on_arp_edit_) on_arp_edit_(part_idx_, a); break;
        case 6: a.latch = !a.latch; if (on_arp_edit_) on_arp_edit_(part_idx_, a); break;
        case 7:
          if (!tempo_read_only_) {
            tempo_bpm_ = std::clamp(tempo_bpm_ + (ev.delta * 1.0f), 20.0f, 300.0f);
            if (on_tempo_edit_) on_tempo_edit_(tempo_bpm_);
          }
          break;
      }
      mark_edits();
      rebuild_panel();
      return;
    } else if (current_section_ == PartSection::SoundEngine) {
      if (is_engine_unavailable_) {
        return;
      }
      if (is_organ()) {
        if (enc_idx < 8) {
          organ_drawbars_[enc_idx] = std::clamp(organ_drawbars_[enc_idx] + ev.delta, 0, 8);
          mark_edits();
          rebuild_panel();
        }
      } else if (is_fm()) {
        // Knob 2 Brightness mapping (modulators / brightness_offset)
        if (enc_idx == 1) {
          fm_settings_.brightness_offset = std::clamp(fm_settings_.brightness_offset + ev.delta, 0, 127);
          mark_edits();
          rebuild_panel();
        }
      } else if (is_va()) {
        // Knob 2 Brightness mapping (Filter Cutoff / brightness_offset)
        if (enc_idx == 1) {
          va_settings_.brightness_offset = std::clamp(va_settings_.brightness_offset + ev.delta, 0, 127);
          va_settings_.cutoff = va_settings_.brightness_offset;
          mark_edits();
          rebuild_panel();
        }
      } else {
        switch (enc_idx) {
          case 0: brightness_ = std::clamp(brightness_ + (ev.delta * 0.02f), 0.0f, 1.0f); break;
          case 1: cutoff_hz_ = std::clamp(cutoff_hz_ + (ev.delta * 200.0f), 20.0f, 20000.0f); break;
          case 2: resonance_ = std::clamp(resonance_ + (ev.delta * 0.02f), 0.0f, 1.0f); break;
          case 3: vel_sense_ = std::clamp(vel_sense_ + (ev.delta * 0.02f), 0.0f, 1.0f); break;
        }
        mark_edits();
        rebuild_panel();
      }
      return;
    } else if (current_section_ == PartSection::EffectsSends) {
      switch (enc_idx) {
        case 0: reverb_send_ = std::clamp(reverb_send_ + (ev.delta * 0.02f), 0.0f, 1.0f); break;
        case 1: delay_send_ = std::clamp(delay_send_ + (ev.delta * 0.02f), 0.0f, 1.0f); break;
      }
      mark_edits();
      rebuild_panel();
      return;
    }
  }

  if (ev.type == PanelEventType::Button && ev.pressed) {
    if (ev.id == MCU_BTN_EXIT) {
      if (handle_exit_step()) {
        return;
      }
      if (on_close_) {
        on_close_();
      }
      return;
    }

    if (ev.id == MCU_BTN_VALUE_PUSH || ev.id == MCU_BTN_WRITE) {
      if (current_section_ == PartSection::Timbre) {
        open_timbre_picker();
        return;
      }
      if (current_section_ == PartSection::SoundEngine) {
        if (is_engine_unavailable_) {
          if (on_open_library_) {
            on_open_library_(part_idx_);
          } else {
            open_timbre_picker();
          }
          return;
        }
        if (is_fm() || is_va()) {
          if (sound_group_focus_) {
            sound_group_focus_ = false;
            field_focus_idx_ = 0;
            is_editing_field_ = false;
          } else {
            is_editing_field_ = !is_editing_field_;
          }
          rebuild_panel();
          return;
        }
        if (is_organ() && field_focus_idx_ == 0) {
          organ_percussion_on_ = !organ_percussion_on_;
          mark_edits();
          rebuild_panel();
          return;
        }
      } else if (current_section_ == PartSection::ZoneKeyboard && field_focus_idx_ == 4) {
        keyboard_on_ = !keyboard_on_;
        mark_edits();
        rebuild_panel();
        return;
      } else if (current_section_ == PartSection::LevelOutput && field_focus_idx_ == 1) {
        aux_output_ = !aux_output_;
        mark_edits();
        rebuild_panel();
        return;
      } else if (current_section_ == PartSection::EffectsSends && field_focus_idx_ == 0) {
        open_inserts_chain();
        return;
      } else if (current_section_ == PartSection::Arpeggiator) {
        auto& a = arp_settings_[part_idx_];
        if (field_focus_idx_ == 0) {
          a.on = !a.on;
          mark_edits();
          if (on_arp_edit_) on_arp_edit_(part_idx_, a);
          rebuild_panel();
          return;
        } else if (field_focus_idx_ == 6) {
          a.latch = !a.latch;
          mark_edits();
          if (on_arp_edit_) on_arp_edit_(part_idx_, a);
          rebuild_panel();
          return;
        }
      }
      return;
    }
  }
}

void PartView::update_from_state(const ws::v1::State& state) {
  for (const auto& pv : state.performance_init()) {
    if (pv.param_id() == ws::v1::PARAM_TEMPO_BPM) {
      set_tempo(pv.value());
      break;
    }
  }
}

void PartView::on_nav_item_click(lv_event_t* e) {
  auto* self = static_cast<PartView*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->nav_item_objs_.size(); ++i) {
    if (self->nav_item_objs_[i] == target) {
      self->set_section(static_cast<PartSection>(i));
      break;
    }
  }
}

void PartView::on_field_click(lv_event_t* e) {
  auto* self = static_cast<PartView*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->field_objs_.size(); ++i) {
    if (self->field_objs_[i] == target) {
      if (self->is_fm() || self->is_va()) {
        if (!self->sound_group_focus_ && self->field_focus_idx_ == static_cast<int32_t>(i)) {
          self->is_editing_field_ = !self->is_editing_field_;
        } else {
          self->sound_group_focus_ = false;
          self->field_focus_idx_ = static_cast<int32_t>(i);
          self->is_editing_field_ = false;
        }
        self->rebuild_panel();
      } else {
        self->field_focus_idx_ = static_cast<int32_t>(i);
        self->update_field_focus_ui();
      }
      break;
    }
  }
}

void PartView::on_change_timbre_click(lv_event_t* e) {
  auto* self = static_cast<PartView*>(lv_event_get_user_data(e));
  if (self) {
    if (self->on_open_library_) {
      self->on_open_library_(self->part_idx_);
    } else {
      self->open_timbre_picker();
    }
  }
}

void PartView::on_picker_item_click(lv_event_t* e) {
  auto* self = static_cast<PartView*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  // Find which row in picker_list_
  uint32_t child_cnt = lv_obj_get_child_count(self->picker_list_);
  for (uint32_t i = 0; i < child_cnt; ++i) {
    if (lv_obj_get_child(self->picker_list_, static_cast<int32_t>(i)) == target) {
      self->select_timbre(i);
      break;
    }
  }
}

void PartView::on_power_click(lv_event_t* e) {
  auto* self = static_cast<PartView*>(lv_event_get_user_data(e));
  if (self) {
    self->set_power(!self->power_on_);
    self->mark_edits();
  }
}

}  // namespace velora::ui
