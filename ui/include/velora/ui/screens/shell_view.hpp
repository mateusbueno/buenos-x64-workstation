#pragma once

#include <lvgl.h>
#include <string>
#include "ws_v1.pb.h"

namespace velora::ui {

class ShellView {
 public:
  ShellView() = default;
  ~ShellView();

  // Create UI hierarchy on parent (or active screen if parent is nullptr)
  bool create(lv_obj_t* parent = nullptr);

  // Update views based on incoming engine State
  void update_from_state(const ws::v1::State& state);

  // Set tempo display directly
  void set_tempo(float bpm);
  // Set live set name directly
  void set_live_set_name(const std::string& name);
  // Set clock lost status
  void set_clock_lost(bool lost);

  // Accessors for testing
  lv_obj_t* root_container() const noexcept { return root_; }
  lv_obj_t* title_label() const noexcept { return lbl_title_; }
  lv_obj_t* tempo_label() const noexcept { return lbl_tempo_; }
  lv_obj_t* live_set_label() const noexcept { return lbl_liveset_; }
  lv_obj_t* status_label() const noexcept { return lbl_status_; }
  lv_obj_t* hint_label() const noexcept { return lbl_hint_; }

 private:
  lv_obj_t* root_{nullptr};
  lv_obj_t* top_bar_{nullptr};
  lv_obj_t* main_area_{nullptr};
  lv_obj_t* hint_bar_{nullptr};

  lv_obj_t* lbl_title_{nullptr};
  lv_obj_t* lbl_liveset_{nullptr};
  lv_obj_t* lbl_tempo_{nullptr};
  lv_obj_t* lbl_clock_source_{nullptr};

  lv_obj_t* lbl_status_{nullptr};
  lv_obj_t* lbl_hint_{nullptr};

  float current_bpm_{120.0f};
  std::string current_live_set_;
};

}  // namespace velora::ui
