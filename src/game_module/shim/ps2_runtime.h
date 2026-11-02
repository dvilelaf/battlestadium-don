// Shim of ps2_runtime.h (T-601, moved by T-610; brought to patch 0088 for DonFunctionEntry / DonGameModule): generated code sees the same names, backed by don_guest_abi.h.
//   PS2Runtime       -> DonRuntime (members call through DonRuntimeCalls / DonHotState)
//   R5900Context     -> the ABI struct, field for field
//   PS2Exception, PS2Runtime::GuestBranchKind, PS2Runtime::RecompiledFunction -> same names
#ifndef PS2_RUNTIME_H
#define PS2_RUNTIME_H
#include "../don_guest_abi.h"

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

// deps.md section 5: the only STL name generated code uses (119,884 times) is std::memcpy; it becomes the builtin.
namespace std
{
__attribute__((always_inline)) inline void *memcpy(void *d, const void *s, size_t n) { return __builtin_memcpy(d, s, n); }
}

// ps2_runtime.h:246: a no-op in the runtime too.
inline void ps2TraceGuestWrite(uint8_t *, uint32_t, uint32_t, uint64_t, uint64_t, const char *, const R5900Context *) {}

#endif // PS2_RUNTIME_H
