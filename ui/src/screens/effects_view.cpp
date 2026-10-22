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
  output_controls_.clear();
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

  // Top Section Selector: INSERTS, SENDS, MASTER, OUTPUTS
  std::vector<std::string> sec_labels = {
    tr("effects.section.inserts"),
    tr("effects.section.sends"),
    tr("effects.section.master"),
    tr("effects.section.outputs")
  };
  if (!section_selector_.create(skeleton_.content_area(), sec_labels, 120)) {
    return false;
  }
  lv_obj_set_pos(section_selector_.container(), sp.space_5, sp.space_2);
  section_selector_.set_on_change([this](int32_t idx) {
    select_section(static_cast<EffectsSection>(idx));
  });

  // Part selector for Inserts: P1..P8
  std::vector<std::string> part_labels = {"P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"};
  if (!part_selector_.create(skeleton_.content_area(), part_labels, 90)) {
    return false;
  }
  lv_obj_set_pos(part_selector_.container(), sp.space_5, 68);
  part_selector_.set_on_change([this](int32_t idx) {
    select_part(static_cast<uint32_t>(idx));
  });

  // Main container for subviews
  view_container_ = lv_obj_create(skeleton_.content_area());
  lv_obj_set_size(view_container_, 992, 345);
  lv_obj_set_pos(view_container_, sp.space_5, 130);
  lv_obj_set_scrollable(view_container_, true);
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

