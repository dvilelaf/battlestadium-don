/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Public C API of libdon_spu2, the PCSX2 SPU2 (v2.8.2) behind a C interface. Usable from C and C++.
 *
 * Threading: every call must come from one thread (the one that called don_spu2_open), with one
 * exception: don_spu2_pull_output may be called from a single other thread (for example an audio
 * callback) while the first one is calling the other functions. There is one instance per process.
 *
 * Time: the SPU2 has no clock of its own. Time advances only through don_spu2_advance, in IOP
 * cycles (the IOP runs at 36,864,000 Hz; the SPU2 produces one stereo frame of 48 kHz audio every
 * 768 cycles). Register writes, DMA and reads happen at the current time. Host time is never used.
 *
 * Audio: the library never opens an audio device. The frames produced are kept in a buffer of
 * about 5.4 seconds and are taken with don_spu2_pull_output. If the buffer is full the newest
 * frames are dropped.
 *
 * Errors: functions that can fail return a negative value. Errors are written to stderr with the
 * prefix "[don-spu2]". PCSX2's own console output does not exist in this library.
 *
 * Safety: before don_spu2_open and after don_spu2_close every function does nothing and returns 0
 * (or a negative value where the function can fail); none crashes. don_spu2_open works again
 * after don_spu2_close in the same process and gives a clean power-on state.
 */
#ifndef DON_SPU2_H
#define DON_SPU2_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(DON_SPU2_BUILD)
#define DON_SPU2_API __declspec(dllexport)
#else
#define DON_SPU2_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define DON_SPU2_API __attribute__((visibility("default")))
#else
#define DON_SPU2_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DON_SPU2_API_VERSION 1

/* IOP clock and the SPU2 output rate. */
#define DON_SPU2_IOP_HZ 36864000u
#define DON_SPU2_SAMPLE_RATE 48000u
#define DON_SPU2_CYCLES_PER_FRAME 768u

/* First and last byte of the register window accepted by don_spu2_write16 and don_spu2_read16. */
#define DON_SPU2_REG_BASE 0x1F900000u
#define DON_SPU2_REG_END 0x1F9007FFu

/* Event types of don_spu2_poll_event (the numbers are the IOP interrupt lines' DMA channels). */
#define DON_SPU2_EVENT_IRQ 1       /* SPU2 interrupt (a voice or a DMA touched the IRQ address) */
#define DON_SPU2_EVENT_DMA_END_0 4 /* end of the DMA of core 0 (IOP DMA channel 4) */
#define DON_SPU2_EVENT_DMA_END_1 7 /* end of the DMA of core 1 (IOP DMA channel 7) */

/* The version of this API (DON_SPU2_API_VERSION). */
DON_SPU2_API int don_spu2_version(void);

/* Creates the SPU2 in its power-on state at time 0. Returns 0, or a negative value if it is
 * already open. */
DON_SPU2_API int don_spu2_open(void);

/* Destroys it. Safe to call when it is not open. */
DON_SPU2_API void don_spu2_close(void);

/* Back to the power-on state: registers, sound memory and queued events are cleared
 * and the time goes back to 0. Audio already produced and not yet pulled is kept (T-303). */
DON_SPU2_API void don_spu2_reset(void);

/* Register write at the current time. addr is the IOP address of an SPU2 register,
 * DON_SPU2_REG_BASE to DON_SPU2_REG_END; other addresses are ignored. */
DON_SPU2_API void don_spu2_write16(uint32_t addr, uint16_t value);

/* Register read at the current time. Addresses outside the window return 0. */
DON_SPU2_API uint16_t don_spu2_read16(uint32_t addr);

/* DMA from IOP memory into sound memory on core 0 or 1 (IOP DMA channel 4 or 7), as the
 * hardware does after the driver has written the transfer address (TSA) and the core attributes
 * with don_spu2_write16. data is copied before the call returns. bytes is rounded down to an even
 * number and limited to 1 MiB (the rest is ignored, with a message). The end is reported later as
 * an event, at the IOP cycle the hardware would finish it. A transfer started while the previous
 * one on the same core is still running replaces it. Returns the bytes accepted, or a negative
 * value on a bad core. */
DON_SPU2_API int32_t don_spu2_dma_write(int core, const uint8_t* data, uint32_t bytes);

/* DMA from sound memory into dst, from the core's current transfer address. Returns the bytes
 * written to dst (the same rules as don_spu2_dma_write), or a negative value on a bad core. The
 * end event is queued like in a write. */
DON_SPU2_API int32_t don_spu2_dma_read(int core, uint8_t* dst, uint32_t bytes);

/* Advances the SPU2 by iop_cycles IOP cycles, producing one stereo frame every 768 cycles
 * (the remainder is kept for the next call, so the frames produced match the cycles advanced
 * exactly) and queueing the events that happen on the way. */
DON_SPU2_API void don_spu2_advance(uint64_t iop_cycles);

/* The IOP cycle count reached so far (the sum of all don_spu2_advance calls since open/reset). */
DON_SPU2_API uint64_t don_spu2_cycle(void);

/* Takes the oldest queued event. Returns 1 and fills *type (DON_SPU2_EVENT_*) and *iop_cycle (the
 * cycle at which it happened, counted like don_spu2_cycle) if there is one; 0 if not. Either
 * pointer may be NULL. */
DON_SPU2_API int don_spu2_poll_event(int* type, uint64_t* iop_cycle);

/* Copies up to `frames` frames produced so far into stereo, interleaved left/right floats in the
 * range -1 to 1, and returns how many it copied (less than `frames` if fewer were available).
 * Never blocks and never pads: the caller decides what to do about a shortfall. */
DON_SPU2_API uint32_t don_spu2_pull_output(float* stereo, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif
