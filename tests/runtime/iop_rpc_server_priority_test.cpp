// SPDX-License-Identifier: GPL-3.0-or-later
// T-305, patch 0072: an RPC handler runs in the server thread, which only gets the processor when every thread of
// higher priority is blocked. The port runs the handler synchronously on the EE side's call, so it used to land in the
// middle of whatever a higher-priority IOP thread was doing (CRI_ADXI's request 0x409 destroying a stream object that
// the priority-39 transfer thread was about to poll; journal 034 follow-up 22). A worker of priority 39 sets "inside",
// runs for dozens of slices and clears it; the server thread has priority 48. A request that arrives while the worker
// is inside must be answered after it has left. The IRX is built in memory (same scaffolding as iop_no_preempt_test).

#include "ps2x/iop/iop_subsystem.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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


    constexpr uint32_t kLoad = 0x00010000u;
    constexpr uint32_t kHandler = kLoad + 0x200u;
    constexpr uint32_t kServerThread = kLoad + 0x300u;
    constexpr uint32_t kWorkerThread = kLoad + 0x400u;
    constexpr uint32_t kSifcmdTable = kLoad + 0x520u;
    constexpr uint32_t kRegisterRpc = kSifcmdTable + 20u;
    constexpr uint32_t kSetRpcQueue = kSifcmdTable + 28u;
    constexpr uint32_t kRpcLoop = kSifcmdTable + 36u;
    constexpr uint32_t kThbaseTable = kLoad + 0x560u;
    constexpr uint32_t kCreateThread = kThbaseTable + 20u;
    constexpr uint32_t kStartThread = kThbaseTable + 28u;
    constexpr uint32_t kDelayThread = kThbaseTable + 36u;
    constexpr uint32_t kDescriptorServer = kLoad + 0x5A0u;
    constexpr uint32_t kDescriptorWorker = kLoad + 0x5C0u;
    constexpr uint32_t kData = kLoad + 0x600u;
    constexpr uint32_t kServerData = kData + 0x40u;
    constexpr uint32_t kServerBuffer = kData + 0x80u;
    constexpr uint32_t kQueue = kData + 0xC0u;
    constexpr uint32_t kSid = 0x90000200u;

    enum Slot : uint32_t
    {
        SlotInside = 0x00,  // 1 while the worker is in its long run
        SlotSeen = 0x04,    // the value of SlotInside that the handler found
        SlotHandled = 0x08, // 1 once the handler has run
        SlotRegistered = 0x0C,
        SlotDone = 0x10,    // 1 after the worker's run
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
        for (const uint32_t descriptor : {kDescriptorServer, kDescriptorWorker})
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

        // The RPC handler: remember what the worker's flag was when it ran. It returns the server buffer.
        Code handler;
        handler.li(kT0, kData);
        handler.emit(lw(kT1, SlotInside, kT0));
        handler.emit(sw(kT1, SlotSeen, kT0));
        handler.emit(ori(kT2, kZero, 1));
        handler.emit(sw(kT2, SlotHandled, kT0));
        handler.emit(move(kV0, kA1));
        handler.emit(kJrRa);
        handler.emit(kNop);

        // The server thread (priority 48): sceSifRegisterRpc, then sceSifRpcLoop on the queue.
        Code server;
        server.emit(addiu(kSp, kSp, -32));
        server.li(kA0, kServerData);
        server.li(kA1, kSid);
        server.li(kA2, kHandler);
        server.li(kA3, kServerBuffer);
        server.emit(sw(kZero, 16, kSp));
        server.emit(sw(kZero, 20, kSp));
        server.li(kT0, kQueue);
        server.emit(sw(kT0, 24, kSp));
        callImport(server, kRegisterRpc);
        server.li(kT0, kData);
        server.emit(ori(kT1, kZero, 1));
        server.emit(sw(kT1, SlotRegistered, kT0));
        server.li(kA0, kQueue);
        callImport(server, kRpcLoop);
        server.emit(jump(kServerThread + static_cast<uint32_t>(server.words.size()) * 4u));
        server.emit(kNop);

        // The worker (priority 39): waits 20 us, then inside = 1; a long run; inside = 0; done = 1; then sleeps for ever.
        Code worker;
        worker.li(kA0, 20u);
        callImport(worker, kDelayThread);
        worker.li(kT0, kData);
        worker.emit(ori(kT1, kZero, 1));
        worker.emit(sw(kT1, SlotInside, kT0));
        worker.li(kT2, kLoopLaps);
        worker.emit(addiu(kT2, kT2, -1));
        worker.emit(bne(kT2, kZero, -2));
        worker.emit(kNop);
        worker.emit(sw(kZero, SlotInside, kT0));
        worker.emit(sw(kT1, SlotDone, kT0));
        const uint32_t sleepAt = kWorkerThread + static_cast<uint32_t>(worker.words.size()) * 4u;
        worker.li(kA0, 1000000u);
        callImport(worker, kDelayThread);
        worker.emit(jump(sleepAt));
        worker.emit(kNop);

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
        const auto sifcmd = importTable({0x63666973u, 0x0000646Du}, {17u, 19u, 22u});
        const auto thbase = importTable({0x61626874u, 0x00006573u}, {4u, 6u, 33u});
        const uint32_t descriptorServer[] = {0u, 0u, kServerThread, 0x400u, 48u};
        const uint32_t descriptorWorker[] = {0u, 0u, kWorkerThread, 0x400u, 39u};

        constexpr uint32_t imageSize = 0x800u;
        std::vector<uint8_t> segment(imageSize, 0u);
        const auto put = [&](uint32_t address, const void *data, size_t size)
        {
            std::memcpy(segment.data() + (address - kLoad), data, size);
        };
        put(kLoad, entry.words.data(), entry.words.size() * 4u);
        put(kHandler, handler.words.data(), handler.words.size() * 4u);
        put(kServerThread, server.words.data(), server.words.size() * 4u);
        put(kWorkerThread, worker.words.data(), worker.words.size() * 4u);
        put(kSifcmdTable, sifcmd.data(), sifcmd.size() * 4u);
        put(kThbaseTable, thbase.data(), thbase.size() * 4u);
        put(kDescriptorServer, descriptorServer, sizeof(descriptorServer));
        put(kDescriptorWorker, descriptorWorker, sizeof(descriptorWorker));
        expect(entry.words.size() * 4u <= 0x200u && handler.words.size() * 4u <= 0x100u &&
               server.words.size() * 4u <= 0x100u && worker.words.size() * 4u <= 0x100u, "code overlaps");

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
        TestHost host(0x20000u);
        IopSubsystem iop(host);
        buildImage(host, 0x100u);
        const ModuleLoadResult module = iop.loadModuleBuffer(0x100u);
        expect(module.handled && module.startResult == 0, "the test IRX did not start");

        // The worker is inside its long run, the server thread (lower priority) waits in its RPC loop.
        int steps = 0;
        while ((slot(iop, SlotInside) == 0u || slot(iop, SlotRegistered) == 0u) && steps++ < 400)
            iop.runEeCycles(kSliceEe);
        expectEq<uint64_t>(slot(iop, SlotInside), 1u, "the worker is inside its run");
        expectEq<uint64_t>(slot(iop, SlotRegistered), 1u, "the server registered");
        expectEq<uint64_t>(slot(iop, SlotDone), 0u, "the worker has not finished yet");

        RpcRequest request{};
        request.sid = kSid;
        request.function = 0x409u;
        const RpcResult result = iop.handleRpc(request);
        expect(result.handled, "the request was handled");
        expectEq<uint64_t>(slot(iop, SlotHandled), 1u, "the handler ran");
        expectEq<uint64_t>(slot(iop, SlotSeen), 0u, "the handler ran after the higher-priority worker left its run");
        expectEq<uint64_t>(slot(iop, SlotDone), 1u, "the worker finished its run first");
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
    std::printf("iop_rpc_server_priority_test: all checks passed\n");
    return 0;
}