void EffectsView::select_section(EffectsSection sec) {
  section_ = sec;
  section_selector_.set_selected_index(static_cast<int32_t>(sec));
  mode_ = EffectsViewMode::ChainList;

  // Show part selector only in Inserts section
  if (part_selector_.container()) {
    lv_obj_set_hidden(part_selector_.container(), sec != EffectsSection::Inserts);
  }

  // Adjust view_container pos and height based on section
  const auto& sp = Theme::spacing();
  if (sec == EffectsSection::Inserts) {
    lv_obj_set_pos(view_container_, sp.space_5, 130);
    lv_obj_set_size(view_container_, 992, 345);
  } else {
    lv_obj_set_pos(view_container_, sp.space_5, 68);
    lv_obj_set_size(view_container_, 992, 407);
  }

  render_current_mode();
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
    if (section_ == EffectsSection::Inserts && mode_ == EffectsViewMode::ChainList) {
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

void EffectsView::set_part_send_levels(uint32_t part, float reverb, float delay) {
  if (part > 7) return;

  auto* ps = const_cast<ws::v1::PartState*>(get_part_state(part));
  if (!ps) {
    while (current_state_.parts_size() <= static_cast<int>(part)) {
      auto* np = current_state_.add_parts();
      np->set_part(static_cast<uint32_t>(current_state_.parts_size() - 1));
    }
    ps = current_state_.mutable_parts(part);
  }
  ps->set_send_reverb(reverb);
  ps->set_send_delay(delay);

  if (client_) {
    ws::v1::ClientMessage msg;
    msg.set_request_id(101);
    auto* cmd = msg.mutable_set_send_levels();
    cmd->set_part(part);
    cmd->set_reverb(reverb);
    cmd->set_delay(delay);
    client_->send_command(msg);
  }
  render_current_mode();
}

void EffectsView::set_part_output(uint32_t part, ws::v1::PartOutput out) {
  if (part > 7) return;

  auto* ps = const_cast<ws::v1::PartState*>(get_part_state(part));
  if (!ps) {
    while (current_state_.parts_size() <= static_cast<int>(part)) {
      auto* np = current_state_.add_parts();
      np->set_part(static_cast<uint32_t>(current_state_.parts_size() - 1));
    }
    ps = current_state_.mutable_parts(part);
  }
  ps->set_output(out);

  if (client_) {
    ws::v1::ClientMessage msg;
    msg.set_request_id(102);
    auto* cmd = msg.mutable_set_part_output();
    cmd->set_part(part);
    cmd->set_output(out);
    client_->send_command(msg);
  }
  render_current_mode();
}

void EffectsView::set_master_eq(float low_db, float mid_gain_db, float mid_freq_hz, float high_db, bool bypass) {
  (void)low_db; (void)mid_gain_db; (void)mid_freq_hz; (void)high_db; (void)bypass;
  if (client_) {
    ws::v1::ClientMessage msg;
    msg.set_request_id(103);
    auto* cmd = msg.mutable_set_master_params();
    auto* v1 = cmd->add_values();
    v1->set_param_id(ws::v1::PARAM_MASTER_EQ_LOW_GAIN);
    v1->set_value(low_db);
    auto* v2 = cmd->add_values();
    v2->set_param_id(ws::v1::PARAM_MASTER_EQ_MID_GAIN);
    v2->set_value(mid_gain_db);
    auto* v3 = cmd->add_values();
    v3->set_param_id(ws::v1::PARAM_MASTER_EQ_MID_FREQ);
    v3->set_value(mid_freq_hz);
    auto* v4 = cmd->add_values();
    v4->set_param_id(ws::v1::PARAM_MASTER_EQ_HIGH_GAIN);
    v4->set_value(high_db);
    auto* v5 = cmd->add_values();
    v5->set_param_id(ws::v1::PARAM_MASTER_EQ_BYPASS);
    v5->set_value(bypass ? 1.0f : 0.0f);
    client_->send_command(msg);
  }
}

void EffectsView::set_master_comp(float thresh_db, float ratio, float attack_ms, float release_ms, float makeup_db, bool bypass) {
  (void)thresh_db; (void)ratio; (void)attack_ms; (void)release_ms; (void)makeup_db; (void)bypass;
  if (client_) {
    ws::v1::ClientMessage msg;
    msg.set_request_id(104);
    auto* cmd = msg.mutable_set_master_params();
    auto* v1 = cmd->add_values();
    v1->set_param_id(ws::v1::PARAM_MASTER_COMP_THRESHOLD);
    v1->set_value(thresh_db);
    auto* v2 = cmd->add_values();
    v2->set_param_id(ws::v1::PARAM_MASTER_COMP_RATIO);
    v2->set_value(ratio);
    auto* v3 = cmd->add_values();
    v3->set_param_id(ws::v1::PARAM_MASTER_COMP_ATTACK);
    v3->set_value(attack_ms);
    auto* v4 = cmd->add_values();
    v4->set_param_id(ws::v1::PARAM_MASTER_COMP_RELEASE);
    v4->set_value(release_ms);
    auto* v5 = cmd->add_values();
    v5->set_param_id(ws::v1::PARAM_MASTER_COMP_MAKEUP);
    v5->set_value(makeup_db);
    auto* v6 = cmd->add_values();
    v6->set_param_id(ws::v1::PARAM_MASTER_COMP_BYPASS);
    v6->set_value(bypass ? 1.0f : 0.0f);
    client_->send_command(msg);
  }
}

void EffectsView::set_line_in_level(float level) {
  if (client_) {
    ws::v1::ClientMessage msg;
    msg.set_request_id(105);
    auto* cmd = msg.mutable_set_performance_param();
    cmd->set_param_id(ws::v1::PARAM_LINE_IN_LEVEL);
    cmd->set_value(level);
    client_->send_command(msg);
  }
}

void EffectsView::render_current_mode() {
  if (!view_container_) return;
  lv_obj_clean(view_container_);
  row_items_.clear();
  bypass_controls_.clear();
  output_controls_.clear();

  switch (section_) {
    case EffectsSection::Inserts:
      render_inserts_section();
      break;
    case EffectsSection::Sends:
      render_sends_section();
      break;
    case EffectsSection::Master:
      render_master_section();
      break;
    case EffectsSection::Outputs:
      render_outputs_section();
      break;
  }
}

void EffectsView::render_inserts_section() {
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

  auto bypass_row = std::make_unique<ListRow>();
  bypass_row->create(view_container_, 992, 56);
  lv_obj_set_pos(bypass_row->container(), 0, 0);
  bypass_row->set_title(effect_type_name(slot.type()));
  bypass_row->set_meta(slot.bypass() ? tr("effects.bypass.bypassed") : tr("effects.bypass.active"));
  bypass_row->set_on_select([this](ListRow*) {
    toggle_bypass(selected_slot_);
  });
  row_items_.push_back(std::move(bypass_row));

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

void EffectsView::render_sends_section() {
  const auto& sp = Theme::spacing();

  // Reverb master row
  auto rev_row = std::make_unique<ListRow>();
  rev_row->create(view_container_, 992, 56);
  lv_obj_set_pos(rev_row->container(), 0, 0);
  rev_row->set_title(tr("effects.reverb"));
  rev_row->set_meta("Hall · 2.0s · 100%");
  row_items_.push_back(std::move(rev_row));

  // Delay master row
  auto dly_row = std::make_unique<ListRow>();
  dly_row->create(view_container_, 992, 56);
  lv_obj_set_pos(dly_row->container(), 0, 56 + sp.space_2);
  dly_row->set_title(tr("effects.delay"));
  dly_row->set_meta("350ms · 35% FB · 100%");
  row_items_.push_back(std::move(dly_row));

  // Per-part send rows
  int32_t y = 112 + sp.space_2 * 2;
  for (uint32_t i = 0; i < 8; ++i) {
    auto row = std::make_unique<ListRow>();
    row->create(view_container_, 992, 56);
    lv_obj_set_pos(row->container(), 0, y);

    char title_buf[32];
    std::snprintf(title_buf, sizeof(title_buf), "Part %u", i + 1);
    row->set_title(title_buf);

    const auto* ps = get_part_state(i);
    float r = ps ? ps->send_reverb() : 0.0f;
    float d = ps ? ps->send_delay() : 0.0f;
    char meta_buf[64];
    std::snprintf(meta_buf, sizeof(meta_buf), "Rev: %.0f%% · Delay: %.0f%%", r * 100.0f, d * 100.0f);
    row->set_meta(meta_buf);

    uint32_t part_idx = i;
    row->set_on_select([this, part_idx, r, d](ListRow*) {
      float next_r = (r >= 0.9f) ? 0.0f : (r + 0.25f);
      set_part_send_levels(part_idx, next_r, d);
    });

    row_items_.push_back(std::move(row));
    y += 56 + sp.space_2;
  }
}

void EffectsView::render_master_section() {
  const auto& sp = Theme::spacing();

  // 1. Master EQ row
  auto eq_row = std::make_unique<ListRow>();
  eq_row->create(view_container_, 992, 56);
  lv_obj_set_pos(eq_row->container(), 0, 0);
  eq_row->set_title(tr("effects.master.eq"));
  eq_row->set_meta("Low 0dB · Mid 0dB (1.0 kHz) · High 0dB");
  row_items_.push_back(std::move(eq_row));

  // 2. Master Compressor row
  auto comp_row = std::make_unique<ListRow>();
  comp_row->create(view_container_, 992, 56);
  lv_obj_set_pos(comp_row->container(), 0, 56 + sp.space_2);
  comp_row->set_title(tr("effects.master.comp"));
  comp_row->set_meta("Thr -12dB · Ratio 2.0:1 · Makeup 0dB · GR: 0.0 dB");
  row_items_.push_back(std::move(comp_row));

  // 3. Line in level row
  auto line_row = std::make_unique<ListRow>();
  line_row->create(view_container_, 992, 56);
  lv_obj_set_pos(line_row->container(), 0, 112 + sp.space_2 * 2);
  line_row->set_title(tr("effects.master.line_in"));
  line_row->set_meta("0%");
  row_items_.push_back(std::move(line_row));
}

void EffectsView::render_outputs_section() {
  const auto& sp = Theme::spacing();
  int32_t y = 0;

  for (uint32_t i = 0; i < 8; ++i) {
    auto row = std::make_unique<ListRow>();
    row->create(view_container_, 992, 56);
    lv_obj_set_pos(row->container(), 0, y);

    char title_buf[32];
    std::snprintf(title_buf, sizeof(title_buf), "Part %u", i + 1);
    row->set_title(title_buf);

    const auto* ps = get_part_state(i);
    bool is_aux = (ps && ps->output() == ws::v1::PART_OUTPUT_AUX);
    row->set_meta(is_aux ? tr("effects.output.aux") : tr("effects.output.main"));

    uint32_t part_idx = i;
    row->set_on_select([this, part_idx, is_aux](ListRow*) {
      set_part_output(part_idx, is_aux ? ws::v1::PART_OUTPUT_MAIN : ws::v1::PART_OUTPUT_AUX);
    });

    row_items_.push_back(std::move(row));
    y += 56 + sp.space_2;
  }
}

}  // namespace velora::ui
