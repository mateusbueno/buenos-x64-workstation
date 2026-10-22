#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/ws_client.hpp"
#include "velora/ui/locale.hpp"

namespace velora::ui {

NavigationController::NavigationController(WsClient* client)
    : client_(client),
      performance_view_(client),
      setlist_view_(client),
      setlist_browser_view_(client),
      scenes_view_(client),
      part_view_(client) {}

NavigationController::~NavigationController() = default;

bool NavigationController::initialize(lv_obj_t* parent) {
  if (!parent) {
    parent = lv_screen_active();
  }
  if (!parent) return false;

  if (!performance_view_.create(parent)) return false;
  if (!launcher_view_.create(parent)) return false;
  if (!effects_view_.create(parent)) return false;
  if (!setlist_view_.create(parent)) return false;
  if (!setlist_browser_view_.create(parent)) return false;
  if (!scenes_view_.create(parent)) return false;
  if (!part_view_.create(parent)) return false;
  if (!settings_view_.create(parent)) return false;
  if (!write_dialog_.create(parent)) return false;

  write_dialog_.set_on_write([this](ws::v1::WriteScope scope, const std::string& name) {
    handle_write_confirmed(scope, name);
  });

  // Wire callbacks
  performance_view_.set_rev_click_callback([this]() {
    surface_before_launcher_ = current_surface_;
    navigate_to(SurfaceId::Effects);
  });

  performance_view_.set_setlist_click_callback([this]() {
    surface_before_launcher_ = current_surface_;
    navigate_to(SurfaceId::SetlistBrowser);
  });

  performance_view_.set_scene_long_press_callback([this](int32_t slot_idx) {
    surface_before_launcher_ = current_surface_;
    scenes_view_.select_slot(slot_idx);
    navigate_to(SurfaceId::Scenes);
  });

  performance_view_.set_part_select_callback([this](int32_t part_idx) {
    surface_before_launcher_ = current_surface_;
    part_view_.set_part_index(part_idx);
    navigate_to(SurfaceId::Part);
  });

  setlist_view_.set_on_close([this]() {
    navigate_to(surface_before_launcher_);
  });

  setlist_browser_view_.set_on_close([this]() {
    navigate_to(surface_before_launcher_);
  });

  scenes_view_.set_on_close([this]() {
    navigate_to(surface_before_launcher_);
  });

  part_view_.set_on_close([this]() {
    navigate_to(surface_before_launcher_);
  });

  part_view_.set_on_edit([this](int32_t part_idx) {
    set_pending_edits(true, true, "Part " + std::to_string(part_idx + 1) + " edit");
  });

  part_view_.set_on_open_inserts([this](int32_t part_idx) {
    surface_before_launcher_ = current_surface_;
    navigate_to(SurfaceId::Effects);
  });

  settings_view_.set_on_close([this]() {
    navigate_to(surface_before_launcher_);
  });

  settings_view_.set_on_edit([this]() {
    set_pending_edits(true, false, "ASSIGN map edit");
  });

  settings_view_.set_on_assign_changed([this](ws::v1::AssignTarget target, uint32_t slot_1_based, uint32_t param_id) {
    if (client_) {
      ws::v1::ClientMessage msg;
      if (param_id == 0) {
        auto* cmd = msg.mutable_clear_assign_slot();
        cmd->set_target(target);
        cmd->set_slot(slot_1_based);
      } else {
        auto* cmd = msg.mutable_set_assign_slot();
        cmd->set_target(target);
        cmd->set_slot(slot_1_based);
        cmd->set_param_id(param_id);
      }
      client_->send_command(msg);
    }
    if (target == ws::v1::ASSIGN_TARGET_CURRENT_LIVE_SET) {
      set_pending_edits(true, false, "ASSIGN slot " + std::to_string(slot_1_based));
    }
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
  hide_surface(SurfaceId::Setlist);
  hide_surface(SurfaceId::SetlistBrowser);
  hide_surface(SurfaceId::Scenes);
  hide_surface(SurfaceId::Part);
  hide_surface(SurfaceId::Settings);
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
    case SurfaceId::Setlist:
      root = setlist_view_.skeleton().root_container();
      break;
    case SurfaceId::SetlistBrowser:
      root = setlist_browser_view_.skeleton().root_container();
      break;
    case SurfaceId::Scenes:
      root = scenes_view_.skeleton().root_container();
      break;
    case SurfaceId::Part:
      root = part_view_.skeleton().root_container();
      break;
    case SurfaceId::Settings:
      root = settings_view_.skeleton().root_container();
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
    case SurfaceId::Setlist:
      root = setlist_view_.skeleton().root_container();
      break;
    case SurfaceId::SetlistBrowser:
      root = setlist_browser_view_.skeleton().root_container();
      break;
    case SurfaceId::Scenes:
      root = scenes_view_.skeleton().root_container();
      break;
    case SurfaceId::Part:
      root = part_view_.skeleton().root_container();
      break;
    case SurfaceId::Settings:
      root = settings_view_.skeleton().root_container();
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

void NavigationController::open_write_dialog() {
  std::string name = performance_view_.live_set_name();
  if (name.empty()) name = "Live Set";
  write_dialog_.open(name, has_timbre_edit_, change_summary_text_, name + " 2");
}

bool NavigationController::has_pending_edits() const {
  if (manual_pending_edits_) return true;
  if (performance_view_.has_unsaved_edits()) return true;
  if (scenes_view_.has_unsaved_edits()) return true;
  if (part_view_.has_unsaved_edits()) return true;
  if (settings_view_.has_unsaved_edits()) return true;
  return false;
}

void NavigationController::show_nothing_to_write_hint() {
  performance_view_.set_warning_takeover(tr("write_dialog.nothing_to_write"));
}

void NavigationController::handle_write_confirmed(ws::v1::WriteScope scope, const std::string& name) {
  manual_pending_edits_ = false;
  has_timbre_edit_ = false;
  change_summary_text_ = "";
  std::string new_name = (scope == ws::v1::WRITE_SCOPE_SAVE_AS_NEW && !name.empty()) ? name : performance_view_.live_set_name();
  performance_view_.set_live_set(new_name, 0, false);
  performance_view_.clear_warning_takeover();

  if (client_) {
    ws::v1::ClientMessage msg;
    msg.set_request_id(501);
    auto* cmd = msg.mutable_write_live_set();
    cmd->set_scope(scope);
    if (scope == ws::v1::WRITE_SCOPE_SAVE_AS_NEW) {
      cmd->set_new_name(new_name);
    }
    client_->send_command(msg);
  }
}

void NavigationController::set_pending_edits(bool pending, bool has_timbre, const std::string& summary) {
  manual_pending_edits_ = pending;
  has_timbre_edit_ = has_timbre;
  change_summary_text_ = summary;
  performance_view_.set_live_set(performance_view_.live_set_name(), 0, pending);
}

void NavigationController::update_from_state(const ws::v1::State& state) {
  performance_view_.update_from_state(state);
  setlist_view_.update_from_state(state);
  setlist_browser_view_.update_from_state(state);
  scenes_view_.update_from_state(state);
  settings_view_.update_from_state(state);

  if (state.has_edits()) {
    manual_pending_edits_ = state.edits().current_dirty();
    has_timbre_edit_ = state.edits().has_timbre_edit();
    change_summary_text_ = state.edits().change_summary_text();
  }
}

void NavigationController::on_panel_event(const PanelEvent& ev) {
  // If WriteDialog is open, it captures all panel events
  if (write_dialog_.is_open()) {
    write_dialog_.on_panel_event(ev);
    return;
  }

  // Handle WRITE hold vs short press
  if (ev.type == PanelEventType::Button && ev.id == MCU_BTN_WRITE) {
    if (ev.pressed) {
      write_btn_down_time_us_ = (ev.timestamp_us > 0) ? ev.timestamp_us : 1;
      return;
    } else {
      uint64_t dur = (ev.timestamp_us > write_btn_down_time_us_ && write_btn_down_time_us_ > 0) ?
                     (ev.timestamp_us - write_btn_down_time_us_) : 0;
      write_btn_down_time_us_ = 0;
      if (dur >= write_hold_threshold_us_) {
        if (has_pending_edits()) {
          open_write_dialog();
        } else {
          show_nothing_to_write_hint();
        }
        return;
      }

      // Short press release (< 0.5s): forward to active surface
      PanelEvent short_press = ev;
      short_press.pressed = true;
      switch (current_surface_) {
        case SurfaceId::Performance:
          show_nothing_to_write_hint();
          break;
        case SurfaceId::Scenes:
          scenes_view_.on_panel_event(short_press);
          break;
        case SurfaceId::Part:
          part_view_.on_panel_event(short_press);
          break;
        case SurfaceId::Settings:
          settings_view_.on_panel_event(short_press);
          break;
        case SurfaceId::Setlist:
          setlist_view_.on_panel_event(short_press);
          break;
        case SurfaceId::SetlistBrowser:
          setlist_browser_view_.on_panel_event(short_press);
          break;
        case SurfaceId::Effects:
          effects_view_.on_panel_event(short_press);
          break;
        case SurfaceId::Launcher:
          launcher_view_.on_panel_event(short_press);
          break;
        default:
          break;
      }
      return;
    }
  }

  // Global Encoders and Knob Mode toggle: operate per current mode across ANY surface
  if (ev.type == PanelEventType::Button && ev.pressed && ev.id == MCU_BTN_TOGGLE_PART_ALL_ASSIGN) {
    performance_view_.on_panel_event(ev);
    return;
  }
  if (ev.type == PanelEventType::Encoder && ev.id >= MCU_CTRL_ENCODER_1 && ev.id <= MCU_CTRL_ENCODER_8) {
    performance_view_.on_panel_event(ev);
    return;
  }

  // UX-DR82, UX-DR86: On Performance, VALUE push opens Part surface of the knob-target part
  if (current_surface_ == SurfaceId::Performance && ev.type == PanelEventType::Button && ev.pressed && ev.id == MCU_BTN_VALUE_PUSH) {
    surface_before_launcher_ = current_surface_;
    part_view_.set_part_index(performance_view_.selected_knob_part());
    navigate_to(SurfaceId::Part);
    return;
  }

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
      if (current_surface_ == SurfaceId::Setlist) {
        if (setlist_view_.keyboard().is_open()) {
          setlist_view_.on_panel_event(ev);
          return;
        }
        navigate_to(surface_before_launcher_);
        return;
      }
      if (current_surface_ == SurfaceId::SetlistBrowser) {
        if (setlist_browser_view_.action_sheet().is_open() || setlist_browser_view_.keyboard().is_open()) {
          setlist_browser_view_.on_panel_event(ev);
          return;
        }
        navigate_to(surface_before_launcher_);
        return;
      }
      if (current_surface_ == SurfaceId::Scenes) {
        if (scenes_view_.keyboard().is_open()) {
          scenes_view_.on_panel_event(ev);
          return;
        }
        navigate_to(surface_before_launcher_);
        return;
      }
      if (current_surface_ == SurfaceId::Part) {
        if (part_view_.is_timbre_picker_open()) {
          part_view_.on_panel_event(ev);
          return;
        }
        navigate_to(surface_before_launcher_);
        return;
      }
      if (current_surface_ == SurfaceId::Settings) {
        if (settings_view_.is_picker_open()) {
          settings_view_.on_panel_event(ev);
          return;
        }
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
    case SurfaceId::Setlist:
      setlist_view_.on_panel_event(ev);
      break;
    case SurfaceId::SetlistBrowser:
      setlist_browser_view_.on_panel_event(ev);
      break;
    case SurfaceId::Scenes:
      scenes_view_.on_panel_event(ev);
      break;
    case SurfaceId::Part:
      part_view_.on_panel_event(ev);
      break;
    case SurfaceId::Settings:
      settings_view_.on_panel_event(ev);
      break;
    default:
      break;
  }
}

void NavigationController::on_midi_note(uint8_t note_number) {
  part_view_.on_midi_note(note_number);
}

}  // namespace velora::ui
