#pragma once

#include "velora/ui/display_adapter.hpp"

namespace velora::ui {

class SdlAdapter : public IDisplayAdapter {
 public:
  SdlAdapter() = default;
  ~SdlAdapter() override { shutdown(); }

  bool initialize(int width, int height) override;
  void update() override;
  void shutdown() override;
  bool should_close() const override { return should_close_; }
  void set_should_close(bool val) noexcept { should_close_ = val; }
  void set_scale(float scale) noexcept { scale_ = scale; }
  float scale() const noexcept { return scale_; }
  // Dev aid: dump the rendered frame as a BMP file.
  bool save_screenshot(const char* path);
  lv_display_t* get_display() override { return display_; }
  AdapterType type() const noexcept override { return AdapterType::Sdl; }

 private:
  lv_display_t* display_{nullptr};
  bool should_close_{false};
  float scale_{1.0f};
};

}  // namespace velora::ui
