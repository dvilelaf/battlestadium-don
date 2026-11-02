// Single-instance lock on Windows: LockFileEx on a lock file, the twin of instance_posix.cpp (task T-733).
// The file is opened with FILE_SHARE_READ | FILE_SHARE_WRITE so a second copy can open it and then fails to take
// the exclusive byte-range lock (LockFileEx locks conflict per handle, so a second open in the same process also
// fails while the first is held, like flock). The system drops the lock when the process ends, even when it is
// killed, so a dead holder never blocks. The handle is created without bInheritHandle: children do not keep it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <system_error>

#include "../user_paths.h"

namespace don_setup {

struct InstanceLock::Impl {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~Impl() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);  // closing releases the lock
    }
};

// The destructor of the handle needs the complete Impl.
InstanceLock::InstanceLock() = default;
InstanceLock::InstanceLock(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
InstanceLock::InstanceLock(InstanceLock&&) noexcept = default;
InstanceLock& InstanceLock::operator=(InstanceLock&&) noexcept = default;
InstanceLock::~InstanceLock() = default;

InstanceLock acquire_single_instance(const std::filesystem::path& lock_dir) {
    std::error_code ec;
    std::filesystem::create_directories(lock_dir, ec);
    if (ec) return {};
    const std::filesystem::path file = lock_dir / "setup.lock";
    auto impl = std::make_unique<InstanceLock::Impl>();
    impl->handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl->handle == INVALID_HANDLE_VALUE) return {};
    OVERLAPPED ov{};  // offset 0: the lock covers the first byte
    if (!LockFileEx(impl->handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) return {};
    return InstanceLock(std::move(impl));
}

}  // namespace don_setup
