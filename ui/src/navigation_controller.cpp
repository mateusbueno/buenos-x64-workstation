#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/ws_client.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/font_manager.hpp"

namespace velora::ui {

NavigationController::NavigationController(WsClient* client)
    : client_(client),
      performance_view_(client),
      setlist_view_(client),
      setlist_browser_view_(client),
      scenes_view_(client),
      part_view_(client),
      settings_view_(client),
      library_view_(client),
      import_view_(client),
      boot_view_(client) {
  // Story 3.12: LED commands go to the engine over ws.v1 only (the engine writes the panel link). Not delivered (no connection) = dirty until the next resend.
  connected_fn_ = [this]() { return client_ && client_->is_connected(); };
  command_fn_ = [this](const ws::v1::ClientMessage& m) { return client_ && client_->send_command(m); };
  led_driver_.set_send([this](uint8_t id, uint8_t brightness, uint8_t arg) {
    if (!connected_fn_ || !connected_fn_()) return false;
    ws::v1::ClientMessage msg;
    msg.set_request_id(103);
    ws::v1::SetLed* sl = msg.mutable_set_led();
    sl->set_led_id(id);
    sl->set_brightness(brightness);
    sl->set_arg(arg);
    return command_fn_ && command_fn_(msg);
  });
  // The one publication point of the toggle state: every change lights the active state's LED only.
  performance_view_.toggles().set_listener([this]() { sync_leds(); });
}

NavigationController::~NavigationController() = default;

namespace {
constexpr uint8_t kLedPart[8] = {MCU_LED_PART_SCENE_LED_1, MCU_LED_PART_SCENE_LED_2, MCU_LED_PART_SCENE_LED_3, MCU_LED_PART_SCENE_LED_4,
                                 MCU_LED_PART_SCENE_LED_5, MCU_LED_PART_SCENE_LED_6, MCU_LED_PART_SCENE_LED_7, MCU_LED_PART_SCENE_LED_8};
constexpr uint8_t kToggleLeds[7] = {MCU_LED_TOGGLE_LED_DRAWBARS, MCU_LED_TOGGLE_LED_MIXER, MCU_LED_TOGGLE_LED_PARTS, MCU_LED_TOGGLE_LED_SCENES,
                                    MCU_LED_TOGGLE_LED_PART,     MCU_LED_TOGGLE_LED_ALL,   MCU_LED_TOGGLE_LED_ASSIGN};
constexpr uint8_t kLedWeak = 64;
}  // namespace

// Story 3.12: derives every LED the ui owns from the state (toggle state, part / scene state) and gives it to the LED driver. Idempotent: the driver only sends
// a change. Exactly one LED of each toggle is lit.
void NavigationController::sync_leds() {
  led_driver_.tick(now_ms_);  // the driver's clock follows the ui's, so a command made now is stamped now
  const ToggleState& t = performance_view_.toggles();
  for (uint8_t id : kToggleLeds) led_driver_.set(id, t.led_lit(id) ? LedDriver::kOn : 0);
  for (size_t i = 0; i < 8; ++i) {
    LedLevel lv = performance_view_.button_led_level(i);
    // The recall dip is a pattern on top of the steady state: the steady state of the current scene stays lit.
    if (t.scenes() && static_cast<int32_t>(i) == performance_view_.current_scene() && performance_view_.is_scene_led_dipping()) lv = LedLevel::Strong;
    led_driver_.set(kLedPart[i], lv == LedLevel::Strong ? LedDriver::kOn : (lv == LedLevel::Weak ? kLedWeak : 0));
  }
  // Patterns start on the edge of the view's flags (a scene recalled from the part/scene buttons; DRAWBARS pressed with no organ part).
  const bool dip = performance_view_.is_scene_led_dipping();
  if (dip && !prev_scene_dip_) {
    const int32_t cur = performance_view_.current_scene();
    if (cur >= 0 && cur < 8) led_driver_.dip(kLedPart[cur]);
  }
  prev_scene_dip_ = dip;
  const uint32_t refused = performance_view_.drawbars_refused_count();
  if (refused != prev_drawbars_refused_) led_driver_.blink_twice(MCU_LED_TOGGLE_LED_DRAWBARS);
  prev_drawbars_refused_ = refused;
  if (led_view_enabled_) led_view_.update(led_driver_);
}

bool NavigationController::enable_led_view() {
  if (!led_view_.created() && !led_view_.create()) return false;
  led_view_enabled_ = true;
  led_view_.update(led_driver_);
  return true;
}

void NavigationController::resend_leds() {
  sync_leds();
  led_driver_.resend_all();
}

bool NavigationController::initialize(lv_obj_t* parent) {
  if (!parent) {
    parent = lv_screen_active();
  }
  if (!parent) return false;

  FontManager::instance().initialize();
  FontManager::instance().apply_to_screen(parent);

  if (!performance_view_.create(parent)) return false;
  if (!launcher_view_.create(parent)) return false;
  if (!effects_view_.create(parent)) return false;
  if (!setlist_view_.create(parent)) return false;
  if (!setlist_browser_view_.create(parent)) return false;
  if (!scenes_view_.create(parent)) return false;
  if (!part_view_.create(parent)) return false;
  if (!settings_view_.create(parent)) return false;
  if (!library_view_.create(parent)) return false;
  if (!import_view_.create(parent)) return false;
  if (!boot_view_.create(parent)) return false;
  if (!notice_toast_.create(parent)) return false;
  if (!write_dialog_.create(parent)) return false;

  boot_view_.set_on_ready([this]() {
    navigate_to(SurfaceId::Performance);
  });

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

  performance_view_.set_knob_target_select_callback([this](int32_t part_idx) {
    performance_view_.select_knob_part(part_idx);
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

  part_view_.set_on_timbre_change([this](int32_t part_idx, const std::string& timbre_id) {
    set_pending_edits(true, true, "Part " + std::to_string(part_idx + 1) + " timbre");
  });

  part_view_.set_on_edit([this](int32_t part_idx) {
    set_pending_edits(true, false, "Part " + std::to_string(part_idx + 1) + " edit");
  });

  part_view_.set_on_arp_edit([this](int32_t part_idx, const ArpSettings& settings) {
    set_pending_edits(true, false, "Part " + std::to_string(part_idx + 1) + " arpeggiator");
  });

  part_view_.set_on_tempo_edit([this](float bpm) {
    performance_view_.set_tempo(bpm);
    set_pending_edits(true, false, "Tempo");
  });

  part_view_.set_on_open_inserts([this](int32_t part_idx) {
    surface_before_launcher_ = current_surface_;
    navigate_to(SurfaceId::Effects);
  });

  part_view_.set_on_open_library([this](int32_t part_idx) {
    surface_before_launcher_ = current_surface_;
    library_view_.set_mode(LibraryMode::ChooseForPart, part_idx);
    navigate_to(SurfaceId::Library);
  });

  library_view_.set_on_close([this]() {
    navigate_to(surface_before_launcher_);
  });

  library_view_.set_on_timbre_selected([this](int32_t part_idx, const std::string& timbre_id) {
    part_view_.set_timbre(timbre_id, "");
    set_pending_edits(true, true, "Part " + std::to_string(part_idx + 1) + " timbre");
  });

  import_view_.set_on_close([this]() {
    navigate_to(surface_before_launcher_);
  });

  import_view_.set_on_import_finished([this](uint32_t imp, uint32_t skp) {
    char buf[128];
    snprintf(buf, sizeof(buf), tr("notice.import_finished").c_str(), imp, skp);
    notice_toast_.show(buf, "");
  });

  notice_toast_.set_on_action([this]() {
    if (pending_sysex_ && pending_sysex_->ok()) {
      std::vector<ImportItem> items;
      for (const auto& v : pending_sysex_->voices) {
        ImportItem item;
        item.file_name = v.name;
        item.size_bytes = 155;
        item.format_str = "Import 6-OP SysEx";
        item.category = format::Category::kSynthPoly;
        item.status = ImportItemStatus::Ready;
        items.push_back(std::move(item));
      }
      import_view_.set_drive_info("MIDI IN", pending_sysex_->voices.size() * 155, 50ULL * 1024 * 1024 * 1024);
      import_view_.set_items(std::move(items));
      pending_sysex_.reset();
    }
    surface_before_launcher_ = current_surface_;
    navigate_to(SurfaceId::Import);
  });

  notice_toast_.set_on_dismiss([this]() {
    pending_sysex_.reset();
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

  settings_view_.set_on_config_changed([this](const SystemConfig& c) {
    performance_view_.set_touch_mode(c.touch_mode);
    performance_view_.set_long_press_time_ms(c.long_press_ms);
    set_write_hold_ms(c.write_hold_ms);
    set_panel_guard(c.panel_guard);
    boot_view_.set_reduced_motion(c.reduced_motion);
    performance_view_.set_large_text(c.large_text);
    Theme::set_theme((c.theme == "studio") ? ThemeId::Studio : ThemeId::Stage);
    if (c.language != applied_language_) {
      applied_language_ = c.language;
      if (on_language_changed_) on_language_changed_();
    }
  });

  // Apply current system config
  SystemConfig::instance().load_from_file();
  const auto& init_cfg = SystemConfig::instance();
  applied_language_ = init_cfg.language;
  performance_view_.set_touch_mode(init_cfg.touch_mode);
  performance_view_.set_long_press_time_ms(init_cfg.long_press_ms);
  set_write_hold_ms(init_cfg.write_hold_ms);
  set_panel_guard(init_cfg.panel_guard);
  boot_view_.set_reduced_motion(init_cfg.reduced_motion);
  performance_view_.set_large_text(init_cfg.large_text);
  Theme::set_theme((init_cfg.theme == "studio") ? ThemeId::Studio : ThemeId::Stage);

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
      case LauncherTileId::Library:
        navigate_to(SurfaceId::Library);
        break;
      case LauncherTileId::Import:
        navigate_to(SurfaceId::Import);
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
  hide_surface(SurfaceId::Library);
  hide_surface(SurfaceId::Import);
  hide_surface(SurfaceId::Boot);
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
    case SurfaceId::Library:
      root = library_view_.skeleton().root_container();
      break;
    case SurfaceId::Import:
      root = import_view_.skeleton().root_container();
      break;
    case SurfaceId::Boot:
      root = boot_view_.root();
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
    case SurfaceId::Library:
      root = library_view_.skeleton().root_container();
      break;
    case SurfaceId::Import:
      root = import_view_.skeleton().root_container();
      break;
    case SurfaceId::Boot:
      root = boot_view_.root();
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
  if (!pending) {
    has_timbre_edit_ = false;
  } else if (has_timbre) {
    has_timbre_edit_ = true;
  }
  change_summary_text_ = summary;
  performance_view_.set_live_set(performance_view_.live_set_name(), 0, pending);
}

void NavigationController::update_from_state(const ws::v1::State& state) {
  if (current_surface_ == SurfaceId::Boot) {
    if (state.loading().progress() > 0.0f) {
      boot_view_.set_progress(state.loading().progress());
    }
    if (!state.live_set_name().empty()) {
      boot_view_.set_status(state.live_set_name(), state.current_scene() + 1);
    }
    if (state.live_set_loaded()) {
      boot_view_.set_ready(true);
    }
  }

  performance_view_.update_from_state(state);
  sync_leds();
  setlist_view_.update_from_state(state);
  setlist_browser_view_.update_from_state(state);
  scenes_view_.update_from_state(state);
  settings_view_.update_from_state(state);
  part_view_.update_from_state(state);

  if (state.has_edits()) {
    manual_pending_edits_ = state.edits().current_dirty();
    has_timbre_edit_ = state.edits().has_timbre_edit();
    change_summary_text_ = state.edits().change_summary_text();
  }
}

void NavigationController::on_panel_event(const PanelEvent& ev) {
  on_panel_event_impl(ev);
  sync_leds();
}

void NavigationController::on_panel_event_impl(const PanelEvent& ev) {
  // If WriteDialog is open, it captures all panel events
  if (write_dialog_.is_open()) {
    write_dialog_.on_panel_event(ev);
    return;
  }

  // Story 4.36: Footswitch (MCU_CTRL_PEDAL_SWITCH)
  if (ev.id == MCU_CTRL_PEDAL_SWITCH) {
    const bool pressed = (ev.type == PanelEventType::Button && ev.pressed) ||
                         (ev.type == PanelEventType::Fader && ev.delta != 0);
    if (pressed) {
      handle_footswitch(ev.timestamp_us);
    }
    return;
  }

  // Handle WRITE hold vs short press:
  // On Performance and Scenes, WRITE hold (0.5s) triggers WriteDialog / Save Scene.
  // On other surfaces (Launcher, Library, Import, Settings, Part, SetlistBrowser), WRITE is immediate confirm on press!
  if (ev.type == PanelEventType::Button && ev.id == MCU_BTN_WRITE) {
    if (current_surface_ == SurfaceId::Performance || current_surface_ == SurfaceId::Scenes) {
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
        if (current_surface_ == SurfaceId::Performance) {
          show_nothing_to_write_hint();
        } else if (current_surface_ == SurfaceId::Scenes) {
          scenes_view_.on_panel_event(short_press);
        }
        return;
      }
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

  // 1. Global Stepping: LIVE SET ◀ ▶ (with Panel Guard) and SCENE ◀ ▶ work on ANY surface
  if (ev.type == PanelEventType::Button && (ev.id == MCU_BTN_LIVE_SET_PREV || ev.id == MCU_BTN_LIVE_SET_NEXT)) {
    if (!panel_guard_) {
      if (ev.pressed) {
        live_set_step_count_ += (ev.id == MCU_BTN_LIVE_SET_NEXT) ? 1 : -1;
        if (client_) {
          ws::v1::ClientMessage msg;
          msg.set_request_id(101);
          if (ev.id == MCU_BTN_LIVE_SET_PREV) msg.mutable_previous_live_set();
          else msg.mutable_next_live_set();
          client_->send_command(msg);
        }
      }
      return;
    } else {
      // Panel guard = On: brief press does nothing, 300 ms hold steps
      if (ev.pressed) {
        live_set_btn_down_time_us_ = (ev.timestamp_us > 0) ? ev.timestamp_us : 1;
        live_set_btn_pending_id_ = ev.id;
        return;
      } else {
        if (live_set_btn_pending_id_ == ev.id) {
          uint64_t hold_time = (ev.timestamp_us > live_set_btn_down_time_us_ && live_set_btn_down_time_us_ > 0)
                                   ? (ev.timestamp_us - live_set_btn_down_time_us_)
                                   : 0;
          live_set_btn_down_time_us_ = 0;
          live_set_btn_pending_id_ = 0;
          if (hold_time >= 300000) {  // 300 ms in microseconds
            live_set_step_count_ += (ev.id == MCU_BTN_LIVE_SET_NEXT) ? 1 : -1;
            if (client_) {
              ws::v1::ClientMessage msg;
              msg.set_request_id(101);
              if (ev.id == MCU_BTN_LIVE_SET_PREV) msg.mutable_previous_live_set();
              else msg.mutable_next_live_set();
              client_->send_command(msg);
            }
          }
        }
        return;
      }
    }
  }

  if (ev.type == PanelEventType::Button && ev.pressed) {
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
  }

  // Quick path: hold EXIT and turn VALUE from ANY surface
  if (exit_btn_down_ && ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
    exit_turned_value_ = true;
    auto& cfg = SystemConfig::instance();
    int32_t b = static_cast<int32_t>(cfg.screen_brightness) + ev.delta * 5;
    b = std::clamp(b, 5, 100);
    cfg.screen_brightness = static_cast<uint32_t>(b);
    cfg.save_to_file();

    if (!performance_view_.is_assign_overlay_visible()) {
      const std::string& loc = Locale::instance().current_locale();
      std::string pct_str = (loc == "pt-BR") ? (std::to_string(b) + "%") : (std::to_string(b) + " %");
      char toast_msg[64];
      std::snprintf(toast_msg, sizeof(toast_msg), tr("notice.brightness").c_str(), pct_str.c_str());
      notice_toast_.show(toast_msg);
    }
    return;
  }

  // 3. EXIT Button Navigation Rules: Press arms quick path, release executes back navigation (or cancels if turned VALUE)
  if (ev.type == PanelEventType::Button && ev.id == MCU_BTN_EXIT) {
    if (ev.pressed) {
      exit_btn_down_ = true;
      exit_turned_value_ = false;
      return;
    } else {
      exit_btn_down_ = false;
      if (exit_turned_value_) {
        exit_turned_value_ = false;
        return; // Quick path cancelled back navigation
      }
      if (notice_toast_.is_visible()) {
        notice_toast_.hide();
        pending_sysex_.reset();
        return;
      }
      PanelEvent press_ev = ev;
      press_ev.pressed = true;

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
          effects_view_.on_panel_event(press_ev);
          return;
        }
        // Otherwise return to the surface Launcher was opened from
        navigate_to(surface_before_launcher_);
        return;
      }
      if (current_surface_ == SurfaceId::Setlist) {
        if (setlist_view_.keyboard().is_open()) {
          setlist_view_.on_panel_event(press_ev);
          return;
        }
        navigate_to(surface_before_launcher_);
        return;
      }
      if (current_surface_ == SurfaceId::SetlistBrowser) {
        if (setlist_browser_view_.action_sheet().is_open() || setlist_browser_view_.keyboard().is_open()) {
          setlist_browser_view_.on_panel_event(press_ev);
          return;
        }
        navigate_to(surface_before_launcher_);
        return;
      }
      if (current_surface_ == SurfaceId::Scenes) {
        if (scenes_view_.keyboard().is_open()) {
          scenes_view_.on_panel_event(press_ev);
          return;
        }
        navigate_to(surface_before_launcher_);
        return;
      }
      if (current_surface_ == SurfaceId::Part) {
        if (part_view_.is_timbre_picker_open()) {
          part_view_.on_panel_event(press_ev);
          return;
        }
        if (part_view_.handle_exit_step()) {
          return;
        }
        navigate_to(surface_before_launcher_);
        return;
      }
      if (current_surface_ == SurfaceId::Settings) {
        if (settings_view_.is_picker_open()) {
          settings_view_.on_panel_event(press_ev);
          return;
        }
        navigate_to(surface_before_launcher_);
        return;
      }
      if (current_surface_ == SurfaceId::Library) {
        navigate_to(surface_before_launcher_);
        return;
      }
      if (current_surface_ == SurfaceId::Import) {
        if (import_view_.category_picker().is_visible()) {
          import_view_.on_panel_event(press_ev);
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
    case SurfaceId::Library:
      library_view_.on_panel_event(ev);
      break;
    case SurfaceId::Import:
      import_view_.on_panel_event(ev);
      break;
    default:
      break;
  }
}

void NavigationController::on_midi_note(uint8_t note_number) {
  if (current_surface_ == SurfaceId::Library) {
    library_view_.on_midi_note(note_number);
  }
  part_view_.on_midi_note(note_number);
}

void NavigationController::on_usb_drive_inserted(const std::string& drive_name, const std::vector<ImportItem>& items) {
  uint64_t total_size = 0;
  for (const auto& it : items) total_size += it.size_bytes;
  import_view_.set_drive_info(drive_name, total_size, 50ULL * 1024 * 1024 * 1024);
  import_view_.set_items(items);
  notice_toast_.show(tr("notice.usb_drive_found"), tr("notice.action.import"), 8000);
}

void NavigationController::on_sysex_dump_received(const format::SysexParseResult& result) {
  if (!result.ok() || result.voices.empty()) {
    on_sysex_damaged();
    return;
  }
  pending_sysex_ = result;
  const std::string msg = "6-OP SysEx received: " + std::to_string(result.voices.size()) + " voices";
  notice_toast_.show(msg, tr("notice.action.import"), 8000);
}

void NavigationController::on_sysex_damaged() {
  pending_sysex_.reset();
  notice_toast_.show(tr("notice.sysex_damaged"), "");
}

void NavigationController::tick(uint32_t elapsed_ms) {
  now_ms_ += elapsed_ms;
  performance_view_.tick(elapsed_ms);
  sync_leds();  // also advances the LED driver's patterns and deferred edges to now
  // A (re)connection to the engine (or a freshly started ui meeting it): the full LED state again. The engine also replays its own copy to a (re)connected MCU.
  const bool connected = connected_fn_ && connected_fn_();
  if (connected && !was_connected_) led_driver_.resend_all();
  was_connected_ = connected;
  notice_toast_.tick(elapsed_ms);
  import_view_.tick(elapsed_ms);
  boot_view_.tick(elapsed_ms);
}

void NavigationController::handle_footswitch(uint64_t ts) {
  const auto fn = SystemConfig::instance().footswitch_function;
  switch (fn) {
    case FootswitchFunction::NextScene: {
      scene_step_count_ += 1;
      PanelEvent fwd = PanelEvent::button_press(MCU_BTN_SCENE_NEXT, ts);
      performance_view_.on_panel_event(fwd);
      break;
    }
    case FootswitchFunction::PreviousScene: {
      scene_step_count_ -= 1;
      PanelEvent fwd = PanelEvent::button_press(MCU_BTN_SCENE_PREV, ts);
      performance_view_.on_panel_event(fwd);
      break;
    }
    case FootswitchFunction::NextLiveSet: {
      live_set_step_count_ += 1;
      if (client_) {
        ws::v1::ClientMessage msg;
        msg.set_request_id(101);
        msg.mutable_next_live_set();
        client_->send_command(msg);
      }
      break;
    }
    case FootswitchFunction::RotarySpeed: {
      PanelEvent fwd = PanelEvent::button_press(MCU_BTN_ROTARY_SF, ts);
      performance_view_.on_panel_event(fwd);
      break;
    }
  }
}

}  // namespace velora::ui
