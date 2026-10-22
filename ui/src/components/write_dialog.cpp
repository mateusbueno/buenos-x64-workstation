#include "velora/ui/components/write_dialog.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/locale.hpp"

#include <algorithm>
#include <cstdio>

namespace velora::ui {

WriteDialog::WriteDialog() = default;

WriteDialog::~WriteDialog() = default;

bool WriteDialog::create(lv_obj_t* parent) {
  if (!parent) {
    parent = lv_screen_active();
  }
  if (!parent) return false;

  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  // 1. Root Dim Backdrop (covers 1024x600)
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, 1024, 600);
  lv_obj_set_pos(root_, 0, 0);
  lv_obj_set_scrollable(root_, false);
  lv_obj_set_style_bg_color(root_, p.bg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root_, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);
  lv_obj_set_clickable(root_, true);

  // 2. Centered Floating Card (560px wide)
  card_ = lv_obj_create(root_);
  lv_obj_set_width(card_, 560);
  lv_obj_set_height(card_, LV_SIZE_CONTENT);
  lv_obj_align(card_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_scrollable(card_, false);
  lv_obj_set_style_bg_color(card_, p.raised, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(card_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(card_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(card_, r.lg, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card_, sp.space_5, LV_PART_MAIN);

  // Title: "Write changes?"
  lbl_title_ = lv_label_create(card_);
  lv_label_set_text(lbl_title_, tr("write_dialog.title").c_str());
  lv_obj_add_style(lbl_title_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_title_, LV_ALIGN_TOP_LEFT, 0, 0);

  int32_t cur_y = 48;

  // Option 1: Scene (56px high touch target)
  opt_scene_ = lv_obj_create(card_);
  lv_obj_set_size(opt_scene_, 520, sp.touch_target_min);
  lv_obj_set_pos(opt_scene_, 0, cur_y);
  lv_obj_set_scrollable(opt_scene_, false);
  lv_obj_set_clickable(opt_scene_, true);
  lv_obj_set_style_radius(opt_scene_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(opt_scene_, sp.space_3, LV_PART_MAIN);
  lv_obj_add_event_cb(opt_scene_, on_scene_click, LV_EVENT_CLICKED, this);

  opt_scene_radio_ = lv_label_create(opt_scene_);
  lv_label_set_text(opt_scene_radio_, tr("symbol.radio_unselected").c_str());
  lv_obj_align(opt_scene_radio_, LV_ALIGN_LEFT_MID, 0, 0);

  opt_scene_label_ = lv_label_create(opt_scene_);
  lv_label_set_text(opt_scene_label_, tr("write_dialog.scope.scene").c_str());
  lv_obj_add_style(opt_scene_label_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(opt_scene_label_, LV_ALIGN_LEFT_MID, 28, 0);

  opt_scene_reason_ = lv_label_create(opt_scene_);
  lv_label_set_text(opt_scene_reason_, tr("write_dialog.scene_disabled_timbre").c_str());
  lv_obj_add_style(opt_scene_reason_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(opt_scene_reason_, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_hidden(opt_scene_reason_, true);

  cur_y += sp.touch_target_min + sp.space_2;

  // Option 2: Whole Live Set (56px high touch target)
  opt_whole_ = lv_obj_create(card_);
  lv_obj_set_size(opt_whole_, 520, sp.touch_target_min);
  lv_obj_set_pos(opt_whole_, 0, cur_y);
  lv_obj_set_scrollable(opt_whole_, false);
  lv_obj_set_clickable(opt_whole_, true);
  lv_obj_set_style_radius(opt_whole_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(opt_whole_, sp.space_3, LV_PART_MAIN);
  lv_obj_add_event_cb(opt_whole_, on_whole_click, LV_EVENT_CLICKED, this);

  opt_whole_radio_ = lv_label_create(opt_whole_);
  lv_label_set_text(opt_whole_radio_, tr("symbol.radio_selected").c_str());
  lv_obj_align(opt_whole_radio_, LV_ALIGN_LEFT_MID, 0, 0);

  opt_whole_label_ = lv_label_create(opt_whole_);
  lv_label_set_text(opt_whole_label_, tr("write_dialog.scope.whole_live_set").c_str());
  lv_obj_add_style(opt_whole_label_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(opt_whole_label_, LV_ALIGN_LEFT_MID, 28, 0);

  cur_y += sp.touch_target_min + sp.space_2;

  // Option 3: Save as new… (56px high touch target)
  opt_new_ = lv_obj_create(card_);
  lv_obj_set_size(opt_new_, 520, sp.touch_target_min);
  lv_obj_set_pos(opt_new_, 0, cur_y);
  lv_obj_set_scrollable(opt_new_, false);
  lv_obj_set_clickable(opt_new_, true);
  lv_obj_set_style_radius(opt_new_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(opt_new_, sp.space_3, LV_PART_MAIN);
  lv_obj_add_event_cb(opt_new_, on_new_click, LV_EVENT_CLICKED, this);

  opt_new_radio_ = lv_label_create(opt_new_);
  lv_label_set_text(opt_new_radio_, tr("symbol.radio_unselected").c_str());
  lv_obj_align(opt_new_radio_, LV_ALIGN_LEFT_MID, 0, 0);

  opt_new_label_ = lv_label_create(opt_new_);
  lv_label_set_text(opt_new_label_, tr("write_dialog.scope.save_as_new").c_str());
  lv_obj_add_style(opt_new_label_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(opt_new_label_, LV_ALIGN_LEFT_MID, 28, 0);

  cur_y += sp.touch_target_min + sp.space_2;

  // Name Field for Save as new… (56px high touch target)
  name_field_ = lv_obj_create(card_);
  lv_obj_set_size(name_field_, 520, sp.touch_target_min);
  lv_obj_set_pos(name_field_, 0, cur_y);
  lv_obj_set_scrollable(name_field_, false);
  lv_obj_set_clickable(name_field_, true);
  lv_obj_set_style_bg_color(name_field_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(name_field_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(name_field_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(name_field_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(name_field_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(name_field_, sp.space_3, LV_PART_MAIN);
  lv_obj_add_event_cb(name_field_, on_name_click, LV_EVENT_CLICKED, this);
  lv_obj_set_hidden(name_field_, true);

  lbl_name_input_ = lv_label_create(name_field_);
  lv_label_set_text(lbl_name_input_, "");
  lv_obj_add_style(lbl_name_input_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_name_input_, LV_ALIGN_LEFT_MID, 0, 0);

  cur_y += sp.touch_target_min + sp.space_2;

  // Changed: … line
  lbl_changed_ = lv_label_create(card_);
  lv_label_set_text(lbl_changed_, "");
  lv_obj_add_style(lbl_changed_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_set_pos(lbl_changed_, 0, cur_y);

  cur_y += 28;

  // Error Banner
  lbl_error_ = lv_label_create(card_);
  lv_label_set_text(lbl_error_, "");
  lv_obj_set_style_text_color(lbl_error_, p.warn, LV_PART_MAIN);
  lv_obj_set_pos(lbl_error_, 0, cur_y);
  lv_obj_set_hidden(lbl_error_, true);

  cur_y += 28;

  // Actions Row (Cancel and Yes buttons, 56px high)
  actions_row_ = lv_obj_create(card_);
  lv_obj_set_size(actions_row_, 520, sp.touch_target_min);
  lv_obj_set_pos(actions_row_, 0, cur_y);
  lv_obj_set_scrollable(actions_row_, false);
  lv_obj_set_style_bg_opa(actions_row_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(actions_row_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(actions_row_, 0, LV_PART_MAIN);

  int32_t btn_w = 250;

  // btn_cancel_
  btn_cancel_ = lv_obj_create(actions_row_);
  lv_obj_set_size(btn_cancel_, btn_w, sp.touch_target_min);
  lv_obj_set_pos(btn_cancel_, 0, 0);
  lv_obj_set_scrollable(btn_cancel_, false);
  lv_obj_set_clickable(btn_cancel_, true);
  lv_obj_set_style_bg_color(btn_cancel_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn_cancel_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_cancel_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_cancel_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(btn_cancel_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn_cancel_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn_cancel_, on_btn_cancel_click, LV_EVENT_CLICKED, this);

  lv_obj_t* lbl_cancel = lv_label_create(btn_cancel_);
  lv_label_set_text(lbl_cancel, tr("write_dialog.action.cancel").c_str());
  lv_obj_add_style(lbl_cancel, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_cancel, LV_ALIGN_CENTER, 0, 0);

  // btn_yes_
  btn_yes_ = lv_obj_create(actions_row_);
  lv_obj_set_size(btn_yes_, btn_w, sp.touch_target_min);
  lv_obj_set_pos(btn_yes_, 520 - btn_w, 0);
  lv_obj_set_scrollable(btn_yes_, false);
  lv_obj_set_clickable(btn_yes_, true);
  lv_obj_set_style_bg_color(btn_yes_, p.accent, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn_yes_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_yes_, p.accent, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_yes_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(btn_yes_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn_yes_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn_yes_, on_btn_yes_click, LV_EVENT_CLICKED, this);

  lv_obj_t* lbl_yes = lv_label_create(btn_yes_);
  lv_label_set_text(lbl_yes, tr("write_dialog.action.yes").c_str());
  lv_obj_set_style_text_color(lbl_yes, p.on_accent, LV_PART_MAIN);
  lv_obj_align(lbl_yes, LV_ALIGN_CENTER, 0, 0);

  // Keyboard
  keyboard_.create(root_);
  keyboard_.close();

  close();
  return true;
}

void WriteDialog::open(const std::string& current_name,
                       bool has_timbre_edit,
                       const std::string& change_summary,
                       const std::string& suggested_free_name) {
  current_live_set_name_ = current_name;
  scene_scope_available_ = !has_timbre_edit;
  change_summary_text_ = change_summary;

  if (suggested_free_name.empty()) {
    suggested_free_name_ = current_name + " 2";
  } else {
    suggested_free_name_ = suggested_free_name;
  }
  new_name_input_ = suggested_free_name_;

  // Default scope: WholeLiveSet
  current_scope_ = ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET;
  focus_idx_ = 1;

  clear_error();

  if (lbl_changed_) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), tr("write_dialog.changed_prefix").c_str(),
                  change_summary_text_.empty() ? "—" : change_summary_text_.c_str());
    lv_label_set_text(lbl_changed_, buf);
  }

  update_scope_ui();
  update_name_field_ui();
  update_focus_ui();

  is_open_ = true;
  if (root_) {
    lv_obj_set_hidden(root_, false);
  }
}

void WriteDialog::close() {
  is_open_ = false;
  keyboard_.close();
  if (root_) {
    lv_obj_set_hidden(root_, true);
  }
}

void WriteDialog::set_scope(ws::v1::WriteScope scope) {
  if (scope == ws::v1::WRITE_SCOPE_SCENE && !scene_scope_available_) {
    return;
  }
  current_scope_ = scope;
  update_scope_ui();
  update_name_field_ui();
  update_focus_ui();
}

void WriteDialog::update_scope_ui() {
  const auto& p = Theme::current();

  // Scene
  if (opt_scene_) {
    if (!scene_scope_available_) {
      lv_obj_set_style_bg_opa(opt_scene_, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_color(opt_scene_, p.line, LV_PART_MAIN);
      lv_label_set_text(opt_scene_radio_, tr("symbol.radio_unselected").c_str());
      lv_obj_set_style_text_color(opt_scene_radio_, p.muted, LV_PART_MAIN);
      lv_obj_set_style_text_color(opt_scene_label_, p.muted, LV_PART_MAIN);
      lv_obj_set_hidden(opt_scene_reason_, false);
    } else {
      bool is_sel = (current_scope_ == ws::v1::WRITE_SCOPE_SCENE);
      lv_obj_set_style_bg_color(opt_scene_, is_sel ? p.surface : p.raised, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(opt_scene_, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(opt_scene_, is_sel ? p.accent : p.line_strong, LV_PART_MAIN);
      lv_label_set_text(opt_scene_radio_, (is_sel ? tr("symbol.radio_selected") : tr("symbol.radio_unselected")).c_str());
      lv_obj_set_style_text_color(opt_scene_radio_, is_sel ? p.accent : p.text, LV_PART_MAIN);
      lv_obj_set_style_text_color(opt_scene_label_, p.text, LV_PART_MAIN);
      lv_obj_set_hidden(opt_scene_reason_, true);
    }
  }

  // Whole Live Set
  if (opt_whole_) {
    bool is_sel = (current_scope_ == ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
    lv_obj_set_style_bg_color(opt_whole_, is_sel ? p.surface : p.raised, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(opt_whole_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(opt_whole_, is_sel ? p.accent : p.line_strong, LV_PART_MAIN);
    lv_label_set_text(opt_whole_radio_, (is_sel ? tr("symbol.radio_selected") : tr("symbol.radio_unselected")).c_str());
    lv_obj_set_style_text_color(opt_whole_radio_, is_sel ? p.accent : p.text, LV_PART_MAIN);
    lv_obj_set_style_text_color(opt_whole_label_, p.text, LV_PART_MAIN);
  }

  // Save as new
  if (opt_new_) {
    bool is_sel = (current_scope_ == ws::v1::WRITE_SCOPE_SAVE_AS_NEW);
    lv_obj_set_style_bg_color(opt_new_, is_sel ? p.surface : p.raised, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(opt_new_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(opt_new_, is_sel ? p.accent : p.line_strong, LV_PART_MAIN);
    lv_label_set_text(opt_new_radio_, (is_sel ? tr("symbol.radio_selected") : tr("symbol.radio_unselected")).c_str());
    lv_obj_set_style_text_color(opt_new_radio_, is_sel ? p.accent : p.text, LV_PART_MAIN);
    lv_obj_set_style_text_color(opt_new_label_, p.text, LV_PART_MAIN);
  }
}

void WriteDialog::update_name_field_ui() {
  if (!name_field_ || !lbl_name_input_) return;

  bool is_save_new = (current_scope_ == ws::v1::WRITE_SCOPE_SAVE_AS_NEW);
  lv_obj_set_hidden(name_field_, !is_save_new);
  lv_label_set_text(lbl_name_input_, new_name_input_.c_str());
}

void WriteDialog::update_focus_ui() {
  const auto& p = Theme::current();

  // Highlight border on focused widget
  // 0: Scene, 1: Whole, 2: New, 3: Name Field, 4: Cancel, 5: Yes
  auto set_focus_border = [&](lv_obj_t* obj, bool focused) {
    if (!obj) return;
    if (focused) {
      lv_obj_set_style_border_color(obj, p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN);
    } else {
      lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    }
  };

  set_focus_border(opt_scene_, focus_idx_ == 0 && scene_scope_available_);
  set_focus_border(opt_whole_, focus_idx_ == 1);
  set_focus_border(opt_new_, focus_idx_ == 2);
  if (current_scope_ == ws::v1::WRITE_SCOPE_SAVE_AS_NEW) {
    set_focus_border(name_field_, focus_idx_ == 3);
  }
  set_focus_border(btn_cancel_, focus_idx_ == 4);
  set_focus_border(btn_yes_, focus_idx_ == 5);
}

void WriteDialog::show_name_in_use_error(const std::string& name, const std::string& suggested_free_name) {
  if (!lbl_error_) return;
  char buf[256];
  std::snprintf(buf, sizeof(buf), tr("write_dialog.error.name_in_use").c_str(), name.c_str());
  lv_label_set_text(lbl_error_, buf);
  lv_obj_set_hidden(lbl_error_, false);

  if (!suggested_free_name.empty()) {
    new_name_input_ = suggested_free_name;
    update_name_field_ui();
  }
}

void WriteDialog::show_storage_full_error() {
  if (!lbl_error_) return;
  lv_label_set_text(lbl_error_, tr("write_dialog.error.storage_full").c_str());
  lv_obj_set_hidden(lbl_error_, false);
}

void WriteDialog::clear_error() {
  if (lbl_error_) {
    lv_label_set_text(lbl_error_, "");
    lv_obj_set_hidden(lbl_error_, true);
  }
}

void WriteDialog::confirm_write() {
  if (on_write_) {
    std::string name_to_save = (current_scope_ == ws::v1::WRITE_SCOPE_SAVE_AS_NEW) ?
                               new_name_input_ : current_live_set_name_;
    on_write_(current_scope_, name_to_save);
  }
  close();
}

void WriteDialog::cancel() {
  if (on_cancel_) {
    on_cancel_();
  }
  close();
}

void WriteDialog::on_panel_event(const PanelEvent& ev) {
  if (keyboard_.is_open()) {
    keyboard_.on_panel_event(ev);
    return;
  }

  if (ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
    int32_t max_focus = (current_scope_ == ws::v1::WRITE_SCOPE_SAVE_AS_NEW) ? 5 : 5;
    focus_idx_ = std::clamp(focus_idx_ + ev.delta, 0, max_focus);

    // Skip Scene if unavailable
    if (focus_idx_ == 0 && !scene_scope_available_) {
      focus_idx_ = (ev.delta > 0) ? 1 : 0;
    }
    // Skip name field if not in Save As New
    if (focus_idx_ == 3 && current_scope_ != ws::v1::WRITE_SCOPE_SAVE_AS_NEW) {
      focus_idx_ = (ev.delta > 0) ? 4 : 2;
    }

    update_focus_ui();
    return;
  }

  if (ev.type == PanelEventType::Button && ev.pressed) {
    if (ev.id == MCU_BTN_EXIT) {
      cancel();
      return;
    }

    if (ev.id == MCU_BTN_WRITE) {
      confirm_write();
      return;
    }

    if (ev.id == MCU_BTN_VALUE_PUSH) {
      switch (focus_idx_) {
        case 0:
          if (scene_scope_available_) set_scope(ws::v1::WRITE_SCOPE_SCENE);
          break;
        case 1:
          set_scope(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
          break;
        case 2:
          set_scope(ws::v1::WRITE_SCOPE_SAVE_AS_NEW);
          break;
        case 3:
          if (current_scope_ == ws::v1::WRITE_SCOPE_SAVE_AS_NEW) {
            keyboard_.open(new_name_input_, [this](const std::string& entered) {
              if (!entered.empty()) {
                new_name_input_ = entered;
                update_name_field_ui();
              }
            });
          }
          break;
        case 4:
          cancel();
          break;
        case 5:
          confirm_write();
          break;
        default:
          break;
      }
    }
  }
}

void WriteDialog::on_scene_click(lv_event_t* e) {
  auto* self = static_cast<WriteDialog*>(lv_event_get_user_data(e));
  if (self) self->set_scope(ws::v1::WRITE_SCOPE_SCENE);
}

void WriteDialog::on_whole_click(lv_event_t* e) {
  auto* self = static_cast<WriteDialog*>(lv_event_get_user_data(e));
  if (self) self->set_scope(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
}

void WriteDialog::on_new_click(lv_event_t* e) {
  auto* self = static_cast<WriteDialog*>(lv_event_get_user_data(e));
  if (self) self->set_scope(ws::v1::WRITE_SCOPE_SAVE_AS_NEW);
}

void WriteDialog::on_name_click(lv_event_t* e) {
  auto* self = static_cast<WriteDialog*>(lv_event_get_user_data(e));
  if (self) {
    self->keyboard_.open(self->new_name_input_, [self](const std::string& entered) {
      if (!entered.empty()) {
        self->new_name_input_ = entered;
        self->update_name_field_ui();
      }
    });
  }
}

void WriteDialog::on_btn_yes_click(lv_event_t* e) {
  auto* self = static_cast<WriteDialog*>(lv_event_get_user_data(e));
  if (self) self->confirm_write();
}

void WriteDialog::on_btn_cancel_click(lv_event_t* e) {
  auto* self = static_cast<WriteDialog*>(lv_event_get_user_data(e));
  if (self) self->cancel();
}

}  // namespace velora::ui
