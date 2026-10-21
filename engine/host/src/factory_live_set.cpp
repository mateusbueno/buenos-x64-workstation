// Factory Live Sets, Story 6.9. See velora/host/factory_live_set.hpp for the file layout and the install rules. Control thread only.
#include "velora/host/factory_live_set.hpp"

#include <algorithm>
#include <filesystem>
#include <cstring>
#include <set>
#include <sstream>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "velora/format/hash.hpp"

namespace velora::host::factory {
namespace {

void put_le16(std::string& s, std::uint32_t v) {
  s.push_back(static_cast<char>(v & 0xff));
  s.push_back(static_cast<char>((v >> 8) & 0xff));
}
void put_le32(std::string& s, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
std::uint32_t get_le32(const char* p) {
  const auto* u = reinterpret_cast<const std::uint8_t*>(p);
  return static_cast<std::uint32_t>(u[0]) | static_cast<std::uint32_t>(u[1]) << 8 | static_cast<std::uint32_t>(u[2]) << 16 | static_cast<std::uint32_t>(u[3]) << 24;
}
std::uint32_t get_le16(const char* p) {
  const auto* u = reinterpret_cast<const std::uint8_t*>(p);
  return static_cast<std::uint32_t>(u[0]) | static_cast<std::uint32_t>(u[1]) << 8;
}

bool parse_version(std::string_view v, std::uint64_t out[3]) {
  std::size_t pos = 0;
  for (int part = 0; part < 3; ++part) {
    std::size_t end = pos;
    while (end < v.size() && v[end] >= '0' && v[end] <= '9') ++end;
    const std::size_t n = end - pos;
    if (n == 0 || n > 9) return false;
    if (n > 1 && v[pos] == '0') return false;
    std::uint64_t x = 0;
    for (std::size_t i = pos; i < end; ++i) x = x * 10 + static_cast<std::uint64_t>(v[i] - '0');
    out[part] = x;
    pos = end;
    if (part < 2) {
      if (pos >= v.size() || v[pos] != '.') return false;
      ++pos;
    }
  }
  return pos == v.size();
}

bool printable_name(const std::string& s, std::size_t max) {
  if (s.empty() || s.size() > max) return false;
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7f) return false;
  return true;
}

// ---- the state file ------------------------------------------------------------------------------------------------------------------------------
struct State {
  bool present = false;
  std::string release, root;
  std::set<std::uint32_t> programs;
};

constexpr const char* kStateHeader = "velora-factory-live-sets-state 1";

bool parse_program_id(const std::string& n, std::uint32_t& out) {
  if (n.empty() || n.size() > 10 || n.find_first_not_of("0123456789") != std::string::npos) return false;
  const unsigned long long v = std::stoull(n);
  if (v == 0 || v > 0xffffffffull) return false;
  out = static_cast<std::uint32_t>(v);
  return true;
}

// kOk with `st.present` false when the file is absent; kParseError / kIo when it cannot be used.
liveset::Status read_state(const std::string& dir, State& st) {
  std::string text;
  const liveset::Status rs = liveset::read_regular_file(dir + "/" + kStateFileName, 1u << 20, text);
  if (rs == liveset::Status::kNotFound) return liveset::Status::kOk;
  if (rs != liveset::Status::kOk) return rs;
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line != kStateHeader) return liveset::Status::kParseError;
  bool have_release = false;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (line.rfind("release ", 0) == 0 && !have_release) {
      st.release = line.substr(8);
      if (!valid_release_version(st.release)) return liveset::Status::kParseError;
      have_release = true;
    } else if (line.rfind("root ", 0) == 0) {
      st.root = line.substr(5);
    } else if (line.rfind("program ", 0) == 0) {
      std::uint32_t id = 0;
      if (!parse_program_id(line.substr(8), id)) return liveset::Status::kParseError;
      st.programs.insert(id);
    } else if (line.find(' ') == std::string::npos) {
      return liveset::Status::kParseError;
    }  // else: an unknown `key value` line of a newer writer: ignored
  }
  if (!have_release) return liveset::Status::kParseError;
  st.present = true;
  return liveset::Status::kOk;
}

std::string render_state(const State& st) {
  std::string out = std::string(kStateHeader) + "\nrelease " + st.release + "\nroot " + st.root + "\n";
  for (std::uint32_t p : st.programs) out += "program " + std::to_string(p) + "\n";
  return out;
}

