// SPDX-License-Identifier: GPL-3.0-or-later
// T-016 iteration 10: the runtime's own memory inside the guest's RAM, and the console's heap.
//
// The runtime keeps structures of its own in guest RAM: the invocation stacks of queued guest calls,
// the remote-call packet and server pools, the thread-local-storage pool, the boot-mode pool and the
// scratch allocator behind guestMalloc (the GS, font and LibC handlers allocate through it). Before
// this change they sat at 0x01F00000 and above: guestMalloc handed out blocks from the base of the
// game's own heap (0x5A6800) and the invocation stacks grew down from the top of RAM over the main
// thread's stack (0x01F80000-0x02000000), which is why SetupHeap clamped the game's heap to
// 0x01F00000. They now live in the console's free kernel area below the game's image
// (0x00080000-0x00100000, measured all-zero on two console memory images), and the game's heap is the
// console's one: start + size, or the bottom of the calling thread's stack when the size is -1.
//
// The expectations are literals on purpose: this test is the specification of the layout, so it must
// not follow the constants of the code it verifies. Against a runtime without this change it fails
// (the pools answer 0x01Fxxxxx, the invocation stack starts at the top of RAM, EndOfHeap answers
// 0x01F00000).

#include "host_test_support.h"

#include "Syscalls/Common.h" // the pools (rpcAllocPacketAddr, rpcAllocServerAddr, allocTlsAddr, allocBootModeAddr)
#include "Syscalls/System.h" // ps2_syscalls::SetupThread, SetupHeap, EndOfHeap

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <cstring>
#include <string>

namespace
{
    int g_failures = 0;

