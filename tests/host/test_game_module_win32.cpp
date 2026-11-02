// SPDX-License-Identifier: GPL-3.0-or-later
// T-731: the Windows game module loader (src/host/game_module_win32.cpp) against tiny DLLs built next to this program
// (test_game_module_win32_fixture.cpp). Cross-built, run under Wine (tools/container/run_wine.sh). No game needed.
#include <cstdio>
#include <filesystem>
#include <string>

#include "game_module.h"
#include "ps2_runtime.h"  // DonGameModule

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

bool contains(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}

std::filesystem::path programFolder()
{
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
}
} // namespace

int main()
{
    const std::filesystem::path here = programFolder();

    // The good module: loaded, its slot is bound, and it unloads.
    {
        std::string error;
        const std::string dll = (here / "fx_win32_ok.dll").string();
        const bool loaded = don_load_game_module(dll, error);
        check(loaded, "a DLL exporting don_game_module_v1 with the right ABI version loads");
        if (!loaded)
            std::printf("     error: %s\n", error.c_str());
        else
        {
            check(!don_game_module_bound(), "its bind slot is empty before the host binds it");
            int runtime = 0;
            don_bind_game_module(&runtime);
            check(don_game_module_bound(), "after don_bind_game_module the slot holds the host's pointer");
            check(don_game_module().count == 2, "the loaded table has the fixture's two functions");
            check(don_game_module_id().empty(), "a bare DLL has no module id");
            std::string again;
            check(!don_load_game_module(dll, again) && contains(again, "already loaded"), "a second load is refused");
            don_unload_game_module();
        }
    }
    // A file that does not exist: the system's error text, the same "cannot load <path>: " form as POSIX.
    {
        std::string error;
        const std::string dll = (here / "fx_win32_missing.dll").string();
        const bool loaded = don_load_game_module(dll, error);
        std::printf("     error: %s\n", error.c_str());
        check(!loaded && error.rfind("cannot load " + dll + ": ", 0) == 0, "a missing file gives 'cannot load <path>: <system text>'");
    }
    // A DLL that loads but has no don_game_module_v1.
    {
        std::string error;
        const std::string dll = (here / "fx_win32_nosym.dll").string();
        const bool loaded = don_load_game_module(dll, error);
        std::printf("     error: %s\n", error.c_str());
        check(!loaded && error == dll + " does not export " + kDonGameModuleEntrySymbol,
              "a DLL without the export gives '<path> does not export don_game_module_v1'");
    }
    std::printf("%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
