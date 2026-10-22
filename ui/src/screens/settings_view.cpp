#include "velora/ui/screens/settings_view.hpp"

#include <algorithm>
#include <cstdio>

#include "velora/ui/theme.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/ws_client.hpp"

namespace velora::ui {

static const std::vector<AssignableParamItem>& init_assignables() {
  static const std::vector<AssignableParamItem> kList = {
    {100, "Drawbar 16′", "Organ"},
    {101, "Drawbar 5 1/3′", "Organ"},
    {102, "Drawbar 8′", "Organ"},
    {103, "Drawbar 4′", "Organ"},
    {104, "Drawbar 2 2/3′", "Organ"},
    {105, "Drawbar 2′", "Organ"},
    {106, "Drawbar 1 3/5′", "Organ"},
    {107, "Drawbar 1 1/3′", "Organ"},
    {108, "Drawbar 1′", "Organ"},
    {110, "Percussion On", "Organ"},
    {111, "Percussion Harmonic", "Organ"},
    {112, "Percussion Decay", "Organ"},
    {113, "Percussion Volume", "Organ"},
    {114, "Vibrato / Chorus On", "Organ"},
    {115, "Vibrato / Chorus Type", "Organ"},
    {116, "Key Click", "Organ"},
    {117, "Leakage", "Organ"},
    {200, "Rotary Speed", "Organ"},
    {201, "Rotary On", "Organ"},
    {400, "Reverb Send", "Mixer"},
    {401, "Delay Send", "Mixer"},
    {801, "Master Reverb", "Master"},
    {802, "Tempo (BPM)", "Transport"}
  };
  return kList;
}

const std::vector<AssignableParamItem>& SettingsView::get_assignable_parameters() {
  return init_assignables();
}

std::string SettingsView::param_name_for_id(uint32_t param_id) {
  if (param_id == 0) {
    return tr("assign.slot.not_assigned");
  }
  for (const auto& item : get_assignable_parameters()) {
    if (item.param_id == param_id) {
      return item.name;
    }
  }
  return "Param " + std::to_string(param_id);
}

SettingsView::SettingsView(WsClient* client)
    : client_(client) {
  // Built-in factory default assign map: {401, 400, 113, 112, 116, 117, 200, 115}
  global_default_slots_ = {401, 400, 113, 112, 116, 117, 200, 115};
  live_set_slots_ = global_default_slots_;
}

SettingsView::~SettingsView() = default;

bool SettingsView::create(lv_obj_t* parent) {
  if (!skeleton_.create(parent)) return false;

  skeleton_.set_title(tr("settings.title"));

  // Top Bar Right Model/Firmware label
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();

  lv_obj_t* lbl_model = lv_label_create(skeleton_.top_bar());
  lv_label_set_text(lbl_model, tr("settings.model_info").c_str());
  lv_obj_add_style(lbl_model, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_model, LV_ALIGN_RIGHT_MID, -sp.screen_gutter, 0);

  // Setup hints
  skeleton_.set_hints({
    {"VALUE", tr("settings.hint.select_slot")},
    {"WRITE", tr("settings.hint.assign")},
    {"EXIT", tr("hint.verb.back")}
  }, tr("settings.hint.reassurance"));

  lv_obj_t* content = skeleton_.content_area();
  lv_obj_set_scrollable(content, false);

  build_sidebar();
  build_detail();
  create_picker_modal();

  update_sidebar_ui();
  update_knob_cards_ui();
  update_reassurance_ui();

  return true;
}

void SettingsView::build_sidebar() {
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  lv_obj_t* content = skeleton_.content_area();

  // Sidebar container: width 260px, full height 488px
  sidebar_container_ = lv_obj_create(content);
  lv_obj_set_size(sidebar_container_, 260, 488);
  lv_obj_set_pos(sidebar_container_, 0, 0);
  lv_obj_set_scrollable(sidebar_container_, true);
  lv_obj_set_style_bg_color(sidebar_container_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sidebar_container_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(sidebar_container_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(sidebar_container_, 1, LV_PART_MAIN);
  lv_obj_set_style_border_side(sidebar_container_, LV_BORDER_SIDE_RIGHT, LV_PART_MAIN);
  lv_obj_set_style_radius(sidebar_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(sidebar_container_, sp.space_1, LV_PART_MAIN);

  static const char* kSectionKeys[] = {
    "settings.section.display_language",
    "settings.section.keyboard_pedals",
    "settings.section.midi_clock",
    "settings.section.knobs_global_assign",
    "settings.section.computer",
    "settings.section.storage_library",
    "settings.section.system_update"
  };

  section_row_objs_.clear();
  section_label_objs_.clear();
  section_sub_objs_.clear();

  for (size_t i = 0; i < 7; ++i) {
    lv_obj_t* row = lv_obj_create(sidebar_container_);
    lv_obj_set_size(row, 244, 62);
    lv_obj_set_pos(row, 0, static_cast<int32_t>(i * 66));
    lv_obj_set_scrollable(row, false);
    lv_obj_set_style_radius(row, Theme::radius().sm, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, sp.space_2, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, sp.space_1, LV_PART_MAIN);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, tr(kSectionKeys[i]).c_str());
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 4);

    lv_obj_t* sub = lv_label_create(row);
    if (i == 3) {
      lv_label_set_text(sub, "");
      lv_obj_set_hidden(sub, true);
    } else {
      lv_label_set_text(sub, tr("settings.section.coming_soon").c_str());
      lv_obj_add_style(sub, Theme::style_text_muted(), LV_PART_MAIN);
      lv_obj_set_style_text_color(sub, p.muted, LV_PART_MAIN);
      lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    }

    lv_obj_add_event_cb(row, [](lv_event_t* e) {
      auto* self = static_cast<SettingsView*>(lv_event_get_user_data(e));
      auto target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
      for (size_t s = 0; s < self->section_row_objs_.size(); ++s) {
        if (self->section_row_objs_[s] == target) {
          self->select_section(static_cast<SettingsSection>(s));
          break;
        }
      }
    }, LV_EVENT_CLICKED, this);

    section_row_objs_.push_back(row);
    section_label_objs_.push_back(lbl);
    section_sub_objs_.push_back(sub);
  }
}

void SettingsView::build_detail() {
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  lv_obj_t* content = skeleton_.content_area();

  // Detail container: width 764px, full height 488px, pos x=260, y=0
  detail_container_ = lv_obj_create(content);
  lv_obj_set_size(detail_container_, 764, 488);
  lv_obj_set_pos(detail_container_, 260, 0);
  lv_obj_set_scrollable(detail_container_, false);
  lv_obj_set_style_bg_color(detail_container_, p.bg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(detail_container_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(detail_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(detail_container_, 0, LV_PART_MAIN);

  // Scope Segmented Control container: pos (24, 16), size 480x48
  scope_segmented_btn_ = lv_obj_create(detail_container_);
  lv_obj_set_size(scope_segmented_btn_, 480, 48);
  lv_obj_set_pos(scope_segmented_btn_, 24, 16);
  lv_obj_set_scrollable(scope_segmented_btn_, false);
  lv_obj_set_style_bg_color(scope_segmented_btn_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scope_segmented_btn_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(scope_segmented_btn_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(scope_segmented_btn_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(scope_segmented_btn_, Theme::radius().sm, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scope_segmented_btn_, 2, LV_PART_MAIN);

  // Global Default Segment Button: size 236x42
  btn_scope_global_ = lv_obj_create(scope_segmented_btn_);
  lv_obj_set_size(btn_scope_global_, 236, 42);
  lv_obj_set_pos(btn_scope_global_, 0, 0);
  lv_obj_set_scrollable(btn_scope_global_, false);
  lv_obj_set_style_radius(btn_scope_global_, Theme::radius().sm - 1, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_scope_global_, 0, LV_PART_MAIN);

  lbl_scope_global_ = lv_label_create(btn_scope_global_);
  lv_label_set_text(lbl_scope_global_, tr("settings.knobs.scope_global_default").c_str());
  lv_obj_center(lbl_scope_global_);

  lv_obj_add_event_cb(btn_scope_global_, [](lv_event_t* e) {
    auto* self = static_cast<SettingsView*>(lv_event_get_user_data(e));
    self->set_scope(AssignScope::GlobalDefault);
  }, LV_EVENT_CLICKED, this);

  // Live Set Segment Button: size 236x42, pos (238, 0)
  btn_scope_live_set_ = lv_obj_create(scope_segmented_btn_);
  lv_obj_set_size(btn_scope_live_set_, 236, 42);
  lv_obj_set_pos(btn_scope_live_set_, 238, 0);
  lv_obj_set_scrollable(btn_scope_live_set_, false);
  lv_obj_set_style_radius(btn_scope_live_set_, Theme::radius().sm - 1, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_scope_live_set_, 0, LV_PART_MAIN);

  lbl_scope_live_set_ = lv_label_create(btn_scope_live_set_);
  lv_label_set_text(lbl_scope_live_set_, tr("settings.knobs.scope_current_live_set").c_str());
  lv_obj_center(lbl_scope_live_set_);

  lv_obj_add_event_cb(btn_scope_live_set_, [](lv_event_t* e) {
    auto* self = static_cast<SettingsView*>(lv_event_get_user_data(e));
    self->set_scope(AssignScope::ThisLiveSet);
  }, LV_EVENT_CLICKED, this);

  // Reassurance explanation label below scope control: pos (24, 70)
  lbl_reassurance_ = lv_label_create(detail_container_);
  lv_obj_set_pos(lbl_reassurance_, 24, 70);
  lv_obj_add_style(lbl_reassurance_, Theme::style_text_muted(), LV_PART_MAIN);

  // 4x2 Grid of Knob Cards: pos (24, 100), size 716x368
  grid_container_ = lv_obj_create(detail_container_);
  lv_obj_set_size(grid_container_, 716, 368);
  lv_obj_set_pos(grid_container_, 24, 100);
  lv_obj_set_scrollable(grid_container_, false);
  lv_obj_set_style_bg_opa(grid_container_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(grid_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(grid_container_, 0, LV_PART_MAIN);

  slot_card_objs_.clear();
  slot_k_badge_objs_.clear();
  slot_name_objs_.clear();
  slot_sub_objs_.clear();

  const int32_t card_w = 172;
  const int32_t card_h = 170;
  const int32_t gap_x = 9;
  const int32_t gap_y = 12;

  for (size_t i = 0; i < 8; ++i) {
    int32_t col = static_cast<int32_t>(i % 4);
    int32_t row = static_cast<int32_t>(i / 4);

    lv_obj_t* card = lv_obj_create(grid_container_);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_set_pos(card, col * (card_w + gap_x), row * (card_h + gap_y));
    lv_obj_set_scrollable(card, false);
    lv_obj_set_style_radius(card, Theme::radius().md, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, sp.space_2, LV_PART_MAIN);

    // Badge: K1..K8
    lv_obj_t* lbl_k = lv_label_create(card);
    std::string k_str = "K" + std::to_string(i + 1);
    lv_label_set_text(lbl_k, k_str.c_str());
    lv_obj_add_style(lbl_k, Theme::style_text(), LV_PART_MAIN);
    lv_obj_align(lbl_k, LV_ALIGN_TOP_LEFT, 0, 0);

    // Param name
    lv_obj_t* lbl_n = lv_label_create(card);
    lv_label_set_text(lbl_n, tr("assign.slot.not_assigned").c_str());
    lv_label_set_long_mode(lbl_n, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_n, card_w - 24);
    lv_obj_align(lbl_n, LV_ALIGN_TOP_LEFT, 0, 32);

    // Sublabel
    lv_obj_t* lbl_sub = lv_label_create(card);
    lv_label_set_text(lbl_sub, "");
    lv_obj_add_style(lbl_sub, Theme::style_text_muted(), LV_PART_MAIN);
    lv_obj_align(lbl_sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_add_event_cb(card, [](lv_event_t* e) {
      auto* self = static_cast<SettingsView*>(lv_event_get_user_data(e));
      auto target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
      for (size_t s = 0; s < self->slot_card_objs_.size(); ++s) {
        if (self->slot_card_objs_[s] == target) {
          self->select_knob_slot(s);
          self->open_param_picker(s);
          break;
        }
      }
    }, LV_EVENT_CLICKED, this);

    slot_card_objs_.push_back(card);
    slot_k_badge_objs_.push_back(lbl_k);
    slot_name_objs_.push_back(lbl_n);
    slot_sub_objs_.push_back(lbl_sub);
  }
}

void SettingsView::create_picker_modal() {
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  lv_obj_t* content = skeleton_.content_area();

  // Full-size backdrop dimmed (opacity 70%)
  picker_backdrop_ = lv_obj_create(content);
  lv_obj_set_size(picker_backdrop_, 1024, 488);
  lv_obj_set_pos(picker_backdrop_, 0, 0);
  lv_obj_set_scrollable(picker_backdrop_, false);
  lv_obj_set_style_bg_color(picker_backdrop_, p.bg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(picker_backdrop_, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_border_width(picker_backdrop_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(picker_backdrop_, 0, LV_PART_MAIN);
  lv_obj_set_hidden(picker_backdrop_, true);

  // Modal card centered: 560x420
  picker_modal_ = lv_obj_create(picker_backdrop_);
  lv_obj_set_size(picker_modal_, 560, 420);
  lv_obj_center(picker_modal_);
  lv_obj_set_scrollable(picker_modal_, false);
  lv_obj_set_style_bg_color(picker_modal_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(picker_modal_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(picker_modal_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(picker_modal_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(picker_modal_, Theme::radius().lg, LV_PART_MAIN);
  lv_obj_set_style_pad_all(picker_modal_, sp.space_3, LV_PART_MAIN);

  // Title: "Assign Knob K1"
  picker_title_ = lv_label_create(picker_modal_);
  lv_label_set_text(picker_title_, tr("settings.knobs.picker_title").c_str());
  lv_obj_add_style(picker_title_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(picker_title_, LV_ALIGN_TOP_LEFT, 0, 0);

  // List container for assignable parameters: size 536x340, pos (0, 44)
  picker_list_ = lv_obj_create(picker_modal_);
  lv_obj_set_size(picker_list_, 536, 340);
  lv_obj_set_pos(picker_list_, 0, 44);
  lv_obj_set_scrollable(picker_list_, true);
  lv_obj_set_style_bg_opa(picker_list_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(picker_list_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(picker_list_, 0, LV_PART_MAIN);

  // Populate rows: Row 0 is "— Clear slot —", followed by each assignable parameter
  picker_row_objs_.clear();
  picker_param_ids_.clear();

  // Clear slot option (param_id = 0)
  picker_param_ids_.push_back(0);
  const auto& assignables = get_assignable_parameters();
  for (const auto& a : assignables) {
    picker_param_ids_.push_back(a.param_id);
  }

  for (size_t r = 0; r < picker_param_ids_.size(); ++r) {
    uint32_t pid = picker_param_ids_[r];

    lv_obj_t* row = lv_obj_create(picker_list_);
    lv_obj_set_size(row, 516, 56);
    lv_obj_set_pos(row, 0, static_cast<int32_t>(r * 60));
    lv_obj_set_scrollable(row, false);
    lv_obj_set_style_radius(row, Theme::radius().sm, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, sp.space_2, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 0, LV_PART_MAIN);

    lv_obj_t* lbl_name = lv_label_create(row);
    if (pid == 0) {
      lv_label_set_text(lbl_name, tr("settings.knobs.clear_slot").c_str());
      lv_obj_set_style_text_color(lbl_name, p.warn, LV_PART_MAIN);
    } else {
      lv_label_set_text(lbl_name, param_name_for_id(pid).c_str());
      lv_obj_set_style_text_color(lbl_name, p.text, LV_PART_MAIN);
    }
    lv_obj_align(lbl_name, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_add_event_cb(row, [](lv_event_t* e) {
      auto* self = static_cast<SettingsView*>(lv_event_get_user_data(e));
      auto target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
      for (size_t i = 0; i < self->picker_row_objs_.size(); ++i) {
        if (self->picker_row_objs_[i] == target) {
          self->select_param_in_picker(self->picker_param_ids_[i]);
          break;
        }
      }
    }, LV_EVENT_CLICKED, this);

    picker_row_objs_.push_back(row);
  }
}

void SettingsView::update_sidebar_ui() {
  const auto& p = Theme::current();

  for (size_t i = 0; i < section_row_objs_.size(); ++i) {
    lv_obj_t* row = section_row_objs_[i];
    lv_obj_t* lbl = section_label_objs_[i];

    bool is_selected = (static_cast<SettingsSection>(i) == current_section_);
    bool is_enabled = (i == 3); // Knobs · Global Assign is section 3

    if (is_selected) {
      lv_obj_set_style_bg_color(row, p.raised, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(row, p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl, p.accent, LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
      if (is_enabled) {
        lv_obj_set_style_text_color(lbl, p.text, LV_PART_MAIN);
      } else {
        lv_obj_set_style_text_color(lbl, p.muted, LV_PART_MAIN);
      }
    }
  }
}

void SettingsView::update_reassurance_ui() {
  if (!lbl_reassurance_) return;
  if (current_scope_ == AssignScope::GlobalDefault) {
    lv_label_set_text(lbl_reassurance_, tr("settings.knobs.reassurance_global").c_str());
  } else {
    lv_label_set_text(lbl_reassurance_, tr("settings.knobs.reassurance_live_set").c_str());
  }
}

void SettingsView::update_knob_cards_ui() {
  const auto& p = Theme::current();

  // Update segmented scope button styles
  if (btn_scope_global_ && btn_scope_live_set_) {
    if (current_scope_ == AssignScope::GlobalDefault) {
      lv_obj_set_style_bg_color(btn_scope_global_, p.accent, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(btn_scope_global_, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl_scope_global_, p.bg, LV_PART_MAIN);

      lv_obj_set_style_bg_opa(btn_scope_live_set_, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl_scope_live_set_, p.muted, LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_opa(btn_scope_global_, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl_scope_global_, p.muted, LV_PART_MAIN);

      lv_obj_set_style_bg_color(btn_scope_live_set_, p.accent, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(btn_scope_live_set_, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl_scope_live_set_, p.bg, LV_PART_MAIN);
    }
  }

  // Update 8 cards
  const auto& current_slots = (current_scope_ == AssignScope::GlobalDefault) ? global_default_slots_ : live_set_slots_;

  for (size_t i = 0; i < 8; ++i) {
    if (i >= slot_card_objs_.size()) break;

    lv_obj_t* card = slot_card_objs_[i];
    lv_obj_t* lbl_k = slot_k_badge_objs_[i];
    lv_obj_t* lbl_n = slot_name_objs_[i];
    lv_obj_t* lbl_sub = slot_sub_objs_[i];

    bool is_highlighted = (static_cast<int32_t>(i) == selected_slot_idx_);
    uint32_t pid = (i < current_slots.size()) ? current_slots[i] : 0;

    if (is_highlighted) {
      lv_obj_set_style_bg_color(card, p.raised, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 3, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl_k, p.accent, LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl_k, p.muted, LV_PART_MAIN);
    }

    if (pid != 0) {
      lv_label_set_text(lbl_n, param_name_for_id(pid).c_str());
      lv_obj_set_style_text_color(lbl_n, p.text, LV_PART_MAIN);

      // Find category
      std::string cat = "";
      for (const auto& a : get_assignable_parameters()) {
        if (a.param_id == pid) {
          cat = a.category;
          break;
        }
      }
      lv_label_set_text(lbl_sub, cat.c_str());
      lv_obj_set_style_text_color(lbl_sub, p.muted, LV_PART_MAIN);
      lv_obj_set_hidden(lbl_sub, false);
    } else {
      lv_label_set_text(lbl_n, tr("assign.slot.not_assigned").c_str());
      lv_obj_set_style_text_color(lbl_n, p.muted, LV_PART_MAIN);
      lv_obj_set_hidden(lbl_sub, true);
    }
  }
}

void SettingsView::update_picker_ui() {
  const auto& p = Theme::current();

  for (size_t r = 0; r < picker_row_objs_.size(); ++r) {
    lv_obj_t* row = picker_row_objs_[r];
    bool is_selected = (static_cast<int32_t>(r) == picker_selected_row_);

    if (is_selected) {
      lv_obj_set_style_bg_color(row, p.raised, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(row, p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    }
  }
}

void SettingsView::select_section(SettingsSection section) {
  // Only KnobsGlobalAssign is supported in Story 4.35
  if (section == SettingsSection::KnobsGlobalAssign) {
    current_section_ = section;
    update_sidebar_ui();
  }
}

void SettingsView::set_scope(AssignScope scope) {
  current_scope_ = scope;
  update_knob_cards_ui();
  update_reassurance_ui();
}

void SettingsView::select_knob_slot(size_t index) {
  if (index < 8) {
    selected_slot_idx_ = static_cast<int32_t>(index);
    update_knob_cards_ui();
  }
}

void SettingsView::set_global_default_slots(const std::vector<uint32_t>& param_ids) {
  global_default_slots_ = param_ids;
  if (global_default_slots_.size() < 8) {
    global_default_slots_.resize(8, 0);
  }
  update_knob_cards_ui();
}

void SettingsView::set_live_set_slots(const std::vector<uint32_t>& param_ids) {
  live_set_slots_ = param_ids;
  if (live_set_slots_.size() < 8) {
    live_set_slots_.resize(8, 0);
  }
  update_knob_cards_ui();
}

uint32_t SettingsView::get_slot_param(AssignScope scope, size_t slot_idx) const {
  const auto& slots = (scope == AssignScope::GlobalDefault) ? global_default_slots_ : live_set_slots_;
  if (slot_idx < slots.size()) {
    return slots[slot_idx];
  }
  return 0;
}

void SettingsView::open_param_picker(size_t slot_idx) {
  if (slot_idx >= 8) return;
  picker_slot_idx_ = static_cast<int32_t>(slot_idx);
  picker_open_ = true;

  // Title: "Assign Knob K1"
  if (picker_title_) {
    std::string title = tr("settings.knobs.picker_title") + " K" + std::to_string(slot_idx + 1);
    lv_label_set_text(picker_title_, title.c_str());
  }

  // Pre-select current param row
  uint32_t current_pid = get_slot_param(current_scope_, slot_idx);
  picker_selected_row_ = 0;
  for (size_t r = 0; r < picker_param_ids_.size(); ++r) {
    if (picker_param_ids_[r] == current_pid) {
      picker_selected_row_ = static_cast<int32_t>(r);
      break;
    }
  }

  if (picker_backdrop_) {
    lv_obj_set_hidden(picker_backdrop_, false);
  }
  update_picker_ui();

  // Scroll to selected row if needed
  if (picker_selected_row_ < static_cast<int32_t>(picker_row_objs_.size()) && picker_row_objs_[picker_selected_row_]) {
    lv_obj_scroll_to_view(picker_row_objs_[picker_selected_row_], LV_ANIM_OFF);
  }
}

void SettingsView::close_param_picker() {
  picker_open_ = false;
  if (picker_backdrop_) {
    lv_obj_set_hidden(picker_backdrop_, true);
  }
}

void SettingsView::select_param_in_picker(uint32_t param_id) {
  uint32_t slot_1_based = static_cast<uint32_t>(picker_slot_idx_ + 1);
  size_t idx = static_cast<size_t>(picker_slot_idx_);

  ws::v1::AssignTarget target = (current_scope_ == AssignScope::GlobalDefault)
                                    ? ws::v1::ASSIGN_TARGET_GLOBAL_DEFAULT
                                    : ws::v1::ASSIGN_TARGET_CURRENT_LIVE_SET;

  if (current_scope_ == AssignScope::GlobalDefault) {
    if (idx < global_default_slots_.size()) {
      global_default_slots_[idx] = param_id;
    }
  } else {
    if (idx < live_set_slots_.size()) {
      live_set_slots_[idx] = param_id;
    }
    has_unsaved_edits_ = true;
    if (on_edit_) {
      on_edit_();
    }
  }

  if (on_assign_changed_) {
    on_assign_changed_(target, slot_1_based, param_id);
  }

  close_param_picker();
  update_knob_cards_ui();
}

void SettingsView::update_from_state(const ws::v1::State& state) {
  if (state.has_encoders()) {
    const auto& enc = state.encoders();
    if (enc.assign_size() > 0) {
      std::vector<uint32_t> slots(8, 0);
      for (int k = 0; k < enc.assign_size() && k < 8; ++k) {
        slots[k] = enc.assign(k);
      }
      set_live_set_slots(slots);
    }
    if (enc.assign_default_size() > 0) {
      std::vector<uint32_t> def_slots(8, 0);
      for (int k = 0; k < enc.assign_default_size() && k < 8; ++k) {
        def_slots[k] = enc.assign_default(k);
      }
      set_global_default_slots(def_slots);
    }
  }

  if (state.has_edits()) {
    has_unsaved_edits_ = state.edits().current_dirty();
  }
}

void SettingsView::on_panel_event(const PanelEvent& ev) {
  if (picker_open_) {
    if (ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
      int32_t total = static_cast<int32_t>(picker_param_ids_.size());
      if (total > 0) {
        picker_selected_row_ = std::clamp(picker_selected_row_ + ev.delta, 0, total - 1);
        update_picker_ui();
        if (picker_selected_row_ < static_cast<int32_t>(picker_row_objs_.size())) {
          lv_obj_scroll_to_view(picker_row_objs_[picker_selected_row_], LV_ANIM_OFF);
        }
      }
      return;
    }

    if (ev.type == PanelEventType::Button && ev.pressed) {
      if (ev.id == MCU_BTN_VALUE_PUSH || ev.id == MCU_BTN_WRITE) {
        if (picker_selected_row_ >= 0 && picker_selected_row_ < static_cast<int32_t>(picker_param_ids_.size())) {
          select_param_in_picker(picker_param_ids_[picker_selected_row_]);
        }
        return;
      }
      if (ev.id == MCU_BTN_EXIT) {
        close_param_picker();
        return;
      }
    }
    return;
  }

  // Picker closed:
  if (ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
    int32_t next = std::clamp(selected_slot_idx_ + ev.delta, 0, 7);
    select_knob_slot(static_cast<size_t>(next));
    return;
  }

  if (ev.type == PanelEventType::Button && ev.pressed) {
    if (ev.id == MCU_BTN_VALUE_PUSH) {
      open_param_picker(static_cast<size_t>(selected_slot_idx_));
      return;
    }
    if (ev.id == MCU_BTN_EXIT) {
      if (on_close_) {
        on_close_();
      }
      return;
    }
  }
}

}  // namespace velora::ui
