// don_guest_abi.h: everything the generated guest code needs from the runtime, as plain data.
// Origin: T-601/T-605 drafts (work/t605/abi/), moved here by T-610 and brought to patch 0087 (DonHotState).
//
// Includes only <stdint.h>, <stddef.h> and <immintrin.h> (clang's resource directory), so the generated code
// compiles freestanding. The third argument of every guest function ("PS2Runtime *runtime" in generated code) is
// the runtime's own PS2Runtime*, opaque to the module: nothing here dereferences it. The runtime fills one
// DonRuntime (src/host/don_runtime_bridge.cpp) and the loader stores its address in the module's one global slot
// `don_rt`; the DonRuntime methods generated code calls read hot state and call through that slot and pass `this`
// (the opaque real runtime) on. Nothing here throws, allocates, or needs a C++ library.
// DON_ABI_NAMESPACE puts the whole header in a namespace, so the host can include it next to the real R5900Context.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <immintrin.h>

#define DON_ABI_VERSION 1u

#ifdef DON_ABI_NAMESPACE
namespace DON_ABI_NAMESPACE
{
#endif
#include "don_host_fns.h"

// R5900Context: field for field the runtime's struct (ps2_runtime.h:67-170) without constructor or dump().
struct alignas(16) R5900Context
{
    __m128i r[32];

    uint32_t pc;
    uint64_t insn_count;
    uint64_t hi, lo;
    uint64_t hi1, lo1;
    uint32_t sa;

    __m128 vu0_vf[32];
    uint16_t vi[16];
    float vu0_q;
    float vu0_p;
    float vu0_i;
    __m128 vu0_r;
    __m128 vu0_acc;
    uint16_t vu0_status;
    uint32_t vu0_mac_flags;
    uint32_t vu0_clip_flags;
    uint32_t vu0_clip_flags2;
    uint32_t vu0_cmsar0;
    uint32_t vu0_cmsar1;
    uint32_t vu0_cmsar2;
    uint32_t vu0_cmsar3;
    uint32_t vu0_vpu_stat;
    uint32_t vu0_vpu_stat2;
    uint32_t vu0_vpu_stat3;
    uint32_t vu0_vpu_stat4;
    uint32_t vu0_tpc;
    uint32_t vu0_tpc2;
    uint32_t vu0_fbrst;
    uint32_t vu0_fbrst2;
    uint32_t vu0_fbrst3;
    uint32_t vu0_fbrst4;
    uint32_t vu0_itop;
    uint32_t vu0_top;
    uint32_t vu0_info;
    uint32_t vu0_xitop;
    uint32_t vu0_pc;

    float vu0_cf[4];

    uint32_t cop0_index;
    uint32_t cop0_random;
    uint32_t cop0_entrylo0;
    uint32_t cop0_entrylo1;
    uint32_t cop0_context;
    uint32_t cop0_pagemask;
    uint32_t cop0_wired;
    uint32_t cop0_badvaddr;
    uint32_t cop0_count;
    uint32_t cop0_entryhi;
    uint32_t cop0_compare;
    uint32_t cop0_status;
    uint32_t cop0_cause;
    uint32_t cop0_epc;
    uint32_t cop0_prid;
    uint32_t cop0_config;
    uint32_t cop0_badpaddr;
    uint32_t cop0_debug;
    uint32_t cop0_perf;
    uint32_t cop0_taglo;
    uint32_t cop0_taghi;
    uint32_t cop0_errorepc;

    uint32_t llbit;
    uint32_t lladdr;

    bool in_delay_slot;
    uint32_t branch_pc;

    uint32_t cop2_ccr[32];

