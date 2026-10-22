#pragma once

#include <lvgl.h>

namespace velora::ui {

enum class AdapterType {
  Sdl,
  Drm,
  Headless
};

class IDisplayAdapter {
 public:
  virtual ~IDisplayAdapter() = default;

  virtual bool initialize(int width, int height) = 0;
  virtual void update() = 0;
  virtual void shutdown() = 0;
  virtual bool should_close() const = 0;
  virtual lv_display_t* get_display() = 0;
  virtual AdapterType type() const noexcept = 0;
};

}  // namespace velora::ui
