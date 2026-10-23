// Story 4.12: Fader bindings with pickup (FR104, FR105, FR52, AR16).
// Faders 1-8 control part volume and 9 controls REV master send in MIXER;
// in DRAWBARS they control drawbars 16'..1' of the selected organ part.
// Moving a fader never makes a value jump after a recall / mode change / organ change:
// the fader changes nothing until physical position crosses the stored value (pickup).
// DRAWBARS mode is refused when no organ part exists in the Live Set.
#include "velora/host/fader_bindings.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/mcu_ids.hpp"
#include "ws_v1.pb.h"

using namespace velora;
namespace p = core::params;
namespace mcu_ids = velora::host::mcu_ids;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                                                     \
  do {                                                                                                            \
    const double va_ = (a), vb_ = (b);                                                                            \
    if (std::fabs(va_ - vb_) > (tol)) {                                                                           \
      std::printf("FAIL %s:%d: %s = %.6g, expected %.6g (+-%.6g)\n", __FILE__, __LINE__, #a, va_, vb_, (tol)); \
      ++g_fail;                                                                                                   \
    }                                                                                                             \
  } while (0)

struct TestRig {
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>(8);
  host::ControlEventQueue rt_events;
  host::ControlPlane plane;
  host::FaderController faders;

  TestRig()
      : plane(br->snapshots(), br->last_values(), rt_events, [] { return std::uint64_t{0}; }),
        faders(plane) {
    ws::v1::State cfg;
    std::string err;
    CHECK(plane.load_initial(cfg, err));
  }

  void setup_organ_parts(const std::vector<int>& organ_part_indices) {
    core::Snapshot work = *plane.current_snapshot();
    for (int i = 0; i < 8; ++i) {
      work.parts[i].engine_type = static_cast<std::uint32_t>(core::EngineTypeId::kSample);
    }
    for (int idx : organ_part_indices) {
      if (idx >= 0 && idx < 8) {
        work.parts[idx].engine_type = static_cast<std::uint32_t>(core::EngineTypeId::kOrgan);
      }
    }
    std::string detail;
    CHECK(plane.publish_snapshot(work, detail) == host::ControlPlane::PublishStatus::kOk);
  }
};

// 1. Given the fader mode MIXER: Faders 1-8 drive part volumes; 9th fader drives REV send level.
static void test_mixer_mode_bindings() {
  TestRig rig;
  CHECK(rig.faders.mode() == ws::v1::FADER_MODE_MIXER);

  // Set initial stored volumes: part 0 = 0.5, part 1 = 0.8
  core::Snapshot snap = *rig.plane.current_snapshot();
  snap.parts[0].volume = 0.5f;
  snap.parts[1].volume = 0.8f;
  std::string detail;
  CHECK(rig.plane.publish_snapshot(snap, detail) == host::ControlPlane::PublishStatus::kOk);

  // Align physical positions to stored values so pickup is not pending
  rig.faders.set_physical_positions({0.5f, 0.8f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
  CHECK(!rig.faders.is_pending(0));
  CHECK(!rig.faders.is_pending(1));
  CHECK(!rig.faders.is_pending(8));

  // Move Fader 1 (part 0 volume) from 0.5 to 0.75
  CHECK(rig.faders.move_fader(0, 0.75f));
  CHECK_NEAR(rig.plane.current_snapshot()->parts[0].volume, 0.75f, 1e-4);
  CHECK_NEAR(rig.faders.physical_pos(0), 0.75f, 1e-4);
  CHECK(!rig.faders.is_pending(0));

  // Move Fader 2 (part 1 volume) from 0.8 to 0.2
  CHECK(rig.faders.move_fader(1, 0.2f));
  CHECK_NEAR(rig.plane.current_snapshot()->parts[1].volume, 0.2f, 1e-4);
  CHECK_NEAR(rig.faders.physical_pos(1), 0.2f, 1e-4);

  // Move Fader 9 (REV master send) from 1.0 to 0.4
  CHECK(rig.faders.move_fader(8, 0.4f));
  CHECK_NEAR(rig.faders.physical_pos(8), 0.4f, 1e-4);
  // REV send generates a UMP event on the rt_events queue
  host::StampedUmp ev{};
  bool found_rev_send = false;
  while (rig.rt_events.pop(ev)) {
    core::ParamEvent pe;
    if (core::decode_param_event(ev.ump, pe)) {
      if (pe.def->id == core::params::kSendReverbMaster) {
        found_rev_send = true;
        CHECK_NEAR(pe.value, 0.4f, 1e-3);
      }
    }
  }
  CHECK(found_rev_send);
}

// 2. Refusal of DRAWBARS mode when no organ part exists in the Live Set.
static void test_drawbars_refusal_when_no_organ() {
  TestRig rig;
  // Rig starts with all sample parts (no organ)
  CHECK(rig.faders.mode() == ws::v1::FADER_MODE_MIXER);

  std::string err;
  bool ok = rig.faders.set_mode(ws::v1::FADER_MODE_DRAWBARS, &err);
  CHECK(!ok);
  CHECK(err == "no organ in this Live Set");
  // Faders must stay in MIXER mode
  CHECK(rig.faders.mode() == ws::v1::FADER_MODE_MIXER);

  // Also verify via ws.v1 ClientMessage
  ws::v1::ClientMessage msg;
  msg.mutable_set_fader_mode()->set_mode(ws::v1::FADER_MODE_DRAWBARS);
  bool changed = false;
  ws::v1::ServerMessage reply = rig.plane.handle(msg, changed);
  CHECK(reply.has_error());
  CHECK(reply.error().code() == ws::v1::ERROR_CODE_NO_ORGAN);
  CHECK(rig.faders.mode() == ws::v1::FADER_MODE_MIXER);
}

// 3. DRAWBARS mode with organ part selected: Faders 1-9 drive drawbars of that organ part.
static void test_drawbars_mode_bindings() {
  TestRig rig;
  // Setup Part 3 as Organ
  rig.setup_organ_parts({3});

  std::string err;
  CHECK(rig.faders.set_mode(ws::v1::FADER_MODE_DRAWBARS, &err));
  CHECK(rig.faders.mode() == ws::v1::FADER_MODE_DRAWBARS);
  // Auto-selected organ part 3
  CHECK(rig.faders.selected_organ_part() == 3);

  // Drawbar 8' has default 8 (1.0f); other drawbars have default 0 (0.0f).
  // Align physical positions with stored values so pickup is immediately active.
  rig.faders.set_physical_positions({0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
  CHECK(!rig.faders.is_pending(0));
  CHECK(!rig.faders.is_pending(2));
  CHECK(!rig.faders.is_pending(8));

  // Move Fader 1 (16') to 1.0 (drawbar value 8)
  CHECK(rig.faders.move_fader(0, 1.0f));
  CHECK_NEAR(rig.faders.physical_pos(0), 1.0f, 1e-4);

  // Move Fader 3 (8') to 0.5 (drawbar value 4)
  CHECK(rig.faders.move_fader(2, 0.5f));

  // Move Fader 9 (1') to 0.75 (drawbar value 6)
  CHECK(rig.faders.move_fader(8, 0.75f));

  // Verify UMP performance events sent to RT for Part 3
  host::StampedUmp ev{};
  int events_checked = 0;
  while (rig.rt_events.pop(ev)) {
    core::ParamEvent pe;
    if (core::decode_param_event(ev.ump, pe)) {
      if (pe.part == 3) {
        if (pe.def->id == core::params::kDrawbar16) {
          CHECK_NEAR(pe.value, 8.0f, 1e-3);
          ++events_checked;
        } else if (pe.def->id == core::params::kDrawbar8) {
          CHECK_NEAR(pe.value, 4.0f, 1e-3);
          ++events_checked;
        } else if (pe.def->id == core::params::kDrawbar1) {
          CHECK_NEAR(pe.value, 6.0f, 1e-3);
          ++events_checked;
        }
      }
    }
  }
  CHECK(events_checked == 3);
}

// 4. Multiple organ parts: selection via SelectOrganPart and part-button long press.
static void test_multiple_organ_selection() {
  TestRig rig;
  // Setup Parts 1 and 4 as Organs
  rig.setup_organ_parts({1, 4});

  CHECK(rig.faders.set_mode(ws::v1::FADER_MODE_DRAWBARS));
  // Defaults to first organ (Part 1)
  CHECK(rig.faders.selected_organ_part() == 1);

  // Select Part 4 as the organ
  CHECK(rig.faders.select_organ_part(4));
  CHECK(rig.faders.selected_organ_part() == 4);

  // Attempting to select Part 0 (sample part, not organ) is refused
  std::string err;
  CHECK(!rig.faders.select_organ_part(0, &err));
  CHECK(err == "part is not an organ part");
  CHECK(rig.faders.selected_organ_part() == 4);

  // Test via MCU button long-press
  CHECK(rig.faders.on_mcu_button(mcu_ids::buttons::PART_SCENE_2, true, true));  // Part 2 (index 1)
  CHECK(rig.faders.selected_organ_part() == 1);
}

// 5. Pickup rule: fader changes nothing until physical position crosses stored value.
static void test_pickup_rule_crossing_from_below_and_above() {
  TestRig rig;
  // Part 2 stored volume = 0.65
  // Part 0 stored volume = 0.50
  core::Snapshot snap = *rig.plane.current_snapshot();
  snap.parts[2].volume = 0.65f;
  snap.parts[0].volume = 0.50f;
  std::string detail;
  CHECK(rig.plane.publish_snapshot(snap, detail) == host::ControlPlane::PublishStatus::kOk);

  // Physical faders at initial positions: Fader 3 at 0.20 (below 0.65), Fader 1 at 0.90 (above 0.50)
  rig.faders.set_physical_positions({0.90f, 0.50f, 0.20f, 0.50f, 0.50f, 0.50f, 0.50f, 0.50f, 1.0f});

  // Fader 3 (index 2) is pending (phys 0.20 < stored 0.65)
  CHECK(rig.faders.is_pending(2));
  CHECK_NEAR(rig.faders.stored_value(2), 0.65f, 1e-4);
  CHECK_NEAR(rig.faders.physical_pos(2), 0.20f, 1e-4);

  // Fader 1 (index 0) is pending (phys 0.90 > stored 0.50)
  CHECK(rig.faders.is_pending(0));
  CHECK_NEAR(rig.faders.stored_value(0), 0.50f, 1e-4);
  CHECK_NEAR(rig.faders.physical_pos(0), 0.90f, 1e-4);

  // A) Move Fader 3 slightly to 0.30 (still below 0.65): NOTHING changes on Part 3 volume!
  CHECK(rig.faders.move_fader(2, 0.30f));
  CHECK(rig.faders.is_pending(2));  // Still pending
  CHECK_NEAR(rig.faders.physical_pos(2), 0.30f, 1e-4);
  CHECK_NEAR(rig.plane.current_snapshot()->parts[2].volume, 0.65f, 1e-4);  // Unchanged!

  // Move Fader 3 downwards to 0.10: still pending, nothing changes
  CHECK(rig.faders.move_fader(2, 0.10f));
  CHECK(rig.faders.is_pending(2));
  CHECK_NEAR(rig.plane.current_snapshot()->parts[2].volume, 0.65f, 1e-4);

  // Cross stored value: move Fader 3 to 0.70 (crosses 0.65 from below!)
  CHECK(rig.faders.move_fader(2, 0.70f));
  CHECK(!rig.faders.is_pending(2));  // Takeover achieved!
  CHECK_NEAR(rig.faders.physical_pos(2), 0.70f, 1e-4);
  CHECK_NEAR(rig.plane.current_snapshot()->parts[2].volume, 0.70f, 1e-4);  // Updated!

  // Subsequent moves now follow directly
  CHECK(rig.faders.move_fader(2, 0.75f));
  CHECK_NEAR(rig.plane.current_snapshot()->parts[2].volume, 0.75f, 1e-4);

  // B) Now test Fader 1 crossing from above:
  // Move Fader 1 downwards to 0.60 (still above 0.50): NOTHING changes on Part 1 volume!
  CHECK(rig.faders.move_fader(0, 0.60f));
  CHECK(rig.faders.is_pending(0));  // Still pending
  CHECK_NEAR(rig.plane.current_snapshot()->parts[0].volume, 0.50f, 1e-4);  // Unchanged!

  // Cross stored value: move Fader 1 down to 0.40 (crosses 0.50 from above!)
  CHECK(rig.faders.move_fader(0, 0.40f));
  CHECK(!rig.faders.is_pending(0));  // Takeover achieved!
  CHECK_NEAR(rig.plane.current_snapshot()->parts[0].volume, 0.40f, 1e-4);  // Updated!

  // Subsequent moves follow directly
  CHECK(rig.faders.move_fader(0, 0.35f));
  CHECK_NEAR(rig.plane.current_snapshot()->parts[0].volume, 0.35f, 1e-4);
}

// 6. State publication: engine publishes target, stored value, physical position, and pending state.
static void test_state_publication() {
  TestRig rig;
  rig.setup_organ_parts({2});

  // Physical fader at 0.10, target volume at 0.70
  core::Snapshot snap = *rig.plane.current_snapshot();
  snap.parts[0].volume = 0.70f;
  std::string detail;
  CHECK(rig.plane.publish_snapshot(snap, detail) == host::ControlPlane::PublishStatus::kOk);
  rig.faders.set_physical_positions({0.10f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f});

  ws::v1::State st;
  rig.plane.fill_state(&st);
  CHECK(st.has_faders());
  CHECK(st.faders().mode() == ws::v1::FADER_MODE_MIXER);
  CHECK(st.faders().bindings_size() == 9);

  // Fader 1 binding: Part 1 Volume, stored = 0.70, physical = 0.10, is_pending = true
  const auto& b0 = st.faders().bindings(0);
  CHECK(b0.fader_index() == 0);
  CHECK(b0.target_type() == ws::v1::FADER_TARGET_PART_VOLUME);
  CHECK(b0.target_part() == 0);
  CHECK_NEAR(b0.stored_value(), 0.70f, 1e-4);
  CHECK_NEAR(b0.physical_pos(), 0.10f, 1e-4);
  CHECK(b0.is_pending());

  // Switch to DRAWBARS
  CHECK(rig.faders.set_mode(ws::v1::FADER_MODE_DRAWBARS));
  st.Clear();
  rig.plane.fill_state(&st);
  CHECK(st.faders().mode() == ws::v1::FADER_MODE_DRAWBARS);
  CHECK(st.faders().selected_organ_part() == 2);
  const auto& b_draw = st.faders().bindings(0);
  CHECK(b_draw.target_type() == ws::v1::FADER_TARGET_DRAWBAR);
  CHECK(b_draw.target_part() == 2);
  CHECK(b_draw.target_param_id() == core::params::kDrawbar16);
}

// 7. MCU control messages and toggle button
static void test_mcu_control_integration() {
  TestRig rig;
  rig.setup_organ_parts({1});

  // Toggle button toggles from MIXER to DRAWBARS
  CHECK(rig.faders.on_mcu_button(mcu_ids::buttons::TOGGLE_DRAWBARS_MIXER, true));
  CHECK(rig.faders.mode() == ws::v1::FADER_MODE_DRAWBARS);

  // Toggle button toggles back to MIXER
  CHECK(rig.faders.on_mcu_button(mcu_ids::buttons::TOGGLE_DRAWBARS_MIXER, true));
  CHECK(rig.faders.mode() == ws::v1::FADER_MODE_MIXER);

  // Fader 1 MCU control move (raw 12-bit 0..4095)
  // Initially aligned to 0.50 (2048 / 4095 ≈ 0.50)
  rig.faders.set_physical_positions({0.50f, 0.50f, 0.50f, 0.50f, 0.50f, 0.50f, 0.50f, 0.50f, 1.0f});
  CHECK(rig.faders.move_fader_mcu(mcu_ids::controls::FADER_1, 4095));
  CHECK_NEAR(rig.plane.current_snapshot()->parts[0].volume, 1.0f, 1e-4);

  // Fader 9 MCU control move (raw 0)
  CHECK(rig.faders.move_fader_mcu(mcu_ids::controls::FADER_9, 0));
  CHECK_NEAR(rig.faders.physical_pos(8), 0.0f, 1e-4);
}

int main() {
  std::printf("Running fader_bindings_test...\n");
  test_mixer_mode_bindings();
  test_drawbars_refusal_when_no_organ();
  test_drawbars_mode_bindings();
  test_multiple_organ_selection();
  test_pickup_rule_crossing_from_below_and_above();
  test_state_publication();
  test_mcu_control_integration();

  if (g_fail == 0) {
    std::printf("All fader_bindings_tests passed!\n");
    return 0;
  }
  std::printf("%d test(s) FAILED\n", g_fail);
  return 1;
}
