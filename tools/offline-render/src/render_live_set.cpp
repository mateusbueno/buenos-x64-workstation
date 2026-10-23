#include "render_live_set.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "velora/core/assign_events.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/live_set_store.hpp"
#include "velora/host/live_set_switcher.hpp"
#include "velora/host/scene_controller.hpp"
#include "velora/host/setlist_controller.hpp"
#include "velora/host/setlist_store.hpp"

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
  host::RamHeadProvider ram_heads;
  host::HeadProvider& heads = opt.heads ? *opt.heads : static_cast<host::HeadProvider&>(ram_heads);
  if (opt.protected_trust) resolver.set_protected_trust(opt.protected_trust);
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

  // Seamless switches (Story 4.5, Story 4.8): the switcher runs without its worker thread (the stage runs inside jump_to) and holds the commit for the
  // simulated load time, so the render stays deterministic. Every target file is read up front: an unreadable one is an error before any rendering.
  std::vector<SwitchAt> switches = opt.switches;
  std::stable_sort(switches.begin(), switches.end(), [](const SwitchAt& a, const SwitchAt& b) { return a.frame < b.frame; });
  std::unique_ptr<host::LiveSetSwitcher> switcher;
  std::vector<std::string> switch_ids(switches.size());

  struct TempDirGuard {
    std::string path;
    std::unique_ptr<host::setlist::SetlistStore>* setlist_store = nullptr;
    std::unique_ptr<host::liveset::LiveSetStore>* live_store = nullptr;
    std::unique_ptr<host::SetlistController>* ctl = nullptr;
    ~TempDirGuard() {
      if (ctl && *ctl) ctl->reset();
      if (setlist_store && *setlist_store) setlist_store->reset();
      if (live_store && *live_store) live_store->reset();
      if (!path.empty()) {
        ::unlink((path + "/setlists/.writer.lock").c_str());
        ::rmdir((path + "/setlists").c_str());
        ::unlink((path + "/livesets/.writer.lock").c_str());
        ::rmdir((path + "/livesets").c_str());
        ::rmdir(path.c_str());
      }
    }
  } temp_guard;

  const bool needs_setlist = !opt.setlist_loads.empty() || !opt.setlist_steps.empty() || !opt.setlist_dir.empty();
  if (!switches.empty() || needs_setlist) {
    host::SwitcherOptions so;
    so.background_thread = false;
    so.hold_ticks = opt.switch_load_blocks;
    switcher = std::make_unique<host::LiveSetSwitcher>(loader, plane, so);
    for (std::size_t i = 0; i < switches.size(); ++i) {
      host::CatalogEntry e;
      std::string cerr;
      if (!host::catalog_entry_for_file(switches[i].live_set_path, opt.part_count, e, cerr)) {
        error = "--switch-at: " + cerr;
        return false;
      }
      switch_ids[i] = e.id;
      switcher->add_to_catalog(std::move(e));
    }
  }

  std::unique_ptr<host::setlist::SetlistStore> setlist_store;
  std::unique_ptr<host::liveset::LiveSetStore> live_set_store;
  std::unique_ptr<host::LiveSetSwitcherPort> switch_port;
  std::unique_ptr<host::SetlistController> setlist_controller;
  temp_guard.setlist_store = &setlist_store;
  temp_guard.live_store = &live_set_store;
  temp_guard.ctl = &setlist_controller;
  if (needs_setlist) {
    std::string sldir = opt.setlist_dir;
    if (sldir.empty()) {
      char t[] = "/tmp/velora_offline_sl_XXXXXX";
      if (::mkdtemp(t) == nullptr) {
        error = "failed to create temporary setlist store directory";
        return false;
      }
      temp_guard.path = t;
      sldir = temp_guard.path;
      ::mkdir((sldir + "/setlists").c_str(), 0755);
      ::mkdir((sldir + "/livesets").c_str(), 0755);
    }
    host::setlist::StoreOptions sso;
    sso.dir = sldir + "/setlists";
    if (host::setlist::SetlistStore::open(sso, setlist_store) != host::setlist::Status::kOk) {
      error = "failed to open setlist store: " + sso.dir;
      return false;
    }
    host::liveset::StoreOptions lso;
    lso.dir = sldir + "/livesets";
    if (host::liveset::LiveSetStore::open(lso, live_set_store) != host::liveset::Status::kOk) {
      error = "failed to open liveset store: " + lso.dir;
      return false;
    }
    if (switcher) {
      switch_port = std::make_unique<host::LiveSetSwitcherPort>(*switcher);
      setlist_controller = std::make_unique<host::SetlistController>(plane, *setlist_store, *live_set_store, *switch_port);
    }
  }

  // Controllers: sustain goes through as it is; the registry-mapped ones become parameter events of the part that is their channel;
  // every other one goes through as the control change itself (Story 5.2: the keyboard's controllers reach the parts' CcState).
  auto control = [](const NoteEvent& e, Ump& u) {
    const std::uint32_t v32 = static_cast<std::uint32_t>(e.value * 0xFFFFFFFFull / 127);
    const Ump cc = make_control_change(0, e.channel, e.note, v32);
    if (e.note == Ump::kControllerSustain) {
      u = cc;
      return true;
    }
    if (e.channel < params::kPartCount && cc_to_param_event(cc, u)) return true;
    u = cc;
    return true;
  };
  std::uint64_t last = 0;
  for (const NoteEvent& e : events) {
    Ump u;
    if (e.contact) continue;  // contact_k events are an organ keybed feature: not zone-routed (see part_router.hpp)
    if (!e.cc || control(e, u)) last = std::max(last, e.frame);
  }
  std::vector<EncoderAt> encoders = opt.encoders;
  std::stable_sort(encoders.begin(), encoders.end(), [](const EncoderAt& a, const EncoderAt& b) { return a.frame < b.frame; });
  for (const EncoderAt& e : encoders) {
    if (e.encoder < 1 || e.encoder > kEncoderCount || e.delta < -32768 || e.delta > 32767) {
      error = "invalid encoder event (encoder 1..8, delta -32768..32767)";
      return false;
    }
    last = std::max(last, e.frame);
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
  std::vector<SetlistLoadAt> setlist_loads = opt.setlist_loads;
  std::stable_sort(setlist_loads.begin(), setlist_loads.end(), [](const SetlistLoadAt& a, const SetlistLoadAt& b) { return a.frame < b.frame; });
  std::vector<SetlistStepAt> setlist_steps = opt.setlist_steps;
  std::stable_sort(setlist_steps.begin(), setlist_steps.end(), [](const SetlistStepAt& a, const SetlistStepAt& b) { return a.frame < b.frame; });

  std::size_t next_recall = 0, next_switch = 0, next_encoder = 0;
  std::size_t next_setlist_load = 0, next_setlist_step = 0;
  std::uint64_t recalled = 0;
  float l[kBlockFrames], r[kBlockFrames];
  std::size_t next = 0;
  for (std::uint64_t b = 0; b < blocks; ++b) {
    // Setlist actions land between blocks at the block that holds their time.
    while (next_setlist_load < setlist_loads.size() && setlist_loads[next_setlist_load].frame < (b + 1) * kBlockFrames) {
      if (setlist_controller) {
        setlist_controller->load_setlist(setlist_loads[next_setlist_load].setlist_id);
        if (report) ++report->setlists_loaded;
      }
      ++next_setlist_load;
    }
    while (next_setlist_step < setlist_steps.size() && setlist_steps[next_setlist_step].frame < (b + 1) * kBlockFrames) {
      if (setlist_controller) {
        setlist_controller->step(setlist_steps[next_setlist_step].next);
        if (report) ++report->setlist_steps_taken;
      }
      ++next_setlist_step;
    }
    if (setlist_controller) {
      setlist_controller->tick();
    }

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
    // Live Set switches: the jump is made between blocks at the block that holds its time; the commit follows after the simulated load time.
    while (next_switch < switches.size() && switches[next_switch].frame < (b + 1) * kBlockFrames) {
      const host::SwitchOutcome jo = switcher->jump_to(switch_ids[next_switch]);
      if (jo.superseded_previous) {  // two switches closer together than the simulated load: the first would be silently cancelled
        error = "switch superseded before completion: --switch-at " + switches[next_switch].live_set_path +
                " arrived while the previous switch was still loading (raise the time between them or lower --switch-load-blocks)";
        return false;
      }
      ++next_switch;
    }
    if (switcher) {
      switcher->tick();
      if (switcher->status().state == host::SwitchState::kFailed) {
        error = std::string("switch: ") + host::to_string(switcher->status().last_error) + " (" + switcher->status().last_error_detail + ")";
        return false;
      }
    }
    const std::uint64_t end = (b + 1) * kBlockFrames;
    if (b == 0 && opt.assign_mode) br->submit({make_encoder_mode_event(EncoderMode::kAssign), 0});  // before anything else in the render
    // Encoder turns: EncoderStep UMPs at their exact frames (the panel's path), before the keyboard events that share the block.
    while (next_encoder < encoders.size() && encoders[next_encoder].frame < end) {
      const EncoderAt& e = encoders[next_encoder++];
      const std::uint32_t off = static_cast<std::uint32_t>(e.frame - b * kBlockFrames);
      if (!br->submit({make_encoder_step_event(e.encoder - 1, e.delta), off})) {
        error = "event queue overflow (too many events in one block)";
        return false;
      }
    }
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
    if (switcher) plane.reclaim();  // free the generations nobody plays any more, as the control server's tick does
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
  if (next_switch < switches.size()) {
    error = "--switch-at time is after the end of the render (last note plus tail, rounded up to a 64-frame block)";
    return false;
  }
  if (switcher && switcher->loading()) {
    error = "--switch-at: the switch did not complete before the end of the render (--switch-load-blocks is longer than the rest of the render)";
    return false;
  }
  if (report) {
    report->switches_published = switcher ? switcher->switches_published() : 0;
    report->pool_exhausted = br->status_count(EngineStatus::kPoolExhausted);
    report->dropped_out_of_range = br->router().dropped_out_of_range();
    report->unzoned_keys = br->router().unzoned_keys();
    report->scenes_recalled = recalled;
    report->generations_published = recalled + report->switches_published;
    const EncoderState::Counters c = br->encoder_state().counters();
    report->encoder_steps_applied = c.steps_applied;
    report->encoder_steps_empty = c.steps_empty_slot;
    report->encoder_steps_unrouted = c.steps_unrouted;
    report->encoder_steps_rejected = c.steps_rejected;
  }
  return true;
}

}  // namespace velora::offline
