// SPDX-License-Identifier: GPL-3.0-or-later
// Host side of the game-module ABI (T-610, design in docs/tasks/M6-public-release.md A1): builds the one DonRuntime
// (src/game_module/don_guest_abi.h) from the real PS2Runtime and gives it to the loaded module. Only built with
// DON_LINK_GENERATED=OFF.
//
// The module never sees the PS2Runtime's type: the third argument of every guest function stays the real PS2Runtime*,
// and the DonRuntime methods in the module pass it back as the first argument of each call below, where it is cast
// back to PS2Runtime. The host functions ps2_syscalls::X / ps2_stubs::X are in don_runtime_bridge_fns.cpp.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "game_module.h"
#include "game_overrides.h"
#include "ps2_runtime.h"

// The ABI header declares R5900Context, DonHotState, ... under the runtime's own names: it goes in its own namespace.
#define DON_ABI_NAMESPACE don_abi
#include "don_guest_abi.h"

size_t don_bridge_fill_host_fns(void **out, size_t capacity);

#pragma GCC diagnostic ignored "-Winvalid-offsetof"

namespace
{
using don_abi::DonRuntime;

// ---- layout the module relies on (the ABI header's copy against the runtime's) -------------------------------
static_assert(sizeof(don_abi::R5900Context) == sizeof(R5900Context) && alignof(don_abi::R5900Context) == alignof(R5900Context),
              "R5900Context layout");
#define DON_SAME_OFFSET(field) \
    static_assert(offsetof(don_abi::R5900Context, field) == offsetof(R5900Context, field), "R5900Context::" #field)
DON_SAME_OFFSET(r);
DON_SAME_OFFSET(pc);
DON_SAME_OFFSET(insn_count);
DON_SAME_OFFSET(hi);
DON_SAME_OFFSET(lo1);
DON_SAME_OFFSET(sa);
DON_SAME_OFFSET(vu0_vf);
DON_SAME_OFFSET(vi);
DON_SAME_OFFSET(vu0_acc);
DON_SAME_OFFSET(vu0_pc);
DON_SAME_OFFSET(vu0_cf);
DON_SAME_OFFSET(cop0_index);
DON_SAME_OFFSET(cop0_errorepc);
DON_SAME_OFFSET(llbit);
DON_SAME_OFFSET(in_delay_slot);
DON_SAME_OFFSET(branch_pc);
DON_SAME_OFFSET(cop2_ccr);
DON_SAME_OFFSET(f);
DON_SAME_OFFSET(f_acc);
DON_SAME_OFFSET(fcr31);
#undef DON_SAME_OFFSET

#define DON_SAME_HOT(abi_field, field) \
    static_assert(offsetof(don_abi::DonHotState, abi_field) == offsetof(DonHotState, field), "DonHotState::" #field)
static_assert(sizeof(don_abi::DonHotState) == sizeof(DonHotState), "DonHotState size");
DON_SAME_HOT(ee_cycle, eeCycle);
DON_SAME_HOT(next_fast_path_cycle, nextFastPathCycle);
DON_SAME_HOT(iop_pending_ee_cycles, iopPendingEeCycles);
DON_SAME_HOT(iop_batch_ee_cycles, iopBatchEeCycles);
DON_SAME_HOT(dispatch_count, dispatchCount);
DON_SAME_HOT(fast_path_count, fastPathCount);
DON_SAME_HOT(pending_timer_irqs, pendingEeTimerInterrupts);
DON_SAME_HOT(timer_irq_mask, timerIrqMask);
DON_SAME_HOT(timer_pending_cycles, timerPendingCycles);
DON_SAME_HOT(timer_cycles_to_event, timerCyclesToEvent);
DON_SAME_HOT(checkpoint_pending, checkpointPending);
DON_SAME_HOT(unwinding, unwinding);
DON_SAME_HOT(stop_requested, stopRequested);
DON_SAME_HOT(history_enabled, historyEnabled);
DON_SAME_HOT(fn_table_base, fnTableBase);
DON_SAME_HOT(fn_table_end, fnTableEnd);
DON_SAME_HOT(fn_table_slots, fnTableSlots);
DON_SAME_HOT(fn_table, fnTable);
#undef DON_SAME_HOT

static_assert(static_cast<uint32_t>(don_abi::DonBranchKind::DirectJump) == static_cast<uint32_t>(PS2Runtime::GuestBranchKind::DirectJump) &&
                  static_cast<uint32_t>(don_abi::DonBranchKind::DirectCall) == static_cast<uint32_t>(PS2Runtime::GuestBranchKind::DirectCall) &&
                  static_cast<uint32_t>(don_abi::DonBranchKind::IndirectJump) == static_cast<uint32_t>(PS2Runtime::GuestBranchKind::IndirectJump) &&
                  static_cast<uint32_t>(don_abi::DonBranchKind::IndirectCall) == static_cast<uint32_t>(PS2Runtime::GuestBranchKind::IndirectCall) &&
                  static_cast<uint32_t>(don_abi::DonBranchKind::Return) == static_cast<uint32_t>(PS2Runtime::GuestBranchKind::Return),
              "GuestBranchKind values");
static_assert(sizeof(don_abi::DonGuestFn) == sizeof(PS2Runtime::RecompiledFunction), "guest function pointer");

// ---- the calls: the opaque runtime pointer is the real PS2Runtime ----------------------------------------------
PS2Runtime *real(DonRuntime *runtime) { return reinterpret_cast<PS2Runtime *>(runtime); }
R5900Context *real(don_abi::R5900Context *ctx) { return reinterpret_cast<R5900Context *>(ctx); }

uint8_t load8(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c, uint32_t a) { return real(r)->Load8(rdram, real(c), a); }
uint16_t load16(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c, uint32_t a) { return real(r)->Load16(rdram, real(c), a); }
uint32_t load32(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c, uint32_t a) { return real(r)->Load32(rdram, real(c), a); }
uint64_t load64(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c, uint32_t a) { return real(r)->Load64(rdram, real(c), a); }
__m128i load128(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c, uint32_t a) { return real(r)->Load128(rdram, real(c), a); }
void store8(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c, uint32_t a, uint8_t v) { real(r)->Store8(rdram, real(c), a, v); }
void store16(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c, uint32_t a, uint16_t v) { real(r)->Store16(rdram, real(c), a, v); }
void store32(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c, uint32_t a, uint32_t v) { real(r)->Store32(rdram, real(c), a, v); }
void store64(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c, uint32_t a, uint64_t v) { real(r)->Store64(rdram, real(c), a, v); }
void store128(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c, uint32_t a, __m128i v) { real(r)->Store128(rdram, real(c), a, v); }

void handleSyscall(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c, uint32_t id) { real(r)->handleSyscall(rdram, real(c), id); }
void handleBreak(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c) { real(r)->handleBreak(rdram, real(c)); }
void signalException(DonRuntime *r, don_abi::R5900Context *c, int exception)
{
    real(r)->SignalException(real(c), static_cast<PS2Exception>(exception));
}
void clearLLBit(DonRuntime *r, don_abi::R5900Context *c) { real(r)->clearLLBit(real(c)); }
void vu0StartMicroProgram(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c, uint32_t address)
{
    real(r)->vu0StartMicroProgram(rdram, real(c), address);
}

bool checkpointDueSlow(DonRuntime *r) { return real(r)->eeScheduler().checkpointDueSlow(); }
void flushIopDebt(DonRuntime *r) { real(r)->eeScheduler().flushIopDebt(); }
uint32_t applyEeTimers(DonRuntime *r) { return real(r)->memory().applyEeTimers(); }
void pushDispatchPc(DonRuntime *, uint32_t pc) { ps2PushDispatchPc(pc); }
bool dispatchBranchTail(DonRuntime *r, uint8_t *rdram, don_abi::R5900Context *c, uint32_t target, uint32_t source,
                        uint32_t fallthrough, don_abi::DonBranchKind kind, const char *debugName)
{
    return real(r)->dispatchGuestBranchTail(rdram, real(c), target, source, fallthrough,
                                            static_cast<PS2Runtime::GuestBranchKind>(kind), debugName);
}

don_abi::DonRuntimeCalls g_calls;
don_abi::DonRuntime g_runtime;

void fillCalls()
{
    g_calls.abi_version = DON_ABI_VERSION;
    g_calls.size = sizeof(don_abi::DonRuntimeCalls);
    g_calls.load8 = load8;
    g_calls.load16 = load16;
    g_calls.load32 = load32;
    g_calls.load64 = load64;
    g_calls.load128 = load128;
    g_calls.store8 = store8;
    g_calls.store16 = store16;
    g_calls.store32 = store32;
    g_calls.store64 = store64;
    g_calls.store128 = store128;
    g_calls.handle_syscall = handleSyscall;
    g_calls.handle_break = handleBreak;
    g_calls.signal_exception = signalException;
    g_calls.clear_ll_bit = clearLLBit;
    g_calls.vu0_start_microprogram = vu0StartMicroProgram;
    g_calls.checkpoint_due_slow = checkpointDueSlow;
    g_calls.flush_iop_debt = flushIopDebt;
    g_calls.apply_ee_timers = applyEeTimers;
    g_calls.push_dispatch_pc = pushDispatchPc;
    g_calls.dispatch_branch_tail = dispatchBranchTail;

    void *fns[don_abi::DON_HF_COUNT];
    if (don_bridge_fill_host_fns(fns, don_abi::DON_HF_COUNT) != don_abi::DON_HF_COUNT)
    {
        std::fprintf(stderr, "[don] host function table does not match the ABI (%u entries expected): rerun tools/pipeline/gen_hostfns.py\n",
                     static_cast<unsigned>(don_abi::DON_HF_COUNT));
        std::fflush(stderr);
        std::abort();
    }
    for (uint32_t i = 0; i < don_abi::DON_HF_COUNT; ++i)
        g_calls.host_fn[i] = reinterpret_cast<don_abi::DonHostFn>(fns[i]);
}

// Runs from loadELF (the runtime applies every matching descriptor there), after the runtime has filled its function
// table and before the first guest instruction.
void bindModule(PS2Runtime &runtime)
{
    fillCalls();
    g_runtime.hot = reinterpret_cast<don_abi::DonHotState *>(&runtime.donHotState());
    g_runtime.calls = &g_calls;
    g_runtime.host = &runtime;
    don_bind_game_module(&g_runtime);
}

// No ELF name, entry or checksum: the module belongs to whatever game was loaded.
const ps2_game_overrides::Descriptor kBindDescriptor{"game module bind", "", 0u, 0u, bindModule};
const ps2_game_overrides::AutoRegister kBindRegistration(kBindDescriptor);
} // namespace
