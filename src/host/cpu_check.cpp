// SPDX-License-Identifier: GPL-3.0-or-later
// T-518: the processor check of knowledge 22 section 7.2. This file is compiled for baseline
// x86-64 (per-file -march=x86-64 in src/host/CMakeLists.txt), not x86-64-v3: it has to be able to
// run on a processor without AVX2 and say so, where the rest of the build would die with an
// illegal instruction (SIGILL) inside its own prologue.
//
// The three features the build needs are the ones -march=x86-64-v3 adds over x86-64 (Haswell,
// Intel 2013 / AMD Zen, 2017): AVX2, FMA and BMI2.
#include "cpu_check.h"

#include <cstdio>

#include <SDL3/SDL.h>

#if defined(__x86_64__) || defined(_M_X64)
#define DON_CPU_X86 1
#else
// The port builds for x86-64 only; anywhere else there is nothing to check (AGENTS.md portability).
#define DON_CPU_X86 0
#endif

#if DON_CPU_X86 && defined(_MSC_VER)
// MSVC and clang-cl have no __builtin_cpu_supports: the same information comes from CPUID
// (the equivalent the task card names), plus XGETBV for the OS's YMM state.
#include <immintrin.h> // __cpuid, __cpuidex, _xgetbv
#endif

#if defined(_MSC_VER)
#define DON_CPU_CHECK_NOINLINE __declspec(noinline)
#else
#define DON_CPU_CHECK_NOINLINE __attribute__((noinline))
#endif

namespace
{
    // Knowledge 22 section 7.2, verbatim.
    constexpr char kMessage[] =
        "This game needs a processor with AVX2 (Intel 2013 or later, AMD Ryzen)";

#if DON_CPU_X86 && defined(_MSC_VER)
    bool cpu_has_required_features()
    {
        int regs[4] = {0, 0, 0, 0};

        __cpuid(regs, 0);
        const int max_leaf = regs[0];

        __cpuid(regs, 1);
        const bool fma = (regs[2] & (1 << 12)) != 0;      // ECX bit 12: FMA3
        const bool avx = (regs[2] & (1 << 28)) != 0;      // ECX bit 28: AVX
        const bool osxsave = (regs[2] & (1 << 27)) != 0;  // ECX bit 27: OSXSAVE
        if (!avx || !fma || !osxsave)
            return false;

        // AVX needs the OS to save and restore the YMM state (XCR0 bits 1 and 2); without it
        // AVX2 instructions fault even on a processor that has them.
        const unsigned long long xcr0 = _xgetbv(0);
        if ((xcr0 & 0x6u) != 0x6u)
            return false;

        if (max_leaf < 7)
            return false;
        __cpuidex(regs, 7, 0);
        const bool bmi2 = (regs[1] & (1 << 8)) != 0;      // EBX bit 8: BMI2
        const bool avx2 = (regs[1] & (1 << 5)) != 0;      // EBX bit 5: AVX2
        return avx2 && bmi2;
    }
#elif DON_CPU_X86
    bool cpu_has_required_features()
    {
        // The table __builtin_cpu_supports reads is filled in by the compiler runtime's
        // constructor; clang does not promise to add it for this translation unit, so ask for it
        // (harmless when the constructor also ran). Without it the table is all zeros and the
        // check would report every processor as incapable.
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")
               && __builtin_cpu_supports("bmi2");
    }
#else
    bool cpu_has_required_features() { return true; }
#endif
} // namespace

// noinline: the call in main must stay a call to this baseline x86-64 code (with LTO the linker
// could otherwise fold it into the x86-64-v3 caller).
DON_CPU_CHECK_NOINLINE int don_cpu_check()
{
    if (cpu_has_required_features())
        return 0;

    // On stderr first (with a flush): the message is visible when the message box fails, as it
    // does in a headless run. SDL_ShowSimpleMessageBox may be called before SDL_Init.
    std::fprintf(stderr, "[don] %s\n", kMessage);
    std::fflush(stderr);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Battle Stadium D.O.N", kMessage, nullptr);
    return 5;
}
