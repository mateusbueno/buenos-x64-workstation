#pragma once

#include "velora/ui/display_adapter.hpp"

namespace velora::ui {

class DrmAdapter : public IDisplayAdapter {
 public:
  DrmAdapter() = default;
  ~DrmAdapter() override { shutdown(); }

  bool initialize(int width, int height) override;
  void update() override;
  void shutdown() override;
  bool should_close() const override { return should_close_; }
  lv_display_t* get_display() override { return display_; }
  AdapterType type() const noexcept override { return AdapterType::Drm; }

 private:
  lv_display_t* display_{nullptr};
  bool should_close_{false};
};

}  // namespace velora::ui
