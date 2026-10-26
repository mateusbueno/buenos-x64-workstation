// Tier profile (Story 4.1, FR17, AR24, NFR39, AD-27): the two constant profiles and their numbers, the one-time tier resolution (strap, no strap, dev
// override, unreadable strap), the part cap and the State.tier publication of the control plane, and the data-driven rule (the tier ID is branched on in
// the profile table only, tools/ci/check_tier_branching.py runs as its own ctest). The loader / render side (16 parts render on Pro, v1 refuses them, the
// budgets reach the arbiter and the head budget) is in live_set_loader_test.cpp.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#include "velora/core/block_renderer.hpp"
#include "velora/core/fm_engine.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/va_engine.hpp"
#include "velora/core/voice_arbiter.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/tier_profile.hpp"

using namespace velora;
using host::TierProfile;
using ports::TierId;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

namespace {

// A tier source that counts its reads.
class FakeSource final : public ports::TierSource {
 public:
  FakeSource(bool readable, TierId tier, std::string reason = {}) {
    r_.readable = readable;
    r_.tier = tier;
    r_.reason = std::move(reason);
  }
  ports::TierRead read() noexcept override {
    ++reads;
    return r_;
  }
  int reads = 0;

 private:
  ports::TierRead r_;
};

void test_profile_data() {
  const TierProfile& v1 = host::v1_profile();
  const TierProfile& pro = host::pro_profile();
  CHECK(v1.id == TierId::kV1 && pro.id == TierId::kPro);
  CHECK(std::string(v1.name) == "v1" && std::string(pro.name) == "pro");
  // v1: 8 parts, head budget 5 GB (inside the 4-6 GB range of NFR39), the budgets the engine shipped with, no sequencer.
  CHECK(v1.part_count == 8 && v1.head_budget_bytes == 5ull << 30);
  CHECK(v1.head_budget_bytes >= 4ull << 30 && v1.head_budget_bytes <= 6ull << 30);
  CHECK(v1.arbiter_budget == (1u << 20) && v1.sample_floor_voices == 256 && v1.fm_floor_voices == 128 && v1.va_floor_voices == 64);
  CHECK(v1.fx_budget == core::max_fx_cost_for_parts(8) && v1.sample_voice_capacity == 340 && !v1.sequencer_enabled);
  // Pro: 16 parts, 2x the v1 head, voice and cost budgets (the user's decision), sequencer module enabled.
  CHECK(pro.part_count == 16 && pro.head_budget_bytes == 10ull << 30);
  CHECK(pro.arbiter_budget == 2 * v1.arbiter_budget);
  CHECK(pro.sample_floor_voices == 512 && pro.fm_floor_voices == 2 * v1.fm_floor_voices && pro.va_floor_voices == 2 * v1.va_floor_voices);
  CHECK(pro.fx_budget == core::max_fx_cost_for_parts(16) && pro.fx_budget > v1.fx_budget);
  CHECK(pro.sample_voice_capacity >= pro.sample_floor_voices + core::VoiceArbiter::kFadeHeadroomVoices);
  CHECK(v1.sample_voice_capacity >= v1.sample_floor_voices + core::VoiceArbiter::kFadeHeadroomVoices);
  CHECK(pro.sequencer_enabled);
  // The structural capacity of the build carries both tiers (one image, no fork).
  CHECK(core::params::kPartCount == 16 && v1.part_count <= core::params::kPartCount && pro.part_count <= core::params::kPartCount);
  // profile_for is the one mapping, and the instances are the constants.
  CHECK(&host::profile_for(TierId::kV1) == &v1 && &host::profile_for(TierId::kPro) == &pro);
  TierId t = TierId::kV1;
  CHECK(host::parse_tier_name("pro", t) && t == TierId::kPro && host::parse_tier_name("v1", t) && t == TierId::kV1);
  CHECK(!host::parse_tier_name("PRO", t) && !host::parse_tier_name("", t) && !host::parse_tier_name("v2", t));
}

// The arbiter needs floor + kFadeHeadroomVoices <= the engine's voice capacity: for BOTH profiles, with the engines built from the profile as engine-host does.
void test_floors_fit_engine_capacities() {
  for (const TierProfile* t : {&host::v1_profile(), &host::pro_profile()}) {
    const std::uint32_t head = core::VoiceArbiter::kFadeHeadroomVoices;
    CHECK(t->sample_voice_capacity >= t->sample_floor_voices + head);
    core::FmEngine fm(48000.0f, t->fm_voice_capacity);
    core::VaEngine va(48000.0f, t->va_voice_capacity);
    core::OrganEngine organ(t->part_count, t->organ_voice_capacity);
    CHECK(fm.voice_capacity() == t->fm_voice_capacity && fm.voice_capacity() >= t->fm_floor_voices + head);
    CHECK(va.voice_capacity() == t->va_voice_capacity && va.voice_capacity() >= t->va_floor_voices + head);
    CHECK(organ.voice_capacity() == t->organ_voice_capacity && organ.voice_capacity() >= t->part_count * 73u);  // every key of every part
    // The organ runs generators for exactly the tier's parts: its last part is available, the next one is refused.
    CHECK(organ.start_voice(t->part_count - 1, 60, 40000) == core::EngineStatus::kOk);
    CHECK(organ.start_voice(t->part_count, 60, 40000) == core::EngineStatus::kEngineNotAvailable);
  }
  // v1 is byte-identical to the pre-4.1 engines: the default constructors are the v1 profile.
  CHECK(host::v1_profile().fm_voice_capacity == core::FmEngine::kVoiceCapacity && host::v1_profile().va_voice_capacity == core::VaEngine::kVoiceCapacity);
  CHECK(host::v1_profile().organ_voice_capacity == core::OrganEngine::kVoiceCapacity && core::OrganEngine().voice_capacity() == core::OrganEngine::kVoiceCapacity);
}

void test_resolution() {
  // v1 default: the Mac has no strap, nothing else says otherwise.
  host::NoStrapTierSource mac;
  host::TierSelection s = host::resolve_tier(&mac, std::nullopt);
  CHECK(s.profile == &host::v1_profile() && s.origin == "default" && !s.reason.empty());
  CHECK(s.log_line.find("v1") != std::string::npos && s.log_line.find(s.reason) != std::string::npos);
  s = host::resolve_tier(nullptr, std::nullopt);  // no source at all
  CHECK(s.profile == &host::v1_profile() && s.origin == "default");
  // Dev override: Pro on the Mac, with a loud log line; the source is not even read.
  FakeSource unread(true, TierId::kV1);
  s = host::resolve_tier(&unread, TierId::kPro);
  CHECK(s.profile == &host::pro_profile() && s.origin == "override" && unread.reads == 0);
  CHECK(s.log_line.find("DEV OVERRIDE") != std::string::npos && s.log_line.find("pro") != std::string::npos);
  s = host::resolve_tier(&mac, TierId::kV1);
  CHECK(s.profile == &host::v1_profile() && s.origin == "override");
  // The strap says Pro: read exactly once.
  FakeSource strap(true, TierId::kPro);
  s = host::resolve_tier(&strap, std::nullopt);
  CHECK(s.profile == &host::pro_profile() && s.origin == "strap" && strap.reads == 1);
  // The strap says v1.
  FakeSource strap1(true, TierId::kV1);
  CHECK(host::resolve_tier(&strap1, std::nullopt).profile == &host::v1_profile() && strap1.reads == 1);
  // An unreadable strap: v1, with the logged reason (the tier value it carries is ignored).
  FakeSource bad(false, TierId::kPro, "OTP fuse bank not readable");
  s = host::resolve_tier(&bad, std::nullopt);
  CHECK(s.profile == &host::v1_profile() && s.origin == "default" && s.reason == "OTP fuse bank not readable" && s.log_line.find(s.reason) != std::string::npos);
  CHECK(bad.reads == 1);
  // An ID this build does not know is as good as unreadable: v1 and the reason.
  FakeSource odd(true, static_cast<TierId>(9));
  s = host::resolve_tier(&odd, std::nullopt);
  CHECK(s.profile == &host::v1_profile() && s.origin == "default" && s.reason.find("unknown tier id 9") != std::string::npos && odd.reads == 1);
}

// A control plane on a tier: the part cap, the State and the FX budget follow the profile.
struct PlaneRig {
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>();
  host::ControlEventQueue q;
  host::ControlPlane plane{br->snapshots(), br->last_values(), q, [] { return std::uint64_t{0}; }};
  explicit PlaneRig(const TierProfile* tier) {
    if (tier) plane.set_tier_profile(*tier);
    std::string err;
    CHECK(plane.load_initial(ws::v1::State{}, err));
  }
  ws::v1::ServerMessage part_output(std::uint32_t part) {
    ws::v1::ClientMessage m;
    m.set_request_id(1);
    m.mutable_set_part_output()->set_part(part);
    m.mutable_set_part_output()->set_output(ws::v1::PART_OUTPUT_AUX);
    bool changed = false;
    return plane.handle(m, changed);
  }
};

void test_state_publication_and_part_cap() {
  // v1 (the default of a plane): State carries 8 parts and tier v1; part 8 is out of range.
  PlaneRig v1(nullptr);
  ws::v1::State st;
  v1.plane.fill_state(&st);
  CHECK(st.has_tier() && st.tier().tier() == ws::v1::TIER_V1 && st.tier().name() == "v1");
  CHECK(st.tier().part_count() == 8 && st.parts_size() == 8);
  CHECK(st.tier().head_budget_bytes() == host::v1_profile().head_budget_bytes && st.tier().arbiter_budget() == host::v1_profile().arbiter_budget);
  CHECK(st.tier().fx_budget() == host::v1_profile().fx_budget && !st.tier().sequencer_enabled());
  CHECK(st.tier().sample_floor_voices() == 256 && st.tier().fm_floor_voices() == 128 && st.tier().va_floor_voices() == 64);
  CHECK(v1.plane.fx_budget() == host::v1_profile().fx_budget && v1.plane.part_cap() == 8);
  ws::v1::ServerMessage r = v1.part_output(8);
  CHECK(r.has_error() && r.error().code() == ws::v1::ERROR_CODE_INVALID_PART);
  CHECK(v1.part_output(7).has_ack());
  // Pro: 16 parts in the State, the Pro numbers, parts 8..15 accepted, 16 refused.
  PlaneRig pro(&host::pro_profile());
  ws::v1::State sp;
  pro.plane.fill_state(&sp);
  CHECK(sp.tier().tier() == ws::v1::TIER_PRO && sp.tier().name() == "pro" && sp.tier().part_count() == 16 && sp.parts_size() == 16);
  CHECK(sp.tier().head_budget_bytes() == 2 * st.tier().head_budget_bytes() && sp.tier().arbiter_budget() == 2 * st.tier().arbiter_budget());
  CHECK(sp.tier().sample_floor_voices() == 512 && sp.tier().sequencer_enabled());
  CHECK(sp.tier().fx_budget() == host::pro_profile().fx_budget && pro.plane.fx_budget() == host::pro_profile().fx_budget);
  CHECK(pro.part_output(15).has_ack());
  r = pro.part_output(16);
  CHECK(r.has_error() && r.error().code() == ws::v1::ERROR_CODE_INVALID_PART);
  // The per-part live values and performance_init cover exactly the tier's parts.
  ws::v1::PerformanceValues pv;
  pro.plane.fill_live_values(&pv);
  ws::v1::PerformanceValues pv1;
  v1.plane.fill_live_values(&pv1);
  // Exact counts: part-scope performance parameters are listed once per part, the others once.
  std::size_t part_scope = 0, other = 0;
  for (const core::params::ParamDef& d : core::params::kParams) {
    if (d.cls != core::params::Class::kPerformance) continue;
    (d.scope == core::params::Scope::kPart ? part_scope : other) += 1;
  }
  CHECK(part_scope > 0);
  CHECK(static_cast<std::size_t>(pv1.values_size()) == part_scope * 8 + other);
  CHECK(static_cast<std::size_t>(pv.values_size()) == part_scope * 16 + other);
  CHECK(static_cast<std::size_t>(st.performance_init_size()) == part_scope * 8 + other);
  CHECK(static_cast<std::size_t>(sp.performance_init_size()) == part_scope * 16 + other);
  for (int i = 0; i < sp.parts_size(); ++i) CHECK(sp.parts(i).part() == static_cast<std::uint32_t>(i));
  for (int i = 0; i < st.parts_size(); ++i) CHECK(st.parts(i).part() == static_cast<std::uint32_t>(i));
}

// The tier is set once: a second set_tier_profile is ignored (the profile, the part cap and the FX budget stay).
void test_set_tier_profile_is_one_shot() {
  PlaneRig rig(&host::pro_profile());
  rig.plane.set_tier_profile(host::v1_profile());
  CHECK(&rig.plane.tier_profile() == &host::pro_profile() && rig.plane.part_cap() == 16 && rig.plane.fx_budget() == host::pro_profile().fx_budget);
  ws::v1::State st;
  rig.plane.fill_state(&st);
  CHECK(st.tier().tier() == ws::v1::TIER_PRO && st.parts_size() == 16);
  // The default (v1) counts as unset only until the first call: a plane that was never given a profile can still take Pro once.
  PlaneRig d(nullptr);
  CHECK(&d.plane.tier_profile() == &host::v1_profile());
}

}  // namespace

int main() {
  test_profile_data();
  test_floors_fit_engine_capacities();
  test_resolution();
  test_set_tier_profile_is_one_shot();
  test_state_publication_and_part_cap();
  if (g_fail) {
    std::printf("tier_profile_tests: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("tier_profile_tests: ok\n");
  return 0;
}
