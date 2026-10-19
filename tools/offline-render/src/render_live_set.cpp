#include "render_live_set.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/scene_controller.hpp"

namespace velora::offline {

bool render_live_set(const std::vector<NoteEvent>& events, const LiveSetRenderOptions& opt, WavData& out, std::string& error,
                     LiveSetRenderReport* report) {
  using namespace velora::core;
  if (!(opt.tail_seconds >= 0.0) || opt.tail_seconds > 600.0 || opt.sample_voices < 1) {
    error = "invalid render options";
    return false;
  }
  auto br = std::make_unique<BlockRenderer>(8);
  SampleEngine::Config sc;
  sc.max_voices = opt.sample_voices;
  sc.sample_rate = kSampleRate;
  sc.seed = opt.seed;
  SampleEngine sample(sc);
  OrganEngine organ;
  if (!br->bind_engine(sample) || !br->bind_engine(organ)) {
    error = "cannot bind the engines";
    return false;
  }
  host::ControlEventQueue unused_events;  // parameter commands never come through here
  host::ControlPlane plane(br->snapshots(), br->last_values(), unused_events, [] { return std::uint64_t{0}; });
  host::LibraryResolver resolver(host::make_posix_fs(), opt.library_root);
  host::RamHeadProvider heads;
  host::SceneController scenes(plane);
  host::LiveSetLoaderOptions lo;
  lo.scenes = &scenes;
  lo.part_count = opt.part_count;
  lo.is_rendering = [] { return false; };  // the offline renderer renders on this thread, after the load
  host::LiveSetLoader loader(*br, plane, resolver, heads, &sample, lo);
  const host::LiveSetLoadResult loaded = loader.load_file(opt.live_set_path);
  if (!loaded.ok()) {
    error = std::string("live set: ") + host::to_string(loaded.status);
    if (!loaded.detail.empty()) error += " (" + loaded.detail + ")";
    if (loaded.status == host::LiveSetLoadStatus::kInvalidLiveSet)
      error += ", " + std::to_string(loaded.report.errors.size()) + " validation error(s)";
    return false;
  }
  if (report) {
    report->part_states.clear();
    for (const host::PartLoadReport& p : loaded.parts) report->part_states.push_back(p.state);
  }

  // Controllers: sustain goes through as it is; the registry-mapped ones become parameter events of the part that is their channel.
  auto control = [](const NoteEvent& e, Ump& u) {
    const std::uint32_t v32 = static_cast<std::uint32_t>(e.value * 0xFFFFFFFFull / 127);
    const Ump cc = make_control_change(0, e.channel, e.note, v32);
    if (e.note == Ump::kControllerSustain) {
      u = cc;
      return true;
    }
    return e.channel < params::kPartCount && cc_to_param_event(cc, u);
  };
  std::uint64_t last = 0;
  for (const NoteEvent& e : events) {
    Ump u;
    if (e.contact) continue;  // contact_k events are an organ keybed feature: not zone-routed (see part_router.hpp)
    if (!e.cc || control(e, u)) last = std::max(last, e.frame);
  }
  const std::uint64_t tail = static_cast<std::uint64_t>(std::llround(opt.tail_seconds * kSampleRate));
  const std::uint64_t total = last + tail;
  if (total > static_cast<std::uint64_t>(kSampleRate) * 3600) {
    error = "render would exceed one hour";
    return false;
  }
  const std::uint64_t blocks = (total + kBlockFrames) / kBlockFrames;
  out.sample_rate = kSampleRate;
  out.channels = 2;
  out.samples.assign(static_cast<std::size_t>(blocks * kBlockFrames * 2), 0.0f);

  std::vector<SceneRecallAt> recalls = opt.scene_recalls;
  std::stable_sort(recalls.begin(), recalls.end(), [](const SceneRecallAt& a, const SceneRecallAt& b) { return a.frame < b.frame; });
  for (const SceneRecallAt& rc : recalls)
    if (rc.index >= scenes.scene_count()) {
      error = "--scene-at: scene " + std::to_string(rc.index) + " does not exist (the Live Set has " + std::to_string(scenes.scene_count()) + " scene" +
              (scenes.scene_count() == 1 ? "" : "s") + ")";
      return false;
    }
  std::size_t next_recall = 0;
  std::uint64_t recalled = 0;
  float l[kBlockFrames], r[kBlockFrames];
  std::size_t next = 0;
  for (std::uint64_t b = 0; b < blocks; ++b) {
    // Scene recalls land at the start of the block that holds their time (the last block included), before that block's events
    // (control thread, between blocks): the resolution is one block, 1.3 ms.
    while (next_recall < recalls.size() && recalls[next_recall].frame < (b + 1) * kBlockFrames) {
      const host::SceneOutcome o = scenes.recall_scene(recalls[next_recall].index);
      if (!o.ok()) {
        error = "scene " + std::to_string(recalls[next_recall].index) + ": " + host::to_string(o.result);
        return false;
      }
      if (o.published) ++recalled;
      ++next_recall;
    }
    const std::uint64_t end = (b + 1) * kBlockFrames;
    while (next < events.size() && events[next].frame < end) {
      const NoteEvent& e = events[next++];
      if (e.contact) continue;
      const std::uint32_t off = static_cast<std::uint32_t>(e.frame - b * kBlockFrames);
      Ump u;
      if (e.cc) {
        if (!control(e, u)) continue;
      } else {
        u = e.on ? make_note_on(0, e.channel, e.note, e.velocity16) : make_note_off(0, e.channel, e.note, e.velocity16);
      }
      if (!br->submit({u, off})) {
        error = "event queue overflow (too many events in one block)";
        return false;
      }
    }
    // The single-output overload folds the Aux bus into Main (Main + Aux), so a part on Aux is heard.
    br->process_block(l, r);
    if (br->status_count(EngineStatus::kEventOverflow) != 0) {
      error = "event buffer overflow (too many events in one block)";
      return false;
    }
    float* dst = &out.samples[static_cast<std::size_t>(b * kBlockFrames * 2)];
    for (std::uint32_t i = 0; i < kBlockFrames; ++i) {
      dst[2 * i] = l[i];
      dst[2 * i + 1] = r[i];
    }
  }
  if (next_recall < recalls.size()) {  // a recall after the last block would silently do nothing
    error = "--scene-at time is after the end of the render (last note plus tail, rounded up to a 64-frame block)";
    return false;
  }
  if (report) {
    report->pool_exhausted = br->status_count(EngineStatus::kPoolExhausted);
    report->dropped_out_of_range = br->router().dropped_out_of_range();
    report->unzoned_keys = br->router().unzoned_keys();
    report->scenes_recalled = recalled;
    report->generations_published = recalled;
  }
  return true;
}

}  // namespace velora::offline
