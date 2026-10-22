#include "velora/format/parametric_bundle.hpp"

#include <cstring>
#include <fstream>
#include <filesystem>

namespace velora::format {

namespace {

constexpr char kBundleMagic[4] = {'V', 'L', 'P', 'B'};
constexpr uint32_t kBundleVersion = 1;

void put_u8(std::vector<uint8_t>& v, uint8_t x) { v.push_back(x); }
void put_u32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void put_u64(std::vector<uint8_t>& v, uint64_t x) {
  for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void put_str(std::vector<uint8_t>& v, std::string_view s) {
  put_u32(v, static_cast<uint32_t>(s.size()));
  v.insert(v.end(), s.begin(), s.end());
}

uint32_t get_u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t get_u64(const uint8_t* p) {
  return uint64_t(get_u32(p)) | (uint64_t(get_u32(p + 4)) << 32);
}

} // namespace

Error ParametricBundle::set_source_id(std::string_view source_id) {
  if (source_id.empty()) return Error::kInvalidArgument;
  library_id_ = library_id_from_source(source_id);
  return Error::kNone;
}

Error ParametricBundle::add_timbre(ParametricTimbre timbre) {
  if (timbre.key.empty() || timbre.name.empty()) return Error::kInvalidArgument;
  for (const auto& t : timbres_) {
    if (t.key == timbre.key) return Error::kDuplicateKey;
  }
  timbres_.push_back(std::move(timbre));
  return Error::kNone;
}

Result<ParametricBundle> ParametricBundle::from_sysex(const SysexParseResult& sysex,
                                                      std::string_view source_path,
                                                      std::string_view library_name,
                                                      Category default_category) {
  if (!sysex.ok() || sysex.voices.empty()) {
    return Error::kInvalidArgument;
  }

  ParametricBundle bundle;
  if (!source_path.empty()) {
    if (Error e = bundle.set_source_id(source_path); e != Error::kNone) return e;
  } else {
    // Generate deterministic library_id from voice parameters
    Sha256 h;
    for (const auto& v : sysex.voices) {
      uint8_t packed[128]{};
      std::span<uint8_t, 128> s(packed, 128);
      pack_128_voice(v.parameters, s);
      h.update(packed, 128);
    }
    bundle.set_library_id(library_id_from_fingerprint(h.finish()));
  }

  bundle.set_name(library_name);

  for (const auto& v : sysex.voices) {
    ParametricTimbre t;
    t.key = std::to_string(v.slot);
    t.name = v.name;
    t.category = default_category != Category::kNone ? default_category : Category::kSynthPoly;
    t.engine_type_id = kEngineTypeFm;
    t.fm_parameters = v.parameters;
    if (Error e = bundle.add_timbre(std::move(t)); e != Error::kNone) return e;
  }

  return bundle;
}

std::vector<uint8_t> ParametricBundle::serialize() const {
  std::vector<uint8_t> payload;
  put_str(payload, name_);
  put_u32(payload, static_cast<uint32_t>(timbres_.size()));

  for (const auto& t : timbres_) {
    put_str(payload, t.key);
    put_str(payload, t.name);
    put_u8(payload, static_cast<uint8_t>(t.category));
    put_u32(payload, t.engine_type_id);
    if (t.engine_type_id == kEngineTypeFm) {
      uint8_t raw[155]{};
      std::span<uint8_t, 155> s(raw, 155);
      pack_155_voice(t.fm_parameters, s);
      put_u32(payload, 155);
      payload.insert(payload.end(), raw, raw + 155);
    } else {
      put_u32(payload, 0);
    }
  }

  Digest256 payload_hash = Sha256::hash(payload);

  std::vector<uint8_t> out;
  out.reserve(4 + 4 + 16 + 32 + 8 + 32 + payload.size());

  out.insert(out.end(), kBundleMagic, kBundleMagic + 4);
  put_u32(out, kBundleVersion);
  out.insert(out.end(), library_id_.begin(), library_id_.end());
  out.insert(out.end(), payload_hash.begin(), payload_hash.end());
  put_u64(out, payload.size());
  out.insert(out.end(), payload_hash.begin(), payload_hash.end());

  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

Result<ParametricBundle> ParametricBundle::deserialize(std::span<const uint8_t> bytes) {
  constexpr size_t kHeaderSize = 4 + 4 + 16 + 32 + 8 + 32;
  if (bytes.size() < kHeaderSize) return Error::kTruncated;

  if (std::memcmp(bytes.data(), kBundleMagic, 4) != 0) return Error::kBadMagic;

  uint32_t version = get_u32(bytes.data() + 4);
  if (version != kBundleVersion) return Error::kUnsupportedVersion;

  ParametricBundle bundle;
  std::copy(bytes.data() + 8, bytes.data() + 24, bundle.library_id_.begin());
  std::copy(bytes.data() + 24, bytes.data() + 56, bundle.bundle_id_.begin());

  uint64_t payload_size = get_u64(bytes.data() + 56);
  Digest256 payload_hash;
  std::copy(bytes.data() + 64, bytes.data() + 96, payload_hash.begin());

  if (bytes.size() != kHeaderSize + payload_size) return Error::kTruncated;

  std::span<const uint8_t> payload_span(bytes.data() + kHeaderSize, payload_size);
  if (Sha256::hash(payload_span) != payload_hash) return Error::kCorruptManifest;

  const uint8_t* p = payload_span.data();
  const uint8_t* end = p + payload_span.size();

  auto get_str_p = [&]() -> std::string {
    if (p + 4 > end) return {};
    uint32_t len = get_u32(p);
    p += 4;
    if (p + len > end) return {};
    std::string s(reinterpret_cast<const char*>(p), len);
    p += len;
    return s;
  };

  bundle.name_ = get_str_p();
  if (p + 4 > end) return Error::kCorruptManifest;
  uint32_t count = get_u32(p);
  p += 4;

  bundle.timbres_.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    ParametricTimbre t;
    t.key = get_str_p();
    t.name = get_str_p();
    if (p + 1 + 4 + 4 > end) return Error::kCorruptManifest;
    t.category = static_cast<Category>(*p++);
    t.engine_type_id = get_u32(p);
    p += 4;
    uint32_t param_len = get_u32(p);
    p += 4;

    if (p + param_len > end) return Error::kCorruptManifest;
    if (t.engine_type_id == kEngineTypeFm && param_len == 155) {
      std::span<const uint8_t, 155> s(p, 155);
      if (!unpack_155_voice(s, t.fm_parameters)) return Error::kCorruptManifest;
    }
    p += param_len;
    bundle.timbres_.push_back(std::move(t));
  }

  return bundle;
}

Error ParametricBundle::write_file(const std::string& path) const {
  std::vector<uint8_t> data = serialize();
  std::string tmp_path = path + ".tmp";
  {
    std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return Error::kIo;
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    if (!f.good()) return Error::kIo;
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::filesystem::remove(tmp_path, ec);
    return Error::kIo;
  }
  return Error::kNone;
}

Result<ParametricBundle> ParametricBundle::read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return Error::kIo;
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return deserialize(data);
}

} // namespace velora::format
