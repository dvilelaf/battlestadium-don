// don_guest_decls.h: what the generated code needs besides the ABI structs of don_guest_abi.h (T-614; moved here from
// the T-601 shim ps2_runtime.h and from module_extra.h). Freestanding like the ABI: <stdint.h>, <stddef.h>,
// <immintrin.h> only.
//   PS2Runtime       -> DonRuntime (members call through DonRuntimeCalls / DonHotState)
//   R5900Context     -> the ABI struct, field for field
#pragma once
#include "don_guest_abi.h"
#include <immintrin.h>

using PS2Runtime = DonRuntime;

// ps2xRuntime/include/ps2_runtime.h (patch 0088): what register_functions.cpp builds and the loader hands over.
struct DonFunctionEntry
{
    uint32_t address;
    PS2Runtime::RecompiledFunction function;
};

struct DonGameModule
{
    const DonFunctionEntry *entries;
    uint32_t count;
};

const DonGameModule &don_game_module();

// The only STL name generated code uses (119,884 times) is std::memcpy; it becomes the builtin.
namespace std
{
__attribute__((always_inline)) inline void *memcpy(void *d, const void *s, size_t n) { return __builtin_memcpy(d, s, n); }
}

// ps2_runtime.h:246: a no-op in the runtime too.
inline void ps2TraceGuestWrite(uint8_t *, uint32_t, uint32_t, uint64_t, uint64_t, const char *, const R5900Context *) {}

// T-603: the runtime's extra.h without the sqrtf builtin macro; sqrtf is an inline _mm_sqrt_ss (no flag changes).
#define fabsf __builtin_fabsf
static inline float sqrtf(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
static inline uint32_t getRegU32(const R5900Context *ctx, int reg)
{
    if (reg <= 0 || reg > 31) return 0;
    return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[reg], 0));
}

// The runtime's macros for generated code, derived by tools/module/make_ninja.py (through tools/pipeline/
// gen_shim_macros.py) into the build folder; not tracked.
#include "don_guest_macros.h"
