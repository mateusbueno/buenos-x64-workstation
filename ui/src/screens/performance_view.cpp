#include "velora/ui/screens/performance_view.hpp"
#include "velora/ui/ws_client.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

#include <cstdio>
#include <algorithm>

namespace velora::ui {

PerformanceView::PerformanceView(WsClient* client) : client_(client) {
  parts_.resize(8);
  for (size_t i = 0; i < 8; ++i) {
    parts_[i].timbre_name = "";
    parts_[i].category = "";
    parts_[i].volume = 0.75f;
    parts_[i].enabled = true;
    parts_[i].is_empty = (i >= 4);
    parts_[i].missing_library = false;
  }
}

PerformanceView::~PerformanceView() {
  root_ = nullptr;
  top_bar_ = nullptr;
  hero_ = nullptr;
  scene_strip_ = nullptr;
  part_area_ = nullptr;
  status_bar_ = nullptr;
  scene_tile_objs_.clear();
  scene_title_objs_.clear();
  scene_sub_objs_.clear();
  part_card_objs_.clear();
  part_num_objs_.clear();
  part_led_objs_.clear();
  part_power_objs_.clear();
  part_power_lbl_objs_.clear();
  part_name_objs_.clear();
  part_cat_objs_.clear();
  part_vol_track_objs_.clear();
  part_vol_fill_objs_.clear();
  part_ghost_objs_.clear();
  part_chevron_objs_.clear();
  status_normal_container_ = nullptr;
  lbl_status_voices_ = nullptr;
  lbl_status_streaming_ = nullptr;
  lbl_status_rotary_ = nullptr;
  btn_status_rev_ = nullptr;
  lbl_status_rev_ = nullptr;
  lbl_status_takeover_ = nullptr;
}

bool PerformanceView::create(lv_obj_t* parent) {
  if (root_) return true;
  if (!parent) {
    parent = lv_screen_active();
  }
  if (!parent) return false;

  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  const auto& r = Theme::radius();

  // 1. Root container: 1024x600 fixed
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, 1024, 600);
  lv_obj_set_pos(root_, 0, 0);
  lv_obj_set_scrollable(root_, false);
  lv_obj_add_style(root_, Theme::style_root(), LV_PART_MAIN);

  // 2. Top Bar: 1024x56
  top_bar_ = lv_obj_create(root_);
  lv_obj_set_size(top_bar_, 1024, sp.device_topbar);
  lv_obj_set_pos(top_bar_, 0, 0);
  lv_obj_set_scrollable(top_bar_, false);
  lv_obj_add_style(top_bar_, Theme::style_topbar(), LV_PART_MAIN);

  lbl_context_ = lv_label_create(top_bar_);
  lv_label_set_text(lbl_context_, tr("performance.top_bar.no_setlist").c_str());
  lv_obj_add_style(lbl_context_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_context_, LV_ALIGN_LEFT_MID, sp.screen_gutter, 0);

  lbl_mode_mixer_ = lv_label_create(top_bar_);
  lv_label_set_text(lbl_mode_mixer_, tr("performance.mode.mixer").c_str());
  lv_obj_add_style(lbl_mode_mixer_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_mode_mixer_, LV_ALIGN_CENTER, -120, 0);

  lbl_mode_parts_ = lv_label_create(top_bar_);
  lv_label_set_text(lbl_mode_parts_, tr("performance.mode.parts").c_str());
  lv_obj_add_style(lbl_mode_parts_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_mode_parts_, LV_ALIGN_CENTER, -30, 0);

  lbl_mode_knobs_ = lv_label_create(top_bar_);
  lv_label_set_text(lbl_mode_knobs_, tr("performance.mode.part").c_str());
  lv_obj_add_style(lbl_mode_knobs_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_mode_knobs_, LV_ALIGN_CENTER, 70, 0);

  lbl_tempo_ = lv_label_create(top_bar_);
  char tempo_buf[32];
  std::snprintf(tempo_buf, sizeof(tempo_buf), "♩ %.1f", current_bpm_);
  lv_label_set_text(lbl_tempo_, tempo_buf);
  lv_obj_add_style(lbl_tempo_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_tempo_, LV_ALIGN_RIGHT_MID, -sp.screen_gutter, 0);

