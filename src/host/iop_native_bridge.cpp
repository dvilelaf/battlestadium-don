// SPDX-License-Identifier: GPL-3.0-or-later
// Registers the IOP descriptor of a loaded game module (src/game_module/don_iop_abi.h, T-648a) as the translated IOP
// sound module of the emulator (patch 0085): the same IopNativeModule that src/host/iop_native.cpp registers for the
// linked build, whose `run` calls the module's. Only built with DON_LINK_GENERATED=OFF.
#include <cstddef>
#include <cstdio>
#include <string>

#include "don_iop_abi.h"
#include "game_module.h"
#include "iop_cpu.h"     // the emulator's own (ps2xIOP/src/emulator/core), not src/game_module/iop_shim's
#include "iop_memory.h"
#include "ps2x/iop/iop_native_module.h"

namespace
{
namespace detail = ps2x::iop::detail;

// The module reads and writes the real IopCpuState through DonIopCpuState: every offset must agree.
#define DON_IOP_SAME(member) \
    static_assert(offsetof(DonIopCpuState, member) == offsetof(detail::IopCpuState, member), "IopCpuState::" #member)
DON_IOP_SAME(gpr);
DON_IOP_SAME(hi);
DON_IOP_SAME(lo);
DON_IOP_SAME(pc);
DON_IOP_SAME(cop0);
DON_IOP_SAME(pendingLoadReg);
DON_IOP_SAME(pendingLoadValue);
DON_IOP_SAME(pendingLoad);
DON_IOP_SAME(branchPending);
DON_IOP_SAME(branchTarget);
DON_IOP_SAME(stopped);
DON_IOP_SAME(yielded);
DON_IOP_SAME(exception);
#undef DON_IOP_SAME
static_assert(sizeof(DonIopCpuState) == sizeof(detail::IopCpuState), "IopCpuState size");
static_assert(sizeof(DonIopCpuState::gpr) == sizeof(detail::IopCpuState::gpr), "gpr size");
static_assert(sizeof(DonIopCpuState::cop0) == sizeof(detail::IopCpuState::cop0), "cop0 size");

const DonIopDescriptor *g_descriptor = nullptr;
ps2x::iop::IopNativeModule g_module;

void write8(void *host, uint32_t address, uint8_t value) { static_cast<detail::IopMemory *>(host)->write8(address, value); }
void write16(void *host, uint32_t address, uint16_t value) { static_cast<detail::IopMemory *>(host)->write16(address, value); }
void write32(void *host, uint32_t address, uint32_t value) { static_cast<detail::IopMemory *>(host)->write32(address, value); }
const DonIopMemoryCalls kMemoryCalls = {&write8, &write16, &write32};

uint32_t run(detail::IopCpuState &cpu, detail::IopMemory &mem, uint32_t budget)
{
    DonIopMemory view = {mem.ram().data(), &mem, &kMemoryCalls};
    return g_descriptor->run(reinterpret_cast<DonIopCpuState *>(&cpu), &view, budget);
}

bool stale() { return g_descriptor->stale != nullptr && g_descriptor->stale(); }
void resetStale()
{
    if (g_descriptor->reset_stale != nullptr)
        g_descriptor->reset_stale();
}

bool refuse(std::string &error, const std::string &why)
{
    error = "E58 the game module's IOP descriptor is not usable: " + why;
    return false;
}
} // namespace

bool don_register_iop_descriptor(const DonIopDescriptor *descriptor, std::string &error)
{
    if (descriptor == nullptr)
    {
        ps2x::iop::registerNativeModule(nullptr);
        g_descriptor = nullptr;
        return true;
    }
    if (descriptor->struct_size != sizeof(DonIopDescriptor))
        return refuse(error, "struct size " + std::to_string(descriptor->struct_size) + ", this program needs " +
                                 std::to_string(sizeof(DonIopDescriptor)));
    if (descriptor->name == nullptr || descriptor->entry_pcs == nullptr || descriptor->num_entry_pcs == 0 ||
        descriptor->run == nullptr)
        return refuse(error, "a name, entry pcs or run function is missing");
    if (descriptor->ram_size != detail::IopMemory::RamSize)
        return refuse(error, "it was translated for " + std::to_string(descriptor->ram_size) + " bytes of IOP RAM, not " +
                                 std::to_string(detail::IopMemory::RamSize));
    if (descriptor->code_size == 0 || (descriptor->code_lo & 3u) != 0 || (descriptor->code_size & 3u) != 0 ||
        descriptor->code_lo > descriptor->ram_size || descriptor->code_size > descriptor->ram_size - descriptor->code_lo)
        return refuse(error, "its code range is empty, unaligned or outside IOP RAM");
    for (uint32_t i = 0; i < descriptor->num_entry_pcs; ++i)
    {
        const uint32_t pc = descriptor->entry_pcs[i];
        if ((pc & 3u) != 0 || pc < descriptor->code_lo || pc - descriptor->code_lo >= descriptor->code_size)
            return refuse(error, "entry pc " + std::to_string(pc) + " is outside its code range");
    }
    g_descriptor = descriptor;
    g_module = {};
    g_module.name = descriptor->name;
    g_module.codeLo = descriptor->code_lo;
    g_module.codeSize = descriptor->code_size;
    g_module.codeHash = descriptor->code_hash;
    g_module.entryPcs = descriptor->entry_pcs;
    g_module.numEntryPcs = descriptor->num_entry_pcs;
    g_module.run = &run;
    g_module.stale = &stale;
    g_module.resetStale = &resetStale;
    ps2x::iop::registerNativeModule(&g_module);
    return true;
}
