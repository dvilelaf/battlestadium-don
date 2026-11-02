// SPDX-License-Identifier: GPL-3.0-or-later
// T-304, patch 0059: the IOP emulator respects the guest's interrupt-suspend state. A thread that calls
// CpuSuspendIntr, writes A, runs for several slices and writes B must not be interrupted in the middle; the
// handler of an interrupt raised meanwhile runs after CpuResumeIntr, once per raise. A second thread that never
// suspends must stay interruptible while the first one sleeps with its own state suspended (the state belongs to
// the thread, not to the processor). Fake host SPU2 (as in iop_host_spu2_test) raises the interrupt 9 events;
// the IRX is built in memory.


#include "ps2x/iop/iop_host_spu2.h"
#include "ps2x/iop/iop_subsystem.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace ps2x::iop;

    int g_failures = 0;

    bool expect(bool condition, const std::string &message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s\n", message.c_str());
            ++g_failures;
        }
        return condition;
    }

    template <typename T>
    void expectEq(const T &actual, const T &wanted, const std::string &what)
    {
        if (actual != wanted)
        {
            std::fprintf(stderr, "FAIL: %s: got 0x%llx, wanted 0x%llx\n", what.c_str(),
                         static_cast<unsigned long long>(actual), static_cast<unsigned long long>(wanted));
            ++g_failures;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Host for the IOP subsystem: nothing but a little guest memory (the IRX image is staged in it).

    class TestHost final : public IopHost
    {
    public:
        explicit TestHost(size_t bytes) : guest(bytes, 0u) {}

        bool readGuest(uint32_t address, void *destination, size_t size) const override
        {
            if (address > guest.size() || size > guest.size() - address)
                return false;
            std::memcpy(destination, guest.data() + address, size);
            return true;
        }
        bool writeGuest(uint32_t address, const void *source, size_t size) override
        {
            if (address > guest.size() || size > guest.size() - address)
                return false;
            std::memcpy(guest.data() + address, source, size);
            return true;
        }
        bool zeroGuest(uint32_t address, size_t size) override
        {
            if (address > guest.size() || size > guest.size() - address)
                return false;
            std::memset(guest.data() + address, 0, size);
            return true;
        }
        bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const override
        {
            normalized = address;
            return address <= guest.size();
        }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override { ++audioCommands; }
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return 0u; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override
        {
            return false;
        }
        void log(LogLevel, std::string_view) override {}

        std::vector<uint8_t> guest;
        int audioCommands = 0;
    };

    // ---------------------------------------------------------------------------------------------
    // The fake SPU2: records every call in order. A register read returns (low 16 bits of the address) ^ 0xA5A5.

    struct Call
    {
        enum Kind
        {
            Write16,
            Read16,
            DmaWrite,
            DmaRead,
        } kind;
        uint32_t a = 0; // address, or core
        uint32_t b = 0; // value, or byte count
        std::vector<uint8_t> bytes;
    };

    struct FakeSpu2
    {
        std::vector<Call> calls;
        uint64_t advanced = 0;
        int advanceCalls = 0;
        int resets = 0;
        std::deque<std::pair<int, uint64_t>> events;
        std::vector<uint8_t> dmaReadData;

        static uint16_t registerValue(uint32_t addr) { return static_cast<uint16_t>((addr & 0xFFFFu) ^ 0xA5A5u); }
    };

    FakeSpu2 *fake(void *user) { return static_cast<FakeSpu2 *>(user); }

    IopHostSpu2 makeHooks(FakeSpu2 &spu2)
    {
        IopHostSpu2 hooks;
        hooks.user = &spu2;
        hooks.write16 = [](void *user, uint32_t addr, uint16_t value)
        { fake(user)->calls.push_back({Call::Write16, addr, value, {}}); };
        hooks.read16 = [](void *user, uint32_t addr) -> uint16_t
        {
            fake(user)->calls.push_back({Call::Read16, addr, 0u, {}});
            return FakeSpu2::registerValue(addr);
        };
        hooks.dma_write = [](void *user, int core, const uint8_t *data, uint32_t bytes)
        {
            fake(user)->calls.push_back({Call::DmaWrite, static_cast<uint32_t>(core), bytes,
                                         std::vector<uint8_t>(data, data + bytes)});
        };
        hooks.dma_read = [](void *user, int core, uint8_t *dst, uint32_t bytes)
        {
            FakeSpu2 *self = fake(user);
            self->calls.push_back({Call::DmaRead, static_cast<uint32_t>(core), bytes, {}});
            std::memcpy(dst, self->dmaReadData.data(), std::min<size_t>(bytes, self->dmaReadData.size()));
        };
        hooks.advance = [](void *user, uint64_t cycles)
        {
            fake(user)->advanced += cycles;
            ++fake(user)->advanceCalls;
        };
        hooks.poll_event = [](void *user, int *type, uint64_t *cycle) -> int
        {
            FakeSpu2 *self = fake(user);
            if (self->events.empty())
                return 0;
            *type = self->events.front().first;
            *cycle = self->events.front().second;
            self->events.pop_front();
            return 1;
        };
        hooks.reset = [](void *user) { ++fake(user)->resets; };
        return hooks;
    }

    // ---------------------------------------------------------------------------------------------
    // A tiny MIPS assembler, just enough for the program below.

    constexpr uint32_t kZero = 0, kV0 = 2, kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7;
    constexpr uint32_t kT0 = 8, kT1 = 9, kT2 = 10, kT3 = 11, kT4 = 12, kSp = 29, kRa = 31;

    uint32_t iType(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm)
    {
        return (op << 26u) | (rs << 21u) | (rt << 16u) | (imm & 0xFFFFu);
    }
    uint32_t lui(uint32_t rt, uint32_t imm) { return iType(0x0Fu, 0u, rt, imm); }
    uint32_t ori(uint32_t rt, uint32_t rs, uint32_t imm) { return iType(0x0Du, rs, rt, imm); }
    uint32_t addiu(uint32_t rt, uint32_t rs, int32_t imm) { return iType(0x09u, rs, rt, static_cast<uint32_t>(imm)); }
    uint32_t lw(uint32_t rt, int32_t off, uint32_t base) { return iType(0x23u, base, rt, static_cast<uint32_t>(off)); }
    uint32_t sw(uint32_t rt, int32_t off, uint32_t base) { return iType(0x2Bu, base, rt, static_cast<uint32_t>(off)); }
    uint32_t lhu(uint32_t rt, int32_t off, uint32_t base) { return iType(0x25u, base, rt, static_cast<uint32_t>(off)); }
    uint32_t lbu(uint32_t rt, int32_t off, uint32_t base) { return iType(0x24u, base, rt, static_cast<uint32_t>(off)); }
    uint32_t sh(uint32_t rt, int32_t off, uint32_t base) { return iType(0x29u, base, rt, static_cast<uint32_t>(off)); }
    uint32_t sb(uint32_t rt, int32_t off, uint32_t base) { return iType(0x28u, base, rt, static_cast<uint32_t>(off)); }
    uint32_t move(uint32_t rd, uint32_t rs) { return (rs << 21u) | (rd << 11u) | 0x21u; } // addu rd, rs, zero
    uint32_t jal(uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); }
    uint32_t jump(uint32_t target) { return 0x08000000u | ((target >> 2u) & 0x03FFFFFFu); }
    constexpr uint32_t kJrRa = 0x03E00008u;
    constexpr uint32_t kNop = 0u;

    struct Code
    {
        std::vector<uint32_t> words;
        // The R3000A has a load delay slot: the instruction after a load does not see the loaded value, so every
        // load is followed by a nop.
        void emit(uint32_t w)
        {
            words.push_back(w);
            const uint32_t op = w >> 26u;
            if (op == 0x23u || op == 0x24u || op == 0x25u)
                words.push_back(0u);
        }
        void li(uint32_t rt, uint32_t value)
        {
            emit(lui(rt, value >> 16u));
            emit(ori(rt, rt, value & 0xFFFFu));
        }
    };
    uint32_t rType(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t funct)
    {
        return (rs << 21u) | (rt << 16u) | (rd << 11u) | funct;
    }
    uint32_t bne(uint32_t rs, uint32_t rt, int32_t words) { return iType(0x05u, rs, rt, static_cast<uint32_t>(words)); }


    // Layout, all addresses in IOP RAM; the image is loaded at kLoad.
    constexpr uint32_t kLoad = 0x00010000u;
    constexpr uint32_t kHandler = kLoad + 0x200u;
    constexpr uint32_t kThread1 = kLoad + 0x300u;
    constexpr uint32_t kThread2 = kLoad + 0x400u;
    constexpr uint32_t kIntrmanTable = kLoad + 0x500u;
    constexpr uint32_t kRegisterIntrHandler = kIntrmanTable + 20u;
    constexpr uint32_t kEnableIntr = kIntrmanTable + 28u;
    constexpr uint32_t kCpuSuspendIntr = kIntrmanTable + 36u;
    constexpr uint32_t kCpuResumeIntr = kIntrmanTable + 44u;
    constexpr uint32_t kThbaseTable = kLoad + 0x560u;
    constexpr uint32_t kCreateThread = kThbaseTable + 20u;
    constexpr uint32_t kStartThread = kThbaseTable + 28u;
    constexpr uint32_t kDelayThread = kThbaseTable + 36u;
    constexpr uint32_t kDescriptor1 = kLoad + 0x5A0u;
    constexpr uint32_t kDescriptor2 = kLoad + 0x5C0u;
    constexpr uint32_t kData = kLoad + 0x600u;

    enum Slot : uint32_t
    {
        SlotA = 0x00,
        SlotB = 0x04,
        SlotCount = 0x08,     // handler entries
        SlotBad = 0x0C,       // handler entries that found A != B (the structure half written)
        SlotOld1 = 0x10,      // state CpuSuspendIntr returned the first time
        SlotOld2 = 0x14,      // and the second time
        SlotStage = 0x18,     // 1 after the first resume
        SlotThread2 = 0x1C,   // laps of the thread that never suspends
    };

    constexpr uint32_t kLoopLaps = 4000u; // about 12000 instructions: more than forty slices of 256 cycles

    void callImport(Code &c, uint32_t stub)
    {
        c.emit(jal(stub));
        c.emit(kNop);
    }

    void buildImage(TestHost &host, uint32_t at)
    {
        Code entry;
        entry.emit(addiu(kSp, kSp, -32));
        entry.emit(sw(kRa, 28, kSp));
        entry.li(kA0, 9u);
        entry.emit(move(kA1, kZero));
        entry.li(kA2, kHandler);
        entry.emit(move(kA3, kZero));
        callImport(entry, kRegisterIntrHandler);
        entry.li(kA0, 9u);
        callImport(entry, kEnableIntr);
        for (const uint32_t descriptor : {kDescriptor1, kDescriptor2})
        {
            entry.li(kA0, descriptor);
            callImport(entry, kCreateThread);
            entry.emit(move(kA0, kV0));
            entry.emit(move(kA1, kZero));
            callImport(entry, kStartThread);
        }
        entry.emit(lw(kRa, 28, kSp));
        entry.emit(addiu(kSp, kSp, 32));
        entry.emit(move(kV0, kZero));
        entry.emit(kJrRa);
        entry.emit(kNop);

        // The interrupt handler: count it, and count it as bad if A and B differ.
        Code handler;
        handler.li(kT0, kData);
        handler.emit(lw(kT1, SlotCount, kT0));
        handler.emit(addiu(kT1, kT1, 1));
        handler.emit(sw(kT1, SlotCount, kT0));
        handler.emit(lw(kT2, SlotA, kT0));
        handler.emit(lw(kT3, SlotB, kT0));
        handler.emit(rType(kT2, kT3, kT4, 0x26u));    // xor t4, t2, t3
        handler.emit(rType(kZero, kT4, kT4, 0x2Bu));  // sltu t4, zero, t4
        handler.emit(lw(kT2, SlotBad, kT0));
        handler.emit(rType(kT2, kT4, kT2, 0x21u));    // addu t2, t2, t4
        handler.emit(sw(kT2, SlotBad, kT0));
        handler.emit(kJrRa);
        handler.emit(kNop);

        // Thread 1 (priority 20): suspend; A = 1; a long run; B = 1; resume; then suspend again and sleep.
        Code t1;
        t1.li(kA0, kData + SlotOld1);
        callImport(t1, kCpuSuspendIntr);
        t1.li(kT0, kData);
        t1.emit(ori(kT1, kZero, 1));
        t1.emit(sw(kT1, SlotA, kT0));
        t1.li(kT2, kLoopLaps);
        t1.emit(addiu(kT2, kT2, -1));
        t1.emit(bne(kT2, kZero, -2));
        t1.emit(kNop);
        t1.emit(sw(kT1, SlotB, kT0));
        t1.emit(lw(kA0, SlotOld1, kT0));
        callImport(t1, kCpuResumeIntr);
        t1.li(kT0, kData);
        t1.emit(ori(kT1, kZero, 1));
        t1.emit(sw(kT1, SlotStage, kT0));
        t1.li(kA0, kData + SlotOld2);
        callImport(t1, kCpuSuspendIntr);
        t1.li(kA0, 0x00100000u);
        callImport(t1, kDelayThread);
        t1.emit(jump(kThread1 + static_cast<uint32_t>(t1.words.size()) * 4u - 4u));
        t1.emit(kNop);

        // Thread 2 (priority 30): never suspends, counts laps.
        Code t2;
        t2.li(kT0, kData);
        t2.emit(lw(kT1, SlotThread2, kT0));
        t2.emit(addiu(kT1, kT1, 1));
        t2.emit(sw(kT1, SlotThread2, kT0));
        t2.emit(jump(kThread2));
        t2.emit(kNop);

        const auto importTable = [](std::initializer_list<uint32_t> name, std::initializer_list<uint32_t> ordinals)
        {
            std::vector<uint32_t> table = {0x41E00000u, 0u, 0x00000101u};
            table.insert(table.end(), name.begin(), name.end());
            for (uint32_t ordinal : ordinals)
            {
                table.push_back(kJrRa);
                table.push_back(0x24000000u | ordinal);
            }
            table.push_back(0u);
            table.push_back(0u);
            return table;
        };
        const auto intrman = importTable({0x72746E69u, 0x006E616Du}, {4u, 6u, 17u, 18u});
        const auto thbase = importTable({0x61626874u, 0x00006573u}, {4u, 6u, 33u});
        const uint32_t descriptor1[] = {0u, 0u, kThread1, 0x400u, 20u};
        const uint32_t descriptor2[] = {0u, 0u, kThread2, 0x400u, 30u};

        constexpr uint32_t imageSize = 0x800u;
        std::vector<uint8_t> segment(imageSize, 0u);
        const auto put = [&](uint32_t address, const void *data, size_t size)
        {
            std::memcpy(segment.data() + (address - kLoad), data, size);
        };
        put(kLoad, entry.words.data(), entry.words.size() * 4u);
        put(kHandler, handler.words.data(), handler.words.size() * 4u);
        put(kThread1, t1.words.data(), t1.words.size() * 4u);
        put(kThread2, t2.words.data(), t2.words.size() * 4u);
        put(kIntrmanTable, intrman.data(), intrman.size() * 4u);
        put(kThbaseTable, thbase.data(), thbase.size() * 4u);
        put(kDescriptor1, descriptor1, sizeof(descriptor1));
        put(kDescriptor2, descriptor2, sizeof(descriptor2));
        expect(entry.words.size() * 4u <= 0x200u && handler.words.size() * 4u <= 0x100u &&
               t1.words.size() * 4u <= 0x100u && t2.words.size() * 4u <= 0x100u, "code overlaps");

#pragma pack(push, 1)
        struct ElfHeader
        {
            uint8_t ident[16];
            uint16_t type, machine;
            uint32_t version, entry, phoff, shoff, flags;
            uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
        } header{};
        struct ProgramHeader
        {
            uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
        } program{};
#pragma pack(pop)
        header.ident[0] = 0x7Fu;
        header.ident[1] = 'E';
        header.ident[2] = 'L';
        header.ident[3] = 'F';
        header.ident[4] = 1u;
        header.ident[5] = 1u;
        header.ident[6] = 1u;
        header.type = 2u;
        header.machine = 8u;
        header.version = 1u;
        header.entry = kLoad;
        header.phoff = sizeof(header);
        header.ehsize = sizeof(header);
        header.phentsize = sizeof(program);
        header.phnum = 1u;
        constexpr uint32_t codeOffset = 0x100u;
        program.type = 1u;
        program.offset = codeOffset;
        program.vaddr = kLoad;
        program.paddr = kLoad;
        program.filesz = imageSize;
        program.memsz = imageSize;
        program.flags = 7u;
        program.align = 4u;

        std::memset(host.guest.data() + at, 0, codeOffset + imageSize);
        std::memcpy(host.guest.data() + at, &header, sizeof(header));
        std::memcpy(host.guest.data() + at + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + at + codeOffset, segment.data(), segment.size());
    }

    uint32_t slot(IopSubsystem &iop, uint32_t offset)
    {
        uint32_t value = 0xDEADBEEFu;
        if (!iop.readMemory(kData + offset, &value, sizeof(value)))
            expect(false, "could not read IOP RAM");
        return value;
    }

    constexpr uint64_t kSliceEe = 8u * 256u; // one slice of the emulator, in EE cycles

    void run()
    {
        FakeSpu2 spu2;
        const IopHostSpu2 hooks = makeHooks(spu2);
        iopSetHostSpu2(&hooks);

        TestHost host(0x20000u);
        IopSubsystem iop(host);
        buildImage(host, 0x100u);
        const ModuleLoadResult module = iop.loadModuleBuffer(0x100u);
        expect(module.handled && module.startResult == 0, "the test IRX did not start");

        // Thread 1 runs (suspended) until A is written, and has not written B yet: the structure is half done.
        int steps = 0;
        while (slot(iop, SlotA) == 0u && steps++ < 200)
            iop.runEeCycles(kSliceEe);
        expectEq<uint64_t>(slot(iop, SlotA), 1u, "thread 1 wrote A");
        expectEq<uint64_t>(slot(iop, SlotB), 0u, "B not yet written when the interrupts arrive");
        iop.runEeCycles(kSliceEe * 3u);
        // Two interrupts raised in the middle (a count, not a flag: both must be answered).
        spu2.events.push_back({kHostSpu2EventIrq, 0});
        spu2.events.push_back({kHostSpu2EventIrq, 0});
        iop.runEeCycles(kSliceEe * 3u);
        expectEq<uint64_t>(slot(iop, SlotB), 0u, "B still not written");
        expectEq<uint64_t>(slot(iop, SlotCount), 0u, "no handler runs while thread 1 has interrupts suspended");

        // Let thread 1 finish the structure, resume and go to sleep.
        steps = 0;
        while (slot(iop, SlotStage) == 0u && steps++ < 400)
            iop.runEeCycles(kSliceEe);
        expectEq<uint64_t>(slot(iop, SlotStage), 1u, "thread 1 resumed");
        iop.runEeCycles(kSliceEe * 8u);
        expectEq<uint64_t>(slot(iop, SlotB), 1u, "B written");
        expectEq<uint64_t>(slot(iop, SlotCount), 2u, "both interrupts were answered, once each");
        expectEq<uint64_t>(slot(iop, SlotBad), 0u, "the handler never saw the structure half written");
        expectEq<uint64_t>(slot(iop, SlotOld1), 1u, "CpuSuspendIntr returned 'enabled' the first time");

        // Thread 1 now sleeps with interrupts suspended (its own state); thread 2 never suspended.
        iop.runEeCycles(kSliceEe * 8u);
        expectEq<uint64_t>(slot(iop, SlotOld2), 1u, "CpuSuspendIntr returned 'enabled' the second time (resume restored it)");
        const uint32_t laps = slot(iop, SlotThread2);
        expect(laps > 0u, "thread 2 runs while thread 1 sleeps");
        spu2.events.push_back({kHostSpu2EventIrq, 0});
        iop.runEeCycles(kSliceEe * 4u);
        expectEq<uint64_t>(slot(iop, SlotCount), 3u, "thread 2 is interruptible while thread 1 sleeps suspended");
        expectEq<uint64_t>(slot(iop, SlotBad), 0u, "still no bad entry");
        iopSetHostSpu2(nullptr);
    }
}

int main()
{
    run();
    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("iop_intr_suspend_test: all checks passed\n");
    return 0;
}
