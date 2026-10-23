#pragma once
// Fader bindings with pickup (Story 4.12, FR104, FR105, FR52, AR16).
// Fader mode MIXER: Faders 1-8 drive part 1-8 volumes; 9th fader drives REV send level (send_reverb_master).
// Fader mode DRAWBARS: Faders 1-9 drive the 9 drawbars (16' to 1') of the selected organ part.
// Pickup rule: On scene, Live Set or fader-mode change, or change of the selected organ, if a fader's physical
// position differs from the stored value of its new target, it changes nothing until its position crosses the
// stored value; once crossed, it takes over and follows.
// Drawbars refusal: If DRAWBARS is requested when no organ part exists in the Live Set, the engine refuses the mode
// change with ERROR_CODE_NO_ORGAN and faders stay in MIXER.
// State publication: Engine publishes per fader: target, stored value, physical position and pending/taken-over,
// updated on every change.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "velora/core/snapshot.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/mcu_ids.hpp"
#include "ws_v1.pb.h"

namespace velora::host {

class FaderController final : public FaderHandler {
 public:
  static constexpr std::size_t kFaderCount = 9;

  explicit FaderController(ControlPlane& plane);
  ~FaderController() override;
  FaderController(const FaderController&) = delete;
  FaderController& operator=(const FaderController&) = delete;

  ws::v1::FaderMode mode() const noexcept { return mode_; }
  std::int32_t selected_organ_part() const noexcept { return selected_organ_part_; }

  const ws::v1::FaderBindingState& binding(std::size_t idx) const { return bindings_[idx]; }
  const std::array<ws::v1::FaderBindingState, kFaderCount>& bindings() const noexcept { return bindings_; }

  bool is_pending(std::size_t idx) const noexcept {
    return idx < kFaderCount ? bindings_[idx].is_pending() : false;
  }
  float physical_pos(std::size_t idx) const noexcept {
    return idx < kFaderCount ? bindings_[idx].physical_pos() : 0.0f;
  }
  float stored_value(std::size_t idx) const noexcept {
    return idx < kFaderCount ? bindings_[idx].stored_value() : 0.0f;
  }

  // Switches fader mode. If mode == DRAWBARS and no organ part exists in the current snapshot,
  // refuses the change and returns false (stays in MIXER).
  bool set_mode(ws::v1::FaderMode mode, std::string* err_detail = nullptr);

  // Selects which organ part the drawbars control. Must be a valid part index with an organ engine.
  bool select_organ_part(std::uint32_t part, std::string* err_detail = nullptr);

  // Moves fader idx (0..8) to normalized physical position [0.0..1.0].
  // If pickup is pending, checks for crossing: changes nothing until crossed.
  // Once crossed or if not pending, applies the value to the bound parameter.
  bool move_fader(std::uint32_t fader_idx, float position);

  // Direct MCU control message handling (MCU_CTRL_FADER_1..MCU_CTRL_FADER_9).
  bool move_fader_mcu(std::uint16_t control_id, std::uint16_t raw_12bit);

  // Direct MCU button press (e.g. MCU_BTN_TOGGLE_DRAWBARS_MIXER, MCU_BTN_PART_SCENE_1..8).
  bool on_mcu_button(std::uint16_t button_id, bool pressed, bool long_press = false);

  // Test / calibration helper: sets hardware fader physical positions directly without triggering takeover logic.
  void set_physical_positions(const std::array<float, kFaderCount>& positions);

  // FaderHandler seam implementation
  bool run(const ws::v1::ClientMessage& msg, ws::v1::ServerMessage& reply, HandlerFailure& err,
           bool& bump_revision, bool& push_state) override;
  void fill_status(ws::v1::State* out) const override;
  void on_snapshot_published(const core::Snapshot& snap) override;

 private:
  void recalculate_pickup(bool is_initial = false);
  void apply_target(const ws::v1::FaderBindingState& b, float pos);
  int count_organ_parts(int* first_organ_idx = nullptr) const;

  ControlPlane& plane_;
  ws::v1::FaderMode mode_ = ws::v1::FADER_MODE_MIXER;
  std::int32_t selected_organ_part_ = -1;
  std::array<ws::v1::FaderBindingState, kFaderCount> bindings_{};
  // -1 if physical was > stored, +1 if physical was < stored, 0 if equal
  std::array<std::int8_t, kFaderCount> direction_{};
};

}  // namespace velora::host
