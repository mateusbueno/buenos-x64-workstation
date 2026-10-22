#include "velora/ui/screens/effects_view.hpp"
#include "velora/ui/ws_client.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

#include <cstdio>

namespace velora::ui {

static std::string effect_type_name(ws::v1::InsertType type) {
  switch (type) {
    case ws::v1::INSERT_TYPE_ROTARY: return tr("effects.insert.rotary");
    case ws::v1::INSERT_TYPE_CHORUS_PHASER: return tr("effects.insert.chorus_phaser");
    case ws::v1::INSERT_TYPE_EP_AMP: return tr("effects.insert.ep_amp");
    case ws::v1::INSERT_TYPE_EQ: return tr("effects.insert.eq");
    case ws::v1::INSERT_TYPE_COMPRESSOR: return tr("effects.insert.compressor");
    default: return tr("effects.title");
  }
}

EffectsView::EffectsView(WsClient* client) : client_(client) {}

EffectsView::~EffectsView() {
  view_container_ = nullptr;
  row_items_.clear();
  bypass_controls_.clear();
}

bool EffectsView::create(lv_obj_t* parent) {
  if (!skeleton_.create(parent)) return false;

  skeleton_.set_title(tr("effects.title"));

  std::vector<HintItem> hints = {
    {"VALUE", tr("hint.verb.scroll")},
    {"WRITE", tr("hint.verb.select")},
    {"EXIT", tr("hint.verb.back")}
  };
  skeleton_.set_hints(hints, tr("shell.hint.ready"));

  const auto& sp = Theme::spacing();

  // Part selector at top of content area: P1..P8
  std::vector<std::string> part_labels = {"P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"};
  if (!part_selector_.create(skeleton_.content_area(), part_labels, 110)) {
    return false;
  }
  lv_obj_set_pos(part_selector_.container(), sp.space_5, sp.space_3);
  part_selector_.set_on_change([this](int32_t idx) {
    select_part(static_cast<uint32_t>(idx));
  });

  // Main container for subviews (chain list, picker, or param editor)
  view_container_ = lv_obj_create(skeleton_.content_area());
  lv_obj_set_size(view_container_, 992, 400);
  lv_obj_set_pos(view_container_, sp.space_5, 75);
  lv_obj_set_scrollable(view_container_, false);
  lv_obj_set_style_bg_opa(view_container_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(view_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(view_container_, 0, LV_PART_MAIN);

  if (client_) {
    client_->set_state_callback([this](const ws::v1::State& state) {
      update_from_state(state);
    });
    client_->set_error_callback([this](const ws::v1::Error& error) {
      handle_error(error);
    });
  }

  render_current_mode();
  return true;
}

void EffectsView::update_from_state(const ws::v1::State& state) {
  current_state_ = state;
  render_current_mode();
}

void EffectsView::handle_error(const ws::v1::Error& error) {
  if (error.code() == ws::v1::ERROR_CODE_FX_BUDGET_EXCEEDED) {
    skeleton_.hint_bar().set_right_text(tr("effects.error.budget_exceeded"));
  } else if (!error.message().empty()) {
    skeleton_.hint_bar().set_right_text(error.message());
  }
}

void EffectsView::on_panel_event(const PanelEvent& ev) {
  if (ev.type == PanelEventType::Button && ev.pressed) {
    if (ev.id == MCU_BTN_EXIT) {
      if (mode_ != EffectsViewMode::ChainList) {
        close_subview();
      } else if (on_close_) {
        on_close_();
      }
      return;
    }
  }

  if (ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
    if (mode_ == EffectsViewMode::ChainList) {
      part_selector_.on_value_turn(ev.delta);
    }
  }
}

const ws::v1::PartState* EffectsView::get_part_state(uint32_t part) const {
  for (const auto& p : current_state_.parts()) {
    if (p.part() == part) {
      return &p;
    }
  }
  return nullptr;
}

size_t EffectsView::slot_count() const {
  const auto* ps = get_part_state(selected_part_);
  return ps ? static_cast<size_t>(ps->inserts_size()) : 0;
}

void EffectsView::select_part(uint32_t part) {
  if (part > 7) return;
  selected_part_ = part;
  part_selector_.set_selected_index(static_cast<int32_t>(part));
  mode_ = EffectsViewMode::ChainList;
  render_current_mode();
}

void EffectsView::add_insert(ws::v1::InsertType type) {
  auto* ps = const_cast<ws::v1::PartState*>(get_part_state(selected_part_));
  if (!ps) {
    // If state doesn't have this part yet, create parts
    while (current_state_.parts_size() <= static_cast<int>(selected_part_)) {
      auto* new_part = current_state_.add_parts();
      new_part->set_part(static_cast<uint32_t>(current_state_.parts_size() - 1));
    }
    ps = current_state_.mutable_parts(selected_part_);
  }

  if (ps->inserts_size() >= 5) return;

  auto* slot = ps->add_inserts();
  slot->set_type(type);
  slot->set_bypass(false);
  slot->add_params(0.5f);
  slot->add_params(0.5f);
  slot->add_params(0.5f);
  slot->add_params(0.5f);

  send_current_chain();
  mode_ = EffectsViewMode::ChainList;
  render_current_mode();
}

void EffectsView::toggle_bypass(int32_t slot_idx) {
  auto* ps = const_cast<ws::v1::PartState*>(get_part_state(selected_part_));
  if (!ps || slot_idx < 0 || slot_idx >= ps->inserts_size()) return;

  auto* slot = ps->mutable_inserts(slot_idx);
  slot->set_bypass(!slot->bypass());

  send_current_chain();
  render_current_mode();
}

void EffectsView::remove_insert(int32_t slot_idx) {
  auto* ps = const_cast<ws::v1::PartState*>(get_part_state(selected_part_));
  if (!ps || slot_idx < 0 || slot_idx >= ps->inserts_size()) return;

  // Rebuild inserts without slot_idx
  std::vector<ws::v1::InsertSlot> kept;
  for (int i = 0; i < ps->inserts_size(); ++i) {
    if (i != slot_idx) {
      kept.push_back(ps->inserts(i));
    }
  }
  ps->clear_inserts();
  for (const auto& s : kept) {
    *ps->add_inserts() = s;
  }

  send_current_chain();
  render_current_mode();
}

void EffectsView::move_insert(int32_t slot_idx, int32_t new_idx) {
  auto* ps = const_cast<ws::v1::PartState*>(get_part_state(selected_part_));
  if (!ps || slot_idx < 0 || slot_idx >= ps->inserts_size()) return;
  if (new_idx < 0 || new_idx >= ps->inserts_size() || slot_idx == new_idx) return;

  std::vector<ws::v1::InsertSlot> slots;
  for (int i = 0; i < ps->inserts_size(); ++i) {
    slots.push_back(ps->inserts(i));
  }
  auto item = slots[slot_idx];
  slots.erase(slots.begin() + slot_idx);
  slots.insert(slots.begin() + new_idx, item);

  ps->clear_inserts();
  for (const auto& s : slots) {
    *ps->add_inserts() = s;
  }

  send_current_chain();
  render_current_mode();
}

void EffectsView::open_param_editor(int32_t slot_idx) {
  selected_slot_ = slot_idx;
  selected_param_idx_ = 0;
  mode_ = EffectsViewMode::ParamEditor;
  render_current_mode();
}

void EffectsView::close_subview() {
  mode_ = EffectsViewMode::ChainList;
  selected_slot_ = -1;
  render_current_mode();
}

void EffectsView::send_current_chain() {
  if (!client_) return;

  ws::v1::ClientMessage msg;
  msg.set_request_id(100);
  auto* cmd = msg.mutable_set_insert_chain();
  cmd->set_part(selected_part_);

  const auto* ps = get_part_state(selected_part_);
  if (ps) {
    for (const auto& s : ps->inserts()) {
      *cmd->add_slots() = s;
    }
  }

  client_->send_command(msg);
}

void EffectsView::render_current_mode() {
  if (!view_container_) return;
  lv_obj_clean(view_container_);
  row_items_.clear();
  bypass_controls_.clear();

  switch (mode_) {
    case EffectsViewMode::ChainList:
      render_chain_list();
      break;
    case EffectsViewMode::AddPicker:
      render_add_picker();
      break;
    case EffectsViewMode::ParamEditor:
      render_param_editor();
      break;
  }
}

void EffectsView::render_chain_list() {
  const auto* ps = get_part_state(selected_part_);
  int count = ps ? ps->inserts_size() : 0;
  const auto& sp = Theme::spacing();

  int32_t y = 0;
  for (int i = 0; i < count; ++i) {
    const auto& slot = ps->inserts(i);

    auto row = std::make_unique<ListRow>();
    row->create(view_container_, 992, 56);
    lv_obj_set_pos(row->container(), 0, y);

    char title_buf[64];
    std::snprintf(title_buf, sizeof(title_buf), "%d. %s", i + 1, effect_type_name(slot.type()).c_str());
    row->set_title(title_buf);

    std::string bypass_str = slot.bypass() ? tr("effects.bypass.bypassed") : tr("effects.bypass.active");
    row->set_meta(bypass_str);

    int slot_idx = i;
    row->set_on_select([this, slot_idx](ListRow*) {
      open_param_editor(slot_idx);
    });

    row_items_.push_back(std::move(row));
    y += 56 + sp.space_2;
  }

  // Add Effect row if slots < 5
  if (count < 5) {
    auto add_row = std::make_unique<ListRow>();
    add_row->create(view_container_, 992, 56);
    lv_obj_set_pos(add_row->container(), 0, y);
    add_row->set_title(tr("effects.add_effect"));
    add_row->set_on_select([this](ListRow*) {
      mode_ = EffectsViewMode::AddPicker;
      render_current_mode();
    });
    row_items_.push_back(std::move(add_row));
  }
}

void EffectsView::render_add_picker() {
  const auto& sp = Theme::spacing();
  std::vector<ws::v1::InsertType> types = {
    ws::v1::INSERT_TYPE_ROTARY,
    ws::v1::INSERT_TYPE_CHORUS_PHASER,
    ws::v1::INSERT_TYPE_EP_AMP,
    ws::v1::INSERT_TYPE_EQ,
    ws::v1::INSERT_TYPE_COMPRESSOR
  };

  int32_t y = 0;
  for (auto t : types) {
    auto row = std::make_unique<ListRow>();
    row->create(view_container_, 992, 56);
    lv_obj_set_pos(row->container(), 0, y);
    row->set_title(effect_type_name(t));

    row->set_on_select([this, t](ListRow*) {
      add_insert(t);
    });

    row_items_.push_back(std::move(row));
    y += 56 + sp.space_2;
  }
}

void EffectsView::render_param_editor() {
  const auto* ps = get_part_state(selected_part_);
  if (!ps || selected_slot_ < 0 || selected_slot_ >= ps->inserts_size()) {
    mode_ = EffectsViewMode::ChainList;
    render_chain_list();
    return;
  }

  const auto& slot = ps->inserts(selected_slot_);
  const auto& sp = Theme::spacing();

  // Bypass toggle row at top of parameter editor
  auto bypass_row = std::make_unique<ListRow>();
  bypass_row->create(view_container_, 992, 56);
  lv_obj_set_pos(bypass_row->container(), 0, 0);
  bypass_row->set_title(effect_type_name(slot.type()));
  bypass_row->set_meta(slot.bypass() ? tr("effects.bypass.bypassed") : tr("effects.bypass.active"));
  bypass_row->set_on_select([this](ListRow*) {
    toggle_bypass(selected_slot_);
  });
  row_items_.push_back(std::move(bypass_row));

  // Parameters list
  int32_t y = 56 + sp.space_2;
  for (int p_idx = 0; p_idx < slot.params_size(); ++p_idx) {
    auto p_row = std::make_unique<ListRow>();
    p_row->create(view_container_, 992, 56);
    lv_obj_set_pos(p_row->container(), 0, y);

    char name_buf[32];
    std::snprintf(name_buf, sizeof(name_buf), "Param %d", p_idx + 1);
    p_row->set_title(name_buf);

    char val_buf[32];
    std::snprintf(val_buf, sizeof(val_buf), "%.2f", slot.params(p_idx));
    p_row->set_meta(val_buf);

    row_items_.push_back(std::move(p_row));
    y += 56 + sp.space_2;
  }
}

}  // namespace velora::ui
