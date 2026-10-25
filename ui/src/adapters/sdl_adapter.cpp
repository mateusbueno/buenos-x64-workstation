#include "velora/ui/adapters/sdl_adapter.hpp"

#if LV_USE_SDL
#include <SDL.h>
#include <lvgl.h>
#include "velora/ui/virtual_panel.hpp"

namespace velora::ui {

namespace {

int sdl_event_filter(void* userdata, SDL_Event* event) {
  auto* self = static_cast<SdlAdapter*>(userdata);
  if (!self || !event) return 1;

  if (event->type == SDL_QUIT) {
    self->set_should_close(true);
  } else if (event->type == SDL_KEYDOWN) {
    VirtualPanel::instance().handle_key(event->key.keysym.sym, true);
  } else if (event->type == SDL_KEYUP) {
    VirtualPanel::instance().handle_key(event->key.keysym.sym, false);
  }
  return 1;
}

}  // namespace

bool SdlAdapter::initialize(int width, int height) {
  if (display_) return true;

  display_ = lv_sdl_window_create(width, height);
  if (!display_) return false;

  lv_sdl_window_set_title(display_, "Velora 73");
  lv_sdl_window_set_resizeable(display_, true);
  if (scale_ > 0.1f && (scale_ < 0.99f || scale_ > 1.01f)) {
    lv_sdl_window_set_zoom(display_, scale_);
  }
  lv_sdl_mouse_create();

  SDL_AddEventWatch(sdl_event_filter, this);
  should_close_ = false;
  return true;
}

bool SdlAdapter::save_screenshot(const char* path) {
  if (!display_) return false;
  auto* renderer = static_cast<SDL_Renderer*>(lv_sdl_window_get_renderer(display_));
  if (!renderer) return false;
  int w = 0, h = 0;
  SDL_GetRendererOutputSize(renderer, &w, &h);
  SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!surf) return false;
  bool ok = SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch) == 0 &&
            SDL_SaveBMP(surf, path) == 0;
  SDL_FreeSurface(surf);
  return ok;
}

void SdlAdapter::update() {
  SDL_PumpEvents();
}

void SdlAdapter::shutdown() {
  SDL_DelEventWatch(sdl_event_filter, this);
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
