#include "velora/ui/screens/launcher_view.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

namespace velora::ui {

LauncherView::LauncherView() {
  tiles_ = {
    {LauncherTileId::SetlistBrowser, "launcher.tile.setlist_browser", true},
    {LauncherTileId::Setlist,        "launcher.tile.setlist",         true},
    {LauncherTileId::Library,        "launcher.tile.library",         false}, // Available Epic 5
    {LauncherTileId::Effects,        "launcher.tile.effects",         true},
    {LauncherTileId::Scenes,         "launcher.tile.scenes",          true},
    {LauncherTileId::Import,         "launcher.tile.import",          false}, // Available Epic 5
    {LauncherTileId::Settings,       "launcher.tile.settings",        true},
  };
}

LauncherView::~LauncherView() {
  tile_objs_.clear();
  title_lbl_objs_.clear();
  status_lbl_objs_.clear();
}

bool LauncherView::create(lv_obj_t* parent) {
  if (!skeleton_.create(parent)) {
    return false;
  }

  skeleton_.set_title(tr("launcher.title"));

  // Hints: VALUE · move, WRITE · open, EXIT · back
  std::vector<HintItem> hints = {
    {"VALUE", tr("hint.verb.scroll")},
    {"WRITE", tr("hint.verb.confirm")},
    {"EXIT", tr("hint.verb.back")}
  };
  skeleton_.set_hints(hints, "");

  lv_obj_t* content = skeleton_.content_area();
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  const auto& r = Theme::radius();

  tile_objs_.resize(7);
  title_lbl_objs_.resize(7);
  status_lbl_objs_.resize(7);

  // Layout: 4 columns in Row 1, 3 columns in Row 2
  int32_t col_gap = sp.space_4;
  int32_t row_gap = sp.space_4;
  int32_t tile_w = 233;
  int32_t tile_h = 210;
  int32_t start_y = (488 - (tile_h * 2 + row_gap)) / 2;

  for (size_t i = 0; i < 7; ++i) {
    int32_t row = (i < 4) ? 0 : 1;
    int32_t col = (i < 4) ? static_cast<int32_t>(i) : static_cast<int32_t>(i - 4);

    int32_t start_x = sp.screen_gutter;
    if (row == 1) {
      // Center the 3 tiles in row 2
      start_x += (tile_w + col_gap) / 2;
    }

    int32_t x = start_x + col * (tile_w + col_gap);
    int32_t y = start_y + row * (tile_h + row_gap);

    lv_obj_t* tile = lv_obj_create(content);
    lv_obj_set_size(tile, tile_w, tile_h);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_scrollable(tile, false);
    lv_obj_set_clickable(tile, true);
    lv_obj_set_style_radius(tile, r.lg, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, sp.space_4, LV_PART_MAIN);
    lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_title = lv_label_create(tile);
    lv_label_set_text(lbl_title, tr(tiles_[i].title_key).c_str());
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* lbl_status = lv_label_create(tile);
    if (!tiles_[i].is_available) {
      lv_label_set_text(lbl_status, tr("launcher.status.not_available").c_str());
    } else {
      lv_label_set_text(lbl_status, "");
    }
    lv_obj_add_style(lbl_status, Theme::style_text_muted(), LV_PART_MAIN);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    tile_objs_[i] = tile;
    title_lbl_objs_[i] = lbl_title;
    status_lbl_objs_[i] = lbl_status;
  }

  update_tile_styles();
  return true;
}

void LauncherView::set_highlight(int32_t index) {
  if (index < 0) index = 0;
  if (index >= 7) index = 6;
  highlight_idx_ = index;
  update_tile_styles();
}

void LauncherView::activate_highlighted() {
  if (highlight_idx_ >= 0 && highlight_idx_ < 7) {
    if (tiles_[highlight_idx_].is_available && on_tile_select_) {
      on_tile_select_(tiles_[highlight_idx_].id);
    }
  }
}

void LauncherView::on_panel_event(const PanelEvent& ev) {
  if (ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
    set_highlight(highlight_idx_ + ev.delta);
  } else if (ev.type == PanelEventType::Button && ev.pressed) {
    if (ev.id == MCU_BTN_WRITE) {
      activate_highlighted();
    } else if (ev.id == MCU_BTN_EXIT || ev.id == MCU_BTN_MENU) {
      if (on_exit_) {
        on_exit_();
      }
    }
  }
}

void LauncherView::tile_click_cb(lv_event_t* e) {
  auto* self = static_cast<LauncherView*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->tile_objs_.size(); ++i) {
    if (self->tile_objs_[i] == target) {
      self->set_highlight(static_cast<int32_t>(i));
      self->activate_highlighted();
      break;
    }
  }
}

void LauncherView::update_tile_styles() {
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();

  for (size_t i = 0; i < tile_objs_.size(); ++i) {
    lv_obj_t* tile = tile_objs_[i];
    lv_obj_t* lbl_t = title_lbl_objs_[i];
    bool is_highlighted = (static_cast<int32_t>(i) == highlight_idx_);
    bool is_avail = tiles_[i].is_available;

    if (!is_avail) {
      // Disabled state
      lv_obj_set_style_bg_color(tile, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(tile, p.line, LV_PART_MAIN);
      lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl_t, p.muted, LV_PART_MAIN);
    } else if (is_highlighted) {
      // Highlighted state: raised + accent border
      lv_obj_set_style_bg_color(tile, p.raised, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(tile, p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(tile, sp.selection_border, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl_t, p.text, LV_PART_MAIN);
    } else {
      // Normal state
      lv_obj_set_style_bg_color(tile, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(tile, p.line, LV_PART_MAIN);
      lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl_t, p.text, LV_PART_MAIN);
    }
  }
}

}  // namespace velora::ui
