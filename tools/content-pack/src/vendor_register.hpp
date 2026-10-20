#pragma once
// Story 6.3: the vendor ID register (tools/content-pack/vendor-ids). A committed text file, one line per assigned library:
//   library_id TAB name TAB owner TAB date          ('#' starts a comment line; fields never contain TAB; date is YYYY-MM-DD)
// The register is the single place that says which vendor library_id belongs to which library. The tool refuses a pack whose id is assigned to another
// library name (or whose name is assigned to another id); a new id is added only by an explicit register step.
#include <string>
#include <vector>

#include "errors.hpp"
#include "pack_description.hpp"

namespace velora::contentpack {

struct RegisterEntry {
  std::string library_id, name, owner, date;
};

// Identity of a library in the register is its name (normalize_name: trimmed, whitespace-collapsed, ASCII case-folded) together with its id. There is NO source column:
// the register does not bind an id to a source tree, only an id to a name and a name to an id.
struct VendorRegister {
  std::vector<RegisterEntry> entries;
  const RegisterEntry* find_id(const std::string& id) const;
  const RegisterEntry* find_name(const std::string& name) const;
};

// A missing file is an empty register only when `must_exist` is false. A malformed line, a duplicate id or a non-canonical id is kRegisterIo.
Failure load_register(const std::string& path, VendorRegister& out, bool must_exist = true);

// An exclusive flock on `<register>.lock` (opened O_NOFOLLOW), held for the object's lifetime. run_pack takes it before it checks the register and keeps it
// until the pack is published and the id appended, so two packs of the same id cannot both pass the check.
class RegisterLock {
 public:
  RegisterLock() = default;
  ~RegisterLock();
  RegisterLock(const RegisterLock&) = delete;
  RegisterLock& operator=(const RegisterLock&) = delete;
  Failure acquire(const std::string& register_path);
  bool held() const { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

enum class RegisterState { kRegistered, kNew, kDevRange };
bool is_random_v4(const velora::format::Uuid& u);
// kLibraryIdConflict when the id is assigned to another name or the name to another id. kNew when neither is in the register (and the id is outside the
// dev range, which is accepted for root `dev` only: the other roots report it as unregistered by the caller).
Failure check_register(const VendorRegister& reg, const PackDescription& d, RegisterState& state);

// Appends the entry with the lock already held by the caller (re-checks, writes a temp file, fsyncs, renames, fsyncs the directory). A repeat of an identical
// entry is a no-op. Only a random (version 4) id outside the dev range can be registered. The date is the caller's (YYYY-MM-DD).
Failure append_register_locked(const std::string& path, const PackDescription& d, const std::string& owner, const std::string& date);
// Same, taking the lock itself.
Failure append_register(const std::string& path, const PackDescription& d, const std::string& owner, const std::string& date);

}  // namespace velora::contentpack
