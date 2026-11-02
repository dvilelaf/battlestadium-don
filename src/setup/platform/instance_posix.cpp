// Single-instance lock on POSIX: flock(2) on a lock file. The kernel drops it when the process dies, so a crash
// never leaves a stale lock. flock locks belong to the open file description, so a second open in the same process
// also fails while the first is held.
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <system_error>

#include "../user_paths.h"

namespace don_setup {

struct InstanceLock::Impl {
    int fd = -1;
    ~Impl() {
        if (fd >= 0) ::close(fd);  // closing releases the lock
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
    impl->fd = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (impl->fd < 0) return {};
    if (::flock(impl->fd, LOCK_EX | LOCK_NB) != 0) return {};
    return InstanceLock(std::move(impl));
}

}  // namespace don_setup
