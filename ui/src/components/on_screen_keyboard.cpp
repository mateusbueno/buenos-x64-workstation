#include "velora/ui/components/on_screen_keyboard.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

#include <algorithm>
#include <cctype>

namespace velora::ui {

static void utf8_pop_back(std::string& s) {
  if (s.empty()) return;
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
    s.pop_back();
  }
  if (!s.empty()) {
    s.pop_back();
  }
}

static std::string to_lower_utf8(const std::string& str) {
  if (str == "Ç") return "ç";
  if (str == "Ñ") return "ñ";
  if (str == "Á") return "á";
  if (str == "À") return "à";
  if (str == "Â") return "â";
  if (str == "Ã") return "ã";
  if (str == "É") return "é";
  if (str == "Ê") return "ê";
  if (str == "Í") return "í";
  if (str == "Ó") return "ó";
  if (str == "Ô") return "ô";
  if (str == "Õ") return "õ";
  if (str == "Ú") return "ú";
  if (str == "Ü") return "ü";

  std::string result = str;
  for (char& c : result) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return result;
}

OnScreenKeyboard::OnScreenKeyboard() = default;

OnScreenKeyboard::~OnScreenKeyboard() {
  root_ = nullptr;
  btn_done_ = nullptr;
  lbl_done_ = nullptr;
  btn_shift_ = nullptr;
  btn_space_ = nullptr;
  lbl_space_ = nullptr;
  btn_backspace_ = nullptr;
  row_objs_.clear();
  key_btns_.clear();
  key_lbls_.clear();
  key_glyphs_.clear();
}

