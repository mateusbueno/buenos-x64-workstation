#pragma once
// Category suggestion (Story 5.5): from an instrument's file name, folder names and metadata strings to ONE
// suggested category or none. A suggestion only: the import still needs an explicit category from the caller
// (the Import surface, Story 5.12/5.7 preflight, or `--category`).
//
// Algorithm (deterministic, no learning, no network):
//  1. Tokenise every text: UTF-8 decoded, accents folded (PT/ES diacritics, also NFD combining marks), lower-cased,
//     split on anything that is not a letter, on underscores, camelCase boundaries ("EPiano" -> e piano,
//     "XMLFile" -> xml file) and digits (digits are dropped: "Rhodes_Mk1" -> rhodes mk).
//  2. Match the versioned keyword table (librarian/data/category_keywords.json, EN + PT-BR + ES): a phrase is a
//     sequence of whole tokens. Candidates are taken in order of phrase length in tokens (longest first), then
//     weight (heavier first), then position in the text, and may not overlap: "bass clarinet" (Woodwind) consumes "bass".
//  3. Precedence: file name, then metadata (SFZ labels/comments, SF2 preset and bank names), then folder names.
//     The FIRST source that has any match decides, and its decision is final:
//       - an explicit exception phrase (category `none`, e.g. "viola caipira") -> none;
//       - otherwise the category with the highest summed weight (each distinct phrase counts once); a tie for the
//         top -> none (never guess ambiguously);
//     a source without any match falls through to the next one.
//  Nothing matches -> none. Prefer none over a wrong guess.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "velora/format/category.hpp"

namespace velora::librarian {

struct SuggestInput {
  std::string file_name;                    // with or without a known extension, a path is reduced to its last component
  std::vector<std::string> folder_names;    // the folders the file sits in
  std::vector<std::string> metadata_strings;  // SFZ <control> labels / comments, SF2 preset and bank names
};

// Tokens of one text (see above). Exposed for tests.
std::vector<std::string> tokenize_for_category(std::string_view text);

struct KeywordEntry {
  std::string phrase;                 // normalised: tokens joined by one space
  std::vector<std::string> tokens;
  velora::format::Category category;  // kNone = an explicit exception ("do not guess here")
  int weight = 10;
};

struct KeywordTableLoaded;

class KeywordTable {
 public:
  // Parses the JSON table: {"table_version": N, "entries": [{"category": slug|"none", "weight": 1..100,
  // "phrases": [..]}, ...]}. Fails on unknown categories, empty phrases, a phrase listed twice (also under another
  // category after normalisation) and bad weights.
  static bool parse(std::string_view json, KeywordTable& out, std::string* error = nullptr);
  static bool load_file(const std::string& path, KeywordTable& out, std::string* error = nullptr);
  // The table compiled into the binary from librarian/data/category_keywords.json (CMake embeds it at configure time).
  static const KeywordTable& embedded();
  // Empty when the embedded table parsed; otherwise why not (the table is then empty and nothing is ever suggested).
  // Failure is loud: a message on stderr and an assert in debug builds. The CLI exits 3 on it.
  static const std::string& embedded_error();
  // Parses `json` as the embedded table would be (the hook tests use to corrupt the embedded text).
  static KeywordTableLoaded load_embedded(std::string_view json);
  bool valid() const { return !entries_.empty(); }
  static std::string_view embedded_json();

  std::uint32_t version() const { return version_; }
  const std::vector<KeywordEntry>& entries() const { return entries_; }

 private:
  std::uint32_t version_ = 0;
  std::vector<KeywordEntry> entries_;
};

struct KeywordTableLoaded {
  KeywordTable table;
  std::string error;  // empty = loaded
};

// The suggestion, or nullopt for none.
std::optional<velora::format::Category> suggest_category(const SuggestInput& in,
                                                         const KeywordTable& table = KeywordTable::embedded());

// Metadata strings of an SFZ source: the values of `label_*` opcodes and the text of `//` comments, scanned from
// the file text (no #include is followed; `/* */` blocks are skipped, `//` is a comment only at a line start or after
// whitespace). Only a regular file is read (a FIFO or device gives nothing), at most 1 MiB and 4096 bytes of each line.
// Up to 64 label values then up to 32 comments (separate caps), 200 bytes each; empty on an unreadable file.
std::vector<std::string> sfz_metadata_strings(const std::string& path);

// The input for a source file: the file name, its parent folders (up to 3 nearest, after dropping volume and account
// names such as /Volumes/<vol>) and `metadata`. A path containing '/' is made absolute and normalised first, so the
// spelling (relative, `./`, `..`) does not change the result; a bare name has no folders.
SuggestInput suggest_input_for_path(const std::string& path, std::vector<std::string> metadata);

}  // namespace velora::librarian
