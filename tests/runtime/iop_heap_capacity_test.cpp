// T-304: the IOP heap must hold what the game's modules ask for. At the start-up of the effects driver (sndfi)
// the heap already held the 31 blocks below (sizes and alignments measured with a breakpoint on
// IopMemory::allocate, English disc) and sndfi then asks for 0x4280, 0x8000 and 0x8000 bytes. With the heap at
// 0x120000..0x1F0000 the three were refused (AllocSysMemory returned 0), the driver answered its init with -2 and
// the game never sent an effect. Patch 0057 moves the heap base down to the end of the module arena.
#include "core/iop_memory.h"

#include <cstdint>
#include <cstdio>

int main()
{
    using ps2x::iop::detail::IopMemory;
    struct Req { uint32_t size, align; };
    static const Req before[] = {
        {0x1a00, 64}, {0x840, 16}, {0x2a80, 64}, {0x840, 16}, {0x840, 16}, {0x177c0, 64}, {0x1ce0, 64}, {0x1040, 16},
        {0xb300, 64}, {0x840, 16}, {0x840, 16}, {0x8300, 64}, {0x7000, 64}, {0x7000, 64}, {0xf640, 64}, {0x55c0, 64},
        {0x7a0, 64}, {0x23e0, 64}, {0x840, 16}, {0x10e80, 64}, {0xa, 16}, {0x840, 16}, {0x840, 16}, {0x8d0, 64},
        {0x840, 16}, {0x41040, 64}, {0x8d0, 64}, {0x4040, 64}, {0x18ac0, 64}, {0x840, 16}, {0x840, 16}};
    static const Req sndfi[] = {{0x4280, 16}, {0x8000, 64}, {0x8000, 64}};
    IopMemory memory;
    int failures = 0;
    for (const Req &r : before)
        if (memory.allocate(r.size, r.align) == 0u) { std::printf("FAIL: earlier block %#x refused\n", r.size); ++failures; }
    for (const Req &r : sndfi)
        if (memory.allocate(r.size, r.align) == 0u) { std::printf("FAIL: sndfi block %#x refused\n", r.size); ++failures; }
    // The heap must stay above the module arena (the 14 modules end at 0x9BD00).
    if (IopMemory::HeapBase < 0x9BD00u + 0x10000u) { std::printf("FAIL: heap base %#x too low\n", IopMemory::HeapBase); ++failures; }
    std::printf(failures ? "iop_heap_capacity_test: %d failure(s)\n" : "iop_heap_capacity_test: ok\n", failures);
    return failures ? 1 : 0;
}
