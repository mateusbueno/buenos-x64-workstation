#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace velora::ui {

class WsClient;

class BootView {
 public:
  using ReadyCallback = std::function<void()>;

  explicit BootView(WsClient* client = nullptr);
  ~BootView();

  bool create(lv_obj_t* parent = nullptr);

  void set_visible(bool visible);
  bool is_visible() const noexcept { return visible_; }

  void set_reduced_motion(bool rm);
  bool reduced_motion() const noexcept { return reduced_motion_; }

  void tick(uint32_t elapsed_ms);

  void set_progress(float fraction);
  float progress() const noexcept { return progress_; }

  void set_status(const std::string& live_set_name, int scene_number);
  void set_status_text(const std::string& text);
  const std::string& status_text() const noexcept { return current_status_text_; }

  void set_sub_status_text(const std::string& text);
  const std::string& sub_status_text() const noexcept { return current_sub_status_text_; }

  void set_ready(bool ready);
  bool is_ready() const noexcept { return is_ready_; }

  void set_on_ready(ReadyCallback cb) { on_ready_ = std::move(cb); }

  // Inspection helpers for tests
  int32_t bar_height(size_t idx) const;
  lv_opa_t wordmark_opacity() const;
  bool animation_finished() const noexcept { return animation_finished_; }
  uint32_t transition_duration_ms() const noexcept;

  lv_obj_t* root() const noexcept { return root_; }

 private:
  void update_drawbars_layout();
  void update_wordmark_opacity();
  void trigger_transition_to_performance();

  WsClient* client_{nullptr};
  ReadyCallback on_ready_;

  bool visible_{true};
  bool reduced_motion_{false};
  bool is_ready_{false};
  bool animation_finished_{false};
  bool transition_triggered_{false};

  float progress_{0.0f};
  std::string current_status_text_;
  std::string current_sub_status_text_;

  uint32_t anim_elapsed_ms_{0};

  // UI objects
  lv_obj_t* root_{nullptr};
  lv_obj_t* symbol_container_{nullptr};
  std::vector<lv_obj_t*> drawbar_objs_;
  lv_obj_t* lbl_wordmark_{nullptr};
  lv_obj_t* progress_bar_{nullptr};
  lv_obj_t* lbl_status_{nullptr};
  lv_obj_t* lbl_sub_status_{nullptr};

  // Dimensions
  static constexpr int kNumBars = 5;
  static constexpr int kBarWidth = 18;
  static constexpr int kBarGap = 14;
  static constexpr int kTallBarHeight = 140;
  static constexpr int kShortBarHeight = 44;
  static constexpr int kBaselineY = 240;
  static constexpr int kCenterX = 512;
  static constexpr int kTotalSymbolWidth = kNumBars * kBarWidth + (kNumBars - 1) * kBarGap;  // 146
};

}  // namespace velora::ui
