#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "velora/format/category.hpp"
#include "velora/format/error.hpp"
#include "velora/format/hash.hpp"
#include "velora/format/sysex_6op.hpp"
#include "velora/format/va_format.hpp"
#include "velora/format/ep_format.hpp"

namespace velora::format {

// Engine Type IDs corresponding to core/host EngineTypeId
inline constexpr uint32_t kEngineTypeSample = 1;
inline constexpr uint32_t kEngineTypeOrgan = 2;
inline constexpr uint32_t kEngineTypeFm = 3;
inline constexpr uint32_t kEngineTypeVa = 4;
inline constexpr uint32_t kEngineTypeEp = 5;

// One parametric timbre in the bundle.
struct ParametricTimbre {
  std::string key;               // instrument_key (e.g. "1".."32")
  std::string name;              // display name
  Category category{Category::kSynthPoly};
  uint32_t engine_type_id{kEngineTypeFm};
  FmVoiceParameters fm_parameters;
  VaVoiceParameters va_parameters;
  EpVoiceParameters ep_parameters;

  friend bool operator==(const ParametricTimbre&, const ParametricTimbre&) = default;
};

// Container for parametric synth sound libraries (FM, VA) without PCM audio.
// Follows the same stable identity rules (library_id, instrument_key) as sample libraries (FR23 semantics).
class ParametricBundle {
 public:
  ParametricBundle() = default;

  void set_library_id(const Uuid& id) { library_id_ = id; }
  Error set_source_id(std::string_view source_id);

  const Uuid& library_id() const noexcept { return library_id_; }
  const Digest256& bundle_id() const noexcept { return bundle_id_; }
  const std::string& name() const noexcept { return name_; }
  void set_name(std::string_view name) { name_ = std::string(name); }

  const std::vector<ParametricTimbre>& timbres() const noexcept { return timbres_; }
  std::vector<ParametricTimbre>& timbres() noexcept { return timbres_; }

  Error add_timbre(ParametricTimbre timbre);

  // Converts a valid SysEx parse result (bulk or single dump) into a ParametricBundle.
  static Result<ParametricBundle> from_sysex(const SysexParseResult& sysex,
                                             std::string_view source_path,
                                             std::string_view library_name,
                                             Category default_category = Category::kSynthPoly);

  // Binary serialization
  std::vector<uint8_t> serialize() const;
  static Result<ParametricBundle> deserialize(std::span<const uint8_t> bytes);

  // File I/O
  Error write_file(const std::string& path) const;
  static Result<ParametricBundle> read_file(const std::string& path);

 private:
  Uuid library_id_{};
  Digest256 bundle_id_{};
  std::string name_{"FM Library"};
  std::vector<ParametricTimbre> timbres_;
};

} // namespace velora::format
