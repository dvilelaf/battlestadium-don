// SPDX-License-Identifier: GPL-3.0-or-later
// T-016: loading an IRX from a buffer, the way the game does it, needs neither the game nor a window.
//
// The game (loader 0x136670) allocates IOP memory (sceSifAllocSysMemory -> IopSubsystem::allocateMemory), copies the
// decompressed module there with sceSifSetDma (-> IopSubsystem::writeMemory), and calls sceSifLoadModuleBuffer with
// that IOP address (-> IopSubsystem::loadModuleBuffer). So the bytes are in the emulator's own RAM. The host's
// "guest" memory (the main processor's RAM) holds something unrelated at that address: here, a pattern that is no ELF.
//
// Built here: a relocatable IRX (type 0xFF80) with one PT_LOAD segment and a PT_SCE_IOPMOD header, no imports, whose
// start function writes a marker word to IOP RAM and returns 0.
//
// What this does not cover: the runtime's PS2IopHostAdapter and the replaced SDK handlers (they need the whole
// runtime library); the module here is built in the test, not one of the game's.

#include "ps2x/iop/iop_subsystem.h"

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

    // Host: a main memory of 4 MiB that can be read from address 0 up, like the EE RAM seen through the adapter.
    class TestHost final : public IopHost
    {
    public:
        explicit TestHost(size_t bytes) : guest(bytes, 0x5Au) {}

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
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
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
    };

    constexpr uint32_t kMarkerAddress = 0x00030000u;
    constexpr uint32_t kMarkerValue = 0xC0FFEE01u;

    uint32_t iType(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm)
    {
        return (op << 26u) | (rs << 21u) | (rt << 16u) | (imm & 0xFFFFu);
    }

    // The IRX: ELF header, two program headers (PT_LOAD, PT_SCE_IOPMOD), the code at 0x100, the iopmod record at 0x80.
    std::vector<uint8_t> buildModule()
    {
        // start: lui t0,hi(marker); lui t1,hi(value); ori t1,t1,lo(value); sw t1,0(t0); jr ra; addiu v0,zero,0
        const uint32_t code[] = {
            iType(0x0Fu, 0u, 8u, kMarkerAddress >> 16u),
            iType(0x0Fu, 0u, 9u, kMarkerValue >> 16u),
            iType(0x0Du, 9u, 9u, kMarkerValue & 0xFFFFu),
            iType(0x2Bu, 8u, 9u, kMarkerAddress & 0xFFFFu),
            0x03E00008u,
            iType(0x09u, 0u, 2u, 0u),
        };

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
        } load{}, iopmod{};
#pragma pack(pop)
        header.ident[0] = 0x7Fu;
        header.ident[1] = 'E';
        header.ident[2] = 'L';
        header.ident[3] = 'F';
        header.ident[4] = 1u;
        header.ident[5] = 1u;
        header.ident[6] = 1u;
        header.type = 0xFF80u; // ET_SCE_IOPRELEXEC
        header.machine = 8u;
        header.version = 1u;
        header.phoff = sizeof(header);
        header.ehsize = sizeof(header);
        header.phentsize = sizeof(ProgramHeader);
        header.phnum = 2u;

        constexpr uint32_t iopmodOffset = 0x80u;
        constexpr uint32_t codeOffset = 0x100u;
        load.type = 1u;
        load.offset = codeOffset;
        load.vaddr = 0u;
        load.paddr = 0u;
        load.filesz = sizeof(code);
        load.memsz = 0x40u;
        load.flags = 7u;
        load.align = 16u;
        iopmod.type = 0x70000080u; // PT_SCE_IOPMOD
        iopmod.offset = iopmodOffset;
        iopmod.filesz = 0x20u;
        iopmod.memsz = 0x20u;
        iopmod.flags = 4u;
        iopmod.align = 4u;

        std::vector<uint8_t> image(codeOffset + sizeof(code), 0u);
        std::memcpy(image.data(), &header, sizeof(header));
        std::memcpy(image.data() + sizeof(header), &load, sizeof(load));
        std::memcpy(image.data() + sizeof(header) + sizeof(load), &iopmod, sizeof(iopmod));
        // iopmod record: moduleinfo, entry, gp, text size, data size, bss size, version, name
        const uint32_t record[] = {0u, 0u, 0u, sizeof(code), 0u, 0u, 0x0101u};
        std::memcpy(image.data() + iopmodOffset, record, sizeof(record));
        std::memcpy(image.data() + iopmodOffset + sizeof(record), "ModBufTest", 11u);
        std::memcpy(image.data() + codeOffset, code, sizeof(code));
        return image;
    }

    uint32_t readIopWord(IopSubsystem &iop, uint32_t address)
    {
        uint32_t value = 0u;
        if (!iop.readMemory(address, &value, sizeof(value)))
            return 0xDEADDEADu;
        return value;
    }

    // The game's path: allocate in IOP RAM, copy there, load from that IOP address.
    void testModuleInIopRam()
    {
        TestHost host(4u * 1024u * 1024u);
        IopSubsystem iop(host);
        const std::vector<uint8_t> image = buildModule();

        const uint32_t iopAddress = iop.allocateMemory(static_cast<uint32_t>(image.size()), 64u);
        expect(iopAddress != 0u, "allocateMemory returned 0");
        expect(iop.writeMemory(iopAddress, image.data(), image.size()), "writeMemory into IOP RAM failed");
        std::fprintf(stderr, "[test] module of %zu bytes at IOP address 0x%x\n", image.size(), iopAddress);

        const ModuleLoadResult result = iop.loadModuleBuffer(iopAddress);
        std::fprintf(stderr, "[test] loadModuleBuffer: handled=%d moduleId=%d\n", result.handled ? 1 : 0,
                     static_cast<int>(result.moduleId));
        expect(result.handled, "load not handled");
        expect(result.moduleId > 0, "module id is not above zero (loaded from IOP RAM)");
        expect(readIopWord(iop, kMarkerAddress) == kMarkerValue, "the start function did not run (marker not written)");
    }

    // A host that hands over a main-memory address keeps working: the emulator's own tests do that.
    void testModuleInMainMemory()
    {
        TestHost host(4u * 1024u * 1024u);
        IopSubsystem iop(host);
        const std::vector<uint8_t> image = buildModule();
        expect(host.writeGuest(0x100u, image.data(), image.size()), "staging in main memory failed");

        const ModuleLoadResult result = iop.loadModuleBuffer(0x100u);
        expect(result.handled && result.moduleId > 0, "module staged in main memory did not load");
        expect(readIopWord(iop, kMarkerAddress) == kMarkerValue, "start function did not run (main memory path)");
    }

    // Garbage in both memories is still refused.
    void testNoModule()
    {
        TestHost host(4u * 1024u * 1024u);
        IopSubsystem iop(host);
        const uint32_t iopAddress = iop.allocateMemory(0x1000u, 64u);
        const ModuleLoadResult result = iop.loadModuleBuffer(iopAddress);
        expect(result.moduleId <= 0, "a buffer that is no ELF returned a module id above zero");
    }
}

int main()
{
    testModuleInIopRam();
    testModuleInMainMemory();
    testNoModule();
    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("module_buffer_load_test: all checks passed\n");
    return 0;
}
