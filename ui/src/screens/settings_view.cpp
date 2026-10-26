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
  libraries_ = {
    {"factory", "Factory Library", 1284, 412ULL * 1024 * 1024 * 1024, true},
    {"stage_pianos", "Stage Pianos Collection", 7, 18ULL * 1024 * 1024 * 1024, false}
  };
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
  build_storage_detail();
  build_display_detail();
  build_keyboard_pedals_detail();
  build_midi_clock_detail();
  build_computer_detail();
  build_system_update_detail();
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
    lv_label_set_text(sub, "");
    lv_obj_set_hidden(sub, true);

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

    if (is_selected) {
      lv_obj_set_style_bg_color(row, p.raised, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(row, p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl, p.accent, LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl, p.text, LV_PART_MAIN);
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

static std::string format_storage_size(uint64_t bytes, const std::string& locale_code) {
  double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
  char buf[32];
  if (locale_code == "pt-BR" || locale_code == "es") {
    snprintf(buf, sizeof(buf), "%.1f GB", gb);
    for (char* p = buf; *p; ++p) {
      if (*p == '.') *p = ',';
    }
  } else {
    snprintf(buf, sizeof(buf), "%.1f GB", gb);
  }
  return std::string(buf);
}

void SettingsView::build_storage_detail() {
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  const auto& r = Theme::radius();
  lv_obj_t* content = skeleton_.content_area();

  // Storage container: width 764px, full height 488px, pos x=260, y=0
  storage_container_ = lv_obj_create(content);
  lv_obj_set_size(storage_container_, 764, 488);
  lv_obj_set_pos(storage_container_, 260, 0);
  lv_obj_set_scrollable(storage_container_, false);
  lv_obj_set_style_bg_color(storage_container_, p.bg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(storage_container_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(storage_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(storage_container_, sp.space_3, LV_PART_MAIN);
  lv_obj_set_flex_flow(storage_container_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_hidden(storage_container_, true);

  // Free SSD label (e.g. "240.5 GB free of 480 GB")
  lbl_storage_ssd_ = lv_label_create(storage_container_);
  lv_obj_add_style(lbl_storage_ssd_, Theme::style_text(), LV_PART_MAIN);
  set_storage_info(storage_free_bytes_, storage_total_bytes_);

  // Error label
  lbl_storage_error_ = lv_label_create(storage_container_);
  lv_label_set_text(lbl_storage_error_, "");
  lv_obj_set_style_text_color(lbl_storage_error_, p.warn, LV_PART_MAIN);

  // Section title: Installed Libraries
  lv_obj_t* lbl_inst = lv_label_create(storage_container_);
  lv_label_set_text(lbl_inst, tr("storage.installed_libraries").c_str());
  lv_obj_add_style(lbl_inst, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_set_style_pad_top(lbl_inst, sp.space_2, LV_PART_MAIN);

  // List of libraries container
  storage_list_obj_ = lv_obj_create(storage_container_);
  lv_obj_set_size(storage_list_obj_, LV_PCT(100), 320);
  lv_obj_set_style_bg_opa(storage_list_obj_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(storage_list_obj_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(storage_list_obj_, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(storage_list_obj_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(storage_list_obj_, sp.space_2, LV_PART_MAIN);

  rebuild_storage_list();
}

void SettingsView::rebuild_storage_list() {
  if (!storage_list_obj_) return;
  lv_obj_clean(storage_list_obj_);

  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  const auto& r = Theme::radius();
  const std::string& loc = Locale::instance().current_locale();

  for (size_t i = 0; i < libraries_.size(); ++i) {
    const auto& lib = libraries_[i];

    lv_obj_t* row = lv_obj_create(storage_list_obj_);
    lv_obj_set_size(row, LV_PCT(100), 56);
    lv_obj_set_style_bg_color(row, p.surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, p.line, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(row, r.md, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, sp.space_3, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Left info
    lv_obj_t* col_info = lv_obj_create(row);
    lv_obj_set_size(col_info, 480, LV_PCT(100));
    lv_obj_set_style_bg_opa(col_info, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col_info, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(col_info, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(col_info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* lbl_name = lv_label_create(col_info);
    lv_label_set_text(lbl_name, lib.name.c_str());
    lv_obj_add_style(lbl_name, Theme::style_text(), LV_PART_MAIN);

    std::string sz = format_storage_size(lib.size_bytes, loc);
    char meta_buf[128];
    snprintf(meta_buf, sizeof(meta_buf), "%u timbres · %s", lib.timbre_count, sz.c_str());
    lv_obj_t* lbl_meta = lv_label_create(col_info);
    lv_label_set_text(lbl_meta, meta_buf);
    lv_obj_add_style(lbl_meta, Theme::style_text_muted(), LV_PART_MAIN);

    // Remove button (if not factory)
    if (lib.id != "factory") {
      lv_obj_t* btn_rem = lv_obj_create(row);
      lv_obj_set_size(btn_rem, 110, 40);
      lv_obj_set_style_bg_color(btn_rem, p.raised, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(btn_rem, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(btn_rem, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(btn_rem, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(btn_rem, r.sm, LV_PART_MAIN);
      lv_obj_set_flex_flow(btn_rem, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(btn_rem, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

      lv_obj_t* lbl_rem = lv_label_create(btn_rem);
      lv_label_set_text(lbl_rem, tr("storage.remove_library").c_str());
      lv_obj_set_style_text_color(lbl_rem, p.warn, LV_PART_MAIN);

      std::string captured_id = lib.id;
      lv_obj_add_event_cb(btn_rem, [](lv_event_t* e) {
        auto* pair = static_cast<std::pair<SettingsView*, std::string>*>(lv_event_get_user_data(e));
        if (pair && pair->first) {
          pair->first->remove_library(pair->second);
        }
      }, LV_EVENT_CLICKED, new std::pair<SettingsView*, std::string>(this, captured_id));
    }
  }
}

void SettingsView::set_storage_info(uint64_t free_bytes, uint64_t total_bytes) {
  storage_free_bytes_ = free_bytes;
  storage_total_bytes_ = total_bytes;
  if (lbl_storage_ssd_) {
    const std::string& loc = Locale::instance().current_locale();
    std::string free_str = format_storage_size(storage_free_bytes_, loc);
    std::string total_str = format_storage_size(storage_total_bytes_, loc);
    char buf[128];
    snprintf(buf, sizeof(buf), tr("storage.ssd_free").c_str(), free_str.c_str(), total_str.c_str());
    lv_label_set_text(lbl_storage_ssd_, buf);
  }
}

void SettingsView::set_installed_libraries(std::vector<InstalledLibraryInfo> libs) {
  libraries_ = std::move(libs);
  rebuild_storage_list();
}

bool SettingsView::remove_library(const std::string& lib_id) {
  for (auto it = libraries_.begin(); it != libraries_.end(); ++it) {
    if (it->id == lib_id) {
      if (it->in_use) {
        storage_error_ = tr("storage.in_use_refused");
        if (lbl_storage_error_) {
          lv_label_set_text(lbl_storage_error_, storage_error_.c_str());
        }
        return false;
      }
      storage_free_bytes_ += it->size_bytes;
      libraries_.erase(it);
      storage_error_.clear();
      if (lbl_storage_error_) {
        lv_label_set_text(lbl_storage_error_, "");
      }
      set_storage_info(storage_free_bytes_, storage_total_bytes_);
      rebuild_storage_list();
      return true;
    }
  }
  return false;
}

void SettingsView::select_section(SettingsSection section) {
  current_section_ = section;
  update_sidebar_ui();
  if (detail_container_) {
    lv_obj_set_hidden(detail_container_, current_section_ != SettingsSection::KnobsGlobalAssign);
  }
  if (storage_container_) {
    lv_obj_set_hidden(storage_container_, current_section_ != SettingsSection::StorageLibrary);
  }
  if (display_container_) {
    lv_obj_set_hidden(display_container_, current_section_ != SettingsSection::DisplayLanguage);
  }
  if (keyboard_pedals_container_) {
    lv_obj_set_hidden(keyboard_pedals_container_, current_section_ != SettingsSection::KeyboardPedals);
  }
  if (midi_clock_container_) {
    lv_obj_set_hidden(midi_clock_container_, current_section_ != SettingsSection::MidiClock);
  }
  if (computer_container_) {
    lv_obj_set_hidden(computer_container_, current_section_ != SettingsSection::Computer);
  }
  if (system_update_container_) {
    lv_obj_set_hidden(system_update_container_, current_section_ != SettingsSection::SystemUpdate);
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

static lv_obj_t* create_setting_card_helper(lv_obj_t* parent, int32_t y, int32_t height) {
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  const auto& r = Theme::radius();
  lv_obj_t* card = lv_obj_create(parent);
  lv_obj_set_size(card, 720, height);
  lv_obj_set_pos(card, 0, y);
  lv_obj_set_scrollable(card, false);
  lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(card, p.line, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(card, r.md, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, sp.space_2, LV_PART_MAIN);
  return card;
}

void SettingsView::build_display_detail() {
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  lv_obj_t* content = skeleton_.content_area();

  display_container_ = lv_obj_create(content);
  lv_obj_set_size(display_container_, 764, 488);
  lv_obj_set_pos(display_container_, 260, 0);
  lv_obj_set_scrollable(display_container_, true);
  lv_obj_set_style_bg_opa(display_container_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(display_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(display_container_, sp.space_3, LV_PART_MAIN);

  const auto& cfg = SystemConfig::instance();

  // 1. Theme (Stage / Studio) + guidance text
  lv_obj_t* card_th = create_setting_card_helper(display_container_, 0, 84);
  lv_obj_t* lbl_th = lv_label_create(card_th);
  lv_label_set_text(lbl_th, tr("settings.display.theme").c_str());
  lv_obj_add_style(lbl_th, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_th, LV_ALIGN_TOP_LEFT, 8, 4);

  lv_obj_t* lbl_th_guide = lv_label_create(card_th);
  lv_obj_set_width(lbl_th_guide, 440);
  lv_label_set_long_mode(lbl_th_guide, LV_LABEL_LONG_WRAP);
  lv_label_set_text(lbl_th_guide, tr("settings.display.theme_guidance").c_str());
  lv_obj_add_style(lbl_th_guide, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl_th_guide, p.muted, LV_PART_MAIN);
  lv_obj_align(lbl_th_guide, LV_ALIGN_BOTTOM_LEFT, 8, -4);

  seg_theme_.create(card_th, {
    tr("settings.display.theme_stage"),
    tr("settings.display.theme_studio")
  }, 80);
  lv_obj_align(seg_theme_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  seg_theme_.set_selected_index((cfg.theme == "studio") ? 1 : 0);
  seg_theme_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    c.theme = (idx == 1) ? "studio" : "stage";
    Theme::set_theme((idx == 1) ? ThemeId::Studio : ThemeId::Stage);
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 2. Language (English / Português / Español)
  lv_obj_t* card_lang = create_setting_card_helper(display_container_, 92, 72);
  lv_obj_t* lbl_lang = lv_label_create(card_lang);
  lv_label_set_text(lbl_lang, tr("settings.display.language").c_str());
  lv_obj_add_style(lbl_lang, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_lang, LV_ALIGN_LEFT_MID, 8, 0);

  seg_language_.create(card_lang, {
    tr("settings.display.lang_en"),
    tr("settings.display.lang_pt"),
    tr("settings.display.lang_es")
  }, 110);
  lv_obj_align(seg_language_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  int lang_idx = 0;
  if (cfg.language == "pt-BR") lang_idx = 1;
  else if (cfg.language == "es") lang_idx = 2;
  seg_language_.set_selected_index(lang_idx);
  seg_language_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    static const char* kLangs[] = {"en", "pt-BR", "es"};
    if (idx >= 0 && idx < 3) {
      c.language = kLangs[idx];
      Locale::instance().set_locale(c.language);
    }
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 3. Screen brightness
  lv_obj_t* card_sb = create_setting_card_helper(display_container_, 172, 72);
  lv_obj_t* lbl_sb = lv_label_create(card_sb);
  lv_label_set_text(lbl_sb, tr("settings.display.screen_brightness").c_str());
  lv_obj_add_style(lbl_sb, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_sb, LV_ALIGN_LEFT_MID, 8, 0);

  seg_screen_brightness_.create(card_sb, {
    "20 %", "40 %", "60 %", "80 %", "100 %"
  }, 60);
  lv_obj_align(seg_screen_brightness_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  int sb_idx = 3; // 80% default
  if (cfg.screen_brightness <= 25) sb_idx = 0;
  else if (cfg.screen_brightness <= 45) sb_idx = 1;
  else if (cfg.screen_brightness <= 65) sb_idx = 2;
  else if (cfg.screen_brightness <= 85) sb_idx = 3;
  else sb_idx = 4;
  seg_screen_brightness_.set_selected_index(sb_idx);
  seg_screen_brightness_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    static const uint32_t kSbVals[] = {20, 40, 60, 80, 100};
    if (idx >= 0 && idx < 5) c.screen_brightness = kSbVals[idx];
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 4. Panel LED brightness
  lv_obj_t* card_led = create_setting_card_helper(display_container_, 252, 72);
  lv_obj_t* lbl_led = lv_label_create(card_led);
  lv_label_set_text(lbl_led, tr("settings.display.led_brightness").c_str());
  lv_obj_add_style(lbl_led, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_led, LV_ALIGN_LEFT_MID, 8, 0);

  seg_led_brightness_.create(card_led, {
    tr("settings.display.led_low"),
    tr("settings.display.led_mid"),
    tr("settings.display.led_high"),
    tr("settings.display.led_follow")
  }, 100);
  lv_obj_align(seg_led_brightness_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  seg_led_brightness_.set_selected_index(static_cast<int32_t>(cfg.led_brightness));
  seg_led_brightness_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    c.led_brightness = static_cast<LedBrightnessMode>(idx);
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 5. Large text on Performance
  lv_obj_t* card_lt = create_setting_card_helper(display_container_, 332, 72);
  lv_obj_t* lbl_lt = lv_label_create(card_lt);
  lv_label_set_text(lbl_lt, tr("settings.display.large_text").c_str());
  lv_obj_add_style(lbl_lt, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_lt, LV_ALIGN_LEFT_MID, 8, 0);

  seg_large_text_.create(card_lt, {
    tr("state.off"),
    tr("state.on")
  }, 70);
  lv_obj_align(seg_large_text_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  seg_large_text_.set_selected_index(cfg.large_text ? 1 : 0);
  seg_large_text_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    c.large_text = (idx == 1);
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 6. Touch on Performance (Full / Safe / Off)
  lv_obj_t* card_touch = create_setting_card_helper(display_container_, 412, 72);
  lv_obj_t* lbl_touch = lv_label_create(card_touch);
  lv_label_set_text(lbl_touch, tr("settings.display.touch_performance").c_str());
  lv_obj_add_style(lbl_touch, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_touch, LV_ALIGN_LEFT_MID, 8, 0);

  seg_touch_.create(card_touch, {
    tr("settings.display.touch_full"),
    tr("settings.display.touch_safe"),
    tr("settings.display.touch_off")
  }, 90);
  lv_obj_align(seg_touch_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  seg_touch_.set_selected_index(static_cast<int32_t>(cfg.touch_mode));
  seg_touch_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    c.touch_mode = static_cast<TouchMode>(idx);
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 7. Write hold (0.3 s / 0.5 s / 0.8 s / 1.0 s)
  lv_obj_t* card_wh = create_setting_card_helper(display_container_, 492, 72);
  lv_obj_t* lbl_wh = lv_label_create(card_wh);
  lv_label_set_text(lbl_wh, tr("settings.display.write_hold").c_str());
  lv_obj_add_style(lbl_wh, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_wh, LV_ALIGN_LEFT_MID, 8, 0);

  seg_write_hold_.create(card_wh, {
    tr("settings.display.write_hold_03"),
    tr("settings.display.write_hold_05"),
    tr("settings.display.write_hold_08"),
    tr("settings.display.write_hold_10")
  }, 70);
  lv_obj_align(seg_write_hold_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  int wh_idx = 1;
  if (cfg.write_hold_ms <= 350) wh_idx = 0;
  else if (cfg.write_hold_ms <= 650) wh_idx = 1;
  else if (cfg.write_hold_ms <= 850) wh_idx = 2;
  else wh_idx = 3;
  seg_write_hold_.set_selected_index(wh_idx);
  seg_write_hold_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    static const uint32_t kHoldVals[] = {300, 500, 800, 1000};
    if (idx >= 0 && idx < 4) c.write_hold_ms = kHoldVals[idx];
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 8. Long press (400 ms / 500 ms / 800 ms)
  lv_obj_t* card_lp = create_setting_card_helper(display_container_, 572, 72);
  lv_obj_t* lbl_lp = lv_label_create(card_lp);
  lv_label_set_text(lbl_lp, tr("settings.display.long_press").c_str());
  lv_obj_add_style(lbl_lp, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_lp, LV_ALIGN_LEFT_MID, 8, 0);

  seg_long_press_.create(card_lp, {
    tr("settings.display.long_press_400"),
    tr("settings.display.long_press_500"),
    tr("settings.display.long_press_800")
  }, 80);
  lv_obj_align(seg_long_press_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  int lp_idx = 1;
  if (cfg.long_press_ms <= 450) lp_idx = 0;
  else if (cfg.long_press_ms <= 650) lp_idx = 1;
  else lp_idx = 2;
  seg_long_press_.set_selected_index(lp_idx);
  seg_long_press_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    static const uint32_t kLpVals[] = {400, 500, 800};
    if (idx >= 0 && idx < 3) c.long_press_ms = kLpVals[idx];
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 9. Panel guard (Off / On + helper text)
  lv_obj_t* card_pg = create_setting_card_helper(display_container_, 652, 84);
  lv_obj_t* lbl_pg = lv_label_create(card_pg);
  lv_label_set_text(lbl_pg, tr("settings.display.panel_guard").c_str());
  lv_obj_add_style(lbl_pg, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_pg, LV_ALIGN_TOP_LEFT, 8, 4);

  lv_obj_t* lbl_pg_desc = lv_label_create(card_pg);
  lv_obj_set_width(lbl_pg_desc, 440);
  lv_label_set_long_mode(lbl_pg_desc, LV_LABEL_LONG_WRAP);
  lv_label_set_text(lbl_pg_desc, tr("settings.display.panel_guard_desc").c_str());
  lv_obj_add_style(lbl_pg_desc, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl_pg_desc, p.muted, LV_PART_MAIN);
  lv_obj_align(lbl_pg_desc, LV_ALIGN_BOTTOM_LEFT, 8, -4);

  seg_panel_guard_.create(card_pg, {
    tr("state.off"),
    tr("state.on")
  }, 70);
  lv_obj_align(seg_panel_guard_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  seg_panel_guard_.set_selected_index(cfg.panel_guard ? 1 : 0);
  seg_panel_guard_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    c.panel_guard = (idx == 1);
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 10. Reduced motion (Off / On)
  lv_obj_t* card_rm = create_setting_card_helper(display_container_, 744, 72);
  lv_obj_t* lbl_rm = lv_label_create(card_rm);
  lv_label_set_text(lbl_rm, tr("settings.display.reduced_motion").c_str());
  lv_obj_add_style(lbl_rm, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_rm, LV_ALIGN_LEFT_MID, 8, 0);

  seg_reduced_motion_.create(card_rm, {
    tr("settings.display.reduced_motion_off"),
    tr("settings.display.reduced_motion_on")
  }, 70);
  lv_obj_align(seg_reduced_motion_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  seg_reduced_motion_.set_selected_index(cfg.reduced_motion ? 1 : 0);
  seg_reduced_motion_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    c.reduced_motion = (idx == 1);
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 11. Boot into
  lv_obj_t* card_bi = create_setting_card_helper(display_container_, 824, 72);
  lv_obj_t* lbl_bi = lv_label_create(card_bi);
  lv_label_set_text(lbl_bi, tr("settings.display.boot_into").c_str());
  lv_obj_add_style(lbl_bi, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_bi, LV_ALIGN_LEFT_MID, 8, 0);

  seg_boot_into_.create(card_bi, {
    tr("settings.display.boot_last"),
    tr("settings.display.boot_setlist"),
    tr("settings.display.boot_default")
  }, 95);
  lv_obj_align(seg_boot_into_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  seg_boot_into_.set_selected_index(cfg.boot_into);
  seg_boot_into_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    c.boot_into = static_cast<uint8_t>(idx);
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  lv_obj_set_hidden(display_container_, true);
}

void SettingsView::build_keyboard_pedals_detail() {
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  const auto& r = Theme::radius();
  lv_obj_t* content = skeleton_.content_area();

  keyboard_pedals_container_ = lv_obj_create(content);
  lv_obj_set_size(keyboard_pedals_container_, 764, 488);
  lv_obj_set_pos(keyboard_pedals_container_, 260, 0);
  lv_obj_set_scrollable(keyboard_pedals_container_, true);
  lv_obj_set_style_bg_opa(keyboard_pedals_container_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(keyboard_pedals_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(keyboard_pedals_container_, sp.space_3, LV_PART_MAIN);

  const auto& cfg = SystemConfig::instance();

  // 1. Velocity curve (Light / Normal / Heavy / Fixed)
  lv_obj_t* card_vc = create_setting_card_helper(keyboard_pedals_container_, 0, 72);
  lv_obj_t* lbl_vc = lv_label_create(card_vc);
  lv_label_set_text(lbl_vc, tr("settings.key.velocity_curve").c_str());
  lv_obj_add_style(lbl_vc, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_vc, LV_ALIGN_LEFT_MID, 8, 0);

  seg_velocity_curve_.create(card_vc, {
    tr("settings.key.vel_light"),
    tr("settings.key.vel_normal"),
    tr("settings.key.vel_heavy"),
    tr("settings.key.vel_fixed")
  }, 75);
  lv_obj_align(seg_velocity_curve_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  seg_velocity_curve_.set_selected_index(static_cast<int32_t>(cfg.velocity_curve));
  seg_velocity_curve_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    c.velocity_curve = static_cast<VelocityCurve>(idx);
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 2. Aftertouch (conditional row!)
  card_aftertouch_ = create_setting_card_helper(keyboard_pedals_container_, 80, 72);
  lv_obj_t* lbl_at = lv_label_create(card_aftertouch_);
  lv_label_set_text(lbl_at, tr("settings.key.aftertouch").c_str());
  lv_obj_add_style(lbl_at, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_at, LV_ALIGN_LEFT_MID, 8, 0);

  seg_aftertouch_.create(card_aftertouch_, {
    tr("state.off"),
    tr("state.on")
  }, 70);
  lv_obj_align(seg_aftertouch_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  seg_aftertouch_.set_selected_index(cfg.aftertouch_enabled ? 1 : 0);
  seg_aftertouch_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    c.aftertouch_enabled = (idx == 1);
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });
  lv_obj_set_hidden(card_aftertouch_, !cfg.aftertouch_available);

  // 3. Footswitch function (Next scene / Previous scene / Next Live Set / Rotary speed)
  lv_obj_t* card_foot = create_setting_card_helper(keyboard_pedals_container_, 160, 72);
  lv_obj_t* lbl_foot = lv_label_create(card_foot);
  lv_label_set_text(lbl_foot, tr("settings.key.footswitch").c_str());
  lv_obj_add_style(lbl_foot, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_foot, LV_ALIGN_LEFT_MID, 8, 0);

  seg_footswitch_.create(card_foot, {
    tr("settings.key.foot_next_scene"),
    tr("settings.key.foot_prev_scene"),
    tr("settings.key.foot_next_liveset"),
    tr("settings.key.foot_rotary")
  }, 115);
  lv_obj_align(seg_footswitch_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  seg_footswitch_.set_selected_index(static_cast<int32_t>(cfg.footswitch_function));
  seg_footswitch_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    c.footswitch_function = static_cast<FootswitchFunction>(idx);
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 4. Sustain calibration
  lv_obj_t* card_sus = create_setting_card_helper(keyboard_pedals_container_, 240, 72);
  lv_obj_t* lbl_sus = lv_label_create(card_sus);
  lv_label_set_text(lbl_sus, tr("settings.key.sustain_calib").c_str());
  lv_obj_add_style(lbl_sus, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_sus, LV_ALIGN_LEFT_MID, 8, 0);

  lbl_sustain_status_ = lv_label_create(card_sus);
  lv_label_set_text(lbl_sustain_status_, "");
  lv_obj_add_style(lbl_sustain_status_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_sustain_status_, LV_ALIGN_CENTER, 40, 0);

  btn_sustain_calib_ = lv_obj_create(card_sus);
  lv_obj_set_size(btn_sustain_calib_, 110, 42);
  lv_obj_set_style_bg_color(btn_sustain_calib_, p.accent, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn_sustain_calib_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(btn_sustain_calib_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_sustain_calib_, 0, LV_PART_MAIN);
  lv_obj_align(btn_sustain_calib_, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_obj_t* lbl_btn_sus = lv_label_create(btn_sustain_calib_);
  lv_label_set_text(lbl_btn_sus, tr("settings.key.calib_action").c_str());
  lv_obj_set_style_text_color(lbl_btn_sus, p.on_accent, LV_PART_MAIN);
  lv_obj_center(lbl_btn_sus);
  lv_obj_add_event_cb(btn_sustain_calib_, [](lv_event_t* e) {
    auto* self = static_cast<SettingsView*>(lv_event_get_user_data(e));
    self->calibrate_sustain();
  }, LV_EVENT_CLICKED, this);

  // 5. Expression calibration
  lv_obj_t* card_exp = create_setting_card_helper(keyboard_pedals_container_, 320, 72);
  lv_obj_t* lbl_exp = lv_label_create(card_exp);
  lv_label_set_text(lbl_exp, tr("settings.key.expression_calib").c_str());
  lv_obj_add_style(lbl_exp, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_exp, LV_ALIGN_LEFT_MID, 8, 0);

  lbl_expression_status_ = lv_label_create(card_exp);
  lv_label_set_text(lbl_expression_status_, "");
  lv_obj_add_style(lbl_expression_status_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_expression_status_, LV_ALIGN_CENTER, 40, 0);

  btn_expression_calib_ = lv_obj_create(card_exp);
  lv_obj_set_size(btn_expression_calib_, 110, 42);
  lv_obj_set_style_bg_color(btn_expression_calib_, p.accent, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn_expression_calib_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(btn_expression_calib_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_expression_calib_, 0, LV_PART_MAIN);
  lv_obj_align(btn_expression_calib_, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_obj_t* lbl_btn_exp = lv_label_create(btn_expression_calib_);
  lv_label_set_text(lbl_btn_exp, tr("settings.key.calib_action").c_str());
  lv_obj_set_style_text_color(lbl_btn_exp, p.on_accent, LV_PART_MAIN);
  lv_obj_center(lbl_btn_exp);
  lv_obj_add_event_cb(btn_expression_calib_, [](lv_event_t* e) {
    auto* self = static_cast<SettingsView*>(lv_event_get_user_data(e));
    self->calibrate_expression();
  }, LV_EVENT_CLICKED, this);

  lv_obj_set_hidden(keyboard_pedals_container_, true);
}

void SettingsView::build_midi_clock_detail() {
  const auto& sp = Theme::spacing();
  lv_obj_t* content = skeleton_.content_area();

  midi_clock_container_ = lv_obj_create(content);
  lv_obj_set_size(midi_clock_container_, 764, 488);
  lv_obj_set_pos(midi_clock_container_, 260, 0);
  lv_obj_set_scrollable(midi_clock_container_, true);
  lv_obj_set_style_bg_opa(midi_clock_container_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(midi_clock_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(midi_clock_container_, sp.space_3, LV_PART_MAIN);

  const auto& cfg = SystemConfig::instance();

  // 1. Clock source (Internal / DIN / USB)
  lv_obj_t* card_cs = create_setting_card_helper(midi_clock_container_, 0, 72);
  lv_obj_t* lbl_cs = lv_label_create(card_cs);
  lv_label_set_text(lbl_cs, tr("settings.midi.clock_source").c_str());
  lv_obj_add_style(lbl_cs, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_cs, LV_ALIGN_LEFT_MID, 8, 0);

  seg_clock_source_.create(card_cs, {
    tr("settings.midi.clock_internal"),
    tr("settings.midi.clock_din"),
    tr("settings.midi.clock_usb")
  }, 85);
  lv_obj_align(seg_clock_source_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  seg_clock_source_.set_selected_index(static_cast<int32_t>(cfg.clock_source));
  seg_clock_source_.set_on_change([this](int32_t idx) {
    if (applying_sysd_) return;
    auto& c = SystemConfig::instance();
    const auto mode = static_cast<ClockSourceMode>(idx);
    if (sysd_) {
      // sysd is the single source of truth (Story 7.7): ask it, and let its report update the cache and the rest of the ui. Sound is never involved.
      sysd_->request_clock_source(mode);
      return;
    }
    c.clock_source = mode;
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 2. MIDI out channel (1..16)
  lv_obj_t* card_moc = create_setting_card_helper(midi_clock_container_, 80, 72);
  lv_obj_t* lbl_moc = lv_label_create(card_moc);
  lv_label_set_text(lbl_moc, tr("settings.midi.channel").c_str());
  lv_obj_add_style(lbl_moc, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_moc, LV_ALIGN_LEFT_MID, 8, 0);

  std::vector<std::string> ch_opts;
  for (int i = 1; i <= 16; ++i) ch_opts.push_back(std::to_string(i));
  seg_midi_out_ch_.create(card_moc, ch_opts, 32);
  lv_obj_align(seg_midi_out_ch_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  int ch_idx = std::clamp<int>(static_cast<int>(cfg.midi_out_channel) - 1, 0, 15);
  seg_midi_out_ch_.set_selected_index(ch_idx);
  seg_midi_out_ch_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    c.midi_out_channel = static_cast<uint8_t>(idx + 1);
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 3. Keybed to MIDI OUT (Off / On)
  lv_obj_t* card_kmo = create_setting_card_helper(midi_clock_container_, 160, 72);
  lv_obj_t* lbl_kmo = lv_label_create(card_kmo);
  lv_label_set_text(lbl_kmo, tr("settings.midi.keybed_out").c_str());
  lv_obj_add_style(lbl_kmo, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_kmo, LV_ALIGN_LEFT_MID, 8, 0);

  seg_keybed_out_.create(card_kmo, {
    tr("state.off"),
    tr("state.on")
  }, 70);
  lv_obj_align(seg_keybed_out_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  seg_keybed_out_.set_selected_index(cfg.keybed_to_midi_out ? 1 : 0);
  seg_keybed_out_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    c.keybed_to_midi_out = (idx == 1);
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  // 4. MIDI THRU (Off / On)
  lv_obj_t* card_mt = create_setting_card_helper(midi_clock_container_, 240, 72);
  lv_obj_t* lbl_mt = lv_label_create(card_mt);
  lv_label_set_text(lbl_mt, tr("settings.midi.thru").c_str());
  lv_obj_add_style(lbl_mt, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_mt, LV_ALIGN_LEFT_MID, 8, 0);

  seg_midi_thru_.create(card_mt, {
    tr("state.off"),
    tr("state.on")
  }, 70);
  lv_obj_align(seg_midi_thru_.container(), LV_ALIGN_RIGHT_MID, -8, 0);
  seg_midi_thru_.set_selected_index(cfg.midi_thru ? 1 : 0);
  seg_midi_thru_.set_on_change([this](int32_t idx) {
    auto& c = SystemConfig::instance();
    c.midi_thru = (idx == 1);
    c.save_to_file();
    if (on_config_changed_) on_config_changed_(c);
  });

  lv_obj_set_hidden(midi_clock_container_, true);
}

void SettingsView::build_computer_detail() {
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  lv_obj_t* content = skeleton_.content_area();

  computer_container_ = lv_obj_create(content);
  lv_obj_set_size(computer_container_, 764, 488);
  lv_obj_set_pos(computer_container_, 260, 0);
  lv_obj_set_scrollable(computer_container_, false);
  lv_obj_set_style_bg_opa(computer_container_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(computer_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(computer_container_, sp.space_3, LV_PART_MAIN);

  lv_obj_t* card = create_setting_card_helper(computer_container_, 0, 96);
  lv_obj_t* lbl_title = lv_label_create(card);
  lv_label_set_text(lbl_title, tr("settings.computer.title").c_str());
  lv_obj_add_style(lbl_title, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 8, 8);

  lbl_computer_status_ = lv_label_create(card);
  lv_label_set_text(lbl_computer_status_, tr("settings.computer.status").c_str());
  lv_obj_add_style(lbl_computer_status_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl_computer_status_, p.muted, LV_PART_MAIN);
  lv_obj_align(lbl_computer_status_, LV_ALIGN_BOTTOM_LEFT, 8, -8);

  lv_obj_set_hidden(computer_container_, true);
}

void SettingsView::build_system_update_detail() {
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  lv_obj_t* content = skeleton_.content_area();

  system_update_container_ = lv_obj_create(content);
  lv_obj_set_size(system_update_container_, 764, 488);
  lv_obj_set_pos(system_update_container_, 260, 0);
  lv_obj_set_scrollable(system_update_container_, false);
  lv_obj_set_style_bg_opa(system_update_container_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(system_update_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(system_update_container_, sp.space_3, LV_PART_MAIN);

  lv_obj_t* card = create_setting_card_helper(system_update_container_, 0, 96);
  lbl_update_version_ = lv_label_create(card);
  lv_label_set_text(lbl_update_version_, tr("settings.model_info").c_str());
  lv_obj_add_style(lbl_update_version_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_update_version_, LV_ALIGN_TOP_LEFT, 8, 8);

  lbl_update_status_ = lv_label_create(card);
  lv_label_set_text(lbl_update_status_, tr("settings.update.up_to_date").c_str());
  lv_obj_add_style(lbl_update_status_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl_update_status_, p.ok, LV_PART_MAIN);
  lv_obj_align(lbl_update_status_, LV_ALIGN_BOTTOM_LEFT, 8, -8);

  lv_obj_set_hidden(system_update_container_, true);
}

void SettingsView::calibrate_sustain() {
  auto& c = SystemConfig::instance();
  c.sustain_min = 0;
  c.sustain_max = 4095;
  c.save_to_file();
  if (lbl_sustain_status_) {
    lv_label_set_text(lbl_sustain_status_, tr("settings.key.calib_sustain_prompt").c_str());
  }
  if (on_config_changed_) on_config_changed_(c);
}

void SettingsView::calibrate_expression() {
  auto& c = SystemConfig::instance();
  c.expression_min = 0;
  c.expression_max = 4095;
  c.save_to_file();
  if (lbl_expression_status_) {
    lv_label_set_text(lbl_expression_status_, tr("settings.key.calib_expr_prompt").c_str());
  }
  if (on_config_changed_) on_config_changed_(c);
}

void SettingsView::update_conditional_rows() {
  const auto& cfg = SystemConfig::instance();
  if (card_aftertouch_) {
    lv_obj_set_hidden(card_aftertouch_, !cfg.aftertouch_available);
  }
}


void SettingsView::poll_sysd() {
  if (!sysd_) return;
  SysdConfigSender::Event ev;
  while (sysd_->poll(ev)) {
    auto& c = SystemConfig::instance();
    if (ev.kind == SysdConfigSender::Event::Kind::Config) {
      const bool changed = c.clock_source != ev.clock_source;
      const bool first = !sysd_reported_;
      sysd_reported_ = true;
      c.clock_source = ev.clock_source;  // the cache of sysd's value
      if (changed) c.save_to_file();
      applying_sysd_ = true;
      if (midi_clock_container_) seg_clock_source_.set_selected_index(static_cast<int32_t>(ev.clock_source));
      applying_sysd_ = false;
      if ((changed || first) && on_config_changed_) on_config_changed_(c);  // the first report always, so the rest of the ui shows sysd's value even when equal to the cache
    } else {
      // The change did not reach sysd (absent, refused): the control goes back to the last value sysd reported, and the player is told.
      applying_sysd_ = true;
      if (midi_clock_container_) seg_clock_source_.set_selected_index(static_cast<int32_t>(c.clock_source));
      applying_sysd_ = false;
      if (on_notice_) on_notice_(tr(ev.unreachable ? "notice.clock_source_unreachable" : "notice.clock_source_refused"));
    }
  }
}

}  // namespace velora::ui
