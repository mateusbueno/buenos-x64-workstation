#include "velora/ui/adapters/sdl_adapter.hpp"

#if LV_USE_SDL
#include <SDL.h>
#include <lvgl.h>

namespace velora::ui {

bool SdlAdapter::initialize(int width, int height) {
  if (display_) return true;

  display_ = lv_sdl_window_create(width, height);
  if (!display_) return false;

  lv_sdl_window_set_title(display_, "Velora 73");
  lv_sdl_mouse_create();
  should_close_ = false;
  return true;
}

void SdlAdapter::update() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT) {
      should_close_ = true;
    }
  }
}

void SdlAdapter::shutdown() {
  if (display_) {
    lv_display_delete(display_);
    display_ = nullptr;
  }
  should_close_ = false;
}

}  // namespace velora::ui
#else
namespace velora::ui {
bool SdlAdapter::initialize(int, int) { return false; }
void SdlAdapter::update() {}
void SdlAdapter::shutdown() {}
}  // namespace velora::ui
#endif