  // 3. Live Set Hero: 1024x118, pos (0, 56)
  hero_ = lv_obj_create(root_);
  lv_obj_set_size(hero_, 1024, sp.device_hero);
  lv_obj_set_pos(hero_, 0, sp.device_topbar);
  lv_obj_set_scrollable(hero_, false);
  lv_obj_set_style_bg_color(hero_, p.bg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(hero_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(hero_, p.line, LV_PART_MAIN);
  lv_obj_set_style_border_width(hero_, 1, LV_PART_MAIN);
  lv_obj_set_style_border_side(hero_, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_radius(hero_, 0, LV_PART_MAIN);

  // Hero left section
  lbl_hero_meta_ = lv_label_create(hero_);
  lv_label_set_text(lbl_hero_meta_, "");
  lv_obj_add_style(lbl_hero_meta_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_set_pos(lbl_hero_meta_, sp.screen_gutter, 16);

  lbl_hero_name_ = lv_label_create(hero_);
  lv_label_set_text(lbl_hero_name_, "");
  lv_obj_add_style(lbl_hero_name_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_set_pos(lbl_hero_name_, sp.screen_gutter, 45);

  lbl_hero_unsaved_ = lv_label_create(hero_);
  lv_label_set_text(lbl_hero_unsaved_, tr("list_row.indicator.unsaved").c_str());
  lv_obj_set_style_text_color(lbl_hero_unsaved_, p.warn, LV_PART_MAIN);
  lv_obj_set_hidden(lbl_hero_unsaved_, true);

  // Hero right section (NEXT)
  lbl_next_header_ = lv_label_create(hero_);
  lv_label_set_text(lbl_next_header_, tr("performance.hero.next").c_str());
  lv_obj_add_style(lbl_next_header_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_next_header_, LV_ALIGN_RIGHT_MID, -sp.screen_gutter, -28);

  lbl_next_title_ = lv_label_create(hero_);
  lv_label_set_text(lbl_next_title_, "");
  lv_obj_add_style(lbl_next_title_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_next_title_, LV_ALIGN_RIGHT_MID, -sp.screen_gutter, 0);

  lbl_next_status_ = lv_label_create(hero_);
  lv_label_set_text(lbl_next_status_, tr("performance.hero.ready").c_str());
  lv_obj_set_style_text_color(lbl_next_status_, p.ok, LV_PART_MAIN);
  lv_obj_align(lbl_next_status_, LV_ALIGN_RIGHT_MID, -sp.screen_gutter, 28);

  // 4. Scene Strip: 1024x72, pos (0, 174)
  scene_strip_ = lv_obj_create(root_);
  lv_obj_set_size(scene_strip_, 1024, sp.device_scene_strip);
  lv_obj_set_pos(scene_strip_, 0, sp.device_topbar + sp.device_hero);
  lv_obj_set_scrollable(scene_strip_, false);
  lv_obj_set_style_bg_opa(scene_strip_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(scene_strip_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scene_strip_, 0, LV_PART_MAIN);

  // 8 Scene Tiles
  scene_tile_objs_.resize(8);
  scene_title_objs_.resize(8);
  scene_sub_objs_.resize(8);

  int32_t tile_w = 115;
  int32_t tile_h = sp.device_scene_strip;
  int32_t start_x = sp.screen_gutter;
  int32_t gap = sp.space_2;

  for (int i = 0; i < 8; ++i) {
    lv_obj_t* tile = lv_obj_create(scene_strip_);
    lv_obj_set_size(tile, tile_w, tile_h);
    lv_obj_set_pos(tile, start_x + i * (tile_w + gap), 0);
    lv_obj_set_scrollable(tile, false);
    lv_obj_set_clickable(tile, true);
    lv_obj_set_style_pad_all(tile, sp.space_1, LV_PART_MAIN);
    lv_obj_set_style_radius(tile, r.md, LV_PART_MAIN);
    lv_obj_add_event_cb(tile, scene_click_cb, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_t = lv_label_create(tile);
    lv_obj_align(lbl_t, LV_ALIGN_TOP_LEFT, 0, 2);

    lv_obj_t* lbl_s = lv_label_create(tile);
    lv_obj_align(lbl_s, LV_ALIGN_BOTTOM_LEFT, 0, -2);

    scene_tile_objs_[i] = tile;
    scene_title_objs_[i] = lbl_t;
    scene_sub_objs_[i] = lbl_s;
  }

  // 5. Part Area: 1024x298, pos (0, 246)
  part_area_ = lv_obj_create(root_);
  lv_obj_set_size(part_area_, 1024, sp.device_part_area);
  lv_obj_set_pos(part_area_, 0, sp.device_topbar + sp.device_hero + sp.device_scene_strip);
  lv_obj_set_scrollable(part_area_, false);
  lv_obj_add_style(part_area_, Theme::style_root(), LV_PART_MAIN);

  // Mixer Container (8 Part Strips)
  mixer_container_ = lv_obj_create(part_area_);
  lv_obj_set_size(mixer_container_, 1024, sp.device_part_area);
  lv_obj_set_pos(mixer_container_, 0, 0);
  lv_obj_set_scrollable(mixer_container_, false);
  lv_obj_set_style_bg_opa(mixer_container_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(mixer_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(mixer_container_, 0, LV_PART_MAIN);

  // 8 Part Strips
  part_card_objs_.resize(8);
  part_num_objs_.resize(8);
  part_led_objs_.resize(8);
  part_power_objs_.resize(8);
  part_power_lbl_objs_.resize(8);
  part_name_objs_.resize(8);
  part_cat_objs_.resize(8);
  part_vol_track_objs_.resize(8);
  part_vol_fill_objs_.resize(8);
  part_ghost_objs_.resize(8);
  part_chevron_objs_.resize(8);

  int32_t strip_w = 115;
  int32_t strip_h = 282;

  for (int i = 0; i < 8; ++i) {
    lv_obj_t* card = lv_obj_create(mixer_container_);
    lv_obj_set_size(card, strip_w, strip_h);
    lv_obj_set_pos(card, start_x + i * (strip_w + gap), sp.space_2);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, true);
    lv_obj_set_style_bg_color(card, p.surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, r.md, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, p.line, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, sp.space_2, LV_PART_MAIN);
    lv_obj_add_event_cb(card, part_card_click_cb, LV_EVENT_CLICKED, this);

    // Number + LED dot row
    lv_obj_t* lbl_n = lv_label_create(card);
    char n_buf[8];
    std::snprintf(n_buf, sizeof(n_buf), "%d", i + 1);
    lv_label_set_text(lbl_n, n_buf);
    lv_obj_add_style(lbl_n, Theme::style_text_muted(), LV_PART_MAIN);
    lv_obj_set_pos(lbl_n, 4, 4);

    lv_obj_t* led = lv_obj_create(card);
    lv_obj_set_size(led, 8, 8);
    lv_obj_set_pos(led, 22, 8);
    lv_obj_set_scrollable(led, false);
    lv_obj_set_style_radius(led, r.full, LV_PART_MAIN);

    // Power toggle button (>=56 px touch target)
    lv_obj_t* btn_p = lv_obj_create(card);
    lv_obj_set_size(btn_p, 56, 56);
    lv_obj_set_pos(btn_p, strip_w - 56 - sp.space_2, 0);
    lv_obj_set_scrollable(btn_p, false);
    lv_obj_set_clickable(btn_p, true);
    lv_obj_set_style_bg_opa(btn_p, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_p, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_p, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_p, part_power_click_cb, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_p = lv_label_create(btn_p);
    lv_label_set_text(lbl_p, tr("performance.part.power").c_str());
    lv_obj_align(lbl_p, LV_ALIGN_CENTER, 0, 0);

    // Timbre name (2 lines, end ellipsis)
    lv_obj_t* lbl_nm = lv_label_create(card);
    lv_obj_set_size(lbl_nm, strip_w - sp.space_4, 38);
    lv_obj_set_pos(lbl_nm, 4, 58);
    lv_label_set_long_mode(lbl_nm, LV_LABEL_LONG_DOT);
    lv_label_set_text(lbl_nm, "");
    lv_obj_add_style(lbl_nm, Theme::style_text(), LV_PART_MAIN);

    // Category / status label
    lv_obj_t* lbl_c = lv_label_create(card);
    lv_obj_set_size(lbl_c, strip_w - sp.space_4, 20);
    lv_obj_set_pos(lbl_c, 4, 98);
    lv_label_set_text(lbl_c, "");
    lv_obj_add_style(lbl_c, Theme::style_text_muted(), LV_PART_MAIN);

    // Vertical Volume Bar (width 16, height 120)
    int32_t vol_h = 120;
    int32_t vol_w = 16;
    lv_obj_t* vol_track = lv_obj_create(card);
    lv_obj_set_size(vol_track, vol_w, vol_h);
    lv_obj_set_pos(vol_track, (strip_w - sp.space_4 - vol_w) / 2, 130);
    lv_obj_set_scrollable(vol_track, false);
    lv_obj_set_style_bg_color(vol_track, p.raised, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vol_track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(vol_track, p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_border_width(vol_track, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(vol_track, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_all(vol_track, 0, LV_PART_MAIN);

    lv_obj_t* vol_fill = lv_obj_create(vol_track);
    lv_obj_set_size(vol_fill, vol_w, 0);
    lv_obj_align(vol_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_scrollable(vol_fill, false);
    lv_obj_set_style_bg_color(vol_fill, p.accent, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vol_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(vol_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(vol_fill, r.sm, LV_PART_MAIN);

    // Ghost line (3px warn line with 2px bg keylines -> 7px tall, 24px wide)
    lv_obj_t* ghost = lv_obj_create(card);
    lv_obj_set_size(ghost, 24, 7);
    lv_obj_set_pos(ghost, (strip_w - sp.space_4 - 24) / 2, 130);
    lv_obj_set_scrollable(ghost, false);
    lv_obj_set_style_bg_color(ghost, p.warn, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ghost, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(ghost, p.bg, LV_PART_MAIN);
    lv_obj_set_style_border_width(ghost, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ghost, 0, LV_PART_MAIN);
    lv_obj_set_hidden(ghost, true);

    // Direction chevron
    lv_obj_t* chev = lv_label_create(card);
    lv_label_set_text(chev, "");
    lv_obj_set_style_text_color(chev, p.warn, LV_PART_MAIN);
    lv_obj_set_pos(chev, (strip_w - sp.space_4 + vol_w) / 2 + 4, 130);
    lv_obj_set_hidden(chev, true);

    part_card_objs_[i] = card;
    part_num_objs_[i] = lbl_n;
    part_led_objs_[i] = led;
    part_power_objs_[i] = btn_p;
    part_power_lbl_objs_[i] = lbl_p;
    part_name_objs_[i] = lbl_nm;
    part_cat_objs_[i] = lbl_c;
    part_vol_track_objs_[i] = vol_track;
    part_vol_fill_objs_[i] = vol_fill;
    part_ghost_objs_[i] = ghost;
    part_chevron_objs_[i] = chev;
  }

  // Drawbars Container (Organ Card + 9 Drawbar Columns)
  drawbars_container_ = lv_obj_create(part_area_);
  lv_obj_set_size(drawbars_container_, 1024, sp.device_part_area);
  lv_obj_set_pos(drawbars_container_, 0, 0);
  lv_obj_set_scrollable(drawbars_container_, false);
  lv_obj_set_style_bg_opa(drawbars_container_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(drawbars_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(drawbars_container_, 0, LV_PART_MAIN);
  lv_obj_set_hidden(drawbars_container_, true);

  // Organ Card: 310x282 at pos (22, 8)
  organ_card_obj_ = lv_obj_create(drawbars_container_);
  lv_obj_set_size(organ_card_obj_, 310, strip_h);
  lv_obj_set_pos(organ_card_obj_, sp.screen_gutter, sp.space_2);
  lv_obj_set_scrollable(organ_card_obj_, false);
  lv_obj_set_style_bg_color(organ_card_obj_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(organ_card_obj_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(organ_card_obj_, r.md, LV_PART_MAIN);
  lv_obj_set_style_border_color(organ_card_obj_, p.accent, LV_PART_MAIN);
  lv_obj_set_style_border_width(organ_card_obj_, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_all(organ_card_obj_, sp.space_3, LV_PART_MAIN);

  lbl_organ_meta_ = lv_label_create(organ_card_obj_);
  lv_label_set_text(lbl_organ_meta_, "");
  lv_obj_add_style(lbl_organ_meta_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_set_pos(lbl_organ_meta_, 4, 4);

  lbl_organ_title_ = lv_label_create(organ_card_obj_);
  lv_label_set_text(lbl_organ_title_, "");
  lv_obj_add_style(lbl_organ_title_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_set_pos(lbl_organ_title_, 4, 34);

  lbl_organ_cat_ = lv_label_create(organ_card_obj_);
  lv_label_set_text(lbl_organ_cat_, "");
  lv_obj_set_style_text_color(lbl_organ_cat_, p.accent, LV_PART_MAIN);
  lv_obj_set_pos(lbl_organ_cat_, 4, 62);

  lbl_organ_details_ = lv_label_create(organ_card_obj_);
  lv_label_set_text(lbl_organ_details_, "");
  lv_obj_add_style(lbl_organ_details_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_set_pos(lbl_organ_details_, 4, 98);

  lbl_organ_reg_ = lv_label_create(organ_card_obj_);
  lv_label_set_text(lbl_organ_reg_, "");
  lv_obj_add_style(lbl_organ_reg_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_set_pos(lbl_organ_reg_, 4, 218);

  // 9 Drawbar Columns
  drawbar_col_objs_.resize(9);
  drawbar_track_objs_.resize(9);
  drawbar_head_objs_.resize(9);
  drawbar_val_objs_.resize(9);

  const char* footages[9] = {"16'", "5⅓'", "8'", "4'", "2⅔'", "2'", "1⅗'", "1⅓'", "1'"};
  int32_t dbs_start_x = 348;
  int32_t col_w = 64;
  int32_t col_gap = 9;

  for (size_t i = 0; i < 9; ++i) {
    lv_obj_t* col = lv_obj_create(drawbars_container_);
    lv_obj_set_size(col, col_w, strip_h);
    lv_obj_set_pos(col, dbs_start_x + static_cast<int32_t>(i) * (col_w + col_gap), sp.space_2);
    lv_obj_set_scrollable(col, false);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(col, 0, LV_PART_MAIN);

    // Footage label
    lv_obj_t* lbl_ft = lv_label_create(col);
    lv_label_set_text(lbl_ft, footages[i]);
    lv_obj_add_style(lbl_ft, Theme::style_text_muted(), LV_PART_MAIN);
    lv_obj_align(lbl_ft, LV_ALIGN_TOP_MID, 0, 4);

    // Track
    int32_t track_w = 28;
    int32_t track_h = 200;
    lv_obj_t* track = lv_obj_create(col);
    lv_obj_set_size(track, track_w, track_h);
    lv_obj_align(track, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_scrollable(track, false);
    lv_obj_set_style_bg_color(track, p.raised, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(track, p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_border_width(track, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(track, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_all(track, 0, LV_PART_MAIN);

    // Rod
    lv_obj_t* rod = lv_obj_create(track);
    lv_obj_set_size(rod, 4, track_h);
    lv_obj_align(rod, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_scrollable(rod, false);
    lv_obj_set_style_bg_color(rod, p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rod, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(rod, 0, LV_PART_MAIN);

    // Head (28x24)
    lv_obj_t* head = lv_obj_create(track);
    lv_obj_set_size(head, track_w, 24);
    lv_obj_set_pos(head, 0, 0);
    lv_obj_set_scrollable(head, false);
    lv_obj_set_style_radius(head, r.sm, LV_PART_MAIN);

    const auto& inv = Theme::invariant();
    if (i == 0 || i == 1) {
      // Brown: 16', 5⅓'
      lv_obj_set_style_bg_color(head, inv.drawbar_brown, LV_PART_MAIN);
      lv_obj_set_style_border_width(head, 0, LV_PART_MAIN);
    } else if (i == 2 || i == 3 || i == 5 || i == 8) {
      // White: 8', 4', 2', 1'
      lv_obj_set_style_bg_color(head, inv.drawbar_white, LV_PART_MAIN);
      lv_obj_set_style_border_width(head, 0, LV_PART_MAIN);
    } else {
      // Black: 2⅔', 1⅗', 1⅓'
      lv_obj_set_style_bg_color(head, inv.drawbar_black, LV_PART_MAIN);
      lv_obj_set_style_border_color(head, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(head, 1, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, LV_PART_MAIN);

    // Value label below track
    lv_obj_t* lbl_val = lv_label_create(col);
    char val_buf[4];
    std::snprintf(val_buf, sizeof(val_buf), "%u", drawbars_[i]);
    lv_label_set_text(lbl_val, val_buf);
    lv_obj_add_style(lbl_val, Theme::style_text(), LV_PART_MAIN);
    lv_obj_align(lbl_val, LV_ALIGN_BOTTOM_MID, 0, -4);

    drawbar_col_objs_[i] = col;
    drawbar_track_objs_[i] = track;
    drawbar_head_objs_[i] = head;
    drawbar_val_objs_[i] = lbl_val;
  }

  // 6. Status Bar: 1024x56, pos (0, 544)
  status_bar_ = lv_obj_create(root_);
  lv_obj_set_size(status_bar_, 1024, sp.device_status);
  lv_obj_set_pos(status_bar_, 0, 600 - sp.device_status);
  lv_obj_set_scrollable(status_bar_, false);
  lv_obj_add_style(status_bar_, Theme::style_topbar(), LV_PART_MAIN);

  // Status normal container
  status_normal_container_ = lv_obj_create(status_bar_);
  lv_obj_set_size(status_normal_container_, 1024, sp.device_status);
  lv_obj_set_pos(status_normal_container_, 0, 0);
  lv_obj_set_scrollable(status_normal_container_, false);
  lv_obj_set_style_bg_opa(status_normal_container_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(status_normal_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(status_normal_container_, 0, LV_PART_MAIN);

  // VOICES n / max
  lbl_status_voices_ = lv_label_create(status_normal_container_);
  char v_buf[64];
  std::snprintf(v_buf, sizeof(v_buf), "%s %u / %u", tr("performance.status.voices").c_str(), voices_active_, voices_max_);
  lv_label_set_text(lbl_status_voices_, v_buf);
  lv_obj_add_style(lbl_status_voices_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_status_voices_, LV_ALIGN_LEFT_MID, sp.screen_gutter, 0);

  // STREAMING OK
  lbl_status_streaming_ = lv_label_create(status_normal_container_);
  lv_label_set_text(lbl_status_streaming_, tr("performance.status.streaming_ok").c_str());
  lv_obj_set_style_text_color(lbl_status_streaming_, p.ok, LV_PART_MAIN);
  lv_obj_align(lbl_status_streaming_, LV_ALIGN_LEFT_MID, sp.screen_gutter + 180, 0);

  // ROTARY
  lbl_status_rotary_ = lv_label_create(status_normal_container_);
  lv_label_set_text(lbl_status_rotary_, tr("performance.status.rotary_slow").c_str());
  lv_obj_set_style_text_color(lbl_status_rotary_, p.accent, LV_PART_MAIN);
  lv_obj_align(lbl_status_rotary_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_hidden(lbl_status_rotary_, true);

  // REV button (opens Effects surface)
  btn_status_rev_ = lv_obj_create(status_normal_container_);
  lv_obj_set_size(btn_status_rev_, 100, sp.device_status);
  lv_obj_align(btn_status_rev_, LV_ALIGN_RIGHT_MID, -sp.screen_gutter, 0);
  lv_obj_set_scrollable(btn_status_rev_, false);
  lv_obj_set_clickable(btn_status_rev_, true);
  lv_obj_set_style_bg_opa(btn_status_rev_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_status_rev_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn_status_rev_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn_status_rev_, rev_click_cb, LV_EVENT_CLICKED, this);

  lbl_status_rev_ = lv_label_create(btn_status_rev_);
  char rev_buf[32];
  std::snprintf(rev_buf, sizeof(rev_buf), "%s %d%%", tr("performance.status.rev").c_str(), static_cast<int>(rev_send_ * 100.0f));
  lv_label_set_text(lbl_status_rev_, rev_buf);
  lv_obj_set_style_text_color(lbl_status_rev_, p.accent, LV_PART_MAIN);
  lv_obj_align(lbl_status_rev_, LV_ALIGN_CENTER, 0, 0);

  // Status takeover label (warning takeover line)
  lbl_status_takeover_ = lv_label_create(status_bar_);
  lv_obj_set_size(lbl_status_takeover_, 1024 - 2 * sp.screen_gutter, sp.device_status);
  lv_label_set_text(lbl_status_takeover_, "");
  lv_obj_set_style_text_color(lbl_status_takeover_, p.warn, LV_PART_MAIN);
  lv_obj_align(lbl_status_takeover_, LV_ALIGN_LEFT_MID, sp.screen_gutter, 0);
  lv_obj_set_hidden(lbl_status_takeover_, true);

  set_live_set(tr("performance.hero.default_name"), 0, false);
  update_scene_tiles();
  update_top_bar_modes();
  update_part_strips();
  update_status_bar();
  return true;
}

void PerformanceView::set_tempo(float bpm) {
  current_bpm_ = bpm;
  if (lbl_tempo_) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "♩ %.1f", bpm);
    lv_label_set_text(lbl_tempo_, buf);
  }
}

void PerformanceView::set_setlist_context(const std::string& setlist_name, int32_t current_idx, int32_t total_count) {
  if (!lbl_context_) return;

  if (setlist_name.empty()) {
    std::string text = tr("performance.top_bar.no_setlist") + " · " + live_set_name_;
    lv_label_set_text(lbl_context_, text.c_str());
  } else {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s · %s · %d / %d",
                  tr("performance.top_bar.setlist").c_str(),
                  setlist_name.c_str(),
                  current_idx + 1,
                  total_count);
    lv_label_set_text(lbl_context_, buf);
  }
}

void PerformanceView::set_live_set(const std::string& name, int32_t index, bool dirty) {
  live_set_name_ = name;
  has_unsaved_edits_ = dirty;

  if (lbl_hero_name_) {
    lv_label_set_text(lbl_hero_name_, name.c_str());
  }

  if (lbl_hero_meta_) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s %02d", tr("performance.hero.live_set").c_str(), index + 1);
    lv_label_set_text(lbl_hero_meta_, buf);
  }

  if (lbl_hero_unsaved_) {
    lv_obj_set_hidden(lbl_hero_unsaved_, !dirty);
    if (dirty && lbl_hero_name_) {
      lv_obj_align_to(lbl_hero_unsaved_, lbl_hero_name_, LV_ALIGN_OUT_RIGHT_MID, Theme::spacing().space_2, 0);
    }
  }
}

void PerformanceView::set_next_live_set(const std::string& title, const std::string& status_text, bool ready) {
  if (lbl_next_title_) {
    lv_label_set_text(lbl_next_title_, title.c_str());
  }
  if (lbl_next_status_) {
    lv_label_set_text(lbl_next_status_, status_text.c_str());
    lv_obj_set_style_text_color(lbl_next_status_,
                                ready ? Theme::current().ok : Theme::current().muted,
                                LV_PART_MAIN);
  }
}

void PerformanceView::set_scenes(const std::vector<SceneTileData>& scenes, int32_t current_scene_idx) {
  scenes_ = scenes;
  current_scene_idx_ = current_scene_idx;
  update_scene_tiles();
}

void PerformanceView::set_modes(bool drawbars, bool scenes_mode, int32_t knob_mode, int32_t knob_part) {
  mode_drawbars_ = drawbars;
  mode_scenes_ = scenes_mode;
  mode_knobs_ = knob_mode;
  knob_part_ = knob_part;
  if (mixer_container_) lv_obj_set_hidden(mixer_container_, mode_drawbars_);
  if (drawbars_container_) lv_obj_set_hidden(drawbars_container_, !mode_drawbars_);
  if (mode_drawbars_) {
    update_drawbars_ui();
  }
  update_top_bar_modes();
}

void PerformanceView::recall_scene(int32_t scene_idx) {
  if (scene_idx < 0 || scene_idx >= 8) return;
  current_scene_idx_ = scene_idx;
  update_scene_tiles();

  if (client_) {
    ws::v1::ClientMessage msg;
    msg.set_request_id(200);
    auto* cmd = msg.mutable_recall_scene();
    cmd->set_index(static_cast<uint32_t>(scene_idx));
    client_->send_command(msg);
  }
}

void PerformanceView::set_parts(const std::vector<PartStripData>& parts) {
  parts_ = parts;
  if (parts_.size() < 8) {
    parts_.resize(8);
  }

  // Check for any missing library warnings
  bool found_missing = false;
  for (size_t i = 0; i < 8; ++i) {
    if (parts_[i].missing_library) {
      char w_buf[64];
      std::snprintf(w_buf, sizeof(w_buf), "%s %zu: %s",
                    tr("performance.mode.part").c_str(),
                    i + 1,
                    tr("performance.part.missing_library").c_str());
      set_warning_takeover(w_buf);
      found_missing = true;
      break;
    }
  }

  if (!found_missing && warning_active_) {
    clear_warning_takeover();
  }

  update_part_strips();
  if (mode_drawbars_) {
    update_drawbars_ui();
  }
}

void PerformanceView::set_part_enabled(int32_t part_idx, bool enabled) {
  if (part_idx < 0 || part_idx >= 8) return;
  parts_[part_idx].enabled = enabled;
  update_part_strips();

  if (on_part_toggle_) {
    on_part_toggle_(part_idx, enabled);
  }
}

void PerformanceView::set_part_volume(int32_t part_idx, float volume) {
  if (part_idx < 0 || part_idx >= 8) return;
  parts_[part_idx].volume = std::clamp(volume, 0.0f, 1.0f);
  update_part_strips();
  if (mode_drawbars_) {
    update_drawbars_ui();
  }
}

void PerformanceView::set_status(uint32_t voices_active, uint32_t voices_max, bool streaming_ok, int32_t rotary_state, float rev_send) {
  voices_active_ = voices_active;
  voices_max_ = voices_max;
  streaming_ok_ = streaming_ok;
  rotary_state_ = rotary_state;
  rev_send_ = rev_send;
  update_status_bar();
}

void PerformanceView::set_warning_takeover(const std::string& warning_text) {
  warning_active_ = true;
  warning_text_ = warning_text;

  if (lbl_status_takeover_) {
    std::string text = "⚠ " + warning_text;
    lv_label_set_text(lbl_status_takeover_, text.c_str());
    lv_obj_set_hidden(lbl_status_takeover_, false);
  }
  if (status_normal_container_) {
    lv_obj_set_hidden(status_normal_container_, true);
  }
}

void PerformanceView::clear_warning_takeover() {
  warning_active_ = false;
  warning_text_.clear();

  if (lbl_status_takeover_) {
    lv_label_set_text(lbl_status_takeover_, "");
    lv_obj_set_hidden(lbl_status_takeover_, true);
  }
  if (status_normal_container_) {
    lv_obj_set_hidden(status_normal_container_, false);
  }
}

void PerformanceView::update_scene_tiles() {
  if (scene_tile_objs_.empty()) return;

  const auto& p = Theme::current();

  for (size_t i = 0; i < 8; ++i) {
    lv_obj_t* tile = scene_tile_objs_[i];
    lv_obj_t* lbl_t = scene_title_objs_[i];
    lv_obj_t* lbl_s = scene_sub_objs_[i];

    bool has_data = (i < scenes_.size() && !scenes_[i].is_empty);
    bool is_current = (static_cast<int32_t>(i) == current_scene_idx_);

    char num_buf[32];
    std::snprintf(num_buf, sizeof(num_buf), "%zu", i + 1);

    if (has_data) {
      std::string title_str = std::string(num_buf) + " " + scenes_[i].name;
      lv_label_set_text(lbl_t, title_str.c_str());
      lv_label_set_text(lbl_s, scenes_[i].sub_label.c_str());

      if (is_current) {
        lv_obj_set_style_bg_color(tile, p.accent, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(tile, p.accent, LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl_t, p.on_accent, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl_s, p.on_accent, LV_PART_MAIN);
      } else {
        lv_obj_set_style_bg_color(tile, p.surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(tile, p.line, LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl_t, p.text, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl_s, p.muted, LV_PART_MAIN);
      }
    } else {
      // Empty slot: dashed border, transparent bg, muted number
      lv_label_set_text(lbl_t, num_buf);
      lv_label_set_text(lbl_s, "");
      lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_color(tile, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl_t, p.muted, LV_PART_MAIN);
    }
  }
}

void PerformanceView::update_top_bar_modes() {
  if (!lbl_mode_mixer_) return;

  const auto& p = Theme::current();

  // Mode 1: MIXER / DRAWBARS
  lv_label_set_text(lbl_mode_mixer_,
                    mode_drawbars_ ? tr("performance.mode.drawbars").c_str()
                                   : tr("performance.mode.mixer").c_str());
  lv_obj_set_style_text_color(lbl_mode_mixer_, p.accent, LV_PART_MAIN);

  // Mode 2: PARTS / SCENES
  lv_label_set_text(lbl_mode_parts_,
                    mode_scenes_ ? tr("performance.mode.scenes").c_str()
                                 : tr("performance.mode.parts").c_str());
  lv_obj_set_style_text_color(lbl_mode_parts_, p.accent, LV_PART_MAIN);

  // Mode 3: KNOBS PART n / ALL / ASSIGN
  char buf[32];
  if (mode_knobs_ == 1) {
    std::snprintf(buf, sizeof(buf), "%s %s", tr("performance.mode.knobs").c_str(), tr("performance.mode.all").c_str());
  } else if (mode_knobs_ == 2) {
    std::snprintf(buf, sizeof(buf), "%s %s", tr("performance.mode.knobs").c_str(), tr("performance.mode.assign").c_str());
  } else {
    std::snprintf(buf, sizeof(buf), "%s %s %d", tr("performance.mode.knobs").c_str(), tr("performance.mode.part").c_str(), knob_part_);
  }
  lv_label_set_text(lbl_mode_knobs_, buf);
  lv_obj_set_style_text_color(lbl_mode_knobs_, p.accent, LV_PART_MAIN);
}

void PerformanceView::update_part_strips() {
  if (part_card_objs_.empty()) return;

  const auto& p = Theme::current();

  for (size_t i = 0; i < 8; ++i) {
    if (i >= parts_.size()) break;

    const auto& pt = parts_[i];
    lv_obj_t* card = part_card_objs_[i];
    lv_obj_t* led = part_led_objs_[i];
    lv_obj_t* lbl_p = part_power_lbl_objs_[i];
    lv_obj_t* lbl_nm = part_name_objs_[i];
    lv_obj_t* lbl_c = part_cat_objs_[i];
    lv_obj_t* fill = part_vol_fill_objs_[i];

    // Knob-target selection border (PARTS mode)
    if (!mode_scenes_ && static_cast<int32_t>(i) == selected_knob_part_idx_) {
      lv_obj_set_style_border_color(card, p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, Theme::spacing().selection_border, LV_PART_MAIN);
    } else {
      lv_obj_set_style_border_color(card, p.line, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    }

    // Volume level: height in range [0..120]
    int32_t fill_h = static_cast<int32_t>(120.0f * std::clamp(pt.volume, 0.0f, 1.0f));
    lv_obj_set_size(fill, 16, fill_h);

    if (pt.is_empty) {
      // Empty part: muted styling, "— empty"
      lv_obj_set_style_bg_opa(led, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_color(led, p.muted, LV_PART_MAIN);
      lv_obj_set_style_border_width(led, 1, LV_PART_MAIN);

      lv_label_set_text(lbl_p, tr("performance.part.power").c_str());
      lv_obj_set_style_text_color(lbl_p, p.muted, LV_PART_MAIN);

      lv_label_set_text(lbl_nm, "");
      lv_label_set_text(lbl_c, tr("performance.part.empty").c_str());
      lv_obj_set_style_text_color(lbl_c, p.muted, LV_PART_MAIN);

      lv_obj_set_style_bg_color(fill, p.muted, LV_PART_MAIN);
    } else if (pt.missing_library) {
      // Missing library: warn text in place of category
      lv_obj_set_style_bg_color(led, p.warn, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(led, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(led, 0, LV_PART_MAIN);

      lv_label_set_text(lbl_p, tr("performance.part.power").c_str());
      lv_obj_set_style_text_color(lbl_p, p.warn, LV_PART_MAIN);

      lv_label_set_text(lbl_nm, pt.timbre_name.c_str());
      lv_obj_set_style_text_color(lbl_nm, p.text, LV_PART_MAIN);

      lv_label_set_text(lbl_c, tr("performance.part.missing_library").c_str());
      lv_obj_set_style_text_color(lbl_c, p.warn, LV_PART_MAIN);

      lv_obj_set_style_bg_color(fill, p.warn, LV_PART_MAIN);
    } else if (!pt.enabled) {
      // Off part: muted at full opacity, hollow ring, "OFF" tag
      lv_obj_set_style_bg_opa(led, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_color(led, p.muted, LV_PART_MAIN);
      lv_obj_set_style_border_width(led, 1, LV_PART_MAIN);

      lv_label_set_text(lbl_p, tr("performance.part.power").c_str());
      lv_obj_set_style_text_color(lbl_p, p.muted, LV_PART_MAIN);

      lv_label_set_text(lbl_nm, pt.timbre_name.c_str());
      lv_obj_set_style_text_color(lbl_nm, p.muted, LV_PART_MAIN);

      lv_label_set_text(lbl_c, tr("performance.part.off").c_str());
      lv_obj_set_style_text_color(lbl_c, p.muted, LV_PART_MAIN);

      lv_obj_set_style_bg_color(fill, p.muted, LV_PART_MAIN);
    } else {
      // Normal active part
      lv_obj_set_style_bg_color(led, p.accent, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(led, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(led, 0, LV_PART_MAIN);

      lv_label_set_text(lbl_p, tr("performance.part.power").c_str());
      lv_obj_set_style_text_color(lbl_p, p.accent, LV_PART_MAIN);

      lv_label_set_text(lbl_nm, pt.timbre_name.c_str());
      lv_obj_set_style_text_color(lbl_nm, p.text, LV_PART_MAIN);

      lv_label_set_text(lbl_c, pt.category.c_str());
      lv_obj_set_style_text_color(lbl_c, p.muted, LV_PART_MAIN);

      lv_obj_set_style_bg_color(fill, p.accent, LV_PART_MAIN);
    }

    // Pickup Ghost & Chevron
    lv_obj_t* ghost = part_ghost_objs_[i];
    lv_obj_t* chev = part_chevron_objs_[i];
    if (pt.pickup_pending) {
      lv_obj_set_hidden(ghost, false);
      lv_obj_set_hidden(chev, false);

      // Card outline: 2px warn
      lv_obj_set_style_border_color(card, p.warn, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);

      // Ghost vertical position across Volume Bar (height 120, pos y 130)
      int32_t ghost_y = 130 + 120 - static_cast<int32_t>(120.0f * pt.physical_fader_pos) - 3;
      lv_obj_set_y(ghost, ghost_y);
      lv_obj_set_y(chev, ghost_y - 4);

      if (pt.physical_fader_pos < pt.volume) {
        lv_label_set_text(chev, tr("pickup.chevron.up").c_str());
      } else {
        lv_label_set_text(chev, tr("pickup.chevron.down").c_str());
      }
    } else {
      lv_obj_set_hidden(ghost, true);
      lv_obj_set_hidden(chev, true);
    }
  }
}

void PerformanceView::update_status_bar() {
  if (!lbl_status_voices_) return;

  const auto& p = Theme::current();

  // Voices
  char v_buf[64];
  std::snprintf(v_buf, sizeof(v_buf), "%s %u / %u", tr("performance.status.voices").c_str(), voices_active_, voices_max_);
  lv_label_set_text(lbl_status_voices_, v_buf);

  // Streaming status
  if (streaming_ok_) {
    lv_label_set_text(lbl_status_streaming_, tr("performance.status.streaming_ok").c_str());
    lv_obj_set_style_text_color(lbl_status_streaming_, p.ok, LV_PART_MAIN);
  }

  // Rotary
  if (rotary_state_ < 0) {
    lv_obj_set_hidden(lbl_status_rotary_, true);
  } else {
    lv_obj_set_hidden(lbl_status_rotary_, false);
    if (rotary_state_ == 0) {
      lv_label_set_text(lbl_status_rotary_, tr("performance.status.rotary_stop").c_str());
      lv_obj_set_style_text_color(lbl_status_rotary_, p.muted, LV_PART_MAIN);
    } else if (rotary_state_ == 1) {
      lv_label_set_text(lbl_status_rotary_, tr("performance.status.rotary_slow").c_str());
      lv_obj_set_style_text_color(lbl_status_rotary_, p.accent, LV_PART_MAIN);
    } else {
      lv_label_set_text(lbl_status_rotary_, tr("performance.status.rotary_fast").c_str());
      lv_obj_set_style_text_color(lbl_status_rotary_, p.accent, LV_PART_MAIN);
    }
  }

  // REV %
  char rev_buf[32];
  std::snprintf(rev_buf, sizeof(rev_buf), "%s %d%%", tr("performance.status.rev").c_str(), static_cast<int>(rev_send_ * 100.0f));
  lv_label_set_text(lbl_status_rev_, rev_buf);
}

void PerformanceView::update_from_state(const ws::v1::State& state) {
  for (const auto& pv : state.performance_init()) {
    if (pv.param_id() == ws::v1::PARAM_TEMPO_BPM) {
      set_tempo(pv.value());
      break;
    }
  }

  if (!state.live_set_name().empty()) {
    set_live_set(state.live_set_name(), 0, state.edits().current_dirty());
  }

  if (state.has_setlists()) {
    const auto& s = state.setlists();
    set_setlist_context(s.loaded_setlist_name(),
                        s.last_setlist_position(),
                        static_cast<int32_t>(s.loaded_setlist_size()));
  }

  if (state.scene_count() > 0) {
    std::vector<SceneTileData> sc(state.scene_count());
    for (uint32_t i = 0; i < state.scene_count(); ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "Scene %u", i + 1);
      sc[i].name = buf;
      sc[i].sub_label = "";
      sc[i].is_empty = false;
    }
    set_scenes(sc, state.current_scene());
  }
}

void PerformanceView::toggle_parts_scenes_mode() {
  mode_scenes_ = !mode_scenes_;
  update_top_bar_modes();
  update_part_strips();
  if (mode_drawbars_) {
    update_drawbars_ui();
  }
}

void PerformanceView::select_knob_part(int32_t part_idx) {
  selected_knob_part_idx_ = std::clamp(part_idx, 0, 7);
  knob_part_ = selected_knob_part_idx_ + 1;
  mode_knobs_ = 0; // PART mode
  update_part_strips();
  update_top_bar_modes();
}

void PerformanceView::handle_part_scene_press(int32_t btn_idx, bool pressed, uint64_t timestamp_us) {
  if (btn_idx < 0 || btn_idx >= 8) return;

  if (pressed) {
    part_btn_press_time_[btn_idx] = (timestamp_us > 0) ? timestamp_us : 1;
  } else {
    uint64_t press_t = part_btn_press_time_[btn_idx];
    uint64_t duration = (timestamp_us > press_t && press_t > 0) ? (timestamp_us - press_t) : 0;
    part_btn_press_time_[btn_idx] = 0;

    if (duration >= 500000) {
      // Long press (>= 500 ms) in PARTS mode: selects knob target without toggling
      if (!mode_scenes_) {
        select_knob_part(btn_idx);
      }
    } else {
      // Short press
      if (!mode_scenes_) {
        set_part_enabled(btn_idx, !parts_[btn_idx].enabled);
      } else {
        // SCENES mode: recall scene if non-empty
        if (btn_idx < static_cast<int32_t>(scenes_.size()) && !scenes_[btn_idx].is_empty) {
          recall_scene(btn_idx);
          scene_led_dipping_ = true;
          scene_dip_start_us_ = timestamp_us;
        }
      }
    }
  }
}

LedLevel PerformanceView::button_led_level(size_t index) const {
  if (index >= 8) return LedLevel::Off;

  if (!mode_scenes_) {
    return parts_[index].enabled ? LedLevel::Strong : LedLevel::Off;
  }

  // SCENES mode
  if (static_cast<int32_t>(index) == current_scene_idx_) {
    if (scene_led_dipping_) return LedLevel::Off;
    return LedLevel::Strong;
  }
  if (index < scenes_.size() && !scenes_[index].is_empty) {
    return LedLevel::Weak;
  }
  return LedLevel::Off;
}

void PerformanceView::set_pickup_pending(int32_t part_idx, float physical_pos) {
  if (part_idx < 0 || part_idx >= 8) return;
  parts_[part_idx].pickup_pending = true;
  parts_[part_idx].physical_fader_pos = std::clamp(physical_pos, 0.0f, 1.0f);
  active_pickup_fader_ = part_idx;
  update_part_strips();
  update_pickup_takeover();
}

void PerformanceView::handle_fader_move(int32_t part_idx, float physical_pos) {
  if (part_idx < 0 || part_idx >= 8) return;
  float pos = std::clamp(physical_pos, 0.0f, 1.0f);

  if (parts_[part_idx].pickup_pending) {
    bool was_below = (parts_[part_idx].physical_fader_pos <= parts_[part_idx].volume);
    bool now_above = (pos >= parts_[part_idx].volume);
    bool was_above = (parts_[part_idx].physical_fader_pos >= parts_[part_idx].volume);
    bool now_below = (pos <= parts_[part_idx].volume);

    if ((was_below && now_above) || (was_above && now_below)) {
      // Crossed stored value -> Takeover achieved!
      parts_[part_idx].pickup_pending = false;
      parts_[part_idx].volume = pos;
      parts_[part_idx].physical_fader_pos = pos;
      if (active_pickup_fader_ == part_idx) {
        active_pickup_fader_ = -1;
        for (size_t k = 0; k < 8; ++k) {
          if (parts_[k].pickup_pending) {
            active_pickup_fader_ = static_cast<int32_t>(k);
            break;
          }
        }
      }
    } else {
      parts_[part_idx].physical_fader_pos = pos;
      active_pickup_fader_ = part_idx;
    }
  } else {
    // Already taken over, volume follows fader
    parts_[part_idx].volume = pos;
    parts_[part_idx].physical_fader_pos = pos;
  }

  update_part_strips();
  update_pickup_takeover();
}

void PerformanceView::update_pickup_takeover() {
  if (active_pickup_fader_ >= 0 && active_pickup_fader_ < 8 && parts_[active_pickup_fader_].pickup_pending) {
    const auto& pt = parts_[active_pickup_fader_];
    char buf[64];
    int pct = static_cast<int>(pt.volume * 100.0f + 0.5f);
    std::string chev = (pt.physical_fader_pos < pt.volume) ? tr("pickup.chevron.up") : tr("pickup.chevron.down");
    std::snprintf(buf, sizeof(buf), "%s %s %d → %d%%", chev.c_str(), tr("performance.status.fader").c_str(), active_pickup_fader_ + 1, pct);
    set_warning_takeover(buf);
  } else {
    if (warning_active_ && warning_text_.find(tr("performance.status.fader")) != std::string::npos) {
      clear_warning_takeover();
    }
  }
}

bool PerformanceView::has_organ_part() const {
  for (size_t i = 0; i < parts_.size(); ++i) {
    if (parts_[i].is_empty) continue;
    const auto& cat = parts_[i].category;
    const auto& nm = parts_[i].timbre_name;
    if (cat == "Organ" || cat == "organ" || cat == "Órgão" || cat == "Órgano" ||
        cat == tr("category.organ.short") || cat == tr("category.organ.full") ||
        nm.find("Organ") != std::string::npos || nm.find("organ") != std::string::npos ||
        nm.find("B3") != std::string::npos) {
      return true;
    }
  }
  return false;
}

void PerformanceView::toggle_drawbars_mixer_mode() {
  if (!has_organ_part()) {
    drawbars_led_blinking_ = true;
    set_warning_takeover(tr("organ.no_organ_notice"));
    return;
  }

  drawbars_led_blinking_ = false;
  mode_drawbars_ = !mode_drawbars_;

  if (mode_drawbars_) {
    // Determine active organ part
    if (selected_knob_part_idx_ >= 0 && selected_knob_part_idx_ < static_cast<int32_t>(parts_.size()) &&
        !parts_[selected_knob_part_idx_].is_empty &&
        (parts_[selected_knob_part_idx_].category == "Organ" ||
         parts_[selected_knob_part_idx_].category == tr("category.organ.short") ||
         parts_[selected_knob_part_idx_].timbre_name.find("Organ") != std::string::npos ||
         parts_[selected_knob_part_idx_].timbre_name.find("B3") != std::string::npos)) {
      organ_part_idx_ = selected_knob_part_idx_;
    } else {
      for (size_t i = 0; i < parts_.size(); ++i) {
        if (!parts_[i].is_empty &&
            (parts_[i].category == "Organ" ||
             parts_[i].category == tr("category.organ.short") ||
             parts_[i].timbre_name.find("Organ") != std::string::npos ||
             parts_[i].timbre_name.find("B3") != std::string::npos)) {
          organ_part_idx_ = static_cast<int32_t>(i);
          break;
        }
      }
    }

    if (mixer_container_) lv_obj_set_hidden(mixer_container_, true);
    if (drawbars_container_) lv_obj_set_hidden(drawbars_container_, false);
    update_drawbars_ui();
  } else {
    if (mixer_container_) lv_obj_set_hidden(mixer_container_, false);
    if (drawbars_container_) lv_obj_set_hidden(drawbars_container_, true);
    update_part_strips();
  }

  update_top_bar_modes();
}

void PerformanceView::set_drawbar(size_t index, uint8_t value) {
  if (index >= 9) return;
  drawbars_[index] = std::min<uint8_t>(value, 8);
  update_drawbars_ui();
}

std::string PerformanceView::registration_string() const {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%u%u %u%u%u%u %u%u%u",
                drawbars_[0], drawbars_[1],
                drawbars_[2], drawbars_[3], drawbars_[4], drawbars_[5],
                drawbars_[6], drawbars_[7], drawbars_[8]);
  return std::string(buf);
}

void PerformanceView::set_rotary_speed(int32_t speed) {
  organ_rotary_speed_ = std::clamp(speed, 0, 2);
  rotary_state_ = organ_rotary_speed_;
  update_drawbars_ui();
  update_status_bar();
}

void PerformanceView::update_drawbars_ui() {
  if (!drawbars_container_ || !lbl_organ_meta_) return;

  char meta_buf[64];
  std::snprintf(meta_buf, sizeof(meta_buf), "%s %d · %s",
                tr("performance.mode.part").c_str(),
                organ_part_idx_ + 1,
                tr("organ.meta.drawbars").c_str());
  lv_label_set_text(lbl_organ_meta_, meta_buf);

  std::string organ_name = "Organ";
  std::string organ_cat = tr("category.organ.short");
  if (organ_part_idx_ >= 0 && organ_part_idx_ < static_cast<int32_t>(parts_.size())) {
    if (!parts_[organ_part_idx_].timbre_name.empty()) {
      organ_name = parts_[organ_part_idx_].timbre_name;
    }
    if (!parts_[organ_part_idx_].category.empty()) {
      organ_cat = parts_[organ_part_idx_].category;
    }
  }
  lv_label_set_text(lbl_organ_title_, organ_name.c_str());
  lv_label_set_text(lbl_organ_cat_, organ_cat.c_str());

  std::string rot_str = (organ_rotary_speed_ == 0) ? tr("performance.status.rotary_stop")
                        : (organ_rotary_speed_ == 1) ? tr("performance.status.rotary_slow")
                        : tr("performance.status.rotary_fast");

  char details_buf[128];
  std::snprintf(details_buf, sizeof(details_buf), "%s · %s\n%s",
                tr("organ.meta.percussion").c_str(),
                tr("organ.meta.vibrato").c_str(),
                rot_str.c_str());
  lv_label_set_text(lbl_organ_details_, details_buf);

  std::string reg = registration_string();
  lv_label_set_text(lbl_organ_reg_, reg.c_str());

  for (size_t i = 0; i < 9; ++i) {
    if (i >= drawbar_head_objs_.size()) break;

    uint8_t val = drawbars_[i];
    int32_t head_y = static_cast<int32_t>(val * 22);
    lv_obj_set_y(drawbar_head_objs_[i], head_y);

    char val_buf[4];
    std::snprintf(val_buf, sizeof(val_buf), "%u", val);
    lv_label_set_text(drawbar_val_objs_[i], val_buf);
  }
}

void PerformanceView::on_panel_event(const PanelEvent& ev) {
  if (ev.type == PanelEventType::Fader) {
    if (mode_drawbars_) {
      if (ev.id >= MCU_CTRL_FADER_1 && ev.id <= MCU_CTRL_FADER_8) {
        size_t idx = static_cast<size_t>(ev.id - MCU_CTRL_FADER_1);
        uint8_t val = static_cast<uint8_t>(std::round((static_cast<float>(ev.delta) / 4095.0f) * 8.0f));
        set_drawbar(idx, val);
      } else if (ev.id == MCU_CTRL_FADER_9) {
        uint8_t val = static_cast<uint8_t>(std::round((static_cast<float>(ev.delta) / 4095.0f) * 8.0f));
        set_drawbar(8, val);
      }
      return;
    }
    if (ev.id >= MCU_CTRL_FADER_1 && ev.id <= MCU_CTRL_FADER_8) {
      int32_t part_idx = static_cast<int32_t>(ev.id - MCU_CTRL_FADER_1);
      float pos = static_cast<float>(ev.delta) / 4095.0f;
      handle_fader_move(part_idx, pos);
    }
    return;
  }

  if (ev.type == PanelEventType::Button) {
    if (ev.pressed && ev.id == MCU_BTN_TOGGLE_DRAWBARS_MIXER) {
      toggle_drawbars_mixer_mode();
      return;
    }
    if (ev.pressed && ev.id == MCU_BTN_ROTARY_STOP) {
      set_rotary_speed(0);
      return;
    }
    if (ev.pressed && ev.id == MCU_BTN_ROTARY_SF) {
      set_rotary_speed(organ_rotary_speed_ == 2 ? 1 : 2);
      return;
    }
    if (ev.pressed && ev.id == MCU_BTN_TOGGLE_PARTS_SCENES) {
      toggle_parts_scenes_mode();
      return;
    }
    if (ev.id >= MCU_BTN_PART_SCENE_1 && ev.id <= MCU_BTN_PART_SCENE_8) {
      handle_part_scene_press(static_cast<int32_t>(ev.id - MCU_BTN_PART_SCENE_1), ev.pressed, ev.timestamp_us);
      return;
    }
    if (ev.pressed) {
      if (ev.id == MCU_BTN_SCENE_PREV) {
        if (current_scene_idx_ > 0) {
          recall_scene(current_scene_idx_ - 1);
        }
      } else if (ev.id == MCU_BTN_SCENE_NEXT) {
        if (current_scene_idx_ < 7) {
          recall_scene(current_scene_idx_ + 1);
        }
      }
    }
  }
}

void PerformanceView::scene_click_cb(lv_event_t* e) {
  auto* self = static_cast<PerformanceView*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->scene_tile_objs_.size(); ++i) {
    if (self->scene_tile_objs_[i] == target) {
      self->recall_scene(static_cast<int32_t>(i));
      break;
    }
  }
}

void PerformanceView::part_card_click_cb(lv_event_t* e) {
  auto* self = static_cast<PerformanceView*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->part_card_objs_.size(); ++i) {
    if (self->part_card_objs_[i] == target) {
      if (self->on_part_select_) {
        self->on_part_select_(static_cast<int32_t>(i));
      }
      break;
    }
  }
}

void PerformanceView::part_power_click_cb(lv_event_t* e) {
  auto* self = static_cast<PerformanceView*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->part_power_objs_.size(); ++i) {
    if (self->part_power_objs_[i] == target) {
      bool new_state = !self->parts_[i].enabled;
      self->set_part_enabled(static_cast<int32_t>(i), new_state);
      break;
    }
  }
}

void PerformanceView::rev_click_cb(lv_event_t* e) {
  auto* self = static_cast<PerformanceView*>(lv_event_get_user_data(e));
  if (!self) return;

  if (self->on_rev_click_) {
    self->on_rev_click_();
  }
}

}  // namespace velora::ui
