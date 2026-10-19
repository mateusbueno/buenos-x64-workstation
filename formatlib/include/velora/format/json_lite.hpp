#pragma once
// A small, strict JSON reader for the data files of the host tools (locale catalogs, the category keyword
// table). Not for the engine: it allocates freely. RFC 8259 subset: objects, arrays, strings (all escapes incl.
// \uXXXX with surrogate pairs, UTF-8 validated), numbers, true/false/null; nesting depth <= 32; trailing content
// and a UTF-8 BOM are rejected / skipped as noted; duplicate object keys are kept in order (the consumer decides).
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace velora::format {

struct JsonValue {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<JsonValue> array;
  // Object members in file order, as two parallel vectors (std::vector of an incomplete element is fine, a pair is not).
  std::vector<std::string> keys;
  std::vector<JsonValue> values;

  bool is_string() const { return type == Type::kString; }
  bool is_object() const { return type == Type::kObject; }
  bool is_array() const { return type == Type::kArray; }
  bool is_number() const { return type == Type::kNumber; }
  // First member named `k` of an object, or nullptr.
  const JsonValue* find(std::string_view k) const;
};

// Parses `text` (one value, optional surrounding whitespace, optional leading UTF-8 BOM). On failure returns
// false and `error` (if given) holds "offset N: reason".
// Input larger than kMaxJsonBytes is refused ("input too large").
inline constexpr std::size_t kMaxJsonBytes = 4u << 20;
bool parse_json(std::string_view text, JsonValue& out, std::string* error = nullptr);

// Reads a regular file of at most `max_bytes` (default kMaxJsonBytes) into `out`. A FIFO, device, directory,
// missing or larger file fails with a message in `error` (never blocks: opened non-blocking, then fstat).
bool read_text_file(const std::string& path, std::string& out, std::string* error = nullptr,
                    std::size_t max_bytes = kMaxJsonBytes);

// Number of Unicode code points of a UTF-8 string (invalid bytes count as one each).
std::size_t utf8_length(std::string_view s);

}  // namespace velora::format
