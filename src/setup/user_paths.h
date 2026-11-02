// Where the setup may write (task T-645) and the one-copy-at-a-time lock. The user folder is not decided here:
// it is src/host/user_dir.h's decision (D15, T-524; DON_USER_DIR overrides it). This file only refuses the
// results that are not usable (relative path, i.e. HOME unset) and gives the setup two names for it.
#pragma once

#include <filesystem>
#include <memory>

#include "setup_types.h"

namespace don_setup {

struct UserRoots {
    std::filesystem::path config;  // settings and logs of the setup
    std::filesystem::path data;    // the prepared game (modules, manifests)
};

// Both roots come from the program's single user-folder decision. Linux: the two are equal. Windows: the same
// folder too (%APPDATA%\don); they are separate fields so the layout can split later without changing callers.
// If the decision is not an absolute path (HOME unset and no DON_USER_DIR), *err is E32 and both roots are empty.
// `err` may be null. On success *err is ErrorId::None.
UserRoots user_roots(ErrorId* err);

// Holds the single-instance lock until destroyed (RAII). Empty (operator bool false) when the lock is not held.
class InstanceLock {
public:
    InstanceLock();  // out of line: unique_ptr<Impl> needs the complete Impl (libc++ checks it at class scope)
    InstanceLock(InstanceLock&&) noexcept;
    InstanceLock& operator=(InstanceLock&&) noexcept;
    ~InstanceLock();
    explicit operator bool() const { return impl_ != nullptr; }

    struct Impl;
    explicit InstanceLock(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> impl_;
};

// Takes a non-blocking exclusive lock on a lock file inside `lock_dir` (created if missing). Returns an empty
// lock when another copy, in this process or another, holds it, or when the folder cannot be written.
InstanceLock acquire_single_instance(const std::filesystem::path& lock_dir);

}  // namespace don_setup
