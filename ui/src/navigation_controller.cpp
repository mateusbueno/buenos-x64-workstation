#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/ws_client.hpp"

namespace velora::ui {

NavigationController::NavigationController(WsClient* client)
    : client_(client),
      performance_view_(client) {}

NavigationController::~NavigationController() = default;

bool NavigationController::initialize(lv_obj_t* parent) {
  if (!parent) {
    parent = lv_screen_active();
  }
  if (!parent) return false;

  if (!performance_view_.create(parent)) return false;
  if (!launcher_view_.create(parent)) return false;
  if (!effects_view_.create(parent)) return false;

  // Wire callbacks
  performance_view_.set_rev_click_callback([this]() {
    surface_before_launcher_ = current_surface_;
    navigate_to(SurfaceId::Effects);
  });

  launcher_view_.set_tile_select_callback([this](LauncherTileId tile) {
    switch (tile) {
      case LauncherTileId::Effects:
        navigate_to(SurfaceId::Effects);
        break;
      case LauncherTileId::Setlist:
        navigate_to(SurfaceId::Setlist);
        break;
      case LauncherTileId::SetlistBrowser:
        navigate_to(SurfaceId::SetlistBrowser);
        break;
      case LauncherTileId::Scenes:
        navigate_to(SurfaceId::Scenes);
        break;
      case LauncherTileId::Settings:
        navigate_to(SurfaceId::Settings);
        break;
      default:
        break;
    }
  });

  launcher_view_.set_exit_callback([this]() {
    navigate_back();
  });

  // Start on Performance, hide secondary surfaces
  hide_surface(SurfaceId::Launcher);
  hide_surface(SurfaceId::Effects);
  show_surface(SurfaceId::Performance);

  current_surface_ = SurfaceId::Performance;
  previous_surface_ = SurfaceId::Performance;
  surface_before_launcher_ = SurfaceId::Performance;
  return true;
}

void NavigationController::show_surface(SurfaceId surface) {
  lv_obj_t* root = nullptr;
  switch (surface) {
    case SurfaceId::Performance:
      root = performance_view_.root_container();
      break;
    case SurfaceId::Launcher:
      root = launcher_view_.skeleton().root_container();
      break;
    case SurfaceId::Effects:
      root = effects_view_.skeleton().root_container();
      break;
    default:
      break;
  }
  if (root) {
    lv_obj_set_hidden(root, false);
  }
}

void NavigationController::hide_surface(SurfaceId surface) {
  lv_obj_t* root = nullptr;
  switch (surface) {
    case SurfaceId::Performance:
      root = performance_view_.root_container();
      break;
    case SurfaceId::Launcher:
      root = launcher_view_.skeleton().root_container();
      break;
    case SurfaceId::Effects:
      root = effects_view_.skeleton().root_container();
      break;
    default:
      break;
  }
  if (root) {
    lv_obj_set_hidden(root, true);
  }
}

void NavigationController::navigate_to(SurfaceId surface) {
  if (surface == current_surface_) return;

  hide_surface(current_surface_);
  previous_surface_ = current_surface_;
  current_surface_ = surface;
  show_surface(current_surface_);
}

void NavigationController::navigate_back() {
  if (current_surface_ == SurfaceId::Launcher) {
    navigate_to(surface_before_launcher_);
  } else {
    navigate_to(previous_surface_);
  }
}

void NavigationController::update_from_state(const ws::v1::State& state) {
  performance_view_.update_from_state(state);
}

void NavigationController::on_panel_event(const PanelEvent& ev) {
  // 1. Global Stepping: LIVE SET ◀ ▶ and SCENE ◀ ▶ work on ANY surface
  if (ev.type == PanelEventType::Button && ev.pressed) {
    if (ev.id == MCU_BTN_LIVE_SET_PREV) {
      live_set_step_count_--;
      if (client_) {
        ws::v1::ClientMessage msg;
        msg.set_request_id(101);
        msg.mutable_previous_live_set();
        client_->send_command(msg);
      }
      return;
    }
    if (ev.id == MCU_BTN_LIVE_SET_NEXT) {
      live_set_step_count_++;
      if (client_) {
        ws::v1::ClientMessage msg;
        msg.set_request_id(102);
        msg.mutable_next_live_set();
        client_->send_command(msg);
      }
      return;
    }
    if (ev.id == MCU_BTN_SCENE_PREV || ev.id == MCU_BTN_SCENE_NEXT) {
      scene_step_count_ += (ev.id == MCU_BTN_SCENE_NEXT) ? 1 : -1;
      performance_view_.on_panel_event(ev);
      return;
    }

    // 2. MENU Button: Toggles the Launcher Grid
    if (ev.id == MCU_BTN_MENU) {
      if (current_surface_ == SurfaceId::Launcher) {
        navigate_back();
      } else {
        surface_before_launcher_ = current_surface_;
        navigate_to(SurfaceId::Launcher);
      }
      return;
    }

    // 3. EXIT Button Navigation Rules
    if (ev.id == MCU_BTN_EXIT) {
      if (current_surface_ == SurfaceId::Performance) {
        // Rule: On Performance EXIT does nothing
        return;
      }
      if (current_surface_ == SurfaceId::Launcher) {
        navigate_back();
        return;
      }
      if (current_surface_ == SurfaceId::Effects) {
        // If in parameter editor, Effects handles EXIT to return to chain list
        if (effects_view_.current_mode() == EffectsViewMode::ParamEditor) {
          effects_view_.on_panel_event(ev);
          return;
        }
        // Otherwise return to the surface Launcher was opened from
        navigate_to(surface_before_launcher_);
        return;
      }
      navigate_back();
      return;
    }
  }

  // 4. Dispatch to active surface
  switch (current_surface_) {
    case SurfaceId::Performance:
      performance_view_.on_panel_event(ev);
      break;
    case SurfaceId::Launcher:
      launcher_view_.on_panel_event(ev);
      break;
    case SurfaceId::Effects:
      effects_view_.on_panel_event(ev);
      break;
    default:
      break;
  }
}

}  // namespace velora::ui
