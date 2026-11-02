/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * libdon_vu: the PS2's VU1 (vector unit 1) as a library, built from PCSX2's microVU (the x86-64
 * recompiler for VU microcode) and PCSX2's VU interpreter, v2.8.2. Only C types cross this
 * interface; no PCSX2 header is needed to use it.
 *
 * Model:
 *   - One VU1 per process. Every call must be made from the same thread (the one that calls
 *     don_vu1_open); there is no locking inside.
 *   - The library owns the two 16 KiB memories of the VU1. The caller reads and writes data memory
 *     directly through don_vu1_data() (this is where the VIF unpacks vertex data) and loads
 *     microcode with don_vu1_write_micro().
 *   - A microprogram is started with don_vu1_execute (what MSCAL does) and runs to its end (the E
 *     bit, plus its delay slot) or until the cycle budget is used up. A program stopped by the
 *     budget is continued with don_vu1_continue. When the program executes XGKICK the callback
 *     receives the GIF packet that PATH1 would send.
 *   - Register layout: "vf" is 32 registers x 4 uint32 (x, y, z, w; 512 bytes), "vi" is 32 uint32 in
 *     the order of the VU's control register space (the one PCSX2 and the VIF use):
 *        0..15  integer registers VI00..VI15 (16 bits each; VI00 is always 0)
 *        16     status flag      17  MAC flag     18  clip flag
 *        20     R                21  I            22  Q          23  P
 *        26     TPC (program counter, in 8-byte instruction pairs, not bytes)
 *        28     FBRST            29  VPU_STAT (bit 8: VU1 busy)
 *     The other entries are not used by VU1 programs.
 *
 * Floating point: the library switches the calling thread's SSE control register (MXCSR) to the
 * VU's own (round toward zero, flush-to-zero and denormals-are-zero on, exceptions masked) for the
 * duration of don_vu1_execute / don_vu1_continue, and restores the caller's value on return. The
 * XGKICK callback runs with the caller's value. Nothing else about the caller's floating point
 * state is touched.
 *
 * Both engines are PCSX2's and produce the PS2's results: results that overflow saturate to the
 * largest finite value with the sign kept (never infinity), denormals are flushed to zero.
 *
 * x86-64 only.
 */
#ifndef DON_VU_H
#define DON_VU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define DON_VU_API __declspec(dllexport)
#else
#define DON_VU_API __attribute__((visibility("default")))
#endif

#define DON_VU_VERSION 1

#define DON_VU_ENGINE_MICROVU 0     /* PCSX2 microVU: recompiles each microprogram to x86-64 */
#define DON_VU_ENGINE_INTERPRETER 1 /* PCSX2 VU interpreter: slow, the reference for the other */

#define DON_VU_OPT_CLAMP_MODE 0 /* 0 none, 1 overflow (default), 2 extra, 3 extra and preserve sign */
#define DON_VU_OPT_FLAG_HACK 1  /* 1 (default): microVU skips flag computations nothing reads */
#define DON_VU_OPT_IBIT_HACK 2  /* 0 (default); see PCSX2 "Gamefixes.IbitHack" */
#define DON_VU_OPT_XGKICK_HACK 3 /* 0 (default); microVU: send XGKICK data in pieces */

/*
 * Called for every GIF packet that the program sends with XGKICK: a sequence of GIF tags and their
 * data, up to and including the tag that has EOP set. `packet` is one contiguous copy (a packet
 * that wraps around the end of VU data memory is delivered already joined) and is valid only
 * during the call. `bytes` is a multiple of 16. The callback runs on the calling thread, inside
 * don_vu1_execute / don_vu1_continue, with the caller's floating point control register.
 */
typedef void (*DonVuXgkick)(void* user, const uint8_t* packet, uint32_t bytes);

/* Returns DON_VU_VERSION. */
DON_VU_API int don_vu_version(void);

/*
 * Creates the VU1 with the microVU engine; same as don_vu1_open_ex(DON_VU_ENGINE_MICROVU, ...).
 * `cb` may be NULL (XGKICK packets are dropped). Returns 0 on success, nonzero on failure (for
 * example the executable memory for the recompiler could not be mapped, or a VU1 is already open;
 * a message beginning with "[don-vu]" is printed to stderr).
 * All registers are zero, both memories are zero.
 */
DON_VU_API int don_vu1_open(DonVuXgkick cb, void* user);

/* As don_vu1_open, choosing the engine (DON_VU_ENGINE_*). */
DON_VU_API int don_vu1_open_ex(int engine, DonVuXgkick cb, void* user);

/* Destroys the VU1 and frees its executable memory. Safe to call when not open. */
DON_VU_API void don_vu1_close(void);