// The state as the store shows it: every readable Live Set with a factory mark counts as handled, the highest mark is the floor. Used when the state file is
// missing or damaged. `present` is true only when at least one mark was found.
State rebuild_state(const std::string& dir, const std::string& root) {
  State st;
  st.root = root;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".liveset") continue;
    liveset::LoadResult lr;
    liveset::ReadOptions ro;
    ro.part_count = 0;  // any tier
    if (liveset::read_file(e.path().string(), ro, lr) != liveset::Status::kOk || !lr.live_set.has_factory() || lr.live_set.factory().program_id() == 0) continue;
    st.programs.insert(lr.live_set.factory().program_id());
    const std::string& v = lr.live_set.factory().release_version();
    if (valid_release_version(v) && (!st.present || compare_release_versions(v, st.release) > 0)) st.release = v;
    st.present = true;
  }
  return st;
}

}  // namespace

const char* to_string(ContainerStatus s) {
  switch (s) {
    case ContainerStatus::kOk: return "ok";
    case ContainerStatus::kTooLarge: return "too_large";
    case ContainerStatus::kTruncated: return "truncated";
    case ContainerStatus::kBadMagic: return "bad_magic";
    case ContainerStatus::kUnsupportedVersion: return "unsupported_version";
    case ContainerStatus::kBadLength: return "bad_length";
    case ContainerStatus::kUntrustedKey: return "untrusted_key";
    case ContainerStatus::kSignatureInvalid: return "signature_invalid";
    case ContainerStatus::kBadPayload: return "bad_payload";
    case ContainerStatus::kRootMismatch: return "root_mismatch";
  }
  return "?";
}

const char* to_string(FactoryStatus s) {
  switch (s) {
    case FactoryStatus::kOk: return "ok";
    case FactoryStatus::kIo: return "io";
    case FactoryStatus::kBadContainer: return "bad_container";
    case FactoryStatus::kUntrustedKey: return "untrusted_key";
    case FactoryStatus::kSignatureInvalid: return "signature_invalid";
    case FactoryStatus::kBadRelease: return "bad_release";
    case FactoryStatus::kDuplicateProgramId: return "duplicate_program_id";
    case FactoryStatus::kInvalidLiveSet: return "invalid_live_set";
    case FactoryStatus::kRootMismatch: return "root_mismatch";
    case FactoryStatus::kUnresolvedReferences: return "unresolved_references";
    case FactoryStatus::kDowngrade: return "downgrade";
    case FactoryStatus::kStore: return "store";
  }
  return "?";
}

std::vector<std::uint8_t> signing_statement(const fmt::KeyId& signer, std::string_view payload) {
  std::vector<std::uint8_t> m;
  const std::size_t dl = std::strlen(kSignatureDomain);
  m.insert(m.end(), kSignatureDomain, kSignatureDomain + dl);
  m.push_back(0);
  m.insert(m.end(), signer.begin(), signer.end());
  const auto n = static_cast<std::uint32_t>(payload.size());
  for (int i = 0; i < 4; ++i) m.push_back(static_cast<std::uint8_t>((n >> (8 * i)) & 0xff));
  m.insert(m.end(), payload.begin(), payload.end());
  return m;
}

std::string assemble_container(const fmt::KeyId& signer, const fmt::Signature& signature, std::string_view payload) {
  std::string out(kMagic, 4);
  put_le16(out, kContainerVersion);
  put_le16(out, 0);
  out.append(reinterpret_cast<const char*>(signer.data()), signer.size());
  out.append(reinterpret_cast<const char*>(signature.data()), signature.size());
  put_le32(out, static_cast<std::uint32_t>(payload.size()));
  out.append(payload.data(), payload.size());
  return out;
}

bool serialize_payload(const lsp::FactoryLiveSets& sets, std::string& out) {
  out.clear();
  google::protobuf::io::StringOutputStream sos(&out);
  google::protobuf::io::CodedOutputStream cos(&sos);
  cos.SetSerializationDeterministic(true);
  return sets.SerializeToCodedStream(&cos) && !cos.HadError();
}

