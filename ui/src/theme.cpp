#include "velora/ui/theme.hpp"

namespace velora::ui {

ThemeId Theme::active_theme_id_ = ThemeId::Stage;

static bool s_styles_initialized = false;
static lv_style_t s_style_root;
static lv_style_t s_style_topbar;
static lv_style_t s_style_surface;
static lv_style_t s_style_raised;
static lv_style_t s_style_hintbar;
static lv_style_t s_style_text;
static lv_style_t s_style_text_muted;
static lv_style_t s_style_accent_button;

const ThemePalette& Theme::stage() {
  static const ThemePalette p = {
    .bg = lv_color_hex(0x0B0D0Cu),
    .surface = lv_color_hex(0x151917u),
    .raised = lv_color_hex(0x202622u),
    .line = lv_color_hex(0x2B322Eu),
    .line_strong = lv_color_hex(0x5A6860u),
    .text = lv_color_hex(0xE7EBE8u),
    .muted = lv_color_hex(0x88938Cu),
    .accent = lv_color_hex(0x9FBAA8u),
    .on_accent = lv_color_hex(0x0B0D0Cu),
    .warn = lv_color_hex(0xCDB86Cu),
    .ok = lv_color_hex(0x9FBAA8u),
  };
  return p;
}

const ThemePalette& Theme::studio() {
  static const ThemePalette p = {
    .bg = lv_color_hex(0xF2F4F2u),
    .surface = lv_color_hex(0xE3E8E4u),
    .raised = lv_color_hex(0xFFFFFFu),
    .line = lv_color_hex(0xCBD4CDu),
    .line_strong = lv_color_hex(0x728A77u),
    .text = lv_color_hex(0x131714u),
    .muted = lv_color_hex(0x5A635Cu),
    .accent = lv_color_hex(0x466853u),
    .on_accent = lv_color_hex(0xF2F4F2u),
    .warn = lv_color_hex(0x725F1Eu),
    .ok = lv_color_hex(0x466853u),
  };
  return p;
}

const InvariantColors& Theme::invariant() {
  static const InvariantColors inv = {
    .drawbar_brown = lv_color_hex(0x7F5E47u),
    .drawbar_white = lv_color_hex(0xE7E4DCu),
    .drawbar_black = lv_color_hex(0x1D201Eu),
    .button_cap = lv_color_hex(0xE9ECEAu),
    .button_edge = lv_color_hex(0x9AA19Cu),
    .button_edge_lit = lv_color_hex(0x4B6F58u),
    .fader_track = lv_color_hex(0x7B827Eu),
    .led_on = lv_color_hex(0x9FBAA8u),
    .led_weak = lv_color_hex(0x7F9A88u),
    .led_lens_off = lv_color_hex(0x7D8581u),
    .logo_ink = lv_color_hex(0x111111u),
    .logo_paper = lv_color_hex(0xF4F2EEu),
  };
  return inv;
}

const RadiusTokens& Theme::radius() {
  static const RadiusTokens r = {
    .sm = 5,
    .md = 8,
    .lg = 12,
    .full = 9999,
  };
  return r;
}

const SpacingTokens& Theme::spacing() {
  static const SpacingTokens s = {
    .space_1 = 6,
    .space_2 = 8,
    .space_3 = 10,
    .space_4 = 12,
    .space_5 = 16,
    .screen_gutter = 22,
    .device_topbar = 56,
    .device_hero = 118,
    .device_scene_strip = 72,
    .device_part_area = 298,
    .device_status = 56,
    .device_hint = 56,
    .touch_target_min = 56,
    .selection_border = 3,
  };
  return s;
}

void Theme::init_styles_if_needed() {
  const auto& p = current();
  const auto& r = radius();

  if (!s_styles_initialized) {
    lv_style_init(&s_style_root);
    lv_style_init(&s_style_topbar);
    lv_style_init(&s_style_surface);
    lv_style_init(&s_style_raised);
    lv_style_init(&s_style_hintbar);
    lv_style_init(&s_style_text);
    lv_style_init(&s_style_text_muted);
    lv_style_init(&s_style_accent_button);
    s_styles_initialized = true;
  }

  // Root background
  lv_style_set_bg_color(&s_style_root, p.bg);
  lv_style_set_bg_opa(&s_style_root, LV_OPA_COVER);
  lv_style_set_radius(&s_style_root, 0);

  // Top Bar style
  lv_style_set_bg_color(&s_style_topbar, p.surface);
  lv_style_set_bg_opa(&s_style_topbar, LV_OPA_COVER);
  lv_style_set_border_color(&s_style_topbar, p.line);
  lv_style_set_border_width(&s_style_topbar, 1);
  lv_style_set_border_side(&s_style_topbar, LV_BORDER_SIDE_BOTTOM);
  lv_style_set_radius(&s_style_topbar, 0);

  // Surface style (tonal elevation level 1)
  lv_style_set_bg_color(&s_style_surface, p.surface);
  lv_style_set_bg_opa(&s_style_surface, LV_OPA_COVER);
  lv_style_set_border_color(&s_style_surface, p.line);
  lv_style_set_border_width(&s_style_surface, 1);
  lv_style_set_radius(&s_style_surface, r.md);

  // Raised style (tonal elevation level 2)
  lv_style_set_bg_color(&s_style_raised, p.raised);
  lv_style_set_bg_opa(&s_style_raised, LV_OPA_COVER);
  lv_style_set_border_color(&s_style_raised, p.line_strong);
  lv_style_set_border_width(&s_style_raised, 1);
  lv_style_set_radius(&s_style_raised, r.md);

  // Hint Bar style
  lv_style_set_bg_color(&s_style_hintbar, p.surface);
  lv_style_set_bg_opa(&s_style_hintbar, LV_OPA_COVER);
  lv_style_set_border_color(&s_style_hintbar, p.line);
  lv_style_set_border_width(&s_style_hintbar, 1);
  lv_style_set_border_side(&s_style_hintbar, LV_BORDER_SIDE_TOP);
  lv_style_set_radius(&s_style_hintbar, 0);

  // Text styles
  lv_style_set_text_color(&s_style_text, p.text);
  lv_style_set_text_color(&s_style_text_muted, p.muted);

  // Accent button
  lv_style_set_bg_color(&s_style_accent_button, p.accent);
  lv_style_set_bg_opa(&s_style_accent_button, LV_OPA_COVER);
  lv_style_set_text_color(&s_style_accent_button, p.on_accent);
  lv_style_set_radius(&s_style_accent_button, r.sm);
}

lv_style_t* Theme::style_root() { init_styles_if_needed(); return &s_style_root; }
lv_style_t* Theme::style_topbar() { init_styles_if_needed(); return &s_style_topbar; }
lv_style_t* Theme::style_surface() { init_styles_if_needed(); return &s_style_surface; }
lv_style_t* Theme::style_raised() { init_styles_if_needed(); return &s_style_raised; }
lv_style_t* Theme::style_hintbar() { init_styles_if_needed(); return &s_style_hintbar; }
lv_style_t* Theme::style_text() { init_styles_if_needed(); return &s_style_text; }
lv_style_t* Theme::style_text_muted() { init_styles_if_needed(); return &s_style_text_muted; }
lv_style_t* Theme::style_accent_button() { init_styles_if_needed(); return &s_style_accent_button; }

void Theme::set_theme(ThemeId id) {
  if (active_theme_id_ == id && s_styles_initialized) return;
  active_theme_id_ = id;
  init_styles_if_needed();
  if (lv_is_initialized() && lv_screen_active()) {
    lv_obj_report_style_change(nullptr);
  }
}

void Theme::apply_to_screen(lv_obj_t* screen) {
  if (!screen) return;
  init_styles_if_needed();
  lv_obj_add_style(screen, style_root(), LV_PART_MAIN);
}

}  // namespace velora::ui
