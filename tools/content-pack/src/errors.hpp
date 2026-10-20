#pragma once
// Story 6.3: the typed errors and exit codes of tools/content-pack. Documented in README.md; the numbers are part of the tool's contract.
#include <string>
#include <utility>

namespace velora::contentpack {

enum class Code : int {
  kOk = 0,
  kUsage = 2,               // bad command line
  kBadDescription = 3,      // pack description unreadable, malformed, unknown field, bad value
  kBadKeyFile = 4,          // signing key, KEK, device key or content-key file unreadable, wrong size, too-open permissions
  kRootMismatch = 5,        // a key's sidecar says another root than the description (or a dev key for production)
  kLibraryIdConflict = 6,   // library_id (or name) already assigned to another library in the vendor ID register
  kLibraryIdUnregistered = 7,  // library_id is not in the register (run with --register, or the `register` command, first)
  kRegisterIo = 8,          // register unreadable, malformed or not writable
  kImportSkipped = 9,       // the source library was skipped by the importer (unsupported features or damaged)
  kImportFailed = 10,       // the importer failed (I/O, verification, invalid argument)
  kBundleFailed = 11,       // protected bundle could not be produced or did not verify after writing
  kVaultFailed = 12,        // vault entry / wrapped key could not be produced or opened
  kOutputFailed = 13,       // output directory exists (without --overwrite) or cannot be written
  kVerifyFailed = 14,       // `verify`: signature, hash, key or content check failed
  kInternal = 70,
};

const char* to_string(Code c);  // stable snake_case name, e.g. "library_id_conflict"

struct Failure {
  Code code = Code::kOk;
  std::string message;
  explicit operator bool() const { return code != Code::kOk; }
};
inline Failure fail(Code c, std::string m) { return Failure{c, std::move(m)}; }

}  // namespace velora::contentpack
