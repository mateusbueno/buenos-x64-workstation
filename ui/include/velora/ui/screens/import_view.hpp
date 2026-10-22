#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>

#include "velora/format/category.hpp"
#include "velora/ui/components/category_picker.hpp"
#include "velora/ui/screens/surface_skeleton.hpp"
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

class WsClient;

enum class ImportItemStatus {
  Ready,
  NeedsCategory,
  SkippedUnsupported,
  SkippedDamaged,
  AlreadyImported
};

struct ImportItem {
  std::string file_name;
  uint64_t size_bytes{0};
  std::string format_str{"SFZ"};
  format::Category category{format::Category::kNone};
  bool is_suggested{false};
  ImportItemStatus status{ImportItemStatus::NeedsCategory};
  bool import_again{false}; // For already_imported items
};

enum class ImportViewState {
  Rows,
  Progress,
  InsufficientStorage,
  DriveRemoved,
  Completed
};

class ImportView {
 public:
  using CloseCallback = std::function<void()>;
  using ImportFinishedCallback = std::function<void(uint32_t imported, uint32_t skipped)>;

  explicit ImportView(WsClient* client = nullptr);
  ~ImportView();

  bool create(lv_obj_t* parent = nullptr);

  void on_panel_event(const PanelEvent& ev);
  void tick(uint32_t elapsed_ms);

  void set_drive_info(const std::string& drive_name, uint64_t required_bytes, uint64_t internal_free_bytes);
  void set_items(std::vector<ImportItem> items);

  const std::vector<ImportItem>& items() const noexcept { return items_; }
  ImportViewState view_state() const noexcept { return state_; }
  bool is_in_progress() const noexcept { return state_ == ImportViewState::Progress; }

  void start_import();
  void simulate_drive_removed();

  void set_on_close(CloseCallback cb) { on_close_ = std::move(cb); }
  void set_on_import_finished(ImportFinishedCallback cb) { on_import_finished_ = std::move(cb); }

  CategoryPicker& category_picker() noexcept { return category_picker_; }
  SurfaceSkeleton& skeleton() noexcept { return skeleton_; }

  int32_t selected_row_index() const noexcept { return focused_row_idx_; }
  const std::string& warning_message() const noexcept { return warning_msg_; }

 private:
  void rebuild_rows_ui();
  void update_header_ui();
  void update_progress_ui();
  void update_focus_ui();
  void update_hints_ui();
  void check_and_start_import();

  static void on_row_clicked(lv_event_t* e);
  static void on_category_chip_clicked(lv_event_t* e);

  WsClient* client_{nullptr};
  SurfaceSkeleton skeleton_;
  CategoryPicker category_picker_;
  CloseCallback on_close_;
  ImportFinishedCallback on_import_finished_;

  std::string drive_name_{"USB DRIVE"};
  uint64_t required_bytes_{0};
  uint64_t internal_free_bytes_{10ULL * 1024 * 1024 * 1024}; // 10 GB default
  std::vector<ImportItem> items_;

  ImportViewState state_{ImportViewState::Rows};
  int32_t focused_row_idx_{0};
  uint32_t current_importing_idx_{0};
  uint32_t total_imported_{0};
  uint32_t total_skipped_{0};
  std::string warning_msg_;

  // UI objects
  lv_obj_t* content_container_{nullptr};
  lv_obj_t* rows_container_{nullptr};
  std::vector<lv_obj_t*> row_objs_;
  std::vector<lv_obj_t*> chip_objs_;
  std::vector<lv_obj_t*> chip_lbl_objs_;
  std::vector<lv_obj_t*> status_lbl_objs_;

  // Progress state UI
  lv_obj_t* progress_container_{nullptr};
  lv_obj_t* progress_bar_obj_{nullptr};
  lv_obj_t* progress_lbl_obj_{nullptr};

  // Error/Message state UI
  lv_obj_t* message_container_{nullptr};
  lv_obj_t* message_lbl_obj_{nullptr};
};

}  // namespace velora::ui
