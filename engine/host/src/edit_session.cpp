// Edits in memory and Write scopes, Story 4.9. Control thread only; see velora/host/edit_session.hpp for the model.
#include "velora/host/edit_session.hpp"

#include <cstdio>

#include "live_set_detail.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <thread>
#include <unordered_map>

namespace velora::host {

namespace p = core::params;
namespace lsp = ::velora::liveset::v1;
namespace sl = ::velora::host::setlist;
using B = ws::v1::ClientMessage;

namespace {

using Entries = google::protobuf::RepeatedPtrField<lsp::ParamEntry>;

constexpr std::uint32_t kSendReverb = p::kSendReverbLevel;
constexpr std::uint32_t kSendDelay = p::kSendDelayLevel;
constexpr std::size_t kSummaryItems = 32;     // items published in State.edits / WriteReply
constexpr std::size_t kDirtyIdsPublished = 128;  // ids published in State.edits

bool fail(HandlerFailure& e, ws::v1::ErrorCode code, std::string msg, std::map<std::string, std::string> details = {}) {
  e.code = code;
  e.message = std::move(msg);
  e.details = std::move(details);
  return false;
}

double eps_for(const p::ParamDef* d) {
  if (!d) return 0.0;
  if (d->type == p::Type::kFloat) return 1e-6 * (d->max - d->min);
  return 0.25;  // whole steps
}
bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

std::string pretty_param(std::uint32_t id) {
  std::string n = ws::v1::ParamId_Name(static_cast<ws::v1::ParamId>(id));
  if (n.rfind("PARAM_", 0) == 0) n = n.substr(6);
  if (n.empty() || n == "ParamId_Name") n = "parameter " + std::to_string(id);
  for (char& c : n) c = c == '_' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return n;
}

// ---- parameter lists -----------------------------------------------------------------------------------------------------------
// The loader applies engine params, then envelope, then eq: the LAST list that holds an id decides.
const lsp::ParamEntry* last_entry(const Entries& l, std::uint32_t id) {
  const lsp::ParamEntry* r = nullptr;
  for (const lsp::ParamEntry& e : l)
    if (e.param_id() == id) r = &e;
  return r;
}
lsp::ParamEntry* last_entry_mut(Entries* l, std::uint32_t id) {
  lsp::ParamEntry* r = nullptr;
  for (lsp::ParamEntry& e : *l)
    if (e.param_id() == id) r = &e;
  return r;
}

std::optional<float> part_list_value(const lsp::Part& pt, std::uint32_t id) {
  if (const lsp::ParamEntry* e = last_entry(pt.eq(), id)) return e->value();
  if (const lsp::ParamEntry* e = last_entry(pt.envelope(), id)) return e->value();
  if (const lsp::ParamEntry* e = last_entry(pt.engine().params(), id)) return e->value();
  return std::nullopt;
}
std::optional<float> scene_list_value(const lsp::ScenePart& sp, std::uint32_t id) {
  if (const lsp::ParamEntry* e = last_entry(sp.eq(), id)) return e->value();
  if (const lsp::ParamEntry* e = last_entry(sp.envelope(), id)) return e->value();
  if (const lsp::ParamEntry* e = last_entry(sp.engine_params(), id)) return e->value();
  return std::nullopt;
}

// What the generation's initial value of a part-scope performance parameter is for this part of the base Live Set.
float base_part_value(const lsp::Part& pt, std::uint32_t id) {
  if (auto v = part_list_value(pt, id)) return *v;
  if (id == kSendReverb) return pt.send_reverb();
  if (id == kSendDelay) return pt.send_delay();
  const p::ParamDef* d = p::find(id);
  return d ? static_cast<float>(d->def) : 0.0f;
}
// What the loader turns a file value into (clamped to the registry range, non-finite takes the default): the generation's initial value, which is what
// RT's live value is compared with.
float normalised(std::uint32_t id, float v) {
  const p::ParamDef* d = p::find(id);
  return d ? detail::sane(*d, v) : v;
}

// The value a scene stores for the parameter, if it stores it.
std::optional<float> scene_part_value(const lsp::ScenePart& sp, std::uint32_t id) {
  if (auto v = scene_list_value(sp, id)) return v;
  if (id == kSendReverb && sp.has_send_reverb()) return sp.send_reverb();
  if (id == kSendDelay && sp.has_send_delay()) return sp.send_delay();
  return std::nullopt;
}

bool set_in_scene_part(lsp::ScenePart& sp, std::uint32_t id, float v) {
  if (lsp::ParamEntry* e = last_entry_mut(sp.mutable_eq(), id)) return e->set_value(v), true;
  if (lsp::ParamEntry* e = last_entry_mut(sp.mutable_envelope(), id)) return e->set_value(v), true;
  if (lsp::ParamEntry* e = last_entry_mut(sp.mutable_engine_params(), id)) return e->set_value(v), true;
  if (id == kSendReverb && sp.has_send_reverb()) return sp.set_send_reverb(v), true;
  if (id == kSendDelay && sp.has_send_delay()) return sp.set_send_delay(v), true;
  return false;
}
void set_in_base_part(lsp::Part& pt, std::uint32_t id, float v) {
  if (lsp::ParamEntry* e = last_entry_mut(pt.mutable_eq(), id)) return e->set_value(v);
  if (lsp::ParamEntry* e = last_entry_mut(pt.mutable_envelope(), id)) return e->set_value(v);
  if (lsp::ParamEntry* e = last_entry_mut(pt.mutable_engine()->mutable_params(), id)) return e->set_value(v);
  if (id == kSendReverb) return pt.set_send_reverb(v);
  if (id == kSendDelay) return pt.set_send_delay(v);
  lsp::ParamEntry* e = pt.mutable_engine()->add_params();
  e->set_param_id(id);
  e->set_value(v);
}

float base_master_value(const lsp::LiveSet& ls, std::uint32_t id) {
  if (id == p::kTempoBpm) return ls.tempo();
  if (const lsp::ParamEntry* e = last_entry(ls.master(), id)) return e->value();
  const p::ParamDef* d = p::find(id);
  return d ? static_cast<float>(d->def) : 0.0f;
}

lsp::ScenePart* scene_part_mut(lsp::LiveSet& ls, int scene, std::uint32_t part) {
  if (scene < 0 || scene >= ls.scenes_size()) return nullptr;
  lsp::Scene* sc = ls.mutable_scenes(scene);
  return static_cast<int>(part) < sc->parts_size() ? sc->mutable_parts(static_cast<int>(part)) : nullptr;
}

// The value of a part-scope performance parameter in the edited state for the given current scene (-1 = none).
float edited_part_value(const lsp::LiveSet& ls, int scene, std::uint32_t part, std::uint32_t id) {
  if (scene >= 0 && scene < ls.scenes_size() && static_cast<int>(part) < ls.scenes(scene).parts_size())
    if (auto v = scene_part_value(ls.scenes(scene).parts(static_cast<int>(part)), id)) return normalised(id, *v);
  return normalised(id, base_part_value(ls.parts(static_cast<int>(part)), id));
}
float edited_master_value(const lsp::LiveSet& ls, int scene, std::uint32_t id) {
  if (id != p::kTempoBpm && scene >= 0 && scene < ls.scenes_size())
    if (const lsp::ParamEntry* e = last_entry(ls.scenes(scene).master(), id)) return normalised(id, e->value());
  return normalised(id, base_master_value(ls, id));
}

// Stores a part-scope performance value where the edit belongs: the current scene when it stores the parameter, else the base.
void store_part_value(lsp::LiveSet& ls, int scene, std::uint32_t part, std::uint32_t id, float v) {
  if (lsp::ScenePart* sp = scene_part_mut(ls, scene, part))
    if (set_in_scene_part(*sp, id, v)) return;
  set_in_base_part(*ls.mutable_parts(static_cast<int>(part)), id, v);
}
void store_master_value(lsp::LiveSet& ls, int scene, std::uint32_t id, float v) {
  if (id == p::kTempoBpm) return ls.set_tempo(v);
  if (scene >= 0 && scene < ls.scenes_size())
    if (lsp::ParamEntry* e = last_entry_mut(ls.mutable_scenes(scene)->mutable_master(), id)) return e->set_value(v);
  if (lsp::ParamEntry* e = last_entry_mut(ls.mutable_master(), id)) return e->set_value(v);
  lsp::ParamEntry* e = ls.add_master();
  e->set_param_id(id);
  e->set_value(v);
}

void copy_slots(const google::protobuf::RepeatedPtrField<ws::v1::InsertSlot>& from, google::protobuf::RepeatedPtrField<ws::v1::InsertSlot>* to) {
  to->Clear();
  for (const ws::v1::InsertSlot& s : from) to->Add()->CopyFrom(s);
}

// ---- diff ------------------------------------------------------------------------------------------------------------------------------
float slot_param(const ws::v1::InsertSlot& s, int k) {
  if (k < s.params_size() && std::isfinite(s.params(k))) return s.params(k);
  const p::ParamDef* d = p::find(p::kInsertParam1 + static_cast<std::uint32_t>(k));
  return d ? static_cast<float>(d->def) : 0.5f;
}
bool slots_equal(const google::protobuf::RepeatedPtrField<ws::v1::InsertSlot>& a, const google::protobuf::RepeatedPtrField<ws::v1::InsertSlot>& b) {
  if (a.size() != b.size()) return false;
  for (int i = 0; i < a.size(); ++i) {
    if (a[i].type() != b[i].type() || a[i].bypass() != b[i].bypass()) return false;
    for (int k = 0; k < static_cast<int>(p::kInsertParamCount); ++k)
      if (!near(slot_param(a[i], k), slot_param(b[i], k), 1e-6)) return false;
  }
  return true;
}

std::string timbre_key(const lsp::Part& pt) { return pt.has_timbre() ? pt.timbre().library_id() + "\n" + pt.timbre().instrument_key() : std::string("\n"); }

void collect_ids(const lsp::Part& pt, std::vector<std::uint32_t>& ids) {
  for (const lsp::ParamEntry& e : pt.engine().params()) ids.push_back(e.param_id());
  for (const lsp::ParamEntry& e : pt.envelope()) ids.push_back(e.param_id());
  for (const lsp::ParamEntry& e : pt.eq()) ids.push_back(e.param_id());
}
void collect_ids(const lsp::ScenePart& sp, std::vector<std::uint32_t>& ids) {
  for (const lsp::ParamEntry& e : sp.engine_params()) ids.push_back(e.param_id());
  for (const lsp::ParamEntry& e : sp.envelope()) ids.push_back(e.param_id());
  for (const lsp::ParamEntry& e : sp.eq()) ids.push_back(e.param_id());
}
void unique_sorted(std::vector<std::uint32_t>& v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
}

// An id this build does not know compares by presence and value; a known one against its default.
bool values_differ(const p::ParamDef* d, std::optional<float> a, std::optional<float> b) {
  if (d) return !near(a.value_or(static_cast<float>(d->def)), b.value_or(static_cast<float>(d->def)), eps_for(d));
  if (a.has_value() != b.has_value()) return true;
  return a && *a != *b;
}

void diff_part(const lsp::Part& a, const lsp::Part& b, std::uint32_t idx, std::vector<ChangeItem>& out) {
  const std::string pn = "Part " + std::to_string(idx + 1) + " ";
  auto add = [&](const char* key, const std::string& what, int param = -1) {
    out.push_back(ChangeItem{std::string("part.") + key, static_cast<int>(idx), param, -1, pn + what});
  };
  if (timbre_key(a) != timbre_key(b)) add("timbre", "timbre");
  if (a.engine().engine_type_id() != b.engine().engine_type_id()) add("engine", "engine");
  if (a.low_key() != b.low_key() || a.high_key() != b.high_key()) add("zone", "zone");
  if (a.octave() != b.octave()) add("octave", "octave");
  if (a.transpose() != b.transpose()) add("transpose", "transpose");
  if (a.keyboard_on() != b.keyboard_on()) add("keyboard", "keyboard");
  if (!near(a.volume(), b.volume(), 1e-6)) add("volume", "volume");
  if (a.output() != b.output()) add("output", "output");
  if (!slots_equal(a.inserts(), b.inserts())) add("inserts", "inserts");
  const p::ParamDef* rev = p::find(kSendReverb);
  const p::ParamDef* del = p::find(kSendDelay);
  if (!near(base_part_value(a, kSendReverb), base_part_value(b, kSendReverb), eps_for(rev))) add("send_reverb", "reverb send");
  if (!near(base_part_value(a, kSendDelay), base_part_value(b, kSendDelay), eps_for(del))) add("send_delay", "delay send");
  if ((a.has_arp() ? a.arp().SerializeAsString() : std::string()) != (b.has_arp() ? b.arp().SerializeAsString() : std::string())) add("arp", "arpeggiator");
  std::vector<std::uint32_t> ids;
  collect_ids(a, ids);
  collect_ids(b, ids);
  unique_sorted(ids);
  for (std::uint32_t id : ids) {
    if (id == kSendReverb || id == kSendDelay) continue;  // reported as the sends
    if (values_differ(p::find(id), part_list_value(a, id), part_list_value(b, id))) add("param", pretty_param(id), static_cast<int>(id));
  }
}

void diff_scene(const lsp::Scene& a, const lsp::Scene& b, int si, std::vector<ChangeItem>& out) {
  const std::string sn = "Scene " + std::to_string(si + 1) + " ";
  if (a.name() != b.name()) out.push_back(ChangeItem{"scene.name", -1, -1, si, sn + "name"});
  static const lsp::ScenePart kNone;
  const int parts = std::max(a.parts_size(), b.parts_size());
  for (int pi = 0; pi < parts; ++pi) {
    const lsp::ScenePart& x = pi < a.parts_size() ? a.parts(pi) : kNone;
    const lsp::ScenePart& y = pi < b.parts_size() ? b.parts(pi) : kNone;
    const std::string pn = sn + "Part " + std::to_string(pi + 1) + " ";
    auto add = [&](const char* key, const std::string& what, int param = -1) {
      out.push_back(ChangeItem{std::string("scene.part.") + key, pi, param, si, pn + what});
    };
    auto opt = [&](bool hx, double vx, bool hy, double vy, const char* key, const char* what, double eps) {
      if (hx != hy || (hx && !near(vx, vy, eps))) add(key, what);
    };
    if (x.has_low_key() != y.has_low_key() || (x.has_low_key() && x.low_key() != y.low_key()) || x.has_high_key() != y.has_high_key() ||
        (x.has_high_key() && x.high_key() != y.high_key()))
      add("zone", "zone");
    opt(x.has_octave(), x.octave(), y.has_octave(), y.octave(), "octave", "octave", 0);
    opt(x.has_transpose(), x.transpose(), y.has_transpose(), y.transpose(), "transpose", "transpose", 0);
    opt(x.has_keyboard_on(), x.keyboard_on(), y.has_keyboard_on(), y.keyboard_on(), "keyboard", "keyboard", 0);
    opt(x.has_volume(), x.volume(), y.has_volume(), y.volume(), "volume", "volume", 1e-6);
    opt(x.has_output(), static_cast<int>(x.output()), y.has_output(), static_cast<int>(y.output()), "output", "output", 0);
    if (x.has_inserts() != y.has_inserts() || (x.has_inserts() && !slots_equal(x.inserts().slots(), y.inserts().slots()))) add("inserts", "inserts");
    const p::ParamDef* rev = p::find(kSendReverb);
    const p::ParamDef* del = p::find(kSendDelay);
    auto sx = scene_part_value(x, kSendReverb), sy = scene_part_value(y, kSendReverb);
    if (sx.has_value() != sy.has_value() || (sx && !near(*sx, *sy, eps_for(rev)))) add("send_reverb", "reverb send");
    sx = scene_part_value(x, kSendDelay);
    sy = scene_part_value(y, kSendDelay);
    if (sx.has_value() != sy.has_value() || (sx && !near(*sx, *sy, eps_for(del)))) add("send_delay", "delay send");
    if ((x.has_arp() ? x.arp().SerializeAsString() : std::string()) != (y.has_arp() ? y.arp().SerializeAsString() : std::string())) add("arp", "arpeggiator");
    std::vector<std::uint32_t> ids;
    collect_ids(x, ids);
    collect_ids(y, ids);
    unique_sorted(ids);
    for (std::uint32_t id : ids) {
      if (id == kSendReverb || id == kSendDelay) continue;
      const auto vx = scene_list_value(x, id), vy = scene_list_value(y, id);
      const p::ParamDef* d = p::find(id);
      if (vx.has_value() != vy.has_value() || (vx && !near(*vx, *vy, d ? eps_for(d) : 0.0))) add("param", pretty_param(id), static_cast<int>(id));
    }
  }
  std::vector<std::uint32_t> mids;
  for (const lsp::ParamEntry& e : a.master()) mids.push_back(e.param_id());
  for (const lsp::ParamEntry& e : b.master()) mids.push_back(e.param_id());
  unique_sorted(mids);
  for (std::uint32_t id : mids) {
    const lsp::ParamEntry* ea = last_entry(a.master(), id);
    const lsp::ParamEntry* eb = last_entry(b.master(), id);
    const p::ParamDef* d = p::find(id);
    if ((ea != nullptr) != (eb != nullptr) || (ea && !near(ea->value(), eb->value(), d ? eps_for(d) : 0.0)))
      out.push_back(ChangeItem{"scene.master.param", -1, static_cast<int>(id), si, sn + "master " + pretty_param(id)});
  }
}

}  // namespace

std::vector<ChangeItem> diff_live_sets(const lsp::LiveSet& saved, const lsp::LiveSet& edited) {
  std::vector<ChangeItem> out;
  if (!near(saved.tempo(), edited.tempo(), eps_for(p::find(p::kTempoBpm)))) out.push_back(ChangeItem{"tempo", -1, -1, -1, "Tempo"});
  if (saved.parts_size() != edited.parts_size()) out.push_back(ChangeItem{"parts.list", -1, -1, -1, "Parts"});
  const int parts = std::min(saved.parts_size(), edited.parts_size());
  for (int i = 0; i < parts; ++i) diff_part(saved.parts(i), edited.parts(i), static_cast<std::uint32_t>(i), out);
  std::vector<std::uint32_t> mids;
  for (const lsp::ParamEntry& e : saved.master()) mids.push_back(e.param_id());
  for (const lsp::ParamEntry& e : edited.master()) mids.push_back(e.param_id());
  unique_sorted(mids);
  for (std::uint32_t id : mids) {
    const p::ParamDef* d = p::find(id);
    const lsp::ParamEntry* ea = last_entry(saved.master(), id);
    const lsp::ParamEntry* eb = last_entry(edited.master(), id);
    if (values_differ(d, ea ? std::optional<float>(ea->value()) : std::nullopt, eb ? std::optional<float>(eb->value()) : std::nullopt))
      out.push_back(ChangeItem{"master.param", -1, static_cast<int>(id), -1, "Master " + pretty_param(id)});
  }
  if (saved.assign_size() != edited.assign_size()) out.push_back(ChangeItem{"assign.list", -1, -1, -1, "ASSIGN map"});
  for (int k = 0; k < std::min(saved.assign_size(), edited.assign_size()); ++k)
    if (saved.assign(k) != edited.assign(k)) out.push_back(ChangeItem{"assign.slot", -1, -1, -1, "ASSIGN K" + std::to_string(k + 1), k});
  if (saved.scenes_size() != edited.scenes_size()) out.push_back(ChangeItem{"scenes.list", -1, -1, -1, "Scenes"});
  for (int si = 0; si < std::min(saved.scenes_size(), edited.scenes_size()); ++si) diff_scene(saved.scenes(si), edited.scenes(si), si, out);
  return out;
}

// ---- names -------------------------------------------------------------------------------------------------------------------------------
std::string trim_name(const std::string& s) {
  auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  std::size_t b = 0, e = s.size();
  while (b < e && ws(s[b])) ++b;
  while (e > b && ws(s[e - 1])) --e;
  return s.substr(b, e - b);
}

std::string fold_name(const std::string& s) {
  std::string t = trim_name(s);
  for (char& c : t)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return t;
}

std::string suggest_free_name(const std::string& wanted, const std::unordered_set<std::string>& taken_folded) {
  std::string stem = trim_name(wanted);
  // Strip an existing trailing " N" (one space and 1..9 digits).
  std::size_t d = stem.size();
  while (d > 0 && stem[d - 1] >= '0' && stem[d - 1] <= '9') --d;
  if (d < stem.size() && stem.size() - d <= 9 && d > 0 && stem[d - 1] == ' ') stem = trim_name(stem.substr(0, d - 1));
  if (stem.empty()) stem = "Live Set";
  for (std::uint32_t n = 2;; ++n) {
    const std::string suffix = " " + std::to_string(n);
    std::string s = stem;
    if (s.size() + suffix.size() > liveset::kMaxNameBytes) {
      std::size_t cut = liveset::kMaxNameBytes - suffix.size();
      while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;  // never cut inside a UTF-8 character
      s = trim_name(s.substr(0, cut));
    }
    const std::string candidate = s + suffix;
    if (!taken_folded.count(fold_name(candidate))) return candidate;
  }
}

// ---- session ---------------------------------------------------------------------------------------------------------------------------
EditSession::EditSession(ControlPlane& plane, SceneController& scenes, liveset::LiveSetStore& live, setlist::SetlistStore* setlists, SetlistController* setlist_ctl,
                         SwitchPort* port, EditSessionOptions options)
    : plane_(plane), scenes_(scenes), live_(live), setlists_(setlists), setlist_ctl_(setlist_ctl), port_(port), opt_(std::move(options)) {
  plane_.set_edit_handler(this);
  scenes_.set_before_recall([this] {
    sync_live_values();
    if (!restoring_) plane_.note_device_change();  // Story 9.10: a scene recall changes the values under any Studio view (the restore after a timbre rebuild does not)
  });
  if (setlist_ctl_) setlist_ctl_->set_on_live_set_deleted([this](const std::string& id) { forget(id); });
  adopt_current_from_scenes();
}

EditSession::~EditSession() {
  if (loader_) loader_->set_edit_port(nullptr);
  if (switcher_) switcher_->set_edit_port(nullptr);
  if (setlist_ctl_) setlist_ctl_->set_on_live_set_deleted({});
  scenes_.set_before_recall({});
  plane_.set_edit_handler(nullptr);
}

void EditSession::adopt_current_from_scenes() {
  if (!scenes_.owns_current()) return;
  const lsp::LiveSet& ls = scenes_.source();
  Doc d;
  d.saved = ls;
  d.working = ls;
  docs_[ls.id()] = std::move(d);
  current_id_ = ls.id();
}

EditSession::Doc* EditSession::current_doc() {
  auto it = docs_.find(current_id_);
  return it == docs_.end() ? nullptr : &it->second;
}

const EditSession::Doc* EditSession::find_doc(const std::string& id) const {
  auto it = docs_.find(id);
  return it == docs_.end() ? nullptr : &it->second;
}

const std::vector<ChangeItem>& EditSession::items_of(const Doc& d) const {
  if (d.diff_version != d.version) {
    d.items = diff_live_sets(d.saved, d.working);
    d.has_timbre = false;
    for (const ChangeItem& i : d.items)
      if (i.key == "part.timbre" || i.key == "part.engine") d.has_timbre = true;
    d.diff_version = d.version;
  }
  return d.items;
}

int EditSession::current_scene() const { return scenes_.owns_current() ? scenes_.current_scene() : -1; }

void EditSession::touch(Doc& d) {
  ++d.version;
  ++edit_seq_;
  if (&d == current_doc() && scenes_.owns_current()) scenes_.set_source(d.working);
}

bool EditSession::apply_external_edit(const std::function<void(lsp::LiveSet&)>& f) {
  Doc* d = current_doc();
  if (!d || !scenes_.owns_current()) return false;
  f(d->working);
  touch(*d);
  return true;
}

bool EditSession::is_dirty(const std::string& id) const {
  const Doc* d = find_doc(id);
  return d && !items_of(*d).empty();
}

std::vector<std::string> EditSession::dirty_ids() const {
  std::vector<std::string> v;
  for (const auto& kv : docs_)
    if (!items_of(kv.second).empty()) v.push_back(kv.first);
  return v;
}

std::vector<ChangeItem> EditSession::summary(const std::string& id) const {
  const Doc* d = find_doc(id);
  return d ? items_of(*d) : std::vector<ChangeItem>{};
}

const lsp::LiveSet* EditSession::working_of(const std::string& id) const {
  const Doc* d = find_doc(id);
  return d ? &d->working : nullptr;
}
const lsp::LiveSet* EditSession::saved_of(const std::string& id) const {
  const Doc* d = find_doc(id);
  return d ? &d->saved : nullptr;
}

// ---- live values -----------------------------------------------------------------------------------------------------------------------
bool EditSession::sync_live_values() {
  Doc* d = current_doc();
  if (!d || !scenes_.owns_current() || restoring_ || !plane_.rt_adopted_current()) return false;
  const int scene = current_scene();
  bool changed = false, foreign = false;
  for (const p::ParamDef& def : p::kParams) {
    if (def.cls != p::Class::kPerformance) continue;
    if (def.scope == p::Scope::kPart) {
      for (std::uint32_t part = 0; part < p::kPartCount && static_cast<int>(part) < d->working.parts_size(); ++part) {
        const float live = plane_.live_value(p::perf_slot(def, part));
        if (!std::isfinite(live)) continue;
        if (!near(live, edited_part_value(d->working, scene, part, def.id), eps_for(&def))) {
          store_part_value(d->working, scene, part, def.id, live);
          changed = true;
          if (!plane_.consume_studio_echo(p::perf_slot(def, part), live)) foreign = true;  // Story 9.10: nobody at Velora Studio explains it: the unit's own controls
        }
      }
    } else if (def.id != p::kLineInLevel) {  // the line-in level is device input, not part of a Live Set
      const float live = plane_.live_value(p::perf_slot(def, 0));
      if (!std::isfinite(live)) continue;
      if (!near(live, edited_master_value(d->working, scene, def.id), eps_for(&def))) {
        store_master_value(d->working, scene, def.id, live);
        changed = true;
        if (!plane_.consume_studio_echo(p::perf_slot(def, 0), live)) foreign = true;
      }
    }
  }
  if (changed) touch(*d);
  if (foreign) plane_.note_device_change();  // Story 9.10: a Velora Studio view of this Live Set is stale from here
  return changed;
}

// ---- structural edits ------------------------------------------------------------------------------------------------------------------
void EditSession::on_structural(const ws::v1::ClientMessage& msg) {
  Doc* d = current_doc();
  if (!d || !scenes_.owns_current()) return;
  const int scene = current_scene();
  lsp::LiveSet& ls = d->working;
  auto part_ok = [&](std::uint32_t part) { return static_cast<int>(part) < ls.parts_size(); };
  switch (msg.body_case()) {
    case B::kSetInsertChain: {
      const auto& m = msg.set_insert_chain();
      if (!part_ok(m.part())) return;
      lsp::ScenePart* sp = scene_part_mut(ls, scene, m.part());
      if (sp && sp->has_inserts()) copy_slots(m.slots(), sp->mutable_inserts()->mutable_slots());
      else copy_slots(m.slots(), ls.mutable_parts(static_cast<int>(m.part()))->mutable_inserts());
      break;
    }
    case B::kSetSendLevels: {
      const auto& m = msg.set_send_levels();
      if (!part_ok(m.part())) return;
      store_part_value(ls, scene, m.part(), kSendReverb, m.reverb());
      store_part_value(ls, scene, m.part(), kSendDelay, m.delay());
      break;
    }
    case B::kSetPartOutput: {
      const auto& m = msg.set_part_output();
      if (!part_ok(m.part())) return;
      lsp::ScenePart* sp = scene_part_mut(ls, scene, m.part());
      if (sp && sp->has_output()) sp->set_output(m.output());
      else ls.mutable_parts(static_cast<int>(m.part()))->set_output(m.output());
      break;
    }
    case B::kSetPartVolume: {
      const auto& m = msg.set_part_volume();
      if (!part_ok(m.part())) return;
      lsp::ScenePart* sp = scene_part_mut(ls, scene, m.part());
      if (sp && sp->has_volume()) sp->set_volume(m.volume());
      else ls.mutable_parts(static_cast<int>(m.part()))->set_volume(m.volume());
      break;
    }
    case B::kSetMasterParams:
      for (const ws::v1::ParamValue& pv : msg.set_master_params().values()) store_master_value(ls, scene, static_cast<std::uint32_t>(pv.param_id()), pv.value());
      break;
    default: return;
  }
  touch(*d);
}

bool EditSession::structural_blocked(std::string& reason) const {
  if (reload_active_ || restoring_) {
    reason = "reload_in_progress";
    return true;
  }
  return false;
}

// ---- loader / switcher seam ------------------------------------------------------------------------------------------------------------
const lsp::LiveSet* EditSession::overlay_for(const std::string& id) const {
  const Doc* d = find_doc(id);
  if (!d || items_of(*d).empty()) return nullptr;
  overlay_given_.insert(id);
  return &d->working;
}

std::uint64_t EditSession::overlay_revision(const std::string& id) const {
  const Doc* d = find_doc(id);
  return (!d || items_of(*d).empty()) ? 0 : d->version;
}

double EditSession::now() const {
  return opt_.clock ? opt_.clock() : std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void EditSession::forget(const std::string& id) {
  if (id == current_id_) return;  // the Live Set that sounds cannot be deleted (LIVE_SET_IN_USE); never drop its document
  if (docs_.erase(id)) ++edit_seq_;
}

void EditSession::before_switch() {
  sync_live_values();
  if (reload_active_) pending_scene_ = current_scene();
}

void EditSession::on_published(const lsp::LiveSet& built_from, const core::Snapshot&) {
  const std::string id = built_from.id();
  auto it = docs_.find(id);
  if (reload_active_ && id == current_id_ && it != docs_.end()) {
    reload_active_ = false;  // the rebuild is published: the scene and the live values are put back by tick()
    restoring_ = true;
    restore_scene_ = -2;
    restore_deadline_ = now() + opt_.restore_timeout_s;
    overlay_given_.clear();
    scenes_.set_source(it->second.working);
    return;
  }
  reload_active_ = false;
  restoring_ = false;
  const bool overlay_used = overlay_given_.count(id) > 0;
  overlay_given_.clear();
  std::string a, b;
  if (it != docs_.end() && overlay_used) {
    // Built from this document's edited state (a jump back to a Live Set with unsaved edits): the edits stay, whatever the serialised form says.
  } else if (it != docs_.end() && liveset::serialize(built_from, a) == liveset::Status::kOk && liveset::serialize(it->second.working, b) == liveset::Status::kOk &&
             a == b) {
    // The same content: nothing to do.
  } else {
    if (it != docs_.end() && !items_of(it->second).empty())
      std::fprintf(stderr, "edit session: Live Set %s was loaded from content that is not its edited state; its unsaved edits are dropped\n", id.c_str());
    Doc d;  // built from the file, or from content that is not this document's: that content is the new baseline
    d.saved = built_from;
    d.working = built_from;
    docs_[id] = std::move(d);
    ++edit_seq_;
  }
  if (id != current_id_) plane_.note_device_change();  // Story 9.10: another Live Set sounds: every Studio view of the previous one is stale
  current_id_ = id;
  scenes_.set_source(docs_[id].working);
  ++edit_seq_;
  for (auto i = docs_.begin(); i != docs_.end();) {  // a Live Set that was left without unsaved edits needs no document
    if (i->first != current_id_ && items_of(i->second).empty()) i = docs_.erase(i);
    else ++i;
  }
}

// ---- tick, restore ---------------------------------------------------------------------------------------------------------------------
bool EditSession::tick() {
  if (reload_active_) {
    // A rebuild is tied to its job: it ends when that job is no longer the one in progress (published, failed or superseded by another jump).
    const std::uint64_t t = opt_.active_job_token ? opt_.active_job_token() : (opt_.load_in_progress && opt_.load_in_progress() ? reload_token_ : 0);
    if (t != reload_token_) reload_active_ = false;  // the edit stays in memory, the old generation sounds
  }
  if (!reload_active_ && !(opt_.load_in_progress && opt_.load_in_progress())) overlay_given_.clear();  // a build that was cancelled leaves no claim behind
  if (restoring_) {
    if (now() > restore_deadline_) {
      std::fprintf(stderr, "edit session: RT did not apply the rebuilt generation in time; the scene and live values were not restored\n");
      restoring_ = false;
      pending_scene_ = -1;
      restore_scene_ = -2;
      ++edit_seq_;
    } else {
      if (!plane_.rt_adopted_current() && opt_.pump) opt_.pump();
      if (plane_.rt_adopted_current()) restore();
    }
  } else {
    sync_live_values();
  }
  const bool changed = edit_seq_ != reported_seq_;
  reported_seq_ = edit_seq_;
  return changed;
}

// Puts back, after a rebuild, the scene and the live values RT reset. Retried on the next tick (restoring_ stays set) while the scene recall is refused
// for a transient reason or the control lane is full; given up by the deadline in tick().
void EditSession::restore() {
  Doc* d = current_doc();
  if (!d || !scenes_.owns_current()) {
    restoring_ = false;
    return;
  }
  if (restore_scene_ == -2) {
    int scene = -1;
    if (pending_scene_ >= 0 && pending_scene_ < static_cast<int>(scenes_.scene_count())) {
      const SceneOutcome o = scenes_.recall_scene(static_cast<std::uint32_t>(pending_scene_));
      if (o.result == SceneResult::kOk) scene = pending_scene_;
      else if (o.result == SceneResult::kBusy || o.result == SceneResult::kLoadInProgress) return;  // transient: try again on the next tick
    }
    restore_scene_ = scene;  // recalled, or dropped for good (no such scene any more)
  }
  const core::Snapshot* cur = plane_.current_snapshot();
  if (!cur) {
    restoring_ = false;
    return;
  }
  for (const p::ParamDef& def : p::kParams) {
    if (def.cls != p::Class::kPerformance || def.id == p::kLineInLevel) continue;
    const std::uint32_t parts = def.scope == p::Scope::kPart ? p::kPartCount : 1;
    for (std::uint32_t part = 0; part < parts && static_cast<int>(part) < d->working.parts_size(); ++part) {
      const double want = def.scope == p::Scope::kPart ? edited_part_value(d->working, restore_scene_, part, def.id) : edited_master_value(d->working, restore_scene_, def.id);
      if (!near(cur->perf_init[p::perf_slot(def, part)], want, eps_for(&def)))
        if (!plane_.push_performance_value(def, part, want)) return;  // the control lane is full: keep restoring, push again next tick
    }
  }
  restoring_ = false;
  pending_scene_ = -1;
  restore_scene_ = -2;
  ++edit_seq_;
}

// ---- State -----------------------------------------------------------------------------------------------------------------------------
namespace {
void fill_item(const ChangeItem& i, ws::v1::ChangeItem* o) {
  o->set_key(i.key);
  if (i.part >= 0) o->set_part(static_cast<std::uint32_t>(i.part));
  if (i.param_id >= 0) o->set_param_id(static_cast<std::uint32_t>(i.param_id));
  if (i.scene >= 0) o->set_scene(i.scene);
  if (i.slot >= 0) o->set_slot(static_cast<std::uint32_t>(i.slot));
  o->set_text(i.text);
}
}  // namespace

void EditSession::fill_status(ws::v1::State* out) const {
  if (setlists_) {  // Story 4.10: the global default ASSIGN map
    ws::v1::EncoderStatus* enc = out->mutable_encoders();
    enc->set_assign_default_available(true);
    enc->clear_assign_default();
    for (std::uint32_t v : setlists_->default_assign()) enc->add_assign_default(v);
  }
  ws::v1::EditStatus* e = out->mutable_edits();
  e->set_available(true);
  std::uint32_t dirty = 0;
  for (const auto& kv : docs_) {
    if (items_of(kv.second).empty()) continue;
    ++dirty;
    if (e->dirty_live_set_ids_size() < static_cast<int>(kDirtyIdsPublished)) e->add_dirty_live_set_ids(kv.first);
  }
  e->set_dirty_count(dirty);
  e->set_edit_seq(edit_seq_);
  e->set_writes_done(writes_done_);
  const Doc* d = find_doc(current_id_);
  const int scene = current_scene();
  std::string blocked;
  if (!d || !scenes_.owns_current()) blocked = "no_scene";
  else if (scenes_.scene_count() == 0) blocked = "no_scenes";
  else if (scene < 0) blocked = "no_scene";
  if (d && scenes_.owns_current()) {
    const std::vector<ChangeItem>& items = items_of(*d);
    e->set_current_dirty(!items.empty());
    e->set_change_count(static_cast<std::uint32_t>(items.size()));
    e->set_has_timbre_edit(d->has_timbre);
    std::string text;
    for (std::size_t i = 0; i < items.size() && i < kSummaryItems; ++i) {
      fill_item(items[i], e->add_change_summary());
      if (i < 4) text += (i ? ", " : "") + items[i].text;
    }
    if (items.size() > 4) text += ", +" + std::to_string(items.size() - 4) + " more";
    e->set_change_summary_text(text);
    if (blocked.empty() && d->has_timbre) blocked = "timbre_change";
  }
  e->set_scene_write_available(blocked.empty());
  e->set_scene_write_blocked_reason(blocked);
}

// ---- commands --------------------------------------------------------------------------------------------------------------------------
bool EditSession::run(const ws::v1::ClientMessage& msg, ws::v1::ServerMessage& reply, HandlerFailure& err, bool& bump, bool& push) {
  bump = false;
  push = false;
  const std::uint64_t seq_before = edit_seq_;
  bool ok = false;
  switch (msg.body_case()) {
    case B::kSetPartTimbre: ok = do_set_timbre(msg.set_part_timbre(), err, bump); break;
    case B::kWriteLiveSet: ok = do_write(msg.write_live_set(), reply, err, bump); break;
    case B::kSetAssignSlot:
      ok = do_assign(msg.set_assign_slot().target(), msg.set_assign_slot().slot(), msg.set_assign_slot().param_id(), /*clear=*/false, err, bump);
      break;
    case B::kClearAssignSlot: ok = do_assign(msg.clear_assign_slot().target(), msg.clear_assign_slot().slot(), 0, /*clear=*/true, err, bump); break;
    default: ok = fail(err, ws::v1::ERROR_CODE_INVALID_COMMAND, "not an edit command"); break;
  }
  push = edit_seq_ != seq_before;
  return ok;
}

bool EditSession::busy_reason(std::string& reason) const {
  if (scenes_.load_in_progress() || (opt_.load_in_progress && opt_.load_in_progress())) reason = "load_in_progress";
  else if (reload_active_ || restoring_) reason = "reload_in_progress";
  else return false;
  return true;
}

bool EditSession::do_set_timbre(const ws::v1::SetPartTimbre& m, HandlerFailure& err, bool& bump) {
  Doc* d = current_doc();
  if (!d || !scenes_.owns_current()) return fail(err, ws::v1::ERROR_CODE_NO_LIVE_SET, "no Live Set is loaded");
  std::string why;
  if (busy_reason(why)) return fail(err, ws::v1::ERROR_CODE_BUSY, "a Live Set load is in progress", {{"reason", why}});
  if (m.part() >= static_cast<std::uint32_t>(d->working.parts_size()))
    return fail(err, ws::v1::ERROR_CODE_INVALID_PART, "part index out of range", {{"part", std::to_string(m.part())}});
  if (!opt_.reload_current) return fail(err, ws::v1::ERROR_CODE_INVALID_COMMAND, "timbre edits need the Live Set switcher");
  sync_live_values();  // the live values are captured into the working copy before it is rebuilt from
  const lsp::LiveSet before = d->working;
  lsp::Part* pt = d->working.mutable_parts(static_cast<int>(m.part()));
  if (m.library_id().empty() && m.instrument_key().empty()) {
    if (!m.has_engine_type_id()) pt->clear_timbre();  // only the engine type given: the timbre reference is left as it is
  } else {
    pt->mutable_timbre()->set_library_id(m.library_id());
    pt->mutable_timbre()->set_instrument_key(m.instrument_key());
  }
  if (m.has_engine_type_id()) pt->mutable_engine()->set_engine_type_id(m.engine_type_id());
  const liveset::ValidationReport rep = liveset::validate(d->working, opt_.part_count);
  if (!rep.ok()) {
    d->working = before;
    return fail(err, ws::v1::ERROR_CODE_INVALID_COMMAND, "the timbre reference or engine type is not valid", {{"issue", std::to_string(static_cast<int>(rep.errors.front().code))}});
  }
  if (timbre_key(before.parts(static_cast<int>(m.part()))) == timbre_key(*pt) &&
      before.parts(static_cast<int>(m.part())).engine().engine_type_id() == pt->engine().engine_type_id()) {
    d->working = before;
    return true;  // nothing changes: no rebuild
  }
  touch(*d);
  reload_active_ = true;
  pending_scene_ = current_scene();
  const SwitchOutcome o = opt_.reload_current();
  reload_token_ = o.token;
  if (o.result != SwitchOutcome::Result::kAccepted) {
    reload_active_ = false;
    d->working = before;
    touch(*d);
    return fail(err, ws::v1::ERROR_CODE_LIVE_SET_LOAD_FAILED, "the Live Set could not be rebuilt from its edited state", {{"reason", "reload_refused"}});
  }
  bump = true;
  return true;
}

// SetAssignSlot / ClearAssignSlot (Story 4.10). CURRENT_LIVE_SET: an in-memory edit of the sounding Live Set's map (dirty, change key assign.slot) that
// takes effect at once (a new generation that is a copy of the current one with the map changed; scenes never change it) and is persisted by Write.
// GLOBAL_DEFAULT: a settings edit, written to assign_default.pb at once through the setlist store's atomic writer (no Write needed); existing Live Sets
// are not touched. Duplicates (the same parameter in several slots) are allowed: ids may repeat on purpose and the least surprising behaviour is to do what
// the player set. All validation happens before anything changes.
bool EditSession::do_assign(ws::v1::AssignTarget target, std::uint32_t slot, std::uint32_t param_id, bool clear, HandlerFailure& err, bool& bump) {
  if (target != ws::v1::ASSIGN_TARGET_CURRENT_LIVE_SET && target != ws::v1::ASSIGN_TARGET_GLOBAL_DEFAULT)
    return fail(err, ws::v1::ERROR_CODE_INVALID_COMMAND, "an ASSIGN command needs a target: CURRENT_LIVE_SET or GLOBAL_DEFAULT");
  if (slot < 1 || slot > core::kAssignSlotCount)
    return fail(err, ws::v1::ERROR_CODE_INVALID_SLOT, "the ASSIGN slot must be 1..8 (K1..K8)", {{"slot", std::to_string(slot)}, {"max_slot", std::to_string(core::kAssignSlotCount)}});
  std::uint32_t id = 0;
  if (!clear) {
    const p::ParamDef* pd = p::find(param_id);  // looked up whole: 0x10000 + N never aliases parameter N
    if (!pd) return fail(err, ws::v1::ERROR_CODE_UNKNOWN_PARAM, "unknown parameter id", {{"param_id", std::to_string(param_id)}});
    if (pd->cls != p::Class::kPerformance)
      return fail(err, ws::v1::ERROR_CODE_NOT_ASSIGNABLE, "a structural parameter has no live value an encoder could drive", {{"param_id", std::to_string(param_id)}, {"reason", "structural"}});
    if (!core::is_assignable(*pd))
      return fail(err, ws::v1::ERROR_CODE_NOT_ASSIGNABLE, "the registry marks this parameter as not assignable", {{"param_id", std::to_string(param_id)}, {"reason", "not_assignable"}});
    id = pd->id;
  }
  const std::size_t k = slot - 1;

  if (target == ws::v1::ASSIGN_TARGET_GLOBAL_DEFAULT) {
    if (!setlists_) return fail(err, ws::v1::ERROR_CODE_INVALID_COMMAND, "the global default ASSIGN map needs a data directory");
    std::array<std::uint32_t, setlist::kAssignSlots> map = setlists_->default_assign();
    if (map[k] == id) return true;  // already so: nothing to write
    map[k] = id;
    const setlist::Status st = setlists_->set_default_assign(map);
    if (setlist::applied(st)) {
      bump = true;  // the default is part of the published state
      ++edit_seq_;  // pushes a State (the default is in State.encoders)
      return true;
    }
    if (st == setlist::Status::kNoSpace) return fail(err, ws::v1::ERROR_CODE_STORAGE_FULL, "the storage is full; the global default is unchanged");
    return fail(err, ws::v1::ERROR_CODE_STORAGE_FAILED, "the global default ASSIGN map could not be stored; nothing changed", {{"reason", setlist::to_string(st)}});
  }

  Doc* d = current_doc();
  const core::Snapshot* cur = plane_.current_snapshot();
  if (!d || !scenes_.owns_current() || !cur || !cur->live_set) return fail(err, ws::v1::ERROR_CODE_NO_LIVE_SET, "no Live Set is loaded");
  std::string why;
  if (busy_reason(why)) return fail(err, ws::v1::ERROR_CODE_BUSY, "a Live Set load is in progress", {{"reason", why}});
  if (k < static_cast<std::size_t>(d->working.assign_size()) ? d->working.assign(static_cast<int>(k)) == id && cur->assign[k] == id : id == 0 && cur->assign[k] == 0)
    return true;  // already so: nothing is touched
  if (plane_.can_publish(*cur) != ControlPlane::PublishStatus::kOk)
    return fail(err, ws::v1::ERROR_CODE_BUSY, "too many snapshot generations are still referenced by sounding voices", {{"reason", "ledger_full"}});
  // Publish FIRST: a refusal then leaves the working copy, the dirty state and the edit sequence exactly as they were (there is nothing to roll back).
  core::Snapshot work = *cur;
  work.assign[k] = static_cast<std::uint16_t>(id);
  std::string detail;
  if (plane_.publish_snapshot(work, detail) != ControlPlane::PublishStatus::kOk)
    return fail(err, ws::v1::ERROR_CODE_BUSY, "the ASSIGN map could not be published; nothing changed", {{"reason", "publish_refused"}, {"detail", detail}});
  while (d->working.assign_size() < static_cast<int>(core::kAssignSlotCount)) d->working.add_assign(0);
  d->working.set_assign(static_cast<int>(k), id);
  touch(*d);
  return true;  // publishing moved the revision; the dirty flag changed the edit status
}

bool EditSession::fence_and_wait(HandlerFailure& err) {
  std::uint32_t seq = 0;
  if (!plane_.raise_fence(seq)) return fail(err, ws::v1::ERROR_CODE_BUSY, "the real-time event queue is full", {{"reason", "fence_queue_full"}});
  const auto now = [this] {
    return opt_.clock ? opt_.clock() : std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  const double t0 = now();
  while (!plane_.fence_acked(seq)) {
    if (now() - t0 > opt_.fence_timeout_s) return fail(err, ws::v1::ERROR_CODE_BUSY, "the real-time thread did not acknowledge the fence", {{"reason", "fence_timeout"}});
    if (opt_.fence_idle) opt_.fence_idle();
    else std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return true;
}

std::unordered_set<std::string> EditSession::taken_names(const std::string& setlist_id, bool all_when_none) const {
  std::unordered_set<std::string> taken;
  if (!port_) return taken;
  if (!setlist_id.empty() && setlists_) {
    const ::velora::setlist::v1::Setlist* s = setlists_->find_setlist(setlist_id);
    if (s) {
      std::unordered_map<std::string, const CatalogEntry*> by_id;
      for (const CatalogEntry& e : port_->catalog()) by_id[e.id] = &e;
      for (const std::string& id : s->live_set_ids()) {
        auto it = by_id.find(id);
        if (it != by_id.end()) taken.insert(fold_name(it->second->name));
      }
    }
  } else if (all_when_none) {
    for (const CatalogEntry& e : port_->catalog()) taken.insert(fold_name(e.name));
  }
  return taken;
}

namespace {
bool store_failure(HandlerFailure& err, liveset::Status s) {
  switch (s) {
    case liveset::Status::kNoSpace: return fail(err, ws::v1::ERROR_CODE_STORAGE_FULL, "the storage is full; nothing was written and the edits stay in memory");
    case liveset::Status::kInvalid:
      return fail(err, ws::v1::ERROR_CODE_STORAGE_FAILED, "the edited Live Set is not valid; nothing was written", {{"reason", "invalid_live_set"}});
    default:
      return fail(err, ws::v1::ERROR_CODE_STORAGE_FAILED, "the Live Set could not be written; nothing changed", {{"reason", liveset::to_string(s)}});
  }
}
}  // namespace

bool EditSession::do_write(const ws::v1::WriteLiveSet& m, ws::v1::ServerMessage& reply, HandlerFailure& err, bool& bump) {
  const ws::v1::WriteScope scope = m.scope();
  if (scope != ws::v1::WRITE_SCOPE_SCENE && scope != ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET && scope != ws::v1::WRITE_SCOPE_SAVE_AS_NEW)
    return fail(err, ws::v1::ERROR_CODE_INVALID_COMMAND, "WriteLiveSet needs a scope: SCENE, WHOLE_LIVE_SET or SAVE_AS_NEW");
  Doc* d = current_doc();
  if (!d || !scenes_.owns_current()) return fail(err, ws::v1::ERROR_CODE_NO_LIVE_SET, "no Live Set is loaded");
  std::string why;
  if (busy_reason(why)) return fail(err, ws::v1::ERROR_CODE_BUSY, "a Live Set load is in progress", {{"reason", why}});
  // Story 6.9: a factory Live Set (LiveSet.factory) is read-only; the edits are kept in memory and Save as new makes an ordinary copy of it.
  if (scope != ws::v1::WRITE_SCOPE_SAVE_AS_NEW && d->saved.has_factory())
    return fail(err, ws::v1::ERROR_CODE_WRITE_SCOPE_UNAVAILABLE, "factory Live Sets are read-only; use Save as new", {{"reason", "factory_read_only"}});

  // What needs no live values is decided first.
  const std::string loaded_setlist = setlist_ctl_ ? setlist_ctl_->loaded_setlist_id() : std::string();
  std::string new_name;
  if (scope == ws::v1::WRITE_SCOPE_SAVE_AS_NEW) {
    if (m.has_new_name()) {
      new_name = trim_name(m.new_name());
      if (!liveset::valid_display_name(new_name)) return fail(err, ws::v1::ERROR_CODE_INVALID_NAME, "invalid name (1..64 bytes of UTF-8, no control characters)");
      // With a setlist loaded the name must be free in that setlist; with none, free among all the Live Sets the engine lists.
      const std::unordered_set<std::string> taken = taken_names(loaded_setlist, true);
      if (taken.count(fold_name(new_name)))
        return fail(err, ws::v1::ERROR_CODE_NAME_IN_USE, "name in use", {{"name", new_name}, {"suggested_name", suggest_free_name(new_name, taken)}});
    } else {
      new_name = suggest_free_name(d->working.name(), taken_names(loaded_setlist, true));
    }
  }
  const int scene = current_scene();
  if (scope == ws::v1::WRITE_SCOPE_SCENE) {
    if (scenes_.scene_count() == 0) return fail(err, ws::v1::ERROR_CODE_WRITE_SCOPE_UNAVAILABLE, "the Live Set has no scenes", {{"reason", "no_scenes"}});
    if (scene < 0) return fail(err, ws::v1::ERROR_CODE_WRITE_SCOPE_UNAVAILABLE, "no scene is current", {{"reason", "no_scene"}});
    if (scene >= d->saved.scenes_size() || scene >= d->working.scenes_size())
      return fail(err, ws::v1::ERROR_CODE_WRITE_SCOPE_UNAVAILABLE, "the current scene is not in the saved Live Set", {{"reason", "scene_not_in_saved"}});
  }

  // The fence: everything the control thread sent to RT before now has been applied when it is acknowledged; only then is the table read.
  if (!fence_and_wait(err)) return false;
  // Never write without reading the live table: RT must have applied the newest generation by now (the fence is behind it).
  if (!plane_.rt_adopted_current()) return fail(err, ws::v1::ERROR_CODE_BUSY, "the real-time thread has not applied the newest generation", {{"reason", "generation_not_applied"}});
  sync_live_values();
  d = current_doc();  // (unchanged, but keep the pointer fresh)
  const std::vector<ChangeItem>& items = items_of(*d);
  const std::string live_id = d->working.id();
  if (scope == ws::v1::WRITE_SCOPE_SCENE && d->has_timbre)
    return fail(err, ws::v1::ERROR_CODE_WRITE_SCOPE_UNAVAILABLE, "Timbre changes are saved with the Live Set", {{"reason", "timbre_change"}});

  ws::v1::WriteReply* r = reply.mutable_write_reply();
  r->set_scope(scope);
  r->set_live_set_id(live_id);
  r->set_written_live_set_id(live_id);
  r->set_written_name(d->working.name());
  r->set_scene(scope == ws::v1::WRITE_SCOPE_SCENE ? scene : -1);
  r->set_setlist_index(-1);

  std::vector<ChangeItem> written;
  if (scope == ws::v1::WRITE_SCOPE_SCENE) {
    for (const ChangeItem& i : items)
      if (i.scene == scene) written.push_back(i);
  } else if (scope == ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET) {
    written = items;
  } else {
    written = items;  // a new Live Set holds everything, edited or not
  }
  auto finish = [&](bool nothing) {
    r->set_nothing_to_write(nothing);
    std::vector<ChangeItem> now = items_of(*current_doc());
    r->set_current_dirty(!now.empty());
    r->set_remaining_changes(static_cast<std::uint32_t>(now.size()));
    r->set_written_count(static_cast<std::uint32_t>(nothing ? 0 : written.size()));
    if (!nothing)
      for (std::size_t i = 0; i < written.size() && i < kSummaryItems; ++i) fill_item(written[i], r->add_written());
  };

  if (scope != ws::v1::WRITE_SCOPE_SAVE_AS_NEW && written.empty()) {
    finish(true);
    return true;  // nothing dirty for this scope: no file is touched
  }

  if (scope == ws::v1::WRITE_SCOPE_SAVE_AS_NEW) {
    lsp::LiveSet ns = d->working;
    ns.clear_factory();  // the copy is the user's own Live Set
    std::string id;
    for (int attempt = 0; attempt < liveset::kMaxAdoptAttempts && id.empty(); ++attempt) {
      std::string c = liveset::mint_uuid_v4(opt_.rng ? *opt_.rng : static_cast<liveset::RandomSource&>(os_rng_));
      if (c.empty()) break;
      bool taken = c == live_id;
      if (port_)
        for (const CatalogEntry& e : port_->catalog()) taken = taken || e.id == c;
      liveset::LoadResult lr;
      if (!taken && live_.read(c, lr) != liveset::Status::kNotFound) taken = true;
      if (!taken) id = c;
    }
    if (id.empty()) return fail(err, ws::v1::ERROR_CODE_STORAGE_FAILED, "no new Live Set id could be minted", {{"reason", "random_failed"}});
    ns.set_id(id);
    ns.set_name(new_name);
    const liveset::Status st = live_.write(ns);
    if (!(st == liveset::Status::kOk || st == liveset::Status::kCommittedNotDurable)) return store_failure(err, st);
    bool not_durable = st == liveset::Status::kCommittedNotDurable;
    if (port_) {
      port_->add_to_catalog(CatalogEntry{id, new_name, live_.path_for(id)});
      port_->live_set_written(id);
    }
    if (!loaded_setlist.empty() && setlists_) {
      const ::velora::setlist::v1::Setlist* s = setlists_->find_setlist(loaded_setlist);
      int base = -1;
      if (s) {  // (`s` is read before the store command only: it replaces its library)
        const int size_before = s->live_set_ids_size();  // (the store replaces its library on a command: `s` is not used after it)
        for (int i = 0; i < s->live_set_ids_size(); ++i)
          if (s->live_set_ids(i) == live_id) base = i;
        if (base < 0) base = setlist_ctl_->last_setlist_position();
        std::optional<std::size_t> index;
        if (base >= 0) index = std::min(static_cast<std::size_t>(base) + 1, static_cast<std::size_t>(size_before));
        const sl::Status ss = setlists_->add_live_set(loaded_setlist, id, index);
        if (!sl::applied(ss)) {
          // The setlist could not take it: the new file is removed again so nothing half-done is left behind.
          const liveset::Status rm = live_.remove(id);
          const bool leftover = !(rm == liveset::Status::kOk || rm == liveset::Status::kCommittedNotDurable || rm == liveset::Status::kNotFound);
          if (!leftover && port_) port_->remove_from_catalog(id);  // a file that could not be removed stays listed: it is a valid Live Set
          std::map<std::string, std::string> extra;
          if (leftover) {
            extra["leftover_live_set_id"] = id;  // reported, never silent: the new file is still on disk
            extra["leftover_reason"] = liveset::to_string(rm);
          }
          if (ss == sl::Status::kNoSpace) return fail(err, ws::v1::ERROR_CODE_STORAGE_FULL, "the storage is full; nothing was written and the edits stay in memory", extra);
          if (ss == sl::Status::kLimitReached) {
            extra["limit_name"] = "live_sets_per_setlist";
            extra["limit"] = std::to_string(sl::kMaxLiveSetsPerSetlist);
            return fail(err, ws::v1::ERROR_CODE_LIMIT_REACHED, "the setlist is full", extra);
          }
          extra["reason"] = sl::to_string(ss);
          return fail(err, ws::v1::ERROR_CODE_STORAGE_FAILED, "the setlist could not be written; nothing changed", extra);
        }
        if (ss == sl::Status::kCommittedNotDurable) not_durable = true;
        r->set_setlist_id(loaded_setlist);
        r->set_setlist_index(index ? static_cast<int>(*index) : size_before);
      }
    }
    r->set_written_live_set_id(id);
    r->set_written_name(new_name);
    r->set_not_durable(not_durable);
    ++writes_done_;
    ++edit_seq_;
    bump = true;
    finish(false);  // the current Live Set keeps its edits: remaining_changes says so
    return true;
  }

  // Scene / Whole: the file of the current Live Set. Scene = the saved Live Set with the current scene's stored overrides replaced by the edited ones.
  lsp::LiveSet out = scope == ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET ? d->working : d->saved;
  if (scope == ws::v1::WRITE_SCOPE_SCENE) out.mutable_scenes(scene)->CopyFrom(d->working.scenes(scene));
  const liveset::Status st = live_.write(out);
  if (!(st == liveset::Status::kOk || st == liveset::Status::kCommittedNotDurable)) return store_failure(err, st);
  r->set_not_durable(st == liveset::Status::kCommittedNotDurable);
  if (port_) port_->live_set_written(live_id);  // Story 4.7: a preload of this Live Set (when it is the next one) is stale
  if (port_) {
    // A Live Set whose file is not named <id>.liveset (a file copied into the directory) now has its canonical file: the catalog points to it and the old
    // file, directly in the Live Set directory, is removed so the id exists once (best effort: a leftover is still a valid, older copy).
    std::string old_path;
    for (const CatalogEntry& e : port_->catalog())
      if (e.id == live_id && !e.path.empty() && e.path != live_.path_for(live_id)) old_path = e.path;
    if (!old_path.empty()) {
      port_->add_to_catalog(CatalogEntry{live_id, out.name(), live_.path_for(live_id)});
      (void)live_.remove(live_id, old_path);
    }
  }
  d->saved = std::move(out);
  ++d->version;  // the difference to the saved state changed
  ++writes_done_;
  ++edit_seq_;
  bump = true;
  finish(false);
  return true;
}

}  // namespace velora::host
