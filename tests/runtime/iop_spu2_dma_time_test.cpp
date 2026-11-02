// SPDX-License-Identifier: GPL-3.0-or-later
// T-305, patch 0071: without a host SPU2 the end of an SPU2 DMA comes 24 IOP cycles per halfword after its start (PCSX2
// SPU2 Dma.cpp CYCLES_PER_WORD), and 24 cycles at least, as on the console. It used to come 2 cycles per word (at least
// 64) after: CRI_ADXI's transfer thread then ran about 24 times too often (journal 034 follow-up 20).
#include "core/iop_memory.h"

#include <cstdint>
#include <cstdio>

using ps2x::iop::detail::IopMemory;

static int g_failures = 0;

static void expectEq(uint64_t actual, uint64_t wanted, const char *what)
{
    if (actual != wanted)
    {
        std::fprintf(stderr, "FAIL: %s: got %llu, wanted %llu\n", what, static_cast<unsigned long long>(actual),
                     static_cast<unsigned long long>(wanted));
        ++g_failures;
    }
}

// DMA channel 4 (core 0): MADR 0x1F8010C0, BCR 0x1F8010C4, CHCR 0x1F8010C8; channel 7: 0x1F801500...
static uint64_t delayOf(IopMemory &memory, uint32_t base, uint32_t words, int wantedIrq)
{
    memory.write32(base, 0x1000u);
    memory.write32(base + 4u, (1u << 16u) | words);
    memory.write32(base + 8u, 0x01000201u);
    const auto start = memory.takeDmaStart();
    if (!start)
    {
        std::fprintf(stderr, "FAIL: no DMA start for %u words\n", words);
        ++g_failures;
        return 0;
    }
    expectEq(static_cast<uint64_t>(start->irq), static_cast<uint64_t>(wantedIrq), "irq");
    return start->delayCycles;
}

int main()
{
    IopMemory memory;
    expectEq(delayOf(memory, 0x1F8010C0u, 512u, 0x24), 512u * 2u * 24u, "core 0, 2048 bytes");
    expectEq(delayOf(memory, 0x1F8010C0u, 1u, 0x24), 48u, "core 0, one word");
    expectEq(delayOf(memory, 0x1F801500u, 128u, 0x28), 128u * 2u * 24u, "core 1, 512 bytes");
    // The emulator clamps an empty block control to one word, so this measures the same floor.
    expectEq(delayOf(memory, 0x1F8010C0u, 0u, 0x24), 48u, "core 0, empty block control");
    if (g_failures == 0)
        std::puts("iop_spu2_dma_time_test: ok");
    return g_failures == 0 ? 0 : 1;
}
