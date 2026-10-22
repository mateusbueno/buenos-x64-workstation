#include "velora/ui/adapters/drm_adapter.hpp"

#if defined(__linux__) && LV_USE_LINUX_DRM
#include <lvgl.h>

namespace velora::ui {

bool DrmAdapter::initialize(int width, int height) {
  if (display_) return true;

  display_ = lv_linux_drm_create();
  if (!display_) return false;

  lv_linux_drm_set_file(display_, "/dev/dri/card0");
  
#if LV_USE_EVDEV
  // Open touchscreen event device on Linux
  lv_indev_t* indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event0");
  (void)indev;
#endif

  should_close_ = false;
  return true;
}

void DrmAdapter::update() {
  // DRM/evdev drivers handle buffer flipping and input polling inside LVGL
}

void DrmAdapter::shutdown() {
  if (display_) {
    lv_display_delete(display_);
    display_ = nullptr;
  }
  should_close_ = false;
}

}  // namespace velora::ui
#else
namespace velora::ui {
bool DrmAdapter::initialize(int, int) { return false; }
void DrmAdapter::update() {}
void DrmAdapter::shutdown() {}
}  // namespace velora::ui
#endif
