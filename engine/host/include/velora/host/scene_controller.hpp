#pragma once
// Scene recall (Story 4.4, FR88, AR21). A scene recall is a PARAMETER-ONLY snapshot swap: the control thread copies the currently published
// generation, changes only what a scene stores (see below), and publishes the copy with the usual single atomic store. It never loads or
// releases heads or instruments (the loader's resolve / load functions are not called), never touches a part's timbre, engine type, state,
// sample bank or timbre_ref (the new generation shares the very same `sample_bank` shared_ptrs), and never builds an effect chain that
// did not change.
//
// What a scene stores (live_set.proto `Scene`): a full parameter snapshot excluding the timbre assignment, kept sparsely. A recall sets every
// scene-covered field of the generation to "the scene's value when it has one, else the base Live Set's value" -- so recalling scene 2 after
// scene 1 puts back what scene 1 had overridden and scene 2 does not. Covered per part: zone (low/high key), octave, transpose, keyboard
// on/off, volume, Main/Aux output, the insert chain (and bypass flags), the send levels and the engine / envelope / EQ sound parameters
// (drawbars, ...); and the master list. NOT covered, and never changed: timbre, engine type, part state, sample banks, tempo, the ASSIGN map.
//
// Performance values (AD-2 program_generation). The scene's performance parameters travel as the generation's initial values (`perf_init`)
// tagged with a new `program_generation`; RT applies, once, at that generation, (a) every value that differs from what it applied before,
// as for any generation, and (b) every value the scene explicitly stores (`perf_force`), even when equal. Hence: a recall OVERRIDES a live
// knob move of the parameters the scene stores; it does NOT override one the scene does not store (that value equals the base, which RT has
// already applied, so it is not re-applied) -- unless the previously recalled scene had overridden it, in which case the base value comes
// back (the scene is a full snapshot). Recalling the scene that is already current RESETS its stored performance values to the scene's.
// Notes: part volume, zone, octave, transpose, keyboard on/off, output and inserts are structural fields of the generation (part fader and
// router configuration), not live values.
//
// Sounding notes. Zone, octave, transpose and keyboard on/off are consulted when a note STARTS (PartRouter): notes held, sustained by the
// pedal or releasing at the recall keep their part, pitch and engine and release normally. Volume, output and the insert chain are properties
// of the part's bus, so they apply to the part's sound as a whole through the mixer's existing ramps (volume and output: one block / 5 ms
// ramp, a changed insert chain: a 5 ms cross-fade; instance state is not carried across a changed chain), gap-free.
//
// Stepping. next_scene() / previous_scene() walk the scene order and STOP at the last / first scene (no wrap, nothing published, result
// kAtLast / kAtFirst). With no scene recalled yet (current = -1, a freshly loaded Live Set) next goes to scene 0 and previous stays (kAtFirst).
//
// Threading: control thread only, like the plane it drives. Hostile cases: a missing scene, no Live Set loaded, a load in progress, a full
// snapshot ledger (kBusy) and an FX cost above the budget all return a typed result and change nothing.
#include <cstdint>
#include <functional>
#include <memory>

#include "live_set.pb.h"
#include "velora/core/snapshot.hpp"
#include "velora/host/control_plane.hpp"

namespace velora::host {

// Builds into `work` the parameter-only changes a recall of `scene` makes, given the Live Set's `base` generation (the one the loader built
// before any scene). `work` starts as a copy of the generation to change; on return its covered fields hold the scene's values, `perf_force`
// flags the performance slots the scene stores, and its chains are left as they were (see SceneController::recall for chain handling).
// Pure: no I/O, no publication. Exposed for the loader (FX reservation of the worst scene) and for tests.
void apply_scene(const ::velora::liveset::v1::Scene& scene, const core::Snapshot& base, core::Snapshot& work);

class SceneController final : public SceneHandler {
 public:
  // Registers itself as `plane`'s scene handler; the plane must outlive it.
  explicit SceneController(ControlPlane& plane);
  ~SceneController() override;
  SceneController(const SceneController&) = delete;
  SceneController& operator=(const SceneController&) = delete;

  SceneOutcome recall_scene(std::uint32_t index);
  SceneOutcome next_scene();
  SceneOutcome previous_scene();
  SceneOutcome run(Op op, std::uint32_t index) override;

  bool has_live_set() const noexcept { return loaded_; }
  std::uint32_t scene_count() const noexcept { return loaded_ ? static_cast<std::uint32_t>(live_set_.scenes_size()) : 0; }
  // The scene the current generation was recalled from (-1 = none, or the current generation is not the loaded Live Set's).
  std::int32_t current_scene() const noexcept;
  bool load_in_progress() const noexcept { return loading_; }

  // ---- edits in memory (Story 4.9) -----------------------------------------------------------------------------------------------
  // The current generation was built from the Live Set this controller holds (a recall would work on it).
  bool owns_current() const noexcept;
  // The Live Set scenes are recalled from: the loaded one, or its in-memory edited state after set_source().
  const ::velora::liveset::v1::LiveSet& source() const noexcept { return live_set_; }
  // Replaces the scenes and the base values a recall works from with the Live Set's edited state (same id, same parts and scenes; the published
  // generation, its banks and chains are untouched). The base values are rebuilt from the message (values only; no head is loaded). Control thread.
  void set_source(const ::velora::liveset::v1::LiveSet& edited);
  // Runs on the control thread at the start of every recall (the edit session captures the live values there, before the recall resets them).
  void set_before_recall(std::function<void()> f) { before_recall_ = std::move(f); }

  // ---- LiveSetLoader side ---------------------------------------------------------------------------------------------------------
  // RAII scope of one load attempt: while it lives, recalls answer kLoadInProgress. token() goes into the snapshot the loader builds
  // (Snapshot::scene_source); the loader calls commit() after a successful publication, which makes the loaded Live Set the scene source.
  class LoadScope {
   public:
    explicit LoadScope(SceneController* c) noexcept;
    ~LoadScope();
    LoadScope(const LoadScope&) = delete;
    LoadScope& operator=(const LoadScope&) = delete;
    std::uint64_t token() const noexcept { return token_; }
    // `base`: the generation as published (its banks and chains are not kept).
    void commit(const ::velora::liveset::v1::LiveSet& ls, const core::Snapshot& base);

   private:
    SceneController* c_;
    std::uint64_t token_ = 0;
  };

 private:
  SceneOutcome finish(SceneResult r, bool published) const;
  SceneOutcome do_recall(std::uint32_t index);

  ControlPlane& plane_;
  bool loaded_ = false;
  bool loading_ = false;
  std::uint64_t source_ = 0;     // token of the load whose Live Set and base are held
  std::uint64_t next_token_ = 0;
  ::velora::liveset::v1::LiveSet live_set_;
  core::Snapshot base_;
  std::function<void()> before_recall_;
};

}  // namespace velora::host
