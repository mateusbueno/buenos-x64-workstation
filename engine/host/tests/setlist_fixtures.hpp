#pragma once
// Fixed setlist libraries shared by setlist_test (golden + corpus drift check) and setlist_corpus_gen (regeneration). Deterministic: ids come from
// a seeded random source, no clock, no OS randomness.
#include <string>
#include <vector>

#include "live_set_fixtures.hpp"
#include "velora/host/setlist_store.hpp"

namespace sfixtures {

namespace sl = velora::host::setlist;
namespace slp = velora::setlist::v1;
using fixtures::uuid_n;

inline slp::SetlistLibrary empty() {
  slp::SetlistLibrary l;
  l.set_format_version(sl::kFormatVersion);
  return l;
}

inline slp::SetlistTag* add_tag(slp::SetlistLibrary& l, std::uint64_t seed, const char* name) {
  slp::SetlistTag* t = l.add_tags();
  t->set_id(uuid_n(seed));
  t->set_name(name);
  return t;
}

inline slp::Setlist* add_setlist(slp::SetlistLibrary& l, std::uint64_t seed, const char* name) {
  slp::Setlist* s = l.add_setlists();
  s->set_id(uuid_n(seed));
  s->set_name(name);
  s->set_format_version(sl::kFormatVersion);
  return s;
}

// Three tags and four setlists: a Live Set shared by two setlists, an empty setlist, a setlist that lists a Live Set no file backs (kept, flagged
// at run time), a UTF-8 name, tags on setlists.
inline slp::SetlistLibrary basic() {
  slp::SetlistLibrary l = empty();
  add_tag(l, 7001, "Weddings");
  add_tag(l, 7002, "Jazz club");
  add_tag(l, 7003, "Rehearsal \xC3\xA9");  // "Rehearsal e-acute"
  slp::Setlist* a = add_setlist(l, 7101, "Friday night");
  for (std::uint64_t k : {1, 2, 3}) a->add_live_set_ids(uuid_n(k));
  a->add_tag_ids(uuid_n(7001));
  a->add_tag_ids(uuid_n(7002));
  slp::Setlist* b = add_setlist(l, 7102, "Saturday \xE2\x80\x93 late");  // en dash
  for (std::uint64_t k : {3, 5}) b->add_live_set_ids(uuid_n(k));
  b->add_live_set_ids(uuid_n(999999));  // no Live Set file with this id
  b->add_tag_ids(uuid_n(7002));
  add_setlist(l, 7103, "Empty for now");
  slp::Setlist* d = add_setlist(l, 7104, "Rehearsal");
  d->add_live_set_ids(uuid_n(2));
  d->add_tag_ids(uuid_n(7003));
  return l;
}

// What a newer firmware may add: unknown fields on the library, a tag and a setlist.
inline slp::SetlistLibrary newer_schema() {
  slp::SetlistLibrary l = basic();
  fixtures::unknown_varint(&l, 500, 7);
  l.GetReflection()->MutableUnknownFields(&l)->AddLengthDelimited(501, "from the future");
  fixtures::unknown_varint(l.mutable_tags(0), 600, 1);
  fixtures::unknown_varint(l.mutable_setlists(0), 601, 2);
  return l;
}

inline slp::AssignDefault assign_factory() {
  slp::AssignDefault a;
  a.set_format_version(sl::kFormatVersion);
  for (std::uint32_t id : sl::factory_default_assign()) a.add_assign(id);
  return a;
}

inline slp::AssignDefault assign_custom() {
  slp::AssignDefault a;
  a.set_format_version(sl::kFormatVersion);
  for (std::uint32_t id : {100u, 102u, 0u, 200u, 0u, 802u, 401u, 400u}) a.add_assign(id);
  return a;
}

struct CorpusFile {
  const char* name;
  std::string bytes;
};

inline std::vector<CorpusFile> corpus() {
  std::vector<CorpusFile> v;
  std::string b;
  auto lib = [&](const char* n, const slp::SetlistLibrary& l) {
    (void)sl::serialize(l, b);
    v.push_back({n, b});
  };
  auto as = [&](const char* n, const slp::AssignDefault& a) {
    (void)sl::serialize(a, b);
    v.push_back({n, b});
  };
  lib("empty.library", empty());
  lib("basic.library", basic());
  lib("newer-schema-unknown-fields.library", newer_schema());
  as("assign_default.pb", assign_factory());
  as("assign_default-custom.pb", assign_custom());
  return v;
}

}  // namespace sfixtures
