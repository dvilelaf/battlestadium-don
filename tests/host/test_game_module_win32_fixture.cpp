// SPDX-License-Identifier: GPL-3.0-or-later
// A tiny game module DLL for tests/host/test_game_module_win32.cpp (T-731): the shape of src/game_module/module_entry.cpp
// with a two-slot table. FX_KIND 0 exports don_game_module_v1 with the right ABI version; FX_KIND 1 is a DLL with
// no such export (the entry is under another name).
#ifndef FX_KIND
#define FX_KIND 0
#endif

#include "game_module.h"

// The layout the loader reads (src/host/game_module.h, ps2_runtime.h of patch 0088); same as game_module_fixture.cpp.
struct DonFunctionEntry
{
    uint32_t address;
    void (*function)();
};

struct DonGameModule
{
    const DonFunctionEntry *entries;
    uint32_t count;
};

namespace
{
const DonFunctionEntry kTable[2] = {{0x00100000u, nullptr}, {0x00100008u, nullptr}};
const DonGameModule kModule = {kTable, 2};
const void *g_slot = nullptr;  // the slot the host writes its DonRuntime pointer to
const DonGameModuleEntry kEntry = {kDonGameModuleAbiVersion, static_cast<uint32_t>(sizeof(DonGameModuleEntry)), &kModule,
                                   &g_slot, nullptr};
} // namespace

#if FX_KIND == 1
extern "C" __declspec(dllexport) const DonGameModuleEntry *don_game_module_other_v1()
#else
extern "C" __declspec(dllexport) const DonGameModuleEntry *don_game_module_v1()
#endif
{
    return &kEntry;
}
