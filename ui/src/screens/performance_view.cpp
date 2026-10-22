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

  int32_t strip_w = 115;
  int32_t strip_h = 282;

  for (int i = 0; i < 8; ++i) {
    lv_obj_t* card = lv_obj_create(part_area_);
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

    part_card_objs_[i] = card;
    part_num_objs_[i] = lbl_n;
    part_led_objs_[i] = led;
    part_power_objs_[i] = btn_p;
    part_power_lbl_objs_[i] = lbl_p;
    part_name_objs_[i] = lbl_nm;
    part_cat_objs_[i] = lbl_c;
    part_vol_track_objs_[i] = vol_track;
    part_vol_fill_objs_[i] = vol_fill;
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
    lv_obj_t* led = part_led_objs_[i];
    lv_obj_t* lbl_p = part_power_lbl_objs_[i];
    lv_obj_t* lbl_nm = part_name_objs_[i];
    lv_obj_t* lbl_c = part_cat_objs_[i];
    lv_obj_t* fill = part_vol_fill_objs_[i];

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

void PerformanceView::on_panel_event(const PanelEvent& ev) {
  if (ev.type == PanelEventType::Button && ev.pressed) {
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