bool OnScreenKeyboard::create(lv_obj_t* parent) {
  if (root_) return true;
  if (!parent) parent = lv_screen_active();
  if (!parent) return false;

  const auto& p = Theme::current();

  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, 1024, 276);
  lv_obj_set_pos(root_, 0, 324);
  lv_obj_set_scrollable(root_, false);
  lv_obj_set_style_bg_color(root_, p.raised, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(root_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(root_, 1, LV_PART_MAIN);
  lv_obj_set_style_border_side(root_, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
  lv_obj_set_style_radius(root_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);
  lv_obj_set_hidden(root_, true);

  build_keys();
  return true;
}

void OnScreenKeyboard::build_keys() {
  if (!root_) return;

  // Clear previous children
  lv_obj_clean(root_);
  row_objs_.clear();
  key_btns_.clear();
  key_lbls_.clear();
  key_glyphs_.clear();

  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const int32_t key_h = 58; // >= 56 px minimum touch target
  const int32_t gap = 6;
  const int32_t margin_x = 16;
  const int32_t row_y[4] = {10, 74, 138, 202};

  // Define rows
  std::vector<std::string> r1 = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"};
  std::vector<std::string> r2;
  std::vector<std::string> r3_middle;
  std::vector<std::string> r4_middle;

  if (layout_ == KeyboardLayout::PT_BR) {
    r2 = {"A", "S", "D", "F", "G", "H", "J", "K", "L", "Ç"};
    r3_middle = {"Z", "X", "C", "V", "B", "N", "M", "Ã", "Õ"};
    r4_middle = {",", "Í", "Ó", "Ú"};
  } else if (layout_ == KeyboardLayout::ES) {
    r2 = {"A", "S", "D", "F", "G", "H", "J", "K", "L", "Ñ"};
    r3_middle = {"Z", "X", "C", "V", "B", "N", "M", "Á", "É"};
    r4_middle = {",", "Í", "Ó", "Ú"};
  } else {
    r2 = {"A", "S", "D", "F", "G", "H", "J", "K", "L"};
    r3_middle = {"Z", "X", "C", "V", "B", "N", "M"};
    r4_middle = {","};
  }

  // Row 1
  {
    int32_t n = static_cast<int32_t>(r1.size());
    int32_t total_gaps = (n - 1) * gap;
    int32_t avail_w = 1024 - 2 * margin_x - total_gaps;
    int32_t key_w = avail_w / n;
    int32_t start_x = margin_x;

    for (int32_t i = 0; i < n; ++i) {
      lv_obj_t* btn = lv_obj_create(root_);
      lv_obj_set_size(btn, key_w, key_h);
      lv_obj_set_pos(btn, start_x + i * (key_w + gap), row_y[0]);
      lv_obj_set_scrollable(btn, false);
      lv_obj_set_clickable(btn, true);
      lv_obj_set_style_bg_color(btn, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(btn, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(btn, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

      std::string glyph = shift_active_ ? r1[i] : to_lower_utf8(r1[i]);
      lv_obj_t* lbl = lv_label_create(btn);
      lv_label_set_text(lbl, glyph.c_str());
      lv_obj_add_style(lbl, Theme::style_text(), LV_PART_MAIN);
      lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

      key_btns_.push_back(btn);
      key_lbls_.push_back(lbl);
      key_glyphs_.push_back(glyph);

      lv_obj_add_event_cb(btn, key_click_cb, LV_EVENT_CLICKED, this);
    }
  }

  // Row 2
  {
    int32_t n = static_cast<int32_t>(r2.size());
    int32_t total_gaps = (n - 1) * gap;
    int32_t avail_w = 1024 - 2 * margin_x - total_gaps;
    int32_t key_w = avail_w / n;
    int32_t start_x = margin_x;

    for (int32_t i = 0; i < n; ++i) {
      lv_obj_t* btn = lv_obj_create(root_);
      lv_obj_set_size(btn, key_w, key_h);
      lv_obj_set_pos(btn, start_x + i * (key_w + gap), row_y[1]);
      lv_obj_set_scrollable(btn, false);
      lv_obj_set_clickable(btn, true);
      lv_obj_set_style_bg_color(btn, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(btn, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(btn, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

      std::string glyph = shift_active_ ? r2[i] : to_lower_utf8(r2[i]);
      lv_obj_t* lbl = lv_label_create(btn);
      lv_label_set_text(lbl, glyph.c_str());
      lv_obj_add_style(lbl, Theme::style_text(), LV_PART_MAIN);
      lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

      key_btns_.push_back(btn);
      key_lbls_.push_back(lbl);
      key_glyphs_.push_back(glyph);

      lv_obj_add_event_cb(btn, key_click_cb, LV_EVENT_CLICKED, this);
    }
  }

  // Row 3: Shift (wide) + middle keys + Backspace (wide)
  {
    int32_t side_w = 90;
    int32_t n = static_cast<int32_t>(r3_middle.size());
    int32_t total_gaps = (n + 1) * gap;
    int32_t avail_w = 1024 - 2 * margin_x - 2 * side_w - total_gaps;
    int32_t key_w = avail_w / n;

    // Shift
    btn_shift_ = lv_obj_create(root_);
    lv_obj_set_size(btn_shift_, side_w, key_h);
    lv_obj_set_pos(btn_shift_, margin_x, row_y[2]);
    lv_obj_set_scrollable(btn_shift_, false);
    lv_obj_set_clickable(btn_shift_, true);
    lv_obj_set_style_bg_color(btn_shift_, shift_active_ ? p.accent : p.surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_shift_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_shift_, shift_active_ ? p.accent : p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_shift_, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_shift_, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_shift_, 0, LV_PART_MAIN);

    lv_obj_t* lbl_shift = lv_label_create(btn_shift_);
    lv_label_set_text(lbl_shift, tr("osk.shift").c_str());
    lv_obj_set_style_text_color(lbl_shift, shift_active_ ? p.on_accent : p.text, LV_PART_MAIN);
    lv_obj_align(lbl_shift, LV_ALIGN_CENTER, 0, 0);

    key_btns_.push_back(btn_shift_);
    key_lbls_.push_back(lbl_shift);
    key_glyphs_.push_back("⇧");
    lv_obj_add_event_cb(btn_shift_, key_click_cb, LV_EVENT_CLICKED, this);

    int32_t cur_x = margin_x + side_w + gap;
    for (int32_t i = 0; i < n; ++i) {
      lv_obj_t* btn = lv_obj_create(root_);
      lv_obj_set_size(btn, key_w, key_h);
      lv_obj_set_pos(btn, cur_x + i * (key_w + gap), row_y[2]);
      lv_obj_set_scrollable(btn, false);
      lv_obj_set_clickable(btn, true);
      lv_obj_set_style_bg_color(btn, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(btn, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(btn, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

      std::string glyph = shift_active_ ? r3_middle[i] : to_lower_utf8(r3_middle[i]);
      lv_obj_t* lbl = lv_label_create(btn);
      lv_label_set_text(lbl, glyph.c_str());
      lv_obj_add_style(lbl, Theme::style_text(), LV_PART_MAIN);
      lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

      key_btns_.push_back(btn);
      key_lbls_.push_back(lbl);
      key_glyphs_.push_back(glyph);
      lv_obj_add_event_cb(btn, key_click_cb, LV_EVENT_CLICKED, this);
    }

    // Backspace
    btn_backspace_ = lv_obj_create(root_);
    lv_obj_set_size(btn_backspace_, side_w, key_h);
    lv_obj_set_pos(btn_backspace_, 1024 - margin_x - side_w, row_y[2]);
    lv_obj_set_scrollable(btn_backspace_, false);
    lv_obj_set_clickable(btn_backspace_, true);
    lv_obj_set_style_bg_color(btn_backspace_, p.surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_backspace_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_backspace_, p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_backspace_, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_backspace_, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_backspace_, 0, LV_PART_MAIN);

    lv_obj_t* lbl_bs = lv_label_create(btn_backspace_);
    lv_label_set_text(lbl_bs, tr("osk.backspace").c_str());
    lv_obj_add_style(lbl_bs, Theme::style_text(), LV_PART_MAIN);
    lv_obj_align(lbl_bs, LV_ALIGN_CENTER, 0, 0);

    key_btns_.push_back(btn_backspace_);
    key_lbls_.push_back(lbl_bs);
    key_glyphs_.push_back("⌫");
    lv_obj_add_event_cb(btn_backspace_, key_click_cb, LV_EVENT_CLICKED, this);
  }

  // Row 4: 123 (90) + middle keys + Space (wide) + . (60) + Done (accent-filled 130)
  {
    int32_t btn123_w = 90;
    int32_t done_w = 130;
    int32_t dot_w = 60;
    int32_t mid_n = static_cast<int32_t>(r4_middle.size());
    int32_t mid_key_w = 60;

    int32_t fixed_w = btn123_w + (mid_n * mid_key_w) + dot_w + done_w;
    int32_t total_gaps = (mid_n + 3) * gap;
    int32_t space_w = 1024 - 2 * margin_x - fixed_w - total_gaps;

    int32_t cur_x = margin_x;

    // 123
    lv_obj_t* btn123 = lv_obj_create(root_);
    lv_obj_set_size(btn123, btn123_w, key_h);
    lv_obj_set_pos(btn123, cur_x, row_y[3]);
    lv_obj_set_scrollable(btn123, false);
    lv_obj_set_clickable(btn123, true);
    lv_obj_set_style_bg_color(btn123, p.surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn123, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn123, p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn123, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn123, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn123, 0, LV_PART_MAIN);

    lv_obj_t* lbl123 = lv_label_create(btn123);
    lv_label_set_text(lbl123, tr("osk.mode_123").c_str());
    lv_obj_add_style(lbl123, Theme::style_text(), LV_PART_MAIN);
    lv_obj_align(lbl123, LV_ALIGN_CENTER, 0, 0);

    key_btns_.push_back(btn123);
    key_lbls_.push_back(lbl123);
    key_glyphs_.push_back("123");
    lv_obj_add_event_cb(btn123, key_click_cb, LV_EVENT_CLICKED, this);
    cur_x += btn123_w + gap;

    // Middle keys (e.g. comma, extra accents)
    for (int32_t i = 0; i < mid_n; ++i) {
      lv_obj_t* btn = lv_obj_create(root_);
      lv_obj_set_size(btn, mid_key_w, key_h);
      lv_obj_set_pos(btn, cur_x, row_y[3]);
      lv_obj_set_scrollable(btn, false);
      lv_obj_set_clickable(btn, true);
      lv_obj_set_style_bg_color(btn, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(btn, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(btn, r.sm, LV_PART_MAIN);
      lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

      std::string glyph = shift_active_ ? r4_middle[i] : to_lower_utf8(r4_middle[i]);
      lv_obj_t* lbl = lv_label_create(btn);
      lv_label_set_text(lbl, glyph.c_str());
      lv_obj_add_style(lbl, Theme::style_text(), LV_PART_MAIN);
      lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

      key_btns_.push_back(btn);
      key_lbls_.push_back(lbl);
      key_glyphs_.push_back(glyph);
      lv_obj_add_event_cb(btn, key_click_cb, LV_EVENT_CLICKED, this);
      cur_x += mid_key_w + gap;
    }

    // Space
    btn_space_ = lv_obj_create(root_);
    lv_obj_set_size(btn_space_, space_w, key_h);
    lv_obj_set_pos(btn_space_, cur_x, row_y[3]);
    lv_obj_set_scrollable(btn_space_, false);
    lv_obj_set_clickable(btn_space_, true);
    lv_obj_set_style_bg_color(btn_space_, p.surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_space_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_space_, p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_space_, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_space_, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_space_, 0, LV_PART_MAIN);

    lbl_space_ = lv_label_create(btn_space_);
    lv_label_set_text(lbl_space_, tr("osk.space").c_str());
    lv_obj_add_style(lbl_space_, Theme::style_text_muted(), LV_PART_MAIN);
    lv_obj_align(lbl_space_, LV_ALIGN_CENTER, 0, 0);

    key_btns_.push_back(btn_space_);
    key_lbls_.push_back(lbl_space_);
    key_glyphs_.push_back("Space");
    lv_obj_add_event_cb(btn_space_, key_click_cb, LV_EVENT_CLICKED, this);
    cur_x += space_w + gap;

    // Dot
    lv_obj_t* btn_dot = lv_obj_create(root_);
    lv_obj_set_size(btn_dot, dot_w, key_h);
    lv_obj_set_pos(btn_dot, cur_x, row_y[3]);
    lv_obj_set_scrollable(btn_dot, false);
    lv_obj_set_clickable(btn_dot, true);
    lv_obj_set_style_bg_color(btn_dot, p.surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_dot, p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_dot, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_dot, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_dot, 0, LV_PART_MAIN);

    lv_obj_t* lbl_dot = lv_label_create(btn_dot);
    lv_label_set_text(lbl_dot, ".");
    lv_obj_add_style(lbl_dot, Theme::style_text(), LV_PART_MAIN);
    lv_obj_align(lbl_dot, LV_ALIGN_CENTER, 0, 0);

    key_btns_.push_back(btn_dot);
    key_lbls_.push_back(lbl_dot);
    key_glyphs_.push_back(".");
    lv_obj_add_event_cb(btn_dot, key_click_cb, LV_EVENT_CLICKED, this);
    cur_x += dot_w + gap;

    // Done (Accent-filled)
    btn_done_ = lv_obj_create(root_);
    lv_obj_set_size(btn_done_, done_w, key_h);
    lv_obj_set_pos(btn_done_, cur_x, row_y[3]);
    lv_obj_set_scrollable(btn_done_, false);
    lv_obj_set_clickable(btn_done_, true);
    lv_obj_set_style_bg_color(btn_done_, p.accent, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_done_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_done_, p.accent, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_done_, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_done_, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_done_, 0, LV_PART_MAIN);

    lbl_done_ = lv_label_create(btn_done_);
    lv_label_set_text(lbl_done_, tr("osk.done").c_str());
    lv_obj_set_style_text_color(lbl_done_, p.on_accent, LV_PART_MAIN);
    lv_obj_align(lbl_done_, LV_ALIGN_CENTER, 0, 0);

    key_btns_.push_back(btn_done_);
    key_lbls_.push_back(lbl_done_);
    key_glyphs_.push_back("Done");
    lv_obj_add_event_cb(btn_done_, key_click_cb, LV_EVENT_CLICKED, this);
  }
}

void OnScreenKeyboard::open(const std::string& initial_text, KeyboardLayout layout) {
  text_ = initial_text;
  layout_ = layout;
  is_open_ = true;
  shift_active_ = false;

  if (root_) {
    build_keys();
    lv_obj_set_hidden(root_, false);
  }
}

void OnScreenKeyboard::close() {
  is_open_ = false;
  if (root_) {
    lv_obj_set_hidden(root_, true);
  }
}

void OnScreenKeyboard::set_layout(KeyboardLayout layout) {
  if (layout_ == layout) return;
  layout_ = layout;
  if (root_) {
    build_keys();
  }
}

void OnScreenKeyboard::set_text(const std::string& text) {
  text_ = text;
  if (on_text_change_) {
    on_text_change_(text_);
  }
}

void OnScreenKeyboard::press_key(const std::string& key_glyph) {
  if (key_glyph == "⌫") {
    utf8_pop_back(text_);
    if (on_text_change_) on_text_change_(text_);
  } else if (key_glyph == "Space") {
    text_ += " ";
    if (on_text_change_) on_text_change_(text_);
  } else if (key_glyph == "⇧") {
    shift_active_ = !shift_active_;
    build_keys();
  } else if (key_glyph == "Done") {
    if (on_confirm_) on_confirm_(text_);
    close();
  } else if (key_glyph == "123") {
    // 123 layer toggle can be added, or no-op
  } else {
    // Character key
    text_ += key_glyph;
    if (on_text_change_) on_text_change_(text_);
  }
}

void OnScreenKeyboard::on_panel_event(const PanelEvent& ev) {
  if (!is_open_) return;
  if (ev.type == PanelEventType::Button && ev.pressed) {
    if (ev.id == MCU_BTN_WRITE) {
      if (on_confirm_) on_confirm_(text_);
      close();
      return;
    }
    if (ev.id == MCU_BTN_EXIT) {
      if (on_cancel_) on_cancel_(text_);
      close();
      return;
    }
  }
}

void OnScreenKeyboard::key_click_cb(lv_event_t* e) {
  auto* self = static_cast<OnScreenKeyboard*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->key_btns_.size(); ++i) {
    if (self->key_btns_[i] == target) {
      self->press_key(self->key_glyphs_[i]);
      break;
    }
  }
}

}  // namespace velora::ui