/*
 * Resets the registers, the pipeline state and the compiled-code cache. The contents of both
 * memories are kept. (What a VIF1 FBRST reset does, plus forgetting the compiled programs.)
 */
DON_VU_API void don_vu1_reset(void);

/*
 * Selects one of the DON_VU_OPT_* settings. Changing a setting that affects the generated code
 * discards the compiled programs; do it between programs, not while one is stopped by its budget.
 * Returns 0, or nonzero for an unknown option or a value out of range.
 */
DON_VU_API int don_vu1_set_option(int option, int value);

/* Engine given at open time, or -1 when not open. */
DON_VU_API int don_vu1_engine(void);

/*
 * The 16 KiB of VU1 data memory (1024 qwords), 64-byte aligned. The pointer stays valid until
 * don_vu1_close. The caller reads and writes it freely between programs. Do not write to it while
 * a program is running (that is, from inside the XGKICK callback only read it).
 */
DON_VU_API uint8_t* don_vu1_data(void);

/*
 * Copies `bytes` of microcode to code memory at byte offset `addr_bytes` (wrapping at 16 KiB) and
 * tells the recompiler that code memory changed, so that programs compiled from the old contents
 * are not reused. Writing identical bytes is cheap. An instruction is 4 bytes, a pair (upper and
 * lower) 8 bytes, the upper instruction first.
 */
DON_VU_API void don_vu1_write_micro(uint32_t addr_bytes, const uint8_t* data, uint32_t bytes);

/*
 * The 16 KiB of VU1 code memory, for reading back. Writes must go through don_vu1_write_micro.
 */
DON_VU_API const uint8_t* don_vu1_micro(void);

/*
 * Sets the registers. `vf` (32 x 4 uint32, VF00 is ignored) and `vi` (32 uint32, see the layout
 * above) may each be NULL to leave that part unchanged. The flags (entries 16, 17, 18) are set so
 * that the next program sees them as its initial flags.
 */
DON_VU_API void don_vu1_set_regs(const void* vf, const void* vi);

/*
 * Reads the registers back, same layout as don_vu1_set_regs; either pointer may be NULL.
 * After a program has ended, the integer registers, VF registers, Q, P, I, R and TPC are exact
 * for both engines. Status, MAC and clip flags are exact for the interpreter; microVU computes
 * flags lazily (see DON_VU_OPT_FLAG_HACK), so a flag that no instruction of the program read may
 * differ.
 */
DON_VU_API void don_vu1_get_regs(void* vf, void* vi);

/* ACC, the multiply-accumulate register (4 uint32). */
DON_VU_API void don_vu1_get_acc(uint32_t acc[4]);
DON_VU_API void don_vu1_set_acc(const uint32_t acc[4]);

/*
 * Sets the values that the instructions XTOP and XITOP read: the VIF1 TOP and ITOP registers
 * (double-buffer base and the unpack address of the last MSCAL, 10 bits each).
 */
DON_VU_API void don_vu1_set_top(uint32_t top, uint32_t itop);

/*
 * Starts a microprogram at byte address `start_pc_bytes` of code memory (what MSCAL does; a
 * multiple of 8; 0xFFFFFFFF means "continue at the current TPC", what MSCNT does) and runs it
 * until the E bit takes effect or at least `max_cycles` VU cycles have been used (the recompiled
 * engine only checks the budget at the end of a block, so it may overshoot by a block).
 * Returns the cycles used, according to PCSX2's timing model. If a previous program is still
 * stopped by its budget, that one is first run to its end (as the VIF would wait for it) and its
 * cycles are included.
 * Afterwards don_vu1_running() tells whether the program ended (0) or was stopped by the budget (1).
 */
DON_VU_API uint32_t don_vu1_execute(uint32_t start_pc_bytes, uint32_t max_cycles);

/*
 * Continues a program that don_vu1_execute (or a previous continue) stopped because of the cycle
 * budget, with its pipeline state intact; runs for at least `max_cycles` more cycles or to its end.
 * Returns the cycles used (0 if no program is running). Running a program in slices gives the same
 * final state as running it in one call.
 */
DON_VU_API uint32_t don_vu1_continue(uint32_t max_cycles);

/* Nonzero while a program is running (stopped only by its budget): VPU_STAT bit 8. */
DON_VU_API int don_vu1_running(void);

/* Total VU cycles run since open and number of XGKICK packets delivered since open. */
DON_VU_API uint64_t don_vu1_total_cycles(void);
DON_VU_API uint64_t don_vu1_xgkick_count(void);

#ifdef __cplusplus
}
#endif

#endif /* DON_VU_H */
