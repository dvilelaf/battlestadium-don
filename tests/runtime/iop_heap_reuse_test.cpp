// T-304, patch 0058: the IOP heap offers freed space again (first fit from the base), its "largest free block" is the largest
// gap, and a refused request is reported on standard error (it was silent: the effects driver died of a zero returned here).
#include "core/iop_memory.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using ps2x::iop::detail::IopMemory;
static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } } while (0)

static uint32_t largestGap(const std::vector<IopMemory::Allocation> &live)
{
    auto blocks = live;
    std::sort(blocks.begin(), blocks.end(), [](auto &a, auto &b) { return a.address < b.address; });
    uint32_t best = 0, cursor = IopMemory::HeapBase;
    for (auto &b : blocks) { if (b.address > cursor) best = std::max(best, b.address - cursor); cursor = std::max(cursor, b.address + b.size); }
    if (cursor < IopMemory::HeapLimit) best = std::max(best, IopMemory::HeapLimit - cursor);
    return best;
}

int main()
{
    // 1. rounds of the measured sizes never exhaust the heap; no two live blocks overlap; the query is the largest gap.
    {
        IopMemory m;
        const uint32_t sizes[] = {0x8000, 0x4280, 0x18ac0, 0x840, 0x8d0, 0x4040, 0x41040, 0x10e80};
        std::vector<IopMemory::Allocation> live;
        for (int round = 0; round < 600; ++round)
        {
            const uint32_t size = sizes[round % 8];
            const uint32_t a = m.allocate(size, round % 3 ? 16u : 64u);
            CHECK(a != 0u, "round allocation refused");
            if (!a) break;
            live.push_back({a, (size + 15u) & ~15u});
            if (live.size() > 6)
            {
                const size_t victim = (round * 7u) % live.size();
                CHECK(m.freeAllocation(live[victim].address), "free failed");
                live.erase(live.begin() + victim);
            }
            for (size_t i = 0; i < live.size(); ++i)
                for (size_t j = i + 1; j < live.size(); ++j)
                    CHECK(live[i].address + live[i].size <= live[j].address || live[j].address + live[j].size <= live[i].address, "overlap");
            CHECK(m.maxFreeMemory() == largestGap(live), "largest gap differs");
        }
    }
    // 2. a freed block's space is given out again.
    {
        IopMemory m;
        const uint32_t a = m.allocate(0x8000, 64u), b = m.allocate(0x8000, 64u);
        CHECK(m.freeAllocation(a), "free a");
        CHECK(m.allocate(0x8000, 64u) == a, "freed space not reused");
        CHECK(m.allocate(0x100, 16u) > b, "next request goes after the live blocks");
    }
    // 3. fixed-address requests.
    {
        IopMemory m;
        const uint32_t at = IopMemory::HeapBase + 0x10000u;
        CHECK(m.allocate(0x100, 16u, at) == at, "fixed request");
        CHECK(m.allocate(0x100, 16u, at + 0x10u) == 0u, "overlapping fixed request must be refused");
        CHECK(m.allocate(0x100, 16u, 0x10u) == 0u, "fixed request below the heap must be refused");
    }
    // 4. a refusal prints a line on standard error and returns 0 (stderr is redirected to a file for this part).
    {
        const std::string path = "iop_heap_reuse_test.stderr";
        std::fflush(stderr);
        std::freopen(path.c_str(), "w", stderr);
        IopMemory m;
        const uint32_t huge = m.allocate(IopMemory::HeapLimit, 16u);
        for (int i = 0; i < 10; ++i) (void)m.allocate(0x400000u, 16u);
        std::fflush(stderr);
        std::FILE *f = std::fopen(path.c_str(), "r");
        std::string text; char buf[512];
        while (f && std::fgets(buf, sizeof buf, f)) text += buf;
        if (f) std::fclose(f);
        std::remove(path.c_str());
        CHECK(huge == 0u, "request larger than the heap returned non-zero");
        CHECK(text.find("[ps2xIOP] heap request refused") != std::string::npos, "no refusal line");
        size_t lines = 0; for (char c : text) lines += c == '\n';
        CHECK(lines == 5u, "expected 1 line for the first size and 4 for the repeated size (limit of 4 per size)");
    }
    std::printf(g_failures ? "iop_heap_reuse_test: %d failure(s)\n" : "iop_heap_reuse_test: ok\n", g_failures);
    return g_failures ? 1 : 0;
}
