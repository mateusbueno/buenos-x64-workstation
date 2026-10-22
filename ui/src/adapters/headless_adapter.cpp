#include "velora/ui/adapters/headless_adapter.hpp"

namespace velora::ui {

static void headless_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  (void)area;
  (void)px_map;
  lv_display_flush_ready(disp);
}

bool HeadlessAdapter::initialize(int width, int height) {
  if (display_) return true;
  if (!lv_is_initialized()) {
    lv_init();
  }

  display_ = lv_display_create(width, height);
  if (!display_) return false;

  const size_t buf_size = static_cast<size_t>(width * height);
  buf1_.resize(buf_size, 0);
  buf2_.resize(buf_size, 0);

  lv_display_set_buffers(
      display_,
      buf1_.data(),
      buf2_.data(),
      buf_size * sizeof(uint32_t),
      LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_display_set_flush_cb(display_, headless_flush_cb);
  should_close_ = false;
  return true;
}

void HeadlessAdapter::update() {
  // Headless has no OS window events to poll
}

void HeadlessAdapter::shutdown() {
  if (display_) {
    lv_display_delete(display_);
    display_ = nullptr;
  }
  buf1_.clear();
  buf2_.clear();
}

}  // namespace velora::ui