    bool expect(bool condition, const std::string &message)
    {
        if (!condition)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        }
        return condition;
    }

    using Handler = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    int32_t call(PS2Runtime &rt, Handler fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0)
    {
        R5900Context ctx;
        ctx.r[4] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a0)));
        ctx.r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a1)));
        ctx.r[6] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a2)));
        fn(rt.memory().getRDRAM(), &ctx, &rt);
        return static_cast<int32_t>(getRegU32(&ctx, 2));
    }

    struct Region
    {
        const char *name;
        uint32_t base;
        uint32_t end;
    };

    // The layout from the patch's table: every region is 0x1000-aligned, no two overlap, and all of
    // them end at or below the game's image (0x00100000).
    constexpr Region kRegions[] = {
        {"invocation stacks", 0x00080000u, 0x000C0000u},
        {"rpc packets", 0x000C0000u, 0x000D0000u},
        {"rpc servers", 0x000D0000u, 0x000E0000u},
        {"thread-local storage", 0x000E0000u, 0x000F0000u},
        {"boot mode", 0x000F0000u, 0x000F1000u},
        {"runtime scratch allocator", 0x000F1000u, 0x00100000u},
    };
    constexpr uint32_t kGameImageBase = 0x00100000u;
    constexpr uint32_t kGameImageEnd = 0x02000000u;

    bool inside(uint32_t address, const Region &region)
    {
        return address >= region.base && address < region.end;
    }

    std::string hex(uint32_t value)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08X", value);
        return text;
    }

    void check(const std::string &what, uint32_t address, const Region &expected)
    {
        expect(inside(address, expected), what + ": " + hex(address) + " is in " + expected.name +
                                               " [" + hex(expected.base) + "," + hex(expected.end) + ")");
        expect(!(address >= kGameImageBase && address < kGameImageEnd),
               what + ": " + hex(address) + " is inside the game's image, heap or stacks [0x00100000,0x02000000)");
    }
} // namespace

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    // No two regions overlap and none of them reaches the game's image.
    for (size_t i = 0; i < std::size(kRegions); ++i)
    {
        expect(kRegions[i].base < kRegions[i].end, std::string(kRegions[i].name) + ": non-empty");
        expect(kRegions[i].end <= kGameImageBase,
               std::string(kRegions[i].name) + ": ends at or below the game's image");
        for (size_t j = i + 1; j < std::size(kRegions); ++j)
        {
            const bool overlap = kRegions[i].base < kRegions[j].end && kRegions[j].base < kRegions[i].end;
            expect(!overlap, std::string(kRegions[i].name) + " and " + kRegions[j].name + " overlap");
        }
    }

    PS2Runtime rt;
    rt.setHostOwnsWindow(true);
    expect(rt.initialize("guest-heap-limit-test"), "initialize() with the host owning the window");
    uint8_t *rdram = rt.memory().getRDRAM();

    // The console's kernel: SetupThread puts the stack of a thread that asks for -1/0x80000 at the top
    // of RAM (0x01F80000), and SetupHeap with size -1 ends the heap at the bottom of that stack.
    const int32_t threadReturn = call(rt, ps2_syscalls::SetupThread, 0x004CA000u, 0xFFFFFFFFu, 0x80000u);
    expect(threadReturn == static_cast<int32_t>(0x01F80000u),
           "SetupThread(stack=-1, size=0x80000) answers the stack top 0x01F80000, got " + hex(threadReturn));
    const int32_t heapReturn = call(rt, ps2_syscalls::SetupHeap, 0x005A6800u, 0xFFFFFFFFu);
    expect(heapReturn == static_cast<int32_t>(0x005A6800u),
           "SetupHeap still answers the heap base 0x005A6800, got " + hex(heapReturn));
    const int32_t endOfHeap = call(rt, ps2_syscalls::EndOfHeap);
    expect(endOfHeap == static_cast<int32_t>(0x01F80000u),
           "after SetupHeap(base, -1) EndOfHeap answers the console's 0x01F80000, got " + hex(endOfHeap));

    // With an explicit size the end is start + size, with no clamp.
    call(rt, ps2_syscalls::SetupHeap, 0x005A6800u, 0x00100000u);
    const int32_t endExplicit = call(rt, ps2_syscalls::EndOfHeap);
    expect(endExplicit == static_cast<int32_t>(0x006A6800u),
           "with an explicit size EndOfHeap answers start + size (0x006A6800), got " + hex(endExplicit));
    call(rt, ps2_syscalls::SetupHeap, 0x005A6800u, 0xFFFFFFFFu); // back to the stack-derived end

    // Every runtime structure is in its own region, none in the game's image, heap or stacks.
    check("remote-call server", rpcAllocServerAddr(rdram), kRegions[2]);
    check("remote-call packet", rpcAllocPacketAddr(rdram), kRegions[1]);
    check("thread-local block", allocTlsAddr(rdram), kRegions[3]);
    check("boot-mode entry", allocBootModeAddr(rdram, 0x20u), kRegions[4]);
    expect(rt.reserveAsyncCallbackStack(0x4000u) != 0u, "an invocation stack is served");
    check("invocation stack", rt.reserveAsyncCallbackStack(0x4000u), kRegions[0]);
    check("runtime scratch block", rt.guestMalloc(0x100u, 16u), kRegions[5]);

    // The layout before the change is what fails: say it once in the log for the record.
    std::printf("[guest_heap_limit_test] heap end %s, server %s, packet %s, tls %s, bootmode %s, stack %s, scratch %s\n",
                hex(static_cast<uint32_t>(endOfHeap)).c_str(),
                hex(rpcAllocServerAddr(rdram)).c_str(), hex(rpcAllocPacketAddr(rdram)).c_str(),
                hex(allocTlsAddr(rdram)).c_str(), hex(allocBootModeAddr(rdram, 0x20u)).c_str(),
                hex(rt.reserveAsyncCallbackStack(0x4000u)).c_str(), hex(rt.guestMalloc(0x100u, 16u)).c_str());

    if (g_failures != 0)
    {
        std::fprintf(stderr, "guest_heap_limit_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("guest_heap_limit_test: all checks passed\n");
    return 0;
}