    float f[32];
    float f_acc;
    uint32_t fcr31;
};

struct DonRuntime;
// The module's one global: set once by the loader (through DonGameModuleEntry::bind) before any guest function
// runs. Hidden, zero-initialised, no constructor. Defined in module_entry.cpp.
extern const DonRuntime *don_rt __attribute__((visibility("hidden")));
using DonGuestFn = void (*)(uint8_t *rdram, R5900Context *ctx, DonRuntime *runtime);

// PS2Exception values generated code names (ps2_runtime.h:51-60).
enum PS2Exception : int
{
    EXCEPTION_TLB_REFILL = 0x02,
    EXCEPTION_ADDRESS_ERROR_LOAD = 0x04,
    EXCEPTION_ADDRESS_ERROR_STORE = 0x05,
    EXCEPTION_SYSCALL = 0x08,
    EXCEPTION_BREAKPOINT = 0x09,
    EXCEPTION_RESERVED_INSTRUCTION = 0x0A,
    EXCEPTION_INTEGER_OVERFLOW = 0x0C,
    EXCEPTION_TRAP = 0x0D,
};

enum class DonBranchKind : uint32_t
{
    DirectJump,
    DirectCall,
    IndirectJump,
    IndirectCall,
    Return,
};

// DonHotState: field for field the runtime's (ps2xRuntime/include/runtime/don_hot_state.h, patch 0087; the bridge
// static_asserts the offsets). What dispatchGuestBranch (ps2_dispatch_inline.h) and eeCheckpointDue
// (EeScheduler::checkpointDue + accountCycles + PS2Memory::advanceEeTimers) read and write on the common path.
// Everything else (flushIopDebt, checkpointDueSlow, applyEeTimers) goes through DonRuntimeCalls.
struct DonHotState
{
    // EeScheduler (ee_scheduler.h:496-526,570-580)
    uint64_t ee_cycle;               // m_eeCycle
    uint64_t next_fast_path_cycle;   // m_nextFastPathCycle
    uint64_t iop_pending_ee_cycles;  // m_iopPendingEeCycles
    uint64_t iop_batch_ee_cycles;    // m_iopBatchEeCycles
    uint64_t dispatch_count;         // m_dispatchCount
    uint64_t fast_path_count;        // m_fastPathCount
    uint32_t pending_timer_irqs;     // m_pendingEeTimerInterrupts
    // PS2Memory (ps2_memory.h:502-504)
    uint32_t timer_irq_mask;         // m_pendingEeTimerInterruptMask
    uint64_t timer_pending_cycles;   // m_eeTimerPendingCycles
    uint64_t timer_cycles_to_event;  // m_eeTimerCyclesToEvent
    // Flags (std::atomic<bool>, one byte, lock free, in the runtime). checkpoint_pending and stop_requested are
    // written by other threads: read them with __atomic_load_n(..., __ATOMIC_RELAXED), write with __ATOMIC_RELEASE.
    uint8_t checkpoint_pending;      // m_checkpointPending
    uint8_t unwinding;               // m_unwinding (patch 0054)
    uint8_t stop_requested;          // PS2Runtime::m_stopRequested
    uint8_t history_enabled;         // g_ps2DispatchHistoryEnabled
    // Function table of the guest code (g_ps2RecompiledFunctionTable*)
    uint32_t fn_table_base;
    uint32_t fn_table_end;
    uint32_t fn_table_slots;
    uint32_t reserved0;
    const DonGuestFn *fn_table;
};

static constexpr uint32_t DON_GUEST_DISPATCH_CYCLES = 8u;      // EeScheduler::kGuestDispatchCycles
static constexpr uint32_t DON_GENERATED_CHECKPOINT_CYCLES = 32u; // EeScheduler::kGeneratedCheckpointCycles

// DonRuntimeCalls: one C function pointer per runtime call that generated code makes (deps.md section 2),
// plus the host functions it names directly (ps2_syscalls::X, ps2_stubs::X: 172 targets, indexed by DonHostFnId).
// Every function receives the DonRuntime so the host finds its PS2Runtime in `host`.
using DonHostFn = void (*)(uint8_t *rdram, R5900Context *ctx, DonRuntime *runtime);

struct DonRuntimeCalls
{
    uint32_t abi_version;
    uint32_t size;  // sizeof(DonRuntimeCalls), for a sanity check by the loader

    // Memory slow paths (special addresses): Load8..128 / Store8..128 (ps2_runtime.h:483-493).
    uint8_t (*load8)(DonRuntime *, uint8_t *rdram, R5900Context *, uint32_t vaddr);
    uint16_t (*load16)(DonRuntime *, uint8_t *rdram, R5900Context *, uint32_t vaddr);
    uint32_t (*load32)(DonRuntime *, uint8_t *rdram, R5900Context *, uint32_t vaddr);
    uint64_t (*load64)(DonRuntime *, uint8_t *rdram, R5900Context *, uint32_t vaddr);
    __m128i (*load128)(DonRuntime *, uint8_t *rdram, R5900Context *, uint32_t vaddr);
    void (*store8)(DonRuntime *, uint8_t *rdram, R5900Context *, uint32_t vaddr, uint8_t value);
    void (*store16)(DonRuntime *, uint8_t *rdram, R5900Context *, uint32_t vaddr, uint16_t value);
    void (*store32)(DonRuntime *, uint8_t *rdram, R5900Context *, uint32_t vaddr, uint32_t value);
    void (*store64)(DonRuntime *, uint8_t *rdram, R5900Context *, uint32_t vaddr, uint64_t value);
    void (*store128)(DonRuntime *, uint8_t *rdram, R5900Context *, uint32_t vaddr, __m128i value);

