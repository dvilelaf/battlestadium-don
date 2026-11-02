// SPDX-License-Identifier: GPL-3.0-or-later
// T-302: the IOP emulator's host SPU2 hook, tested with a fake SPU2 that records every call and a small IOP
// program built in memory. Needs neither the game nor the PCSX2 SPU2 library.
//
// The IOP program (an IRX image with the intrman and thbase imports) registers handlers for IOP interrupt 9
// and for the DMA interrupts 0x24 and 0x28, touches SPU2 registers in every access width, starts a DMA on
// channel 4 (to the SPU2) and on channel 7 (from the SPU2), and starts a thread that keeps copying the two
// channels' CHCR registers to RAM, so the test can see when each channel is busy.

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

    // ---------------------------------------------------------------------------------------------
    // Layout of the image, all addresses in IOP RAM. The image is loaded at kLoad.

    constexpr uint32_t kLoad = 0x00010000u;
    constexpr uint32_t kHandlerIrq = kLoad + 0x200u;
    constexpr uint32_t kHandlerDma0 = kLoad + 0x260u;
    constexpr uint32_t kHandlerDma1 = kLoad + 0x2C0u;
    constexpr uint32_t kThreadEntry = kLoad + 0x320u;
    constexpr uint32_t kIntrmanTable = kLoad + 0x360u;
    constexpr uint32_t kRegisterIntrHandler = kIntrmanTable + 20u;
    constexpr uint32_t kEnableIntr = kRegisterIntrHandler + 8u;
    constexpr uint32_t kThbaseTable = kLoad + 0x3A0u;
    constexpr uint32_t kCreateThread = kThbaseTable + 20u;
    constexpr uint32_t kStartThread = kCreateThread + 8u;
    constexpr uint32_t kThreadDescriptor = kLoad + 0x3E0u;
    constexpr uint32_t kData = kLoad + 0x400u;

    // Slots the program writes (offsets from kData).
    enum Slot : uint32_t
    {
        SlotRead16 = 0x00,        // lhu 0x1F900344
        SlotRead32 = 0x04,        // lw  0x1F900344
        SlotRead8 = 0x08,         // lbu 0x1F900345
        SlotChcr4AtStart = 0x0C,  // CHCR of channel 4 right after starting it
        SlotChcr7AtStart = 0x10,  // same for channel 7
        SlotOutside = 0x14,       // sh/lhu at 0x1F900800, outside the SPU2 window
        SlotRead16AfterDma = 0x18,
        SlotIrqCount = 0x20,
        SlotDma0Count = 0x24,
        SlotDma1Count = 0x28,
        SlotDma0Chcr = 0x2C,  // CHCR4 as seen by the channel 4 interrupt handler
        SlotDma1Chcr = 0x30,  // CHCR7 as seen by the channel 7 interrupt handler
        SlotDma0Madr = 0x34,
        SlotDma1Madr = 0x38,
        SlotIrqOrder = 0x3C,  // position of each handler in the order the handlers ran
        SlotDma0Order = 0x40,
        SlotDma1Order = 0x44,
        SlotSequence = 0x50,
        SlotThreadChcr4 = 0x60,  // latest CHCR4 / CHCR7 seen by the polling thread
        SlotThreadChcr7 = 0x64,
    };

    constexpr uint32_t kBuf0 = 0x00020000u; // channel 4: 3 blocks of 4 words = 48 bytes
    constexpr uint32_t kBuf1 = 0x00020100u; // channel 7: 2 blocks of 8 words = 64 bytes
    constexpr uint32_t kBuf0Bytes = 48u;
    constexpr uint32_t kBuf1Bytes = 64u;
    constexpr uint32_t kChcrTr = 1u << 24u;

    std::vector<uint8_t> pattern(uint8_t seed, size_t size)
    {
        std::vector<uint8_t> bytes(size);
        for (size_t i = 0; i < size; ++i)
            bytes[i] = static_cast<uint8_t>(seed + i * 7u);
        return bytes;
    }

    void interruptHandler(Code &code, uint32_t countSlot, uint32_t orderSlot, int dmaChannel)
    {
        code.li(kT0, kData);
        code.emit(lw(kT1, SlotSequence, kT0));
        code.emit(addiu(kT1, kT1, 1));
        code.emit(sw(kT1, SlotSequence, kT0));
        code.emit(sw(kT1, static_cast<int32_t>(orderSlot), kT0));
        code.emit(lw(kT2, static_cast<int32_t>(countSlot), kT0));
        code.emit(addiu(kT2, kT2, 1));
        code.emit(sw(kT2, static_cast<int32_t>(countSlot), kT0));
        if (dmaChannel >= 0)
        {
            code.li(kT3, 0x1F800000u);
            const int32_t base = dmaChannel == 0 ? 0x10C0 : 0x1500;
            code.emit(lw(kT2, base + 8, kT3));
            code.emit(sw(kT2, dmaChannel == 0 ? SlotDma0Chcr : SlotDma1Chcr, kT0));
            code.emit(lw(kT2, base, kT3));
            code.emit(sw(kT2, dmaChannel == 0 ? SlotDma0Madr : SlotDma1Madr, kT0));
        }
        code.emit(kJrRa);
        code.emit(kNop);
    }

    void buildImage(TestHost &host, uint32_t at)
    {
        Code entry;
        entry.emit(addiu(kSp, kSp, -32));
        entry.emit(sw(kRa, 28, kSp));
        const std::pair<uint32_t, uint32_t> handlers[] = {{9u, kHandlerIrq}, {0x24u, kHandlerDma0}, {0x28u, kHandlerDma1}};
        for (const auto &[irq, handler] : handlers)
        {
            entry.li(kA0, irq);
            entry.emit(move(kA1, kZero));
            entry.li(kA2, handler);
            entry.emit(move(kA3, kZero));
            entry.emit(jal(kRegisterIntrHandler));
            entry.emit(kNop);
            entry.li(kA0, irq);
            entry.emit(jal(kEnableIntr));
            entry.emit(kNop);
        }

        // SPU2 register accesses of every width.
        entry.li(kT0, 0x1F900000u);
        entry.li(kT3, kData);
        entry.emit(ori(kT1, kZero, 0x1234));
        entry.emit(sh(kT1, 0x1A8, kT0)); // 16-bit write
        entry.li(kT1, 0xAABBCCDDu);
        entry.emit(sw(kT1, 0x5A8, kT0)); // 32-bit write: two 16-bit writes, low half first
        entry.emit(lhu(kT2, 0x344, kT0));
        entry.emit(sw(kT2, SlotRead16, kT3));
        entry.emit(lw(kT2, 0x344, kT0)); // 32-bit read: two 16-bit reads
        entry.emit(sw(kT2, SlotRead32, kT3));
        entry.emit(lbu(kT2, 0x345, kT0)); // 8-bit read: high byte of the register at 0x344
        entry.emit(sw(kT2, SlotRead8, kT3));
        entry.emit(ori(kT1, kZero, 0x77));
        entry.emit(sb(kT1, 0x401, kT0)); // 8-bit write: read-modify-write of the register at 0x400
        // One halfword just outside the window keeps the old storage and must not reach the host.
        entry.emit(ori(kT1, kZero, 1));
        entry.emit(sh(kT1, 0x800, kT0));
        entry.emit(lhu(kT2, 0x800, kT0));
        entry.emit(sw(kT2, SlotOutside, kT3));

        // DMA: channel 4 (RAM to SPU2 core 0), then channel 7 (SPU2 core 1 to RAM).
        entry.li(kT4, 0x1F800000u);
        entry.li(kT1, kBuf0);
        entry.emit(sw(kT1, 0x10C0, kT4));
        entry.li(kT1, (3u << 16u) | 4u);
        entry.emit(sw(kT1, 0x10C4, kT4));
        entry.li(kT1, 0x01000201u);
        entry.emit(sw(kT1, 0x10C8, kT4));
        entry.emit(lw(kT2, 0x10C8, kT4));
        entry.emit(sw(kT2, SlotChcr4AtStart, kT3));
        entry.li(kT1, kBuf1);
        entry.emit(sw(kT1, 0x1500, kT4));
        entry.li(kT1, (2u << 16u) | 8u);
        entry.emit(sw(kT1, 0x1504, kT4));
        entry.li(kT1, 0x01000200u);
        entry.emit(sw(kT1, 0x1508, kT4));
        entry.emit(lw(kT2, 0x1508, kT4));
        entry.emit(sw(kT2, SlotChcr7AtStart, kT3));
        entry.emit(lhu(kT2, 0x344, kT0)); // what the old code answers here is the faked "DMA complete" bit
        entry.emit(sw(kT2, SlotRead16AfterDma, kT3));

        // The polling thread.
        entry.li(kA0, kThreadDescriptor);
        entry.emit(jal(kCreateThread));
        entry.emit(kNop);
        entry.emit(move(kA0, kV0));
        entry.emit(move(kA1, kZero));
        entry.emit(jal(kStartThread));
        entry.emit(kNop);
        entry.emit(lw(kRa, 28, kSp));
        entry.emit(addiu(kSp, kSp, 32));
        entry.emit(move(kV0, kZero));
        entry.emit(kJrRa);
        entry.emit(kNop);

        Code handlerIrq, handlerDma0, handlerDma1;
        interruptHandler(handlerIrq, SlotIrqCount, SlotIrqOrder, -1);
        interruptHandler(handlerDma0, SlotDma0Count, SlotDma0Order, 0);
        interruptHandler(handlerDma1, SlotDma1Count, SlotDma1Order, 1);

        Code thread;
        thread.li(kT0, 0x1F800000u);
        thread.li(kT2, kData);
        thread.emit(lw(kT1, 0x10C8, kT0));
        thread.emit(sw(kT1, SlotThreadChcr4, kT2));
        thread.emit(lw(kT1, 0x1508, kT0));
        thread.emit(sw(kT1, SlotThreadChcr7, kT2));
        thread.emit(jump(kThreadEntry));
        thread.emit(kNop);

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
        // "intrman" and "thbase" as the two 4-byte words of the 8-byte library name.
        const auto intrman = importTable({0x72746E69u, 0x006E616Du}, {4u, 6u});
        const auto thbase = importTable({0x61626874u, 0x00006573u}, {4u, 6u});
        const uint32_t descriptor[] = {0u, 0u, kThreadEntry, 0x400u, 20u};

        constexpr uint32_t imageSize = 0x600u;
        std::vector<uint8_t> segment(imageSize, 0u);
        const auto put = [&](uint32_t address, const void *data, size_t size)
        {
            std::memcpy(segment.data() + (address - kLoad), data, size);
        };
        put(kLoad, entry.words.data(), entry.words.size() * 4u);
        put(kHandlerIrq, handlerIrq.words.data(), handlerIrq.words.size() * 4u);
        put(kHandlerDma0, handlerDma0.words.data(), handlerDma0.words.size() * 4u);
        put(kHandlerDma1, handlerDma1.words.data(), handlerDma1.words.size() * 4u);
        put(kThreadEntry, thread.words.data(), thread.words.size() * 4u);
        put(kIntrmanTable, intrman.data(), intrman.size() * 4u);
        put(kThbaseTable, thbase.data(), thbase.size() * 4u);
        put(kThreadDescriptor, descriptor, sizeof(descriptor));
        expect(entry.words.size() * 4u <= 0x200u, "entry code overlaps the handlers");
        expect(handlerDma0.words.size() * 4u <= 0x60u && handlerDma1.words.size() * 4u <= 0x60u &&
               thread.words.size() * 4u <= 0x40u, "handler too long");

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

    void stageBuffers(IopSubsystem &iop)
    {
        const auto buf0 = pattern(0x10u, kBuf0Bytes);
        const auto buf1 = std::vector<uint8_t>(kBuf1Bytes, 0xEEu);
        expect(iop.writeMemory(kBuf0, buf0.data(), buf0.size()), "stage buffer 0");
        expect(iop.writeMemory(kBuf1, buf1.data(), buf1.size()), "stage buffer 1");
    }

    // ---------------------------------------------------------------------------------------------
    // With a host SPU2.

    void testWithHostSpu2()
    {
        std::printf("-- with a host SPU2\n");
        FakeSpu2 spu2;
        spu2.dmaReadData = pattern(0xC0u, kBuf1Bytes);
        const IopHostSpu2 hooks = makeHooks(spu2);
        iopSetHostSpu2(&hooks);
        expect(iopHostSpu2() != nullptr, "host SPU2 should be installed");

        TestHost host(0x20000u);
        IopSubsystem iop(host);
        expectEq<uint64_t>(spu2.resets, 1u, "the emulator resets the host SPU2 once at construction");
        stageBuffers(iop);
        buildImage(host, 0x100u);

        // The heuristic sound service must stay off while a host SPU2 is installed.
        (void)iop.loadModule("rom0:LIBSD");
        for (const auto &service : iop.debugSnapshot().services)
            if (service.name == "libsd")
                expect(!service.active, "the HLE libsd service must be inactive with a host SPU2");

        const ModuleLoadResult module = iop.loadModuleBuffer(0x100u);
        expect(module.handled && module.startResult == 0, "the test IRX did not start");

        // 1. What the host received during the module's start routine, in order.
        const std::vector<Call> &calls = spu2.calls;
        struct Wanted
        {
            Call::Kind kind;
            uint32_t a, b;
        };
        const Wanted wanted[] = {
            {Call::Write16, 0x1F9001A8u, 0x1234u},   // sh
            {Call::Write16, 0x1F9005A8u, 0xCCDDu},   // sw, low half
            {Call::Write16, 0x1F9005AAu, 0xAABBu},   // sw, high half
            {Call::Read16, 0x1F900344u, 0u},         // lhu
            {Call::Read16, 0x1F900344u, 0u},         // lw, low half
            {Call::Read16, 0x1F900346u, 0u},         // lw, high half
            {Call::Read16, 0x1F900344u, 0u},         // lbu 0x345
            {Call::Read16, 0x1F900400u, 0u},         // sb 0x401: read ...
            {Call::Write16, 0x1F900400u, 0x77A5u},   // ... merge (0xA1A5 & 0xFF | 0x77 << 8), write
            {Call::DmaWrite, 0u, kBuf0Bytes},        // channel 4
            {Call::DmaRead, 1u, kBuf1Bytes},         // channel 7
            {Call::Read16, 0x1F900344u, 0u},         // lhu after the DMA starts
        };
        expectEq<uint64_t>(calls.size(), std::size(wanted), "number of calls the host SPU2 received");
        for (size_t i = 0; i < std::min(calls.size(), std::size(wanted)); ++i)
        {
            const std::string what = "call " + std::to_string(i);
            expectEq<uint64_t>(calls[i].kind, wanted[i].kind, what + " kind");
            expectEq<uint64_t>(calls[i].a, wanted[i].a, what + " address or core");
            if (wanted[i].kind != Call::Read16)
                expectEq<uint64_t>(calls[i].b, wanted[i].b, what + " value or size");
        }
        if (calls.size() > 9)
        {
            expect(calls[9].bytes == pattern(0x10u, kBuf0Bytes), "channel 4 DMA handed over the wrong bytes");
        }

        // What the program read back from the fake, the outside-window access and the CHCR bits.
        expectEq<uint64_t>(slot(iop, SlotRead16), 0xA6E1u, "lhu result");
        expectEq<uint64_t>(slot(iop, SlotRead32), 0xA6E3A6E1u, "lw result");
        expectEq<uint64_t>(slot(iop, SlotRead8), 0xA6u, "lbu result");
        expectEq<uint64_t>(slot(iop, SlotOutside), 1u, "storage just outside the window");
        expectEq<uint64_t>(slot(iop, SlotRead16AfterDma), 0xA6E1u, "no faked DMA-complete bit with a host");
        expectEq<uint64_t>(slot(iop, SlotChcr4AtStart) & kChcrTr, kChcrTr, "channel 4 busy right after start");
        expectEq<uint64_t>(slot(iop, SlotChcr7AtStart) & kChcrTr, kChcrTr, "channel 7 busy right after start");
        // The read DMA's bytes are in IOP RAM as soon as the host returned them.
        std::vector<uint8_t> landed(kBuf1Bytes);
        expect(iop.readMemory(kBuf1, landed.data(), landed.size()), "read buffer 1");
        expect(landed == pattern(0xC0u, kBuf1Bytes), "channel 7 DMA did not land in IOP RAM");

        // 2. Slices with no events: busy stays set, nothing is delivered, time reaches the host.
        iop.runEeCycles(8u * 3000u);
        expectEq<uint64_t>(slot(iop, SlotThreadChcr4) & kChcrTr, kChcrTr, "channel 4 still busy, no event yet");
        expectEq<uint64_t>(slot(iop, SlotThreadChcr7) & kChcrTr, kChcrTr, "channel 7 still busy, no event yet");
        expectEq<uint64_t>(slot(iop, SlotIrqCount) + slot(iop, SlotDma0Count) + slot(iop, SlotDma1Count), 0u,
                           "no interrupt before any event");
        expectEq<uint64_t>(spu2.calls.size(), std::size(wanted), "the running threads made no SPU2 access");

        // 3. The end of channel 7 only: channel 7 completes, channel 4 stays busy.
        spu2.events.push_back({kHostSpu2EventDmaEnd1, 0});
        iop.runEeCycles(8u * 3000u);
        expectEq<uint64_t>(slot(iop, SlotDma1Count), 1u, "channel 7 interrupt count");
        expectEq<uint64_t>(slot(iop, SlotDma0Count), 0u, "channel 4 interrupt count");
        expectEq<uint64_t>(slot(iop, SlotDma1Chcr) & kChcrTr, 0u, "channel 7 TR already clear in its handler");
        expectEq<uint64_t>(slot(iop, SlotDma1Madr), kBuf1 + kBuf1Bytes, "channel 7 MADR at the end");
        expectEq<uint64_t>(slot(iop, SlotThreadChcr7) & kChcrTr, 0u, "channel 7 not busy after its event");
        expectEq<uint64_t>(slot(iop, SlotThreadChcr4) & kChcrTr, kChcrTr, "channel 4 busy until its own event");

        // 4. The SPU2 interrupt.
        spu2.events.push_back({kHostSpu2EventIrq, 0});
        iop.runEeCycles(8u * 3000u);
        expectEq<uint64_t>(slot(iop, SlotIrqCount), 1u, "SPU2 interrupt reached the IOP handler of line 9");

        // 5. The end of channel 4.
        spu2.events.push_back({kHostSpu2EventDmaEnd0, 0});
        iop.runEeCycles(8u * 3000u);
        expectEq<uint64_t>(slot(iop, SlotDma0Count), 1u, "channel 4 interrupt count");
        expectEq<uint64_t>(slot(iop, SlotDma0Chcr) & kChcrTr, 0u, "channel 4 TR already clear in its handler");
        expectEq<uint64_t>(slot(iop, SlotDma0Madr), kBuf0 + kBuf0Bytes, "channel 4 MADR at the end");
        expectEq<uint64_t>(slot(iop, SlotThreadChcr4) & kChcrTr, 0u, "channel 4 not busy after its event");
        expectEq<uint64_t>(slot(iop, SlotDma1Count), 1u, "channel 7 interrupt not repeated");
        expectEq<uint64_t>(slot(iop, SlotIrqCount), 1u, "SPU2 interrupt not repeated");

        // The handlers ran in the order of the events: 0x28, 9, 0x24.
        expectEq<uint64_t>(slot(iop, SlotDma1Order), 1u, "order: channel 7 first");
        expectEq<uint64_t>(slot(iop, SlotIrqOrder), 2u, "order: SPU2 interrupt second");
        expectEq<uint64_t>(slot(iop, SlotDma0Order), 3u, "order: channel 4 last");

        // 6. An end event for a channel whose DMA the guest never started (or already finished) is dropped.
        spu2.events.push_back({kHostSpu2EventDmaEnd0, 0});
        iop.runEeCycles(8u * 3000u);
        expectEq<uint64_t>(slot(iop, SlotDma0Count), 1u, "a second end event for an idle channel raises nothing");

        // 7. Time: the host got exactly the cycles the emulator ran, in more than one call.
        const uint64_t cycles = iop.debugSnapshot().emulatorCycles;
        expect(spu2.advanced > 0 && spu2.advanceCalls > 1, "advance should be called per slice");
        expectEq<uint64_t>(spu2.advanced, cycles, "total cycles given to advance == emulator cycles");

        // 8. Reset goes through to the host and the time base restarts.
        iop.reset();
        expectEq<uint64_t>(spu2.resets, 2u, "emulator reset resets the host SPU2");
        const uint64_t advancedBeforeReset = spu2.advanced;
        iop.runEeCycles(8u * 1000u);
        expectEq<uint64_t>(spu2.advanced - advancedBeforeReset, iop.debugSnapshot().emulatorCycles,
                           "cycles after a reset are counted from 0");

        expectEq<uint64_t>(host.audioCommands, 0u, "the HLE sound service was never driven");
        iopSetHostSpu2(nullptr);
        expect(iopHostSpu2() == nullptr, "host SPU2 should be removed");
    }

    // An incomplete set of callbacks installs nothing.
    void testIncompleteHooks()
    {
        std::printf("-- incomplete hooks\n");
        FakeSpu2 spu2;
        IopHostSpu2 hooks = makeHooks(spu2);
        hooks.poll_event = nullptr;
        iopSetHostSpu2(&hooks);
        expect(iopHostSpu2() == nullptr, "a host SPU2 without poll_event must not be installed");
        iopSetHostSpu2(nullptr);
    }

    // ---------------------------------------------------------------------------------------------
    // Without one: the old behaviour (storage for the registers, faked DMA completion).

    void testWithoutHostSpu2()
    {
        std::printf("-- without a host SPU2\n");
        FakeSpu2 spu2; // never installed
        TestHost host(0x20000u);
        IopSubsystem iop(host);
        stageBuffers(iop);
        buildImage(host, 0x100u);
        (void)iop.loadModule("rom0:LIBSD");
        for (const auto &service : iop.debugSnapshot().services)
            if (service.name == "libsd")
                expect(service.active, "the HLE libsd service is active without a host SPU2");

        const ModuleLoadResult module = iop.loadModuleBuffer(0x100u);
        expect(module.handled && module.startResult == 0, "the test IRX did not start");

        expectEq<uint64_t>(slot(iop, SlotRead16), 0u, "registers read as stored (nothing written there)");
        expectEq<uint64_t>(slot(iop, SlotRead32), 0u, "32-bit read of storage");
        expectEq<uint64_t>(slot(iop, SlotOutside), 1u, "storage just outside the window");
        expectEq<uint64_t>(slot(iop, SlotChcr4AtStart) & kChcrTr, 0u, "DMA completes at once: TR clear");
        expectEq<uint64_t>(slot(iop, SlotChcr7AtStart) & kChcrTr, 0u, "DMA completes at once: TR clear");
        expectEq<uint64_t>(slot(iop, SlotRead16AfterDma), 0x80u, "the faked DMA-complete bit in the SPU2 status");
        std::vector<uint8_t> untouched(kBuf1Bytes);
        expect(iop.readMemory(kBuf1, untouched.data(), untouched.size()), "read buffer 1");
        expect(untouched == std::vector<uint8_t>(kBuf1Bytes, 0xEEu), "a read DMA does not touch RAM without a host");

        iop.runEeCycles(8u * 3000u);
        expectEq<uint64_t>(slot(iop, SlotDma0Count), 1u, "faked channel 4 interrupt delivered");
        expectEq<uint64_t>(slot(iop, SlotDma1Count), 1u, "faked channel 7 interrupt delivered");
        expectEq<uint64_t>(slot(iop, SlotIrqCount), 0u, "no SPU2 interrupt without a host");
        expectEq<uint64_t>(slot(iop, SlotDma0Chcr) & kChcrTr, 0u, "channel 4 TR clear");
        expectEq<uint64_t>(slot(iop, SlotDma1Chcr) & kChcrTr, 0u, "channel 7 TR clear");
        expectEq<uint64_t>(spu2.calls.size(), 0u, "the fake was never called");
        expectEq<uint64_t>(spu2.advanced, 0u, "the fake was never advanced");
    }
}

int main()
{
    testIncompleteHooks();
    testWithHostSpu2();
    testWithoutHostSpu2();
    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("iop_host_spu2_test: all checks passed\n");
    return 0;
}
