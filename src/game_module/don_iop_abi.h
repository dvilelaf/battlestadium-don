// SPDX-License-Identifier: GPL-3.0-or-later
// The second ABI between the program and a game module (T-648a): the IOP CPU state and memory the translated IOP sound
// module (generated/iop_native/adxi_native.cpp, patch 0085) reads and writes, and the descriptor the module hands the
// loader. Shared by the module (through src/game_module/iop_shim) and the host (src/host/iop_native_bridge.cpp, which
// static_asserts every offset against the emulator's real IopCpuState). Freestanding: stdint only.
#pragma once

#include <stdint.h>

// IopCpuState of ps2xIOP/src/emulator/core/iop_cpu.h, member for member (std::array<uint32_t, N> is uint32_t[N]).
struct DonIopCpuState
{
    uint32_t gpr[32];
    uint32_t hi;
    uint32_t lo;
    uint32_t pc;
    uint32_t cop0[32];
    uint32_t pendingLoadReg;
    uint32_t pendingLoadValue;
    bool pendingLoad;
    bool branchPending;
    uint32_t branchTarget;
    bool stopped;
    bool yielded;
    bool exception;
};

struct DonIopRam
{
    const uint8_t *bytes;
    const uint8_t *data() const { return bytes; }
};

// The stores of IopMemory (they mark the decoded-instruction cache and take the hardware, scratchpad and SPU2 paths);
// the host fills one table of its own, the module only calls through it.
struct DonIopMemoryCalls
{
    void (*write8)(void *host, uint32_t address, uint8_t value);
    void (*write16)(void *host, uint32_t address, uint16_t value);
    void (*write32)(void *host, uint32_t address, uint32_t value);
};

// What the translated code needs of IopMemory: the RAM bytes for loads and the stores. Built by the host for each call.
struct DonIopMemory
{
    const uint8_t *ram_bytes;
    void *host;  // the emulator's IopMemory, opaque to the module
    const DonIopMemoryCalls *calls;

    DonIopRam ram() const { return DonIopRam{ram_bytes}; }
    void write8(uint32_t address, uint8_t value) { calls->write8(host, address, value); }
    void write16(uint32_t address, uint16_t value) { calls->write16(host, address, value); }
    void write32(uint32_t address, uint32_t value) { calls->write32(host, address, value); }
};

// What the module exports for the IOP (nullable in DonGameModuleEntry); the fields of ps2x::iop::IopNativeModule.
struct DonIopDescriptor
{
    uint32_t struct_size;  // sizeof(DonIopDescriptor) in the module
    uint32_t ram_size;     // IOP RAM size the code was translated for (kRamSize in the generated file)
    const char *name;
    uint32_t code_lo;
    uint32_t code_size;
    uint64_t code_hash;
    const uint32_t *entry_pcs;
    uint32_t num_entry_pcs;
    uint32_t (*run)(DonIopCpuState *cpu, DonIopMemory *mem, uint32_t budget);
    bool (*stale)();
    void (*reset_stale)();
};