    // Exceptions and traps.
    void (*handle_syscall)(DonRuntime *, uint8_t *rdram, R5900Context *, uint32_t encoded_syscall_id);
    void (*handle_break)(DonRuntime *, uint8_t *rdram, R5900Context *);
    void (*signal_exception)(DonRuntime *, R5900Context *, int exception);
    void (*clear_ll_bit)(DonRuntime *, R5900Context *);
    void (*vu0_start_microprogram)(DonRuntime *, uint8_t *rdram, R5900Context *, uint32_t address);

    // Scheduler slow paths behind DonHotState.
    bool (*checkpoint_due_slow)(DonRuntime *);          // EeScheduler::checkpointDueSlow
    void (*flush_iop_debt)(DonRuntime *);               // EeScheduler::flushIopDebt
    uint32_t (*apply_ee_timers)(DonRuntime *);          // PS2Memory::applyEeTimers (returns the irq mask)
    void (*push_dispatch_pc)(DonRuntime *, uint32_t pc); // ps2PushDispatchPc
    bool (*dispatch_branch_tail)(DonRuntime *, uint8_t *rdram, R5900Context *, uint32_t target_pc,
                                 uint32_t source_pc, uint32_t fallthrough_pc, DonBranchKind kind,
                                 const char *debug_name); // PS2Runtime::dispatchGuestBranchTail

    // ps2_syscalls::X and ps2_stubs::X, indexed by DonHostFnId (don_host_fns.h).
    DonHostFn host_fn[DON_HF_COUNT];
};

// What the host fills (once, in src/host/don_runtime_bridge.cpp). Generated code's `PS2Runtime` is this type, but a
// pointer of this type is never a real DonRuntime inside the module: it is the runtime's own PS2Runtime*, used only
// as the `this` of the methods below, which reach the real DonRuntime through the module's one slot.
struct DonRuntime
{
    DonHotState *hot;
    const DonRuntimeCalls *calls;
    void *host;  // the runtime's own PS2Runtime*

    using GuestBranchKind = DonBranchKind;
    using RecompiledFunction = DonGuestFn;

    static constexpr bool isSpecialAddress(uint32_t addr);

    // ---- member calls of deps.md section 2 --------------------------------------------------------------
    inline bool eeCheckpointDue(uint32_t cycles = DON_GENERATED_CHECKPOINT_CYCLES);
    inline bool dispatchGuestBranch(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc,
                                    uint32_t fallthroughPc, DonBranchKind kind, const char *debugName);

    inline uint8_t Load8(uint8_t *rdram, R5900Context *ctx, uint32_t a) { return don_rt->calls->load8(this, rdram, ctx, a); }
    inline uint16_t Load16(uint8_t *rdram, R5900Context *ctx, uint32_t a) { return don_rt->calls->load16(this, rdram, ctx, a); }
    inline uint32_t Load32(uint8_t *rdram, R5900Context *ctx, uint32_t a) { return don_rt->calls->load32(this, rdram, ctx, a); }
    inline uint64_t Load64(uint8_t *rdram, R5900Context *ctx, uint32_t a) { return don_rt->calls->load64(this, rdram, ctx, a); }
    inline __m128i Load128(uint8_t *rdram, R5900Context *ctx, uint32_t a) { return don_rt->calls->load128(this, rdram, ctx, a); }
    inline void Store8(uint8_t *rdram, R5900Context *ctx, uint32_t a, uint8_t v) { don_rt->calls->store8(this, rdram, ctx, a, v); }
    inline void Store16(uint8_t *rdram, R5900Context *ctx, uint32_t a, uint16_t v) { don_rt->calls->store16(this, rdram, ctx, a, v); }
    inline void Store32(uint8_t *rdram, R5900Context *ctx, uint32_t a, uint32_t v) { don_rt->calls->store32(this, rdram, ctx, a, v); }
    inline void Store64(uint8_t *rdram, R5900Context *ctx, uint32_t a, uint64_t v) { don_rt->calls->store64(this, rdram, ctx, a, v); }
    inline void Store128(uint8_t *rdram, R5900Context *ctx, uint32_t a, __m128i v) { don_rt->calls->store128(this, rdram, ctx, a, v); }

