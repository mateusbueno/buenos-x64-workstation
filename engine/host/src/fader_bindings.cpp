#include "velora/host/fader_bindings.hpp"

#include <algorithm>
#include <cmath>

#include "velora/core/engine.hpp"
#include "velora/core/params_gen.hpp"

namespace velora::host {

namespace {

static const std::uint32_t kDrawbarIds[9] = {
    core::params::kDrawbar16,
    core::params::kDrawbar513,
    core::params::kDrawbar8,
    core::params::kDrawbar4,
    core::params::kDrawbar223,
    core::params::kDrawbar2,
    core::params::kDrawbar135,
    core::params::kDrawbar113,
    core::params::kDrawbar1,
};

static const char* const kDrawbarNames[9] = {
    "Drawbar 16'", "Drawbar 5 1/3'", "Drawbar 8'", "Drawbar 4'", "Drawbar 2 2/3'",
    "Drawbar 2'", "Drawbar 1 3/5'", "Drawbar 1 1/3'", "Drawbar 1'",
};

}  // namespace

FaderController::FaderController(ControlPlane& plane) : plane_(plane) {
  plane_.set_fader_handler(this);
  for (std::size_t i = 0; i < kFaderCount; ++i) {
    bindings_[i].set_fader_index(static_cast<std::uint32_t>(i));
    bindings_[i].set_physical_pos(0.0f);
  }
  recalculate_pickup(true);
}

FaderController::~FaderController() {
  if (plane_.fader_handler() == this) {
    plane_.set_fader_handler(nullptr);
  }
}

int FaderController::count_organ_parts(int* first_organ_idx) const {
  const core::Snapshot* snap = plane_.current_snapshot();
  if (!snap) return 0;
  int count = 0;
  int first = -1;
  for (std::uint32_t i = 0; i < core::params::kPartCount; ++i) {
    if (snap->parts[i].engine_type == static_cast<std::uint32_t>(core::EngineTypeId::kOrgan)) {
      if (first < 0) first = static_cast<int>(i);
      ++count;
    }
  }
  if (first_organ_idx) *first_organ_idx = first;
  return count;
}

bool FaderController::set_mode(ws::v1::FaderMode mode, std::string* err_detail) {
  if (mode == ws::v1::FADER_MODE_DRAWBARS) {
    int first_organ = -1;
    const int organs = count_organ_parts(&first_organ);
    if (organs == 0) {
      if (err_detail) *err_detail = "no organ in this Live Set";
      return false;  // Refused! Faders stay in MIXER
    }
    const core::Snapshot* snap = plane_.current_snapshot();
    if (selected_organ_part_ < 0 ||
        (snap && snap->parts[selected_organ_part_].engine_type !=
                     static_cast<std::uint32_t>(core::EngineTypeId::kOrgan))) {
      selected_organ_part_ = first_organ;
    }
    mode_ = ws::v1::FADER_MODE_DRAWBARS;
    recalculate_pickup();
    plane_.bump_revision();
    return true;
  }

  mode_ = ws::v1::FADER_MODE_MIXER;
  recalculate_pickup();
  plane_.bump_revision();
  return true;
}

bool FaderController::select_organ_part(std::uint32_t part, std::string* err_detail) {
  if (part >= core::params::kPartCount) {
    if (err_detail) *err_detail = "part index out of range";
    return false;
  }
  const core::Snapshot* snap = plane_.current_snapshot();
  if (!snap || snap->parts[part].engine_type != static_cast<std::uint32_t>(core::EngineTypeId::kOrgan)) {
    if (err_detail) *err_detail = "part is not an organ part";
    return false;
  }
  selected_organ_part_ = static_cast<std::int32_t>(part);
  if (mode_ == ws::v1::FADER_MODE_DRAWBARS) {
    recalculate_pickup();
  }
  plane_.bump_revision();
  return true;
}

void FaderController::set_physical_positions(const std::array<float, kFaderCount>& positions) {
  for (std::size_t i = 0; i < kFaderCount; ++i) {
    bindings_[i].set_physical_pos(std::clamp(positions[i], 0.0f, 1.0f));
  }
  recalculate_pickup();
}

void FaderController::recalculate_pickup(bool /*is_initial*/) {
  const core::Snapshot* snap = plane_.current_snapshot();

  for (std::size_t i = 0; i < kFaderCount; ++i) {
    auto& b = bindings_[i];
    b.set_fader_index(static_cast<std::uint32_t>(i));

    if (mode_ == ws::v1::FADER_MODE_MIXER) {
      if (i < 8) {
        b.set_target_type(ws::v1::FADER_TARGET_PART_VOLUME);
        b.set_target_name("Part " + std::to_string(i + 1) + " Volume");
        b.set_target_part(static_cast<std::uint32_t>(i));
        b.set_target_param_id(0);
        float vol = snap ? snap->parts[i].volume : 1.0f;
        b.set_stored_value(std::clamp(vol, 0.0f, 1.0f));
      } else {
        b.set_target_type(ws::v1::FADER_TARGET_REV_SEND);
        b.set_target_name("REV Send");
        b.set_target_part(0);
        b.set_target_param_id(core::params::kSendReverbMaster);
        const auto* def = core::params::find(core::params::kSendReverbMaster);
        float rev = 1.0f;
        if (def) {
          std::size_t slot = core::params::perf_slot(*def, 0);
          rev = plane_.live_value(slot);
          if (!std::isfinite(rev) && snap) rev = snap->perf_init[slot];
          if (!std::isfinite(rev)) rev = 1.0f;
        }
        b.set_stored_value(std::clamp(rev, 0.0f, 1.0f));
      }
    } else {
      b.set_target_type(ws::v1::FADER_TARGET_DRAWBAR);
      b.set_target_name(kDrawbarNames[i]);
      const std::uint32_t organ_part =
          selected_organ_part_ >= 0 ? static_cast<std::uint32_t>(selected_organ_part_) : 0;
      b.set_target_part(organ_part);
      b.set_target_param_id(kDrawbarIds[i]);
      const auto* def = core::params::find(kDrawbarIds[i]);
      float val = 0.0f;
      if (def && selected_organ_part_ >= 0) {
        std::size_t slot = core::params::perf_slot(*def, organ_part);
        val = plane_.live_value(slot);
        if (!std::isfinite(val) && snap) val = snap->perf_init[slot];
        if (!std::isfinite(val)) val = static_cast<float>(def->def);
      }
      b.set_stored_value(std::clamp(val / 8.0f, 0.0f, 1.0f));
    }

    // Pickup rule: compare physical position against stored target value
    constexpr float kEps = 0.005f;
    const float phys = b.physical_pos();
    const float target = b.stored_value();
    if (std::fabs(phys - target) <= kEps) {
      b.set_is_pending(false);
      direction_[i] = 0;
    } else {
      b.set_is_pending(true);
      direction_[i] = (phys < target) ? 1 : -1;
    }
  }
}

bool FaderController::move_fader(std::uint32_t fader_idx, float position) {
  if (fader_idx >= kFaderCount) return false;
  const float pos = std::clamp(position, 0.0f, 1.0f);
  auto& b = bindings_[fader_idx];

  if (b.is_pending()) {
    const float target = b.stored_value();
    const std::int8_t dir = direction_[fader_idx];
    bool crossed = false;
    if (dir >= 0 && pos >= target) crossed = true;
    if (dir <= 0 && pos <= target) crossed = true;

    if (crossed) {
      // Pickup takeover achieved!
      b.set_is_pending(false);
      b.set_physical_pos(pos);
      direction_[fader_idx] = 0;
      apply_target(b, pos);
    } else {
      // Still pending: update physical position, change nothing on target parameter
      b.set_physical_pos(pos);
    }
  } else {
    // Already taken over: follow directly
    b.set_physical_pos(pos);
    apply_target(b, pos);
  }

  plane_.bump_revision();
  return true;
}

void FaderController::apply_target(const ws::v1::FaderBindingState& b, float pos) {
  if (b.target_type() == ws::v1::FADER_TARGET_PART_VOLUME) {
    ws::v1::ClientMessage msg;
    auto* pv = msg.mutable_set_part_volume();
    pv->set_part(b.target_part());
    pv->set_volume(pos);
    bool changed = false;
    plane_.handle(msg, changed);
  } else if (b.target_type() == ws::v1::FADER_TARGET_REV_SEND) {
    const auto* def = core::params::find(core::params::kSendReverbMaster);
    if (def) {
      plane_.push_performance_value(*def, 0, pos);
    }
  } else if (b.target_type() == ws::v1::FADER_TARGET_DRAWBAR) {
    const auto* def = core::params::find(b.target_param_id());
    if (def) {
      const float drawbar_val = std::clamp(std::round(pos * 8.0f), 0.0f, 8.0f);
      plane_.push_performance_value(*def, b.target_part(), drawbar_val);
    }
  }
}

bool FaderController::move_fader_mcu(std::uint16_t control_id, std::uint16_t raw_12bit) {
  if (control_id >= mcu_ids::controls::FADER_1 && control_id <= mcu_ids::controls::FADER_9) {
    const std::uint32_t idx = control_id - mcu_ids::controls::FADER_1;
    const float pos = static_cast<float>(raw_12bit) / 4095.0f;
    return move_fader(idx, pos);
  }
  return false;
}

bool FaderController::on_mcu_button(std::uint16_t button_id, bool pressed, bool long_press) {
  if (!pressed) return false;
  if (button_id == mcu_ids::buttons::TOGGLE_DRAWBARS_MIXER) {
    if (mode_ == ws::v1::FADER_MODE_MIXER) {
      return set_mode(ws::v1::FADER_MODE_DRAWBARS);
    } else {
      return set_mode(ws::v1::FADER_MODE_MIXER);
    }
  }
  if (long_press && button_id >= mcu_ids::buttons::PART_SCENE_1 &&
      button_id <= mcu_ids::buttons::PART_SCENE_8) {
    const std::uint32_t part = button_id - mcu_ids::buttons::PART_SCENE_1;
    return select_organ_part(part);
  }
  return false;
}

void FaderController::fill_status(ws::v1::State* out) const {
  auto* fs = out->mutable_faders();
  fs->set_mode(mode_);
  fs->set_selected_organ_part(selected_organ_part_);
  fs->clear_bindings();
  for (const auto& b : bindings_) {
    *fs->add_bindings() = b;
  }
}

void FaderController::on_snapshot_published(const core::Snapshot& snap) {
  int first_organ = -1;
  int organ_count = 0;
  for (std::uint32_t i = 0; i < core::params::kPartCount; ++i) {
    if (snap.parts[i].engine_type == static_cast<std::uint32_t>(core::EngineTypeId::kOrgan)) {
      if (first_organ < 0) first_organ = static_cast<int>(i);
      ++organ_count;
    }
  }

  if (organ_count == 0) {
    selected_organ_part_ = -1;
    if (mode_ == ws::v1::FADER_MODE_DRAWBARS) {
      mode_ = ws::v1::FADER_MODE_MIXER;
    }
  } else if (organ_count == 1) {
    selected_organ_part_ = first_organ;
  } else {
    if (selected_organ_part_ < 0 ||
        snap.parts[selected_organ_part_].engine_type !=
            static_cast<std::uint32_t>(core::EngineTypeId::kOrgan)) {
      selected_organ_part_ = first_organ;
    }
  }

  recalculate_pickup();
}

bool FaderController::run(const ws::v1::ClientMessage& msg, ws::v1::ServerMessage& reply,
                          HandlerFailure& err, bool& bump_revision, bool& push_state) {
  if (msg.has_set_fader_mode()) {
    const auto m = msg.set_fader_mode().mode();
    std::string detail;
    if (!set_mode(m, &detail)) {
      err.code = ws::v1::ERROR_CODE_NO_ORGAN;
      err.message = detail;
      err.details["mode"] = "DRAWBARS";
      return false;
    }
    bump_revision = true;
    push_state = true;
    reply.mutable_ack();
    return true;
  }

  if (msg.has_select_organ_part()) {
    const auto p = msg.select_organ_part().part();
    std::string detail;
    if (!select_organ_part(p, &detail)) {
      err.code = ws::v1::ERROR_CODE_NOT_ORGAN_PART;
      err.message = detail;
      err.details["part"] = std::to_string(p);
      return false;
    }
    bump_revision = true;
    push_state = true;
    reply.mutable_ack();
    return true;
  }

  if (msg.has_move_fader()) {
    const auto idx = msg.move_fader().fader_index();
    if (idx >= kFaderCount) {
      err.code = ws::v1::ERROR_CODE_OUT_OF_RANGE;
      err.message = "fader index out of range 0..8";
      err.details["fader_index"] = std::to_string(idx);
      return false;
    }
    const float pos = msg.move_fader().position();
    move_fader(idx, pos);
    bump_revision = true;
    push_state = true;
    reply.mutable_ack();
    return true;
  }

  err.code = ws::v1::ERROR_CODE_INVALID_COMMAND;
  err.message = "unknown fader command";
  return false;
}

}  // namespace velora::host
