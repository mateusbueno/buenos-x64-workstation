#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>

#include "velora/format/category.hpp"
#include "velora/ui/screens/surface_skeleton.hpp"
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

class WsClient;

struct TimbreCatalogEntry {
  std::string id;
  std::string name;
  format::Category category{format::Category::kPiano};
  bool is_factory{true};
  bool is_favorite{false};
  uint64_t size_bytes{0};
  uint32_t last_used_timestamp{0};
};

enum class LibraryMode {
  Browse,
  ChooseForPart
};

class LibraryView {
 public:
  using CloseCallback = std::function<void()>;
  using TimbreSelectedCallback = std::function<void(int32_t part_idx, const std::string& timbre_id)>;
  using FavoriteToggleCallback = std::function<void(const std::string& timbre_id, bool is_favorite)>;

  explicit LibraryView(WsClient* client = nullptr);
  ~LibraryView();

  bool create(lv_obj_t* parent = nullptr);

  void on_panel_event(const PanelEvent& ev);
  void on_midi_note(uint8_t note_number);

  void set_mode(LibraryMode mode, int32_t target_part_idx = 0);
  LibraryMode mode() const noexcept { return mode_; }
  int32_t target_part_idx() const noexcept { return target_part_idx_; }

  void set_catalog(std::vector<TimbreCatalogEntry> entries);
  const std::vector<TimbreCatalogEntry>& catalog() const noexcept { return catalog_; }

  void set_search_query(const std::string& query);
  const std::string& search_query() const noexcept { return search_query_; }

  void set_filter_category(format::Category cat);
  void set_filter_favorites_only(bool fav);
  void set_filter_recent_only(bool rec);

  void toggle_favorite_at_focused();
  void toggle_favorite(size_t catalog_idx);

  int32_t focused_index() const noexcept { return focused_grid_idx_; }
  const std::vector<size_t>& filtered_indices() const noexcept { return filtered_indices_; }
  bool is_audition_sounding() const noexcept { return audition_active_; }
  uint8_t last_auditioned_note() const noexcept { return last_auditioned_note_; }

  void set_on_close(CloseCallback cb) { on_close_ = std::move(cb); }
  void set_on_timbre_selected(TimbreSelectedCallback cb) { on_timbre_selected_ = std::move(cb); }
  void set_on_favorite_toggled(FavoriteToggleCallback cb) { on_favorite_toggled_ = std::move(cb); }

  SurfaceSkeleton& skeleton() noexcept { return skeleton_; }

 private:
  void refilter();
  void ensure_builtin_timbres();
  void rebuild_grid_ui();
  void update_chips_ui();
  void update_header_ui();
  void update_focus_ui();
  void update_hints_ui();
  void assign_focused_timbre();

  static void on_card_clicked(lv_event_t* e);
  static void on_card_long_pressed(lv_event_t* e);
  static void on_chip_clicked(lv_event_t* e);

  WsClient* client_{nullptr};
  SurfaceSkeleton skeleton_;
  CloseCallback on_close_;
  TimbreSelectedCallback on_timbre_selected_;
  FavoriteToggleCallback on_favorite_toggled_;

  LibraryMode mode_{LibraryMode::Browse};
  int32_t target_part_idx_{0};

  std::vector<TimbreCatalogEntry> catalog_;
  std::vector<size_t> filtered_indices_; // Indices into catalog_

  std::string search_query_;
  format::Category filter_cat_{format::Category::kNone};
  bool filter_favorites_only_{false};
  bool filter_recent_only_{false};

  int32_t focused_grid_idx_{0};
  bool audition_active_{false};
  uint8_t last_auditioned_note_{0};

  // UI objects
  lv_obj_t* content_container_{nullptr};
  lv_obj_t* search_bar_obj_{nullptr};
  lv_obj_t* lbl_search_icon_{nullptr};
  lv_obj_t* lbl_search_text_{nullptr};

  lv_obj_t* chips_container_{nullptr};
  lv_obj_t* chip_all_{nullptr};
  lv_obj_t* chip_fav_{nullptr};
  lv_obj_t* chip_rec_{nullptr};
  std::vector<lv_obj_t*> category_chip_objs_;

  lv_obj_t* grid_container_{nullptr};
  std::vector<lv_obj_t*> card_objs_;
  std::vector<lv_obj_t*> card_name_objs_;
  std::vector<lv_obj_t*> card_meta_objs_;
  std::vector<lv_obj_t*> card_star_objs_;
  lv_obj_t* empty_label_obj_{nullptr};
};

}  // namespace velora::ui