    inline void handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t id) { don_rt->calls->handle_syscall(this, rdram, ctx, id); }
    inline void handleBreak(uint8_t *rdram, R5900Context *ctx) { don_rt->calls->handle_break(this, rdram, ctx); }
    inline void SignalException(R5900Context *ctx, PS2Exception e) { don_rt->calls->signal_exception(this, ctx, (int)e); }
    inline void clearLLBit(R5900Context *ctx) { don_rt->calls->clear_ll_bit(this, ctx); }
    inline void vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
    {
        don_rt->calls->vu0_start_microprogram(this, rdram, ctx, address);
    }
};

// Memory accessors on rdram (ps2_runtime_macros.h:150-312, runtime/ps2_memory.h:27-28, ps2_address.h).
static constexpr uint32_t DON_RAM_SIZE = 32u * 1024u * 1024u;
static constexpr uint32_t DON_RAM_MASK = DON_RAM_SIZE - 1u;

static inline bool DonRangeContiguous(uint32_t offset, uint32_t bytes) { return offset <= (DON_RAM_SIZE - bytes); }

static inline uint8_t DonRead8(const uint8_t *rdram, uint32_t addr) { return rdram[addr & DON_RAM_MASK]; }
static inline void DonWrite8(uint8_t *rdram, uint32_t addr, uint8_t v) { rdram[addr & DON_RAM_MASK] = v; }

// Reads/writes of N bytes with the same wrap-around the runtime's Ps2FastRead/Write have. N is 2, 4, 8 or 16.
template <typename T>
static inline T DonReadN(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & DON_RAM_MASK;
    alignas(16) uint8_t bytes[sizeof(T)];
    if (!DonRangeContiguous(offset, sizeof(T)))
    {
        for (uint32_t i = 0; i < sizeof(T); ++i)
            bytes[i] = rdram[(offset + i) & DON_RAM_MASK];
    }
    else
    {
        __builtin_memcpy(bytes, rdram + offset, sizeof(T));
    }
    T value;
    __builtin_memcpy(&value, bytes, sizeof(T));
    return value;
}

template <typename T>
static inline void DonWriteN(uint8_t *rdram, uint32_t addr, T value)
{
    const uint32_t offset = addr & DON_RAM_MASK;
    if (!DonRangeContiguous(offset, sizeof(T)))
    {
        alignas(16) uint8_t bytes[sizeof(T)];
        __builtin_memcpy(bytes, &value, sizeof(T));
        for (uint32_t i = 0; i < sizeof(T); ++i)
            rdram[(offset + i) & DON_RAM_MASK] = bytes[i];
        return;
    }
    __builtin_memcpy(rdram + offset, &value, sizeof(T));
}

// Special-address test (ps2_address.h; constants from ps2_memory.h, copied by the layout_check).
static constexpr uint32_t DON_KSEG0_BASE = 0x80000000u;
static constexpr uint32_t DON_KSEG0_KSEG1_SIZE = 0x40000000u;
static constexpr uint32_t DON_KSEG2_BASE = 0xC0000000u;
static constexpr uint32_t DON_BIOS_BASE = 0x1FC00000u;
static constexpr uint32_t DON_BIOS_SIZE = 0x00400000u;
static constexpr uint32_t DON_SCRATCHPAD_BASE = 0x70000000u;
static constexpr uint32_t DON_SCRATCHPAD_SIZE = 0x00004000u;
static constexpr uint32_t DON_IO_BASE = 0x10000000u;
static constexpr uint32_t DON_IO_SIZE = 0x00010000u;
static constexpr uint32_t DON_GS_PRIV_BASE = 0x12000000u;
static constexpr uint32_t DON_GS_PRIV_SIZE = 0x00002000u;
static constexpr uint32_t DON_VU0_DATA_BASE = 0x11004000u;
static constexpr uint32_t DON_VU1_CODE_END = 0x11008000u + 0x4000u;  // VU1 code base + size (checked by layout_check)

static inline constexpr bool DonInRange(uint32_t v, uint32_t base, uint32_t size) { return (v - base) < size; }

