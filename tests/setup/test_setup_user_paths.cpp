// Tests of the user roots and the single-instance lock (T-645). Only a temp DON_USER_DIR is used.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "user_paths.h"

namespace fs = std::filesystem;
using namespace don_setup;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

// Sets or unsets one variable and restores it on destruction.
struct EnvGuard {
    std::string name, old;
    bool had;
    EnvGuard(const char* n, const char* value) : name(n) {
        const char* o = std::getenv(n);
        had = o != nullptr;
        if (had) old = o;
        if (value) setenv(n, value, 1); else unsetenv(n);
    }
    ~EnvGuard() {
        if (had) setenv(name.c_str(), old.c_str(), 1); else unsetenv(name.c_str());
    }
};

static void test_roots_equal_and_override(const fs::path& tmp) {
    EnvGuard d("DON_USER_DIR", tmp.c_str());
    ErrorId err = ErrorId::E01;
    UserRoots r = user_roots(&err);
    CHECK(err == ErrorId::None);
    CHECK(r.config == r.data);
    CHECK(r.config == tmp);
}

static void test_home_unset(const fs::path&) {
    EnvGuard d("DON_USER_DIR", nullptr), x("XDG_DATA_HOME", nullptr), h("HOME", nullptr);
    ErrorId err = ErrorId::None;
    UserRoots r = user_roots(&err);
    CHECK(err == ErrorId::E32);
    CHECK(r.config.empty() && r.data.empty());
    CHECK(user_roots(nullptr).config.empty());
}

static void test_home_used(const fs::path& tmp) {
    EnvGuard d("DON_USER_DIR", nullptr), x("XDG_DATA_HOME", nullptr), h("HOME", tmp.c_str());
    ErrorId err = ErrorId::E01;
    UserRoots r = user_roots(&err);
    CHECK(err == ErrorId::None);
    CHECK(r.data == tmp / ".local" / "share" / "don");
}

static void test_relative_override_refused(const fs::path&) {
    EnvGuard d("DON_USER_DIR", "relative/dir");
    ErrorId err = ErrorId::None;
    CHECK(user_roots(&err).data.empty());
    CHECK(err == ErrorId::E32);
}

static void test_single_instance(const fs::path& tmp) {
    fs::path dir = tmp / "lock";
    {
        InstanceLock first = acquire_single_instance(dir);
        CHECK(static_cast<bool>(first));
        CHECK(!acquire_single_instance(dir));  // same process

        pid_t pid = fork();  // other process
        if (pid == 0) _exit(acquire_single_instance(dir) ? 1 : 0);
        int status = 0;
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    // Released with the handle.
    InstanceLock again = acquire_single_instance(dir);
    CHECK(static_cast<bool>(again));
    InstanceLock moved = std::move(again);
    CHECK(static_cast<bool>(moved) && !again);
    CHECK(!acquire_single_instance(dir));
}

int main() {
    fs::path tmp = fs::temp_directory_path() / ("don_user_paths_" + std::to_string(getpid()));
    fs::create_directories(tmp);
    test_roots_equal_and_override(tmp);
    test_home_unset(tmp);
    test_home_used(tmp);
    test_relative_override_refused(tmp);
    test_single_instance(tmp);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    if (g_fail) { std::cerr << g_fail << " failure(s)\n"; return 1; }
    std::cout << "ok\n";
    return 0;
}
