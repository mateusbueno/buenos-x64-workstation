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

}  // namespace

PartView::PartView(WsClient* client)
    : client_(client) {
  timbres_catalog_ = {
      {"timbre-grand", "German Grand D-274", "ACOUSTIC PIANO"},
      {"timbre-rhodes", "Rhodes Mark I 1975", "ELECTRIC PIANO"},
      {"timbre-wurlitzer", "Wurlitzer 200A", "ELECTRIC PIANO"},
      {"timbre-clav", "Clavinet D6 Funk", "KEYBOARDS"},
      {"timbre-b3", "B3 Tonewheel Organ", "ORGAN"},
      {"timbre-pad", "Warm Analog Pad", "SYNTH PAD"},
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
      "part_view.nav.level_output"
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
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s · %s · %s",
                tr("part_view.title_prefix").c_str(),
                timbre_name_.c_str(),
                category_name_.c_str());
  char title_formatted[128];
  std::snprintf(title_formatted, sizeof(title_formatted), buf, part_idx_ + 1);
  skeleton_.set_title(title_formatted);
}

void PartView::set_part_index(int32_t idx) {
  part_idx_ = std::clamp(idx, 0, 7);
  rebuild_top_bar();
  rebuild_panel();
}

void PartView::set_timbre(const std::string& name, const std::string& category) {
  timbre_name_ = name;
  category_name_ = category;
  rebuild_top_bar();
  rebuild_panel();
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
  } else if (current_section_ == PartSection::LevelOutput) {
    // 2 Fields: Volume, Output Routing
    char vol_buf[32];
    std::snprintf(vol_buf, sizeof(vol_buf), "%d%%", static_cast<int>(volume_ * 100.0f));

    struct LevelFieldDef {
      std::string label_key;
      std::string val_text;
    };
    std::vector<LevelFieldDef> defs = {
        {"part_view.level.volume", vol_buf},
        {"part_view.level.routing", aux_output_ ? tr("part_view.level.aux") : tr("part_view.level.main")}
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

  update_field_focus_ui();
}

void PartView::update_field_focus_ui() {
  const auto& p = Theme::current();
  for (size_t i = 0; i < field_objs_.size(); ++i) {
    bool focused = (field_focus_idx_ == static_cast<int32_t>(i));
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
  mark_edits();
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
    }
    return;
  }

  if (ev.type == PanelEventType::Button && ev.pressed) {
    if (ev.id == MCU_BTN_EXIT) {
      if (on_close_) {
        on_close_();
      }
      return;
    }

    if (ev.id == MCU_BTN_VALUE_PUSH) {
      if (current_section_ == PartSection::Timbre) {
        open_timbre_picker();
      } else if (current_section_ == PartSection::ZoneKeyboard && field_focus_idx_ == 4) {
        keyboard_on_ = !keyboard_on_;
        mark_edits();
        rebuild_panel();
      } else if (current_section_ == PartSection::LevelOutput && field_focus_idx_ == 1) {
        aux_output_ = !aux_output_;
        mark_edits();
        rebuild_panel();
      }
      return;
    }

    if (ev.id == MCU_BTN_WRITE) {
      if (current_section_ == PartSection::Timbre) {
        open_timbre_picker();
      }
      return;
    }
  }
}

void PartView::update_from_state(const ws::v1::State& state) {
  // Update state from ws::v1::State if needed
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
      self->field_focus_idx_ = static_cast<int32_t>(i);
      self->update_field_focus_ui();
      break;
    }
  }
}

void PartView::on_change_timbre_click(lv_event_t* e) {
  auto* self = static_cast<PartView*>(lv_event_get_user_data(e));
  if (self) self->open_timbre_picker();
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