std::string derive_live_set_id(std::uint32_t program_id) {
  std::vector<std::uint8_t> in;
  const std::size_t dl = std::strlen(kLiveSetIdDomain);
  in.insert(in.end(), kLiveSetIdDomain, kLiveSetIdDomain + dl);
  in.push_back(0);
  for (int i = 3; i >= 0; --i) in.push_back(static_cast<std::uint8_t>((program_id >> (8 * i)) & 0xff));
  const fmt::Digest256 d = fmt::Sha256::hash(in);
  fmt::Uuid u{};
  std::copy_n(d.begin(), 16, u.begin());
  u[6] = static_cast<std::uint8_t>((u[6] & 0x0f) | 0x40);
  u[8] = static_cast<std::uint8_t>((u[8] & 0x3f) | 0x80);
  return fmt::uuid_to_string(u);
}

bool valid_release_version(std::string_view v) {
  std::uint64_t x[3];
  return parse_version(v, x);
}

int compare_release_versions(std::string_view a, std::string_view b) {
  std::uint64_t x[3] = {0, 0, 0}, y[3] = {0, 0, 0};
  (void)parse_version(a, x);
  (void)parse_version(b, y);
  for (int i = 0; i < 3; ++i)
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}

bool is_factory(const lsp::LiveSet& ls) { return ls.has_factory() && ls.factory().program_id() > 0; }

ContainerStatus parse_container(std::string_view bytes, ParsedContainer& out) {
  if (bytes.size() > kMaxFileBytes) return ContainerStatus::kTooLarge;
  if (bytes.size() < kHeaderBytes) return ContainerStatus::kTruncated;
  if (std::memcmp(bytes.data(), kMagic, 4) != 0) return ContainerStatus::kBadMagic;
  if (get_le16(bytes.data() + 4) != kContainerVersion || get_le16(bytes.data() + 6) != 0) return ContainerStatus::kUnsupportedVersion;
  const std::uint32_t n = get_le32(bytes.data() + 80);
  if (n > kMaxPayloadBytes) return ContainerStatus::kTooLarge;
  if (bytes.size() < kHeaderBytes + static_cast<std::size_t>(n)) return ContainerStatus::kTruncated;
  if (bytes.size() > kHeaderBytes + static_cast<std::size_t>(n)) return ContainerStatus::kBadLength;
  std::memcpy(out.signer.data(), bytes.data() + 8, 8);
  std::memcpy(out.signature.data(), bytes.data() + 16, 64);
  out.payload = bytes.substr(kHeaderBytes, n);
  return ContainerStatus::kOk;
}

ContainerStatus verify_container(std::string_view bytes, const fmt::TrustedKeys& trusted, lsp::FactoryLiveSets& out, fmt::KeyId* signer) {
  ParsedContainer pc;
  if (const ContainerStatus s = parse_container(bytes, pc); s != ContainerStatus::kOk) return s;
  const fmt::TrustedKey* key = trusted.find(pc.signer);
  if (!key) return ContainerStatus::kUntrustedKey;
  const std::vector<std::uint8_t> msg = signing_statement(pc.signer, pc.payload);
  if (!fmt::ed25519_verify_strict(key->pub, msg, pc.signature)) return ContainerStatus::kSignatureInvalid;
  lsp::FactoryLiveSets sets;
  google::protobuf::io::CodedInputStream in(reinterpret_cast<const std::uint8_t*>(pc.payload.data()), static_cast<int>(pc.payload.size()));
  in.SetRecursionLimit(32);
  if (!sets.ParseFromCodedStream(&in) || !in.ConsumedEntireMessage()) return ContainerStatus::kBadPayload;
  if (!key->root.empty() && sets.root() != key->root) return ContainerStatus::kRootMismatch;
  out = std::move(sets);
  if (signer) *signer = pc.signer;
  return ContainerStatus::kOk;
}

ReferenceResolver resolver_of(const LibraryResolver& resolver) {
  const LibraryResolver* r = &resolver;
  return [r](const std::string& lib, const std::string& key) { return r->resolve(lib, key).ok(); };
}

InstallResult install_factory_live_sets_file(liveset::LiveSetStore& store, const std::string& path, const InstallOptions& opt) {
  std::string bytes;
  const liveset::Status rs = liveset::read_regular_file(path, kMaxFileBytes, bytes);
  if (rs != liveset::Status::kOk) {
    InstallResult r;
    r.status = rs == liveset::Status::kTooLarge ? FactoryStatus::kBadContainer : FactoryStatus::kIo;
    r.container = rs == liveset::Status::kTooLarge ? ContainerStatus::kTooLarge : ContainerStatus::kOk;
    r.detail = path + ": " + liveset::to_string(rs);
    return r;
  }
  return install_factory_live_sets(store, bytes, opt);
}

