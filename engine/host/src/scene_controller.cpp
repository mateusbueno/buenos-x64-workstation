#include "velora/host/scene_controller.hpp"

#include <algorithm>
#include <array>

#include "live_set_detail.hpp"

namespace velora::host {

namespace p = core::params;
namespace lsp = ::velora::liveset::v1;
using namespace detail;

namespace {

template <class T>
T clamp_to(std::int64_t v, std::int64_t lo, std::int64_t hi) {
  return static_cast<T>(std::min(std::max(v, lo), hi));
}

}  // namespace

void apply_scene(const lsp::Scene& scene, const core::Snapshot& base, core::Snapshot& work) {
  work.perf_force.fill(0);
  for (std::uint32_t i = 0; i < p::kPartCount; ++i) {
    const lsp::ScenePart* sp = static_cast<int>(i) < scene.parts_size() ? &scene.parts(static_cast<int>(i)) : nullptr;
    const core::PartCfg& b = base.parts[i];
    core::PartCfg& w = work.parts[i];
    // Only the scene-covered fields; timbre_ref, state, engine_type, sample_bank and chain are not assigned here.
    w.low_key = sp && sp->has_low_key() ? clamp_to<std::uint8_t>(sp->low_key(), 0, 127) : b.low_key;
    w.high_key = sp && sp->has_high_key() ? clamp_to<std::uint8_t>(sp->high_key(), 0, 127) : b.high_key;
    w.octave = sp && sp->has_octave() ? clamp_to<std::int8_t>(sp->octave(), -4, 4) : b.octave;
    w.transpose = sp && sp->has_transpose() ? clamp_to<std::int8_t>(sp->transpose(), -12, 12) : b.transpose;
    w.keyboard_on = sp && sp->has_keyboard_on() ? (sp->keyboard_on() ? 1 : 0) : b.keyboard_on;
    w.volume = sp && sp->has_volume() ? sane_volume(sp->volume()) : b.volume;
    if (sp && sp->has_output())
      w.output = sp->output() == ws::v1::PART_OUTPUT_AUX ? static_cast<std::uint8_t>(p::kPartOutputAux) : static_cast<std::uint8_t>(p::kPartOutputMain);
    else
      w.output = b.output;
    if (sp && sp->has_inserts()) {
      fill_inserts(w, sp->inserts().slots());
    } else {
      w.inserts = b.inserts;
      w.insert_count = b.insert_count;
    }
    // Performance values: everything starts at the base Live Set's value; what the scene stores overrides it and is flagged for RT.
    for (const p::ParamDef& d : p::kParams)
      if (d.cls == p::Class::kPerformance && d.scope == p::Scope::kPart) {
        const std::size_t slot = p::perf_slot(d, i);
        work.perf_init[slot] = base.perf_init[slot];
      }
    if (sp) {
      if (sp->has_send_reverb()) {
        const std::size_t slot = p::perf_slot(*p::find(p::kSendReverbLevel), i);
        work.perf_init[slot] = sane(*p::find(p::kSendReverbLevel), sp->send_reverb());
        work.perf_force[slot] = 1;
      }
      if (sp->has_send_delay()) {
        const std::size_t slot = p::perf_slot(*p::find(p::kSendDelayLevel), i);
        work.perf_init[slot] = sane(*p::find(p::kSendDelayLevel), sp->send_delay());
        work.perf_force[slot] = 1;
      }
      apply_entries(work, sp->engine_params(), i, false, &work.perf_force);
      apply_entries(work, sp->envelope(), i, false, &work.perf_force);
      apply_entries(work, sp->eq(), i, false, &work.perf_force);
    }
  }
  // Master: structural values and the master-scope performance values (not the tempo) are the base's, then the scene's list.
  work.master = base.master;
  for (const p::ParamDef& d : p::kParams)
    if (d.cls == p::Class::kPerformance && d.scope == p::Scope::kMaster && d.id != p::kTempoBpm) {
      const std::size_t slot = p::perf_slot(d, 0);
      work.perf_init[slot] = base.perf_init[slot];
    }
  apply_entries(work, scene.master(), 0, true, &work.perf_force);
}

SceneController::SceneController(ControlPlane& plane) : plane_(plane) { plane_.set_scene_handler(this); }

SceneController::~SceneController() { plane_.set_scene_handler(nullptr); }

SceneController::LoadScope::LoadScope(SceneController* c) noexcept : c_(c) {
  if (!c_) return;
  token_ = ++c_->next_token_;
  c_->loading_ = true;
}

SceneController::LoadScope::~LoadScope() {
  if (c_) c_->loading_ = false;
}

void SceneController::LoadScope::commit(const lsp::LiveSet& ls, const core::Snapshot& base) {
  if (!c_) return;
  c_->live_set_ = ls;
  c_->base_ = base;
  for (core::PartCfg& pc : c_->base_.parts) {  // the base keeps values only: no instrument and no effect instance outlives its generation
    pc.sample_bank.reset();
    pc.chain.reset();
  }
  c_->source_ = token_;
  c_->loaded_ = true;
}

bool SceneController::owns_current() const noexcept {
  const core::Snapshot* cur = plane_.current_snapshot();
  return cur && loaded_ && cur->live_set && cur->scene_source == source_;
}

void SceneController::set_source(const lsp::LiveSet& edited) {
  if (!loaded_) return;
  live_set_ = edited;
  core::Snapshot b = core::Snapshot::defaults();  // values only: apply_scene reads the covered fields, the performance values and the master
  b.live_set = true;
  for (int i = 0; i < live_set_.parts_size() && i < static_cast<int>(p::kPartCount); ++i) fill_part_values(b, live_set_.parts(i), static_cast<std::uint32_t>(i));
  fill_master_values(b, live_set_);
  b.scene_count = base_.scene_count;
  b.scene_source = base_.scene_source;
  base_ = b;
}

std::int32_t SceneController::current_scene() const noexcept {
  const core::Snapshot* cur = plane_.current_snapshot();
  return cur && loaded_ && cur->live_set && cur->scene_source == source_ ? cur->scene_index : -1;
}

SceneOutcome SceneController::finish(SceneResult r, bool published) const {
  SceneOutcome o;
  o.busy_reason = r == SceneResult::kLoadInProgress ? "load_in_progress" : r == SceneResult::kBusy ? "ledger_full" : "";
  o.result = r;
  o.scene = current_scene();
  o.revision = plane_.revision();
  o.published = published;
  return o;
}

SceneOutcome SceneController::recall_scene(std::uint32_t index) { return do_recall(index); }

SceneOutcome SceneController::next_scene() {
  if (loading_) return finish(SceneResult::kLoadInProgress, false);
  const core::Snapshot* cur = plane_.current_snapshot();
  if (!loaded_ || !cur || !cur->live_set || cur->scene_source != source_) return finish(SceneResult::kNoLiveSet, false);
  const std::int64_t next = static_cast<std::int64_t>(current_scene()) + 1;  // -1 (none) steps to scene 0
  if (next >= live_set_.scenes_size()) return finish(SceneResult::kAtLast, false);
  return do_recall(static_cast<std::uint32_t>(next));
}

SceneOutcome SceneController::previous_scene() {
  if (loading_) return finish(SceneResult::kLoadInProgress, false);
  const core::Snapshot* cur = plane_.current_snapshot();
  if (!loaded_ || !cur || !cur->live_set || cur->scene_source != source_) return finish(SceneResult::kNoLiveSet, false);
  const std::int32_t at = current_scene();
  if (at <= 0) return finish(SceneResult::kAtFirst, false);  // the first scene, or none recalled yet
  return do_recall(static_cast<std::uint32_t>(at - 1));
}

SceneOutcome SceneController::run(Op op, std::uint32_t index) {
  switch (op) {
    case Op::kRecall: return recall_scene(index);
    case Op::kNext: return next_scene();
    case Op::kPrevious: return previous_scene();
  }
  return finish(SceneResult::kInternal, false);
}

SceneOutcome SceneController::do_recall(std::uint32_t index) {
  if (loading_) return finish(SceneResult::kLoadInProgress, false);
  const core::Snapshot* cur = plane_.current_snapshot();
  if (!loaded_ || !cur || !cur->live_set || cur->scene_source != source_) return finish(SceneResult::kNoLiveSet, false);
  if (index >= static_cast<std::uint32_t>(live_set_.scenes_size())) return finish(SceneResult::kNoSuchScene, false);
  if (before_recall_) before_recall_();  // Story 4.9: the edit session captures the live values; it may replace the source (set_source)

  core::Snapshot work = *cur;  // shares every sample_bank and chain with the published generation
  apply_scene(live_set_.scenes(static_cast<int>(index)), base_, work);
  // Forced slots must not be lost when RT skips a program generation: if RT has not adopted the current generation's program yet, its
  // forced slots stay forced in this one (they then take this scene's value for the slot, which is what the newest scene says).
  const core::Snapshot* held = plane_.rt_held_snapshot();
  if (!held || held->program_generation != cur->program_generation)
    for (std::size_t i = 0; i < work.perf_force.size(); ++i) work.perf_force[i] = work.perf_force[i] | cur->perf_force[i];
  // Refusals first, on the configured effect types (a changed chain is built only once the swap is certain); a refusal consumes nothing: no
  // program generation number, no effect instance.
  std::array<bool, p::kPartCount> changed_chain{};
  for (std::uint32_t i = 0; i < p::kPartCount; ++i)
    if (!same_inserts(cur->parts[i], work.parts[i])) {
      changed_chain[i] = true;
      work.parts[i].chain.reset();
    }
  switch (plane_.can_publish(work)) {
    case ControlPlane::PublishStatus::kOk: break;
    case ControlPlane::PublishStatus::kFxBudgetExceeded: return finish(SceneResult::kFxBudgetExceeded, false);
    case ControlPlane::PublishStatus::kBusy: return finish(SceneResult::kBusy, false);
    case ControlPlane::PublishStatus::kInternal: return finish(SceneResult::kInternal, false);
  }
  // A part whose insert chain did not change keeps its effect instances (RT does not cross-fade); a changed one gets new, prepared ones,
  // built here, off the RT thread (freed with `work` if the publication is refused after all).
  for (std::uint32_t i = 0; i < p::kPartCount; ++i)
    if (changed_chain[i]) work.parts[i].chain = core::build_chain(work.parts[i]);
    else work.parts[i].chain = cur->parts[i].chain;
  work.program_generation = plane_.next_program_generation();
  work.scene_index = static_cast<std::int32_t>(index);

  std::string detail;
  switch (plane_.publish_snapshot(work, detail)) {
    case ControlPlane::PublishStatus::kOk: return finish(SceneResult::kOk, true);
    case ControlPlane::PublishStatus::kFxBudgetExceeded: return finish(SceneResult::kFxBudgetExceeded, false);
    case ControlPlane::PublishStatus::kBusy: return finish(SceneResult::kBusy, false);
    case ControlPlane::PublishStatus::kInternal: break;
  }
  return finish(SceneResult::kInternal, false);
}

}  // namespace velora::host
