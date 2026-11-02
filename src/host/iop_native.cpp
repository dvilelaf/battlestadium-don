// Registers the ahead-of-time translated IOP sound module (cri_adxi) with the IOP emulator (patch 0085).
//
// Compiled into the executable only when generated/iop_native/adxi_native.cpp exists (CMake option DON_IOP_NATIVE;
// the file is made from the user's own disc by tools/iop_recomp/make_native.py and is never versioned). The
// emulator checks the hash of the code in guest RAM before first use and falls back to interpreting when it differs.
// DON_IOP_NATIVE=0 turns the module off at run time; DON_IOP_NATIVE_VERIFY=1 replays every native stretch in the
// interpreter and prints `[don-iop] native verify ...` on any difference.
// CMake adds this file only with DON_LINK_GENERATED=ON; the guard makes a stray inclusion fail with a clear message
// instead of a missing generated file.
#ifdef DON_NO_GENERATED
#error "iop_native.cpp needs generated/iop_native/adxi_native.cpp: build with DON_LINK_GENERATED=ON"
#endif
#include "iop_native/adxi_native.cpp"

#include "ps2x/iop/iop_native_module.h"

namespace
{
    namespace native = ps2x::iop::detail::adxi_native;

    const ps2x::iop::IopNativeModule kAdxiNative = {
        .name = "cri_adxi",
        .codeLo = native::kCodeLo,
        .codeSize = native::kCodeSize,
        .codeHash = native::kCodeHash,
        .entryPcs = native::kEntryPcs,
        .numEntryPcs = native::kNumEntryPcs,
        .run = &native::run,
        .stale = &native::stale,
        .resetStale = &native::resetStale,
    };

    struct Registrar
    {
        Registrar() { ps2x::iop::registerNativeModule(&kAdxiNative); }
    } g_registrar;
}
