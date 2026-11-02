// SPDX-License-Identifier: GPL-3.0-or-later
// A tiny game module for tests/host/game_module_test.cpp (T-610): the shape of src/game_module/module_entry.cpp with
// a one-function-free table. CMake builds it once per failure kind, FX_KIND:
//   0 good, 1 wrong ABI version, 2 wrong entry size, 3 null bind, 4 empty table, 5 entry symbol under another name,
//   6 good with an IOP descriptor (T-648a), 7 IOP descriptor with the wrong RAM size, 8 IOP entry pc outside its code.
#ifndef FX_KIND
#define FX_KIND 0
#endif

#include "don_iop_abi.h"
#include "game_module.h"

// The layout the loader reads (src/host/game_module.h, ps2_runtime.h of patch 0088).
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
const DonGameModule kEmpty = {nullptr, 0};

uint32_t iopRun(DonIopCpuState *, DonIopMemory *, uint32_t) { return 0; }
const uint32_t kIopPcs[2] = {0x6e000u, 0x6e018u};
const DonIopDescriptor kIop = {sizeof(DonIopDescriptor), FX_KIND == 7 ? 0x100000u : 0x200000u, "fx_iop", 0x6e000u, 0x100u,
                               0x1234u, kIopPcs, 2, &iopRun, nullptr, nullptr};
const DonIopDescriptor kBadPc = {sizeof(DonIopDescriptor), 0x200000u, "fx_iop", 0x6e000u, 0x10u,
                                 0x1234u, kIopPcs + 1, 1, &iopRun, nullptr, nullptr};

// The slot the host writes its DonRuntime pointer to (the module's `don_rt` in the real module).
const void *g_slot = nullptr;

const DonGameModule *const kTableOfEntry = FX_KIND == 4 ? &kEmpty : &kModule;

const DonGameModuleEntry kEntry = {
    FX_KIND == 1 ? kDonGameModuleAbiVersion + 1 : kDonGameModuleAbiVersion,
    FX_KIND == 2 ? static_cast<uint32_t>(sizeof(DonGameModuleEntry) + 8) : static_cast<uint32_t>(sizeof(DonGameModuleEntry)),
    kTableOfEntry,
    FX_KIND == 3 ? nullptr : &g_slot,
    FX_KIND == 6 || FX_KIND == 7 ? &kIop : FX_KIND == 8 ? &kBadPc : nullptr,
};
} // namespace

#if FX_KIND == 5
extern "C" const DonGameModuleEntry *don_game_module_other_v1()
{
    return &kEntry;
}
#else
extern "C" const DonGameModuleEntry *don_game_module_v1()
{
    return &kEntry;
}
#endif
