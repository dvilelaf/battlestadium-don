// T-705: a setup.lock left behind by a crashed run must not block the next start; a live holder still does.
// Only a temp folder is used.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>

#include "../../src/setup/user_paths.h"

namespace fs = std::filesystem;
using don_setup::acquire_single_instance;
using don_setup::InstanceLock;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

int main() {
    const fs::path dir = fs::temp_directory_path() / ("t705-instance-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    // A leftover file with no holder, with stale content: the start proceeds.
    std::ofstream(dir / "setup.lock") << "99999\n";
    {
        InstanceLock lock = acquire_single_instance(dir);
        CHECK(static_cast<bool>(lock));
        // A live holder: the second start is refused (this process and another one).
        CHECK(!acquire_single_instance(dir));
        const pid_t pid = fork();
        if (pid == 0) _exit(acquire_single_instance(dir) ? 1 : 0);
        int status = 0;
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    // A run that crashed while holding the lock (killed by a signal, nothing cleaned up): the next start proceeds.
    {
        int ready[2];
        CHECK(pipe(ready) == 0);
        const pid_t pid = fork();
        if (pid == 0) {
            InstanceLock held = acquire_single_instance(dir);
            const char c = held ? 'y' : 'n';
            (void)!::write(ready[1], &c, 1);
            ::raise(SIGSEGV);  // default action: dies without releasing anything
            _exit(3);
        }
        char c = 0;
        CHECK(::read(ready[0], &c, 1) == 1 && c == 'y');
        int status = 0;
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
        CHECK(fs::exists(dir / "setup.lock"));  // the file is still there
        InstanceLock next = acquire_single_instance(dir);
        CHECK(static_cast<bool>(next));
        close(ready[0]);
        close(ready[1]);
    }

    fs::remove_all(dir);
    if (g_fail) { std::cerr << g_fail << " failure(s)\n"; return 1; }
    std::cout << "ok\n";
    return 0;
}
