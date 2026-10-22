#include "velora/ui/screens/scenes_view.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/ws_client.hpp"

#include <algorithm>
#include <cstdio>

namespace velora::ui {

ScenesView::ScenesView(WsClient* client)
    : client_(client) {
  scenes_.resize(8);
  scenes_[0] = {"Intro Piano", "Grand Piano + Warm Pad", false};
  scenes_[1] = {"Verse Acoustic", "Acoustic Piano Solo", false};
  scenes_[2] = {"Chorus Full", "Piano + Strings + Brass", false};
  scenes_[3] = {"Bridge Organ", "B3 Drawbars 888 + Leslie", false};
  scenes_[4] = {"Outro Solo", "Grand Piano Reverb", false};
  scenes_[5] = {"", "", true};
  scenes_[6] = {"", "", true};
  scenes_[7] = {"", "", true};
  current_scene_idx_ = 0;
  selected_slot_idx_ = 0;
}

ScenesView::~ScenesView() {
  card_objs_.clear();
  num_objs_.clear();
  tag_objs_.clear();
  name_objs_.clear();
  sub_objs_.clear();
}

bool ScenesView::create(lv_obj_t* parent) {
  if (!skeleton_.create(parent)) return false;

  keyboard_.create(skeleton_.root_container());
  keyboard_.close();

  lv_obj_t* content = skeleton_.content_area();
  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  // 1. 4x2 Grid Container (y = 12, h = 330, w = 992)
  grid_container_ = lv_obj_create(content);
  lv_obj_set_size(grid_container_, 992, 330);
  lv_obj_set_pos(grid_container_, 16, 12);
  lv_obj_set_scrollable(grid_container_, false);
  lv_obj_set_style_bg_opa(grid_container_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(grid_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(grid_container_, 0, LV_PART_MAIN);

  // 2. Reassurance line (y = 352, h = 24)
  lbl_reassurance_ = lv_label_create(content);
  lv_label_set_text(lbl_reassurance_, tr("scenes.reassurance").c_str());
  lv_obj_add_style(lbl_reassurance_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_reassurance_, LV_ALIGN_TOP_LEFT, 20, 352);

  // 3. Action Strip (y = 384, h = 56, w = 992)
  action_strip_ = lv_obj_create(content);
  lv_obj_set_size(action_strip_, 992, sp.touch_target_min);
  lv_obj_set_pos(action_strip_, 16, 384);
  lv_obj_set_scrollable(action_strip_, false);
  lv_obj_set_style_bg_opa(action_strip_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(action_strip_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(action_strip_, 0, LV_PART_MAIN);

  int32_t btn_w = (992 - 4 * sp.space_3) / 5;
  int32_t cur_x = 0;

  // New Scene Button (Accent)
  btn_new_ = lv_obj_create(action_strip_);
  lv_obj_set_size(btn_new_, btn_w, sp.touch_target_min);
  lv_obj_set_pos(btn_new_, cur_x, 0);
  lv_obj_set_scrollable(btn_new_, false);
  lv_obj_set_clickable(btn_new_, true);
  lv_obj_set_style_bg_color(btn_new_, p.accent, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn_new_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_new_, p.accent, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_new_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(btn_new_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn_new_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn_new_, on_new_scene_click, LV_EVENT_CLICKED, this);

  lv_obj_t* lbl_new = lv_label_create(btn_new_);
  lv_label_set_text(lbl_new, tr("scenes.action.new_scene").c_str());
  lv_obj_set_style_text_color(lbl_new, p.on_accent, LV_PART_MAIN);
  lv_obj_align(lbl_new, LV_ALIGN_CENTER, 0, 0);
  cur_x += btn_w + sp.space_3;

  // Rename Button
  btn_rename_ = lv_obj_create(action_strip_);
  lv_obj_set_size(btn_rename_, btn_w, sp.touch_target_min);
  lv_obj_set_pos(btn_rename_, cur_x, 0);
  lv_obj_set_scrollable(btn_rename_, false);
  lv_obj_set_clickable(btn_rename_, true);
  lv_obj_set_style_bg_color(btn_rename_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn_rename_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_rename_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_rename_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(btn_rename_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn_rename_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn_rename_, on_rename_click, LV_EVENT_CLICKED, this);

  lv_obj_t* lbl_rename = lv_label_create(btn_rename_);
  lv_label_set_text(lbl_rename, tr("scenes.action.rename").c_str());
  lv_obj_add_style(lbl_rename, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_rename, LV_ALIGN_CENTER, 0, 0);
  cur_x += btn_w + sp.space_3;

  // Duplicate Button
  btn_dup_ = lv_obj_create(action_strip_);
  lv_obj_set_size(btn_dup_, btn_w, sp.touch_target_min);
  lv_obj_set_pos(btn_dup_, cur_x, 0);
  lv_obj_set_scrollable(btn_dup_, false);
  lv_obj_set_clickable(btn_dup_, true);
  lv_obj_set_style_bg_color(btn_dup_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn_dup_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_dup_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_dup_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(btn_dup_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn_dup_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn_dup_, on_duplicate_click, LV_EVENT_CLICKED, this);

  lv_obj_t* lbl_dup = lv_label_create(btn_dup_);
  lv_label_set_text(lbl_dup, tr("scenes.action.duplicate").c_str());
  lv_obj_add_style(lbl_dup, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_dup, LV_ALIGN_CENTER, 0, 0);
  cur_x += btn_w + sp.space_3;

  // Move Button
  btn_move_ = lv_obj_create(action_strip_);
  lv_obj_set_size(btn_move_, btn_w, sp.touch_target_min);
  lv_obj_set_pos(btn_move_, cur_x, 0);
  lv_obj_set_scrollable(btn_move_, false);
  lv_obj_set_clickable(btn_move_, true);
  lv_obj_set_style_bg_color(btn_move_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn_move_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_move_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_move_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(btn_move_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn_move_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn_move_, on_move_click, LV_EVENT_CLICKED, this);

  lv_obj_t* lbl_move = lv_label_create(btn_move_);
  lv_label_set_text(lbl_move, tr("scenes.action.move").c_str());
  lv_obj_add_style(lbl_move, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_move, LV_ALIGN_CENTER, 0, 0);
  cur_x += btn_w + sp.space_3;

  // Clear Button
  btn_clear_ = lv_obj_create(action_strip_);
  lv_obj_set_size(btn_clear_, btn_w, sp.touch_target_min);
  lv_obj_set_pos(btn_clear_, cur_x, 0);
  lv_obj_set_scrollable(btn_clear_, false);
  lv_obj_set_clickable(btn_clear_, true);
  lv_obj_set_style_bg_color(btn_clear_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn_clear_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_clear_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_clear_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(btn_clear_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn_clear_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn_clear_, on_clear_click, LV_EVENT_CLICKED, this);

  lv_obj_t* lbl_clear = lv_label_create(btn_clear_);
  lv_label_set_text(lbl_clear, tr("scenes.action.clear").c_str());
  lv_obj_add_style(lbl_clear, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_clear, LV_ALIGN_CENTER, 0, 0);

  rebuild_grid();
  update_top_bar();
  update_hints();
  return true;
}

void ScenesView::rebuild_grid() {
  card_objs_.clear();
  num_objs_.clear();
  tag_objs_.clear();
  name_objs_.clear();
  sub_objs_.clear();
  if (!grid_container_) return;
  lv_obj_clean(grid_container_);

  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  int32_t col_w = 236;
  int32_t row_h = 156;
  int32_t gap_x = 16;
  int32_t gap_y = 16;

  for (size_t i = 0; i < 8; ++i) {
    int32_t col = static_cast<int32_t>(i % 4);
    int32_t row = static_cast<int32_t>(i / 4);
    int32_t x = col * (col_w + gap_x);
    int32_t y = row * (row_h + gap_y);

    bool is_empty = (i >= scenes_.size() || scenes_[i].is_empty);
    bool is_cur = (static_cast<int32_t>(i) == current_scene_idx_);
    bool is_sel = (static_cast<int32_t>(i) == selected_slot_idx_);

    lv_obj_t* card = lv_obj_create(grid_container_);
    lv_obj_set_size(card, col_w, row_h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, true);
    lv_obj_set_style_radius(card, r.md, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, sp.space_3, LV_PART_MAIN);

    if (is_cur) {
      lv_obj_set_style_bg_color(card, p.accent, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, is_sel ? p.text : p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, is_sel ? 3 : 1, LV_PART_MAIN);
    } else if (is_empty) {
      lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, is_sel ? p.accent : p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, is_sel ? 3 : 2, LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(card, is_sel ? p.raised : p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, is_sel ? p.accent : p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, is_sel ? 3 : 1, LV_PART_MAIN);
    }

    // Number label
    lv_obj_t* lbl_num = lv_label_create(card);
    char num_buf[16];
    std::snprintf(num_buf, sizeof(num_buf), "%02zu", i + 1);
    lv_label_set_text(lbl_num, num_buf);
    if (is_cur) {
      lv_obj_set_style_text_color(lbl_num, p.on_accent, LV_PART_MAIN);
    } else {
      lv_obj_add_style(lbl_num, Theme::style_text_muted(), LV_PART_MAIN);
    }
    lv_obj_align(lbl_num, LV_ALIGN_TOP_LEFT, 0, 0);

    // Current Tag
    lv_obj_t* lbl_tag = lv_label_create(card);
    if (is_cur) {
      lv_label_set_text(lbl_tag, tr("scenes.tag.current").c_str());
      lv_obj_set_style_text_color(lbl_tag, p.on_accent, LV_PART_MAIN);
      lv_obj_align(lbl_tag, LV_ALIGN_TOP_RIGHT, 0, 0);
      lv_obj_set_hidden(lbl_tag, false);
    } else {
      lv_label_set_text(lbl_tag, "");
      lv_obj_set_hidden(lbl_tag, true);
    }

    // Title label
    lv_obj_t* lbl_name = lv_label_create(card);
    if (is_empty) {
      lv_label_set_text(lbl_name, tr("scenes.empty_slot").c_str());
      lv_obj_add_style(lbl_name, Theme::style_text_muted(), LV_PART_MAIN);
    } else {
      lv_label_set_text(lbl_name, scenes_[i].name.c_str());
      if (is_cur) {
        lv_obj_set_style_text_color(lbl_name, p.on_accent, LV_PART_MAIN);
      } else {
        lv_obj_add_style(lbl_name, Theme::style_text(), LV_PART_MAIN);
      }
    }
    lv_obj_align(lbl_name, LV_ALIGN_LEFT_MID, 0, -8);

    // Sub label
    lv_obj_t* lbl_sub = lv_label_create(card);
    if (is_empty) {
      lv_label_set_text(lbl_sub, tr("scenes.slot_available").c_str());
      lv_obj_add_style(lbl_sub, Theme::style_text_muted(), LV_PART_MAIN);
    } else {
      lv_label_set_text(lbl_sub, scenes_[i].sub_label.c_str());
      if (is_cur) {
        lv_obj_set_style_text_color(lbl_sub, p.on_accent, LV_PART_MAIN);
      } else {
        lv_obj_add_style(lbl_sub, Theme::style_text_muted(), LV_PART_MAIN);
      }
    }
    lv_obj_align(lbl_sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_add_event_cb(card, on_card_click, LV_EVENT_CLICKED, this);

    card_objs_.push_back(card);
    num_objs_.push_back(lbl_num);
    tag_objs_.push_back(lbl_tag);
    name_objs_.push_back(lbl_name);
    sub_objs_.push_back(lbl_sub);
  }
}

size_t ScenesView::used_slot_count() const {
  size_t count = 0;
  for (const auto& s : scenes_) {
    if (!s.is_empty) count++;
  }
  return count;
}

void ScenesView::update_top_bar() {
  std::string title = tr("scenes.title") + " · " + live_set_name_;
  char buf[64];
  std::snprintf(buf, sizeof(buf), tr("scenes.slots_used").c_str(), static_cast<int>(used_slot_count()));
  title += " · ";
  title += buf;
  skeleton_.set_title(title);
}

void ScenesView::update_hints() {
  std::vector<HintItem> hints = {
      {"VALUE", tr("scenes.hint.select_slot")},
      {"WRITE", tr("scenes.hint.recall_scene")},
      {"EXIT", tr("hint.verb.back")}
  };
  skeleton_.set_hints(hints, tr("scenes.hint.hold_write_to_save"));
}

void ScenesView::mark_edits() {
  has_unsaved_edits_ = true;
  update_top_bar();
  rebuild_grid();
}

void ScenesView::set_scenes(const std::vector<SceneTileData>& scenes, int32_t current_idx) {
  scenes_ = scenes;
  if (scenes_.size() < 8) {
    scenes_.resize(8, {"", "", true});
  }
  current_scene_idx_ = current_idx;
  rebuild_grid();
  update_top_bar();
}

void ScenesView::set_live_set_name(const std::string& name) {
  live_set_name_ = name;
  update_top_bar();
}

void ScenesView::select_slot(size_t index) {
  if (index >= 8) return;
  selected_slot_idx_ = static_cast<int32_t>(index);
  rebuild_grid();
}

void ScenesView::recall_selected_scene() {
  if (selected_slot_idx_ < 0 || selected_slot_idx_ >= static_cast<int32_t>(scenes_.size())) return;
  if (scenes_[selected_slot_idx_].is_empty) return;

  current_scene_idx_ = selected_slot_idx_;
  if (client_) {
    ws::v1::ClientMessage msg;
    msg.set_request_id(401);
    auto* cmd = msg.mutable_recall_scene();
    cmd->set_index(static_cast<uint32_t>(current_scene_idx_));
    client_->send_command(msg);
  }
  rebuild_grid();
}

bool ScenesView::create_new_scene() {
  // Find first empty slot
  int empty_slot = -1;
  for (size_t i = 0; i < scenes_.size(); ++i) {
    if (scenes_[i].is_empty) {
      empty_slot = static_cast<int>(i);
      break;
    }
  }
  if (empty_slot < 0) return false;

  // Copy current scene or create template
  if (current_scene_idx_ >= 0 && current_scene_idx_ < static_cast<int32_t>(scenes_.size()) &&
      !scenes_[current_scene_idx_].is_empty) {
    scenes_[empty_slot] = scenes_[current_scene_idx_];
    scenes_[empty_slot].name += " (New)";
  } else {
    scenes_[empty_slot] = {"New Scene", "Default Timbre", false};
  }
  scenes_[empty_slot].is_empty = false;
  selected_slot_idx_ = empty_slot;
  mark_edits();
  return true;
}

bool ScenesView::duplicate_selected_scene() {
  if (selected_slot_idx_ < 0 || selected_slot_idx_ >= static_cast<int32_t>(scenes_.size())) return false;
  if (scenes_[selected_slot_idx_].is_empty) return false;

  // Find first empty slot
  int empty_slot = -1;
  for (size_t i = 0; i < scenes_.size(); ++i) {
    if (scenes_[i].is_empty) {
      empty_slot = static_cast<int>(i);
      break;
    }
  }
  if (empty_slot < 0) return false;

  scenes_[empty_slot] = scenes_[selected_slot_idx_];
  scenes_[empty_slot].name += " (Copy)";
  scenes_[empty_slot].is_empty = false;
  selected_slot_idx_ = empty_slot;
  mark_edits();
  return true;
}

bool ScenesView::rename_selected_scene() {
  if (selected_slot_idx_ < 0 || selected_slot_idx_ >= static_cast<int32_t>(scenes_.size())) return false;
  if (scenes_[selected_slot_idx_].is_empty) return false;

  keyboard_.open(scenes_[selected_slot_idx_].name, [this](const std::string& new_name) {
    if (new_name.empty()) return;
    scenes_[selected_slot_idx_].name = new_name;
    mark_edits();
  });
  return true;
}

bool ScenesView::move_scene_left() {
  if (selected_slot_idx_ <= 0) return false;
  int prev = selected_slot_idx_ - 1;
  std::swap(scenes_[selected_slot_idx_], scenes_[prev]);
  if (current_scene_idx_ == selected_slot_idx_) {
    current_scene_idx_ = prev;
  } else if (current_scene_idx_ == prev) {
    current_scene_idx_ = selected_slot_idx_;
  }
  selected_slot_idx_ = prev;
  mark_edits();
  return true;
}

bool ScenesView::move_scene_right() {
  if (selected_slot_idx_ < 0 || selected_slot_idx_ >= static_cast<int32_t>(scenes_.size() - 1)) return false;
  int next = selected_slot_idx_ + 1;
  std::swap(scenes_[selected_slot_idx_], scenes_[next]);
  if (current_scene_idx_ == selected_slot_idx_) {
    current_scene_idx_ = next;
  } else if (current_scene_idx_ == next) {
    current_scene_idx_ = selected_slot_idx_;
  }
  selected_slot_idx_ = next;
  mark_edits();
  return true;
}

bool ScenesView::clear_selected_scene() {
  if (selected_slot_idx_ < 0 || selected_slot_idx_ >= static_cast<int32_t>(scenes_.size())) return false;
  if (scenes_[selected_slot_idx_].is_empty) return false;

  // Guard: Cannot clear if it is the only scene
  if (used_slot_count() <= 1) {
    return false;
  }

  scenes_[selected_slot_idx_] = {"", "", true};
  // If cleared current scene, pick another used scene
  if (current_scene_idx_ == selected_slot_idx_) {
    for (size_t i = 0; i < scenes_.size(); ++i) {
      if (!scenes_[i].is_empty) {
        current_scene_idx_ = static_cast<int32_t>(i);
        break;
      }
    }
  }
  mark_edits();
  return true;
}

void ScenesView::on_panel_event(const PanelEvent& ev) {
  if (keyboard_.is_open()) {
    keyboard_.on_panel_event(ev);
    return;
  }

  if (ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
    int32_t next = selected_slot_idx_ + ev.delta;
    next = std::clamp(next, 0, 7);
    select_slot(next);
    return;
  }

  if (ev.type == PanelEventType::Button && ev.pressed) {
    switch (ev.id) {
      case MCU_BTN_VALUE_PUSH:
      case MCU_BTN_WRITE:
        recall_selected_scene();
        break;
      case MCU_BTN_EXIT:
        if (on_close_) {
          on_close_();
        }
        break;
      case MCU_BTN_SCENE_PREV:
        move_scene_left();
        break;
      case MCU_BTN_SCENE_NEXT:
        move_scene_right();
        break;
      default:
        break;
    }
  }
}

void ScenesView::update_from_state(const ws::v1::State& state) {
  if (state.current_scene() >= 0) {
    current_scene_idx_ = state.current_scene();
  }
  if (!state.live_set_name().empty()) {
    live_set_name_ = state.live_set_name();
  }
  if (state.has_edits()) {
    has_unsaved_edits_ = state.edits().current_dirty();
  }
  rebuild_grid();
  update_top_bar();
}

void ScenesView::on_card_click(lv_event_t* e) {
  auto* self = static_cast<ScenesView*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->card_objs_.size(); ++i) {
    if (self->card_objs_[i] == target) {
      self->select_slot(i);
      self->recall_selected_scene();
      return;
    }
  }
}

void ScenesView::on_new_scene_click(lv_event_t* e) {
  auto* self = static_cast<ScenesView*>(lv_event_get_user_data(e));
  if (self) self->create_new_scene();
}

void ScenesView::on_rename_click(lv_event_t* e) {
  auto* self = static_cast<ScenesView*>(lv_event_get_user_data(e));
  if (self) self->rename_selected_scene();
}

void ScenesView::on_duplicate_click(lv_event_t* e) {
  auto* self = static_cast<ScenesView*>(lv_event_get_user_data(e));
  if (self) self->duplicate_selected_scene();
}

void ScenesView::on_move_click(lv_event_t* e) {
  auto* self = static_cast<ScenesView*>(lv_event_get_user_data(e));
  if (self) self->move_scene_right();
}

void ScenesView::on_clear_click(lv_event_t* e) {
  auto* self = static_cast<ScenesView*>(lv_event_get_user_data(e));
  if (self) self->clear_selected_scene();
}

}  // namespace velora::ui