static inline constexpr bool DonIsSpecialAddress(uint32_t addr)
{
    if (addr >= DON_KSEG2_BASE)
        return true;
    const uint32_t phys = (addr >= DON_KSEG0_BASE) ? (addr & 0x1FFFFFFFu) : addr;
    return DonInRange(phys, DON_BIOS_BASE, DON_BIOS_SIZE) || DonInRange(phys, DON_SCRATCHPAD_BASE, DON_SCRATCHPAD_SIZE) ||
           DonInRange(phys, DON_IO_BASE, DON_IO_SIZE) || DonInRange(phys, DON_GS_PRIV_BASE, DON_GS_PRIV_SIZE) ||
           (phys >= DON_VU0_DATA_BASE && phys < DON_VU1_CODE_END);
}

constexpr bool DonRuntime::isSpecialAddress(uint32_t addr) { return DonIsSpecialAddress(addr); }

// Hot-path functions (ee_scheduler.h:553-578, ps2_dispatch_inline.h). Same statements, same order.
// Deliberately not noexcept: the real eeCheckpointDue is, but a noexcept frame calling through a function pointer
// makes clang emit a terminate landing pad (__cxa_begin_catch, personality): imports the module must not have.
static inline bool DonCheckpointDue(DonRuntime *rt, uint32_t cycles)
{
    DonHotState *h = don_rt->hot;
    // accountCycles
    const uint64_t elapsed = cycles > 1u ? cycles : 1u;
    h->ee_cycle += elapsed;
    {
        // PS2Memory::advanceEeTimers -> creditEeTimers
        uint32_t pending = 0u;
        h->timer_pending_cycles += elapsed;
        if (h->timer_cycles_to_event > elapsed)
        {
            h->timer_cycles_to_event -= elapsed;
        }
        else
        {
            h->timer_cycles_to_event = 0u;
            h->timer_irq_mask |= don_rt->calls->apply_ee_timers(rt);
        }
        pending = h->timer_irq_mask;
        h->timer_irq_mask = 0u;
        h->pending_timer_irqs |= pending;
    }
    h->iop_pending_ee_cycles += elapsed;
    if (h->iop_pending_ee_cycles >= h->iop_batch_ee_cycles)
        don_rt->calls->flush_iop_debt(rt);
    if (h->pending_timer_irqs != 0u)
        __atomic_store_n(&h->checkpoint_pending, (uint8_t)1, __ATOMIC_RELEASE);
    // checkpointDue
    ++h->dispatch_count;
    if (!__atomic_load_n(&h->checkpoint_pending, __ATOMIC_RELAXED) && h->ee_cycle < h->next_fast_path_cycle)
    {
        ++h->fast_path_count;
        return false;
    }
    return don_rt->calls->checkpoint_due_slow(rt);
}

inline bool DonRuntime::eeCheckpointDue(uint32_t cycles) { return DonCheckpointDue(this, cycles); }

inline bool DonRuntime::dispatchGuestBranch(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc,
                                            uint32_t fallthroughPc, DonBranchKind kind, const char *debugName)
{
    ctx->pc = targetPc;
    if (DonCheckpointDue(this, DON_GUEST_DISPATCH_CYCLES))
        return false;

    DonHotState *h = don_rt->hot;
    if ((kind == DonBranchKind::DirectCall || kind == DonBranchKind::IndirectCall) && (targetPc & 3u) == 0u &&
        targetPc >= h->fn_table_base && targetPc < h->fn_table_end &&
        ((targetPc - h->fn_table_base) >> 2) < h->fn_table_slots)
    {
        const DonGuestFn fn = h->fn_table[(targetPc - h->fn_table_base) >> 2];
        if (fn != nullptr)
        {
            if (h->history_enabled)
                don_rt->calls->push_dispatch_pc(this, targetPc);

            fn(rdram, ctx, this);

            if (__atomic_load_n(&h->stop_requested, __ATOMIC_RELAXED) || ctx->pc == 0u)
                return false;
            if (h->unwinding)
                return false;
            if (ctx->pc == targetPc)
                ctx->pc = fallthroughPc;
            return ctx->pc == fallthroughPc;
        }
    }
    return don_rt->calls->dispatch_branch_tail(this, rdram, ctx, targetPc, sourcePc, fallthroughPc, kind, debugName);
}

#ifdef DON_ABI_NAMESPACE
} // namespace DON_ABI_NAMESPACE
#endif
