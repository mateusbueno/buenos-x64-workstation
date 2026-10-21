#pragma once
// Story 6.9: the typed errors and exit codes of tools/factory-release. Codes 2..14 are exactly those of tools/content-pack (a failure of a library's pack is
// passed through unchanged, so a vendor-id conflict is still 6); the numbers 20 and up are the release's own. The numbers are part of the tool's contract
// (README.md).
#include <string>
#include <utility>

#include "errors.hpp"  // tools/content-pack/src/errors.hpp (via the include path of velora_content_pack)

namespace velora::factoryrelease {

enum class Code : int {
  kOk = 0,
  kUsage = 2,
  kBadDescription = 3,
  kBadKeyFile = 4,
  kRootMismatch = 5,
  kLibraryIdConflict = 6,
  kLibraryIdUnregistered = 7,
  kRegisterIo = 8,
  kImportSkipped = 9,
  kImportFailed = 10,
  kBundleFailed = 11,
  kVaultFailed = 12,
  kOutputFailed = 13,
  kVerifyFailed = 14,
  kDuplicateLibrary = 20,     // the same library_id or the same library name twice in the release
  kDuplicateProgramId = 21,   // a program id is 0 or used twice
  kMissingCategory = 22,      // a library or an instrument without an assignable category
  kUnresolvedReference = 23,  // a factory Live Set names a library that is not in the release, or an instrument the library does not have
  kInvalidLiveSet = 24,       // a factory Live Set that does not validate
  kProgramHistory = 25,       // against --previous-index: a program id removed, or content changed without raising the release version / the library revision
  kInternal = 70,
};

const char* to_string(Code c);

struct Failure {
  Code code = Code::kOk;
  std::string message;
  explicit operator bool() const { return code != Code::kOk; }
};
inline Failure fail(Code c, std::string m) { return Failure{c, std::move(m)}; }
inline Failure from_pack(const velora::contentpack::Failure& f) { return Failure{static_cast<Code>(static_cast<int>(f.code)), f.message}; }

}  // namespace velora::factoryrelease
