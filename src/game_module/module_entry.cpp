// SPDX-License-Identifier: GPL-3.0-or-later
// The module's only exported symbol (see src/host/game_module.h). Built into libdon_game.so (don_game.dll on Windows)
// together with the generated code; freestanding: no C++ library, so no static-local guard, no allocation.
#include "game_module.h"
#include "don_guest_decls.h"  // DonRuntime, DonGameModule, don_game_module()

#ifdef DON_MODULE_IOP
// T-648a: the translated IOP sound module, compiled in when the build gives -DDON_MODULE_IOP and -I generated/ (its
// iop_cpu.h and iop_memory.h are src/game_module/iop_shim's, found first through -I iop_shim).
#include "don_iop_abi.h"
#include "iop_native/adxi_native.cpp"

namespace
{
namespace native = ps2x::iop::detail::adxi_native;

uint32_t iopRun(DonIopCpuState *cpu, DonIopMemory *mem, uint32_t budget) { return native::run(*cpu, *mem, budget); }

const DonIopDescriptor kIop = {sizeof(DonIopDescriptor), native::kRamSize, "cri_adxi", native::kCodeLo,
                               native::kCodeSize,        native::kCodeHash, native::kEntryPcs, native::kNumEntryPcs,
                               &iopRun,                  &native::stale,    &native::resetStale};
}
#define DON_IOP_DESCRIPTOR (&kIop)
#else
#define DON_IOP_DESCRIPTOR nullptr
#endif

// The one global the DonRuntime methods read (don_guest_abi.h); the loader fills it through the entry's `bind`.
const DonRuntime *don_rt = nullptr;

// don_game_module() is defined by the generated register_functions.cpp (hidden visibility): the {address, function} array.

#ifdef _WIN32
#define DON_MODULE_EXPORT __declspec(dllexport)  // the DLL has no .def file: this is what lld-link exports
#else
#define DON_MODULE_EXPORT __attribute__((visibility("default")))  // the build uses -fvisibility=hidden
#endif

extern "C" DON_MODULE_EXPORT const DonGameModuleEntry *don_game_module_v1()
{
    // Constant-initialised; the pointer is filled on the first call (same value every time).
    static DonGameModuleEntry entry = {kDonGameModuleAbiVersion, sizeof(DonGameModuleEntry), nullptr,
                                         reinterpret_cast<const void **>(&don_rt), DON_IOP_DESCRIPTOR};
    entry.module = &don_game_module();
    return &entry;
}