InstallResult install_factory_live_sets(liveset::LiveSetStore& store, std::string_view container_bytes, const InstallOptions& opt) {
  InstallResult res;
  auto refuse = [&](FactoryStatus s, std::string why) {
    res.status = s;
    res.detail = std::move(why);
    return res;
  };
  if (!opt.trusted || !opt.resolves) return refuse(FactoryStatus::kBadContainer, "the install needs trusted keys and a reference resolver");

  // 1. container, trust, signature.
  lsp::FactoryLiveSets sets;
  const ContainerStatus cs = verify_container(container_bytes, *opt.trusted, sets);
  res.container = cs;
  switch (cs) {
    case ContainerStatus::kOk: break;
    case ContainerStatus::kUntrustedKey: return refuse(FactoryStatus::kUntrustedKey, "the signer of the factory Live Sets is not a trusted key");
    case ContainerStatus::kSignatureInvalid: return refuse(FactoryStatus::kSignatureInvalid, "the signature of the factory Live Sets does not verify");
    case ContainerStatus::kRootMismatch: return refuse(FactoryStatus::kRootMismatch, "the signer is trusted for another root than the release's");
    default: return refuse(FactoryStatus::kBadContainer, std::string("the factory Live Set file is not valid: ") + to_string(cs));
  }

  // 2. the payload.
  res.release_name = sets.release_name();
  res.release_version = sets.release_version();
  if (sets.format_version() != kFormatVersion) return refuse(FactoryStatus::kBadRelease, "unsupported payload format_version");
  if (!printable_name(sets.release_name(), kMaxReleaseNameBytes)) return refuse(FactoryStatus::kBadRelease, "bad release name");
  if (!valid_release_version(sets.release_version())) return refuse(FactoryStatus::kBadRelease, "release version is not MAJOR.MINOR.PATCH");
  if (sets.root() != "dev" && sets.root() != "factory-test" && sets.root() != "production") return refuse(FactoryStatus::kBadRelease, "root is not dev, factory-test or production");
  if (sets.entries_size() == 0 || static_cast<std::size_t>(sets.entries_size()) > kMaxEntries) return refuse(FactoryStatus::kBadRelease, "the number of entries is outside 1.." + std::to_string(kMaxEntries));
  std::vector<lsp::LiveSet> sets_norm;
  std::set<std::uint32_t> seen_programs;
  for (const lsp::FactoryLiveSetEntry& e : sets.entries()) {
    res.program_id = e.program_id();
    if (e.program_id() == 0) return refuse(FactoryStatus::kDuplicateProgramId, "program id 0 is not a program id");
    if (!seen_programs.insert(e.program_id()).second) return refuse(FactoryStatus::kDuplicateProgramId, "program id " + std::to_string(e.program_id()) + " is used twice");
    lsp::LiveSet ls = e.live_set();
    liveset::normalize(ls);
    const liveset::ValidationReport rep = liveset::validate(ls, opt.part_count);
    if (!rep.ok()) return refuse(FactoryStatus::kInvalidLiveSet, "program " + std::to_string(e.program_id()) + ": the Live Set is not valid for this tier");
    if (ls.format_version() != liveset::kFormatVersion) return refuse(FactoryStatus::kInvalidLiveSet, "program " + std::to_string(e.program_id()) + ": unsupported Live Set format_version");
    if (ls.id() != derive_live_set_id(e.program_id())) return refuse(FactoryStatus::kInvalidLiveSet, "program " + std::to_string(e.program_id()) + ": the Live Set id is not the one of its program id");
    if (!ls.has_factory() || ls.factory().program_id() != e.program_id() || ls.factory().release_version() != sets.release_version())
      return refuse(FactoryStatus::kInvalidLiveSet, "program " + std::to_string(e.program_id()) + ": the factory mark does not match the entry and the release");
    sets_norm.push_back(std::move(ls));
  }
  res.program_id = 0;

  // 3. every reference resolves, or nothing is written.
  for (std::size_t i = 0; i < sets_norm.size(); ++i) {
    const lsp::LiveSet& ls = sets_norm[i];
    for (int p = 0; p < ls.parts_size(); ++p) {
      const lsp::Part& part = ls.parts(p);
      if (!part.has_timbre()) continue;
      if (!opt.resolves(part.timbre().library_id(), part.timbre().instrument_key()))
        res.missing.push_back(MissingReference{ls.factory().program_id(), static_cast<std::uint32_t>(p), part.timbre().library_id(), part.timbre().instrument_key()});
    }
  }
  if (!res.missing.empty()) {
    std::string d = std::to_string(res.missing.size()) + " instrument reference(s) do not resolve:";
    for (const MissingReference& m : res.missing)
      d += " [program " + std::to_string(m.program_id) + " part " + std::to_string(m.part) + " " + m.library_id + " / " + m.instrument_key + "]";
    return refuse(FactoryStatus::kUnresolvedReferences, d);
  }

  // 4. the state (per root; rebuilt from the factory marks in the store when it is missing or damaged), then each Live Set.
  const std::string dir = store.options().dir;
  State state;
  bool from_file = false;
  {
    State st;
    const liveset::Status rs = read_state(dir, st);
    if (rs == liveset::Status::kOk && st.present && st.root == sets.root()) {
      state = st;
      from_file = true;
    } else if (rs == liveset::Status::kOk && st.present) {
      state = State{};  // another root: its own, fresh state; nothing is deleted
    } else {
      state = rebuild_state(dir, sets.root());
    }
    state.root = sets.root();
  }
  if (state.present && compare_release_versions(sets.release_version(), state.release) < 0)
    return refuse(FactoryStatus::kDowngrade, "release " + sets.release_version() + " is older than the installed " + state.release);

  auto write_state = [&](const std::set<std::uint32_t>& programs) {
    State next;
    next.present = true;
    next.root = sets.root();
    next.release = state.present && compare_release_versions(state.release, sets.release_version()) > 0 ? state.release : sets.release_version();
    next.programs = programs;
    if (from_file && next.release == state.release && next.programs == state.programs) return liveset::Status::kOk;
    return liveset::atomic_replace_file(dir, kStateFileName, std::string(kStateFileName) + ".tmp", render_state(next), store.options().on_stage,
                                        store.options().inject_dir_sync_failure, store.options().inject_write_errno);
  };
  std::set<std::uint32_t> handled = state.programs;  // the ids the state will record: what was there plus what this run actually handled

  for (const lsp::LiveSet& ls : sets_norm) {
    const std::uint32_t pid = ls.factory().program_id();
    liveset::LoadResult cur;
    const liveset::Status rd = store.read(ls.id(), cur);
    bool write = false;
    if (rd == liveset::Status::kNotFound) {
      if (state.programs.count(pid)) res.deleted_by_user.push_back(pid);
      else {
        write = true;
        res.imported.push_back(pid);
      }
    } else if (rd == liveset::Status::kOk && is_factory(cur.live_set) && cur.live_set.factory().program_id() == pid) {
      if (compare_release_versions(sets.release_version(), cur.live_set.factory().release_version()) > 0) {
        write = true;
        res.updated.push_back(pid);
      } else {
        res.unchanged.push_back(pid);
      }
    } else if (rd == liveset::Status::kOk) {
      res.conflicts.push_back(pid);  // a user's Live Set owns that id: never touched
    } else {
      write = true;  // a damaged or unreadable file at a factory id is ours to repair
      res.updated.push_back(pid);
    }
    if (write) {
      const liveset::Status ws = store.write(ls);
      if (!(ws == liveset::Status::kOk || ws == liveset::Status::kCommittedNotDurable)) {
        res.program_id = pid;
        res.imported.erase(std::remove(res.imported.begin(), res.imported.end(), pid), res.imported.end());
        res.updated.erase(std::remove(res.updated.begin(), res.updated.end(), pid), res.updated.end());
        // Record what was actually handled before the failure, so the rerun does not mistake it for a user's deletion.
        if (handled != state.programs) (void)write_state(handled);
        return refuse(FactoryStatus::kStore, "program " + std::to_string(pid) + ": " + liveset::to_string(ws) + " (run the install again)");
      }
    }
    if (std::find(res.conflicts.begin(), res.conflicts.end(), pid) == res.conflicts.end()) handled.insert(pid);
  }

  // 5. the state, last.
  const liveset::Status ss = write_state(handled);
  if (!(ss == liveset::Status::kOk || ss == liveset::Status::kCommittedNotDurable)) return refuse(FactoryStatus::kStore, std::string("state file: ") + liveset::to_string(ss) + " (run the install again)");
  res.status = FactoryStatus::kOk;
  return res;
}

}  // namespace velora::host::factory
