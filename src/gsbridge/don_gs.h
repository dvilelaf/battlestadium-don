/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Public C API of libdon_gs, the PCSX2 GS (v2.8.2) behind a C interface. Usable from C and C++.
 *
 * Threading: every call, from don_gs_open to don_gs_close, must come from the one thread that
 * called don_gs_open. The library starts no thread of its own for the GS: GIF data is processed
 * synchronously inside don_gs_gif_transfer and don_gs_vsync. (The software renderer may use
 * worker threads internally to rasterize; they are not visible to the caller.)
 *
 * Safety: every function may be called in any order. Before don_gs_open and after don_gs_close
 * they return an error or do nothing, and never crash. don_gs_open works again after
 * don_gs_close in the same process.
 *
 * Errors: functions that can fail return a negative value (or 0 bytes) and, when they have an
 * `err` buffer, fill it with a message. Every error is also written to stderr with the prefix
 * "[don-gs]". PCSX2's own console output is written to stderr only when the environment variable
 * DON_GS_LOG=1 is set.
 */
#ifndef DON_GS_H
#define DON_GS_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(DON_GS_BUILD)
#define DON_GS_API __declspec(dllexport)
#else
#define DON_GS_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define DON_GS_API __attribute__((visibility("default")))
#else
#define DON_GS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Renderer values for DonGsConfig.renderer. */
#define DON_GS_RENDERER_VULKAN 0   /* hardware renderer on Vulkan */
#define DON_GS_RENDERER_SOFTWARE 1 /* PCSX2's software rasterizer (still presents through Vulkan) */

/* Values for DonGsConfig.window_type. */
#define DON_GS_WINDOW_NONE 0
#define DON_GS_WINDOW_X11 1
#define DON_GS_WINDOW_WAYLAND 2
#define DON_GS_WINDOW_WIN32 3

/* Number of 64-bit values in the privileged-register array of don_gs_vsync. */
#define DON_GS_PRIV_REG_COUNT 19

typedef struct DonGsConfig {
    int   renderer;          /* DON_GS_RENDERER_*; 0 = Vulkan, 1 = software */
    int   upscale;           /* internal resolution multiplier, 1 = native, valid 1..8 (hardware renderer
                                only; the software renderer always runs at native resolution) */
    int   vsync;             /* 1 = present on the display's vertical sync (only with a window) */
    void* native_display;    /* Display* (X11), wl_display* (Wayland), NULL on Windows or headless */
    void* native_window;     /* Window (X11), wl_surface*, HWND; NULL = headless */
    int   window_type;       /* DON_GS_WINDOW_*: 0 = none, 1 = X11, 2 = Wayland, 3 = Win32 */
    int   width, height;     /* size in pixels of the window surface; > 0 when there is a window,
                                ignored when headless */
} DonGsConfig;

/* Version of this API. Returns 1. */
DON_GS_API int don_gs_version(void);

/* Opens the GS with PCSX2's default configuration for this game, except for the fields of `cfg`
 * (NULL = all zero: Vulkan, native resolution, no vsync, headless). The game has no entry in
 * PCSX2's GameIndex.yaml with graphics fixes, so no per-game hack is applied.
 * Returns 0 on success; on failure returns a negative value and writes a NUL-terminated message
 * to `err` (if not NULL and err_len > 0, truncated to err_len bytes). Returns an error if the GS
 * is already open. */
DON_GS_API int don_gs_open(const DonGsConfig* cfg, char* err, int err_len);

/* Closes the GS and releases every resource. Does nothing if it is not open. */
DON_GS_API void don_gs_close(void);

/* Soft reset, as a write of CSR.RESET: clears the GS's drawing state and parser, keeps the 4 MiB
 * of local memory and the privileged registers. */
DON_GS_API void don_gs_reset(void);

/* Sends GIF data to the GS and processes it before returning.
 *   path:  GIF path 1..3. The data is the packet that came out of the GIF's arbitration; the GS
 *          takes all three paths through one parser, so the path is only validated and logged.
 *   data:  `bytes` bytes of GIF packets (tags and payloads), 16-byte aligned or not; copied if
 *          the GS needs to keep it, so it may be reused on return.
 *   bytes: a multiple of 16 (one quadword each); 0 does nothing.
 * Invalid arguments are logged and the call does nothing. */
DON_GS_API void don_gs_gif_transfer(int path, const uint8_t* data, uint32_t bytes);

/* Marks the end of a display field: hands the GS the privileged registers and lets it compose
 * the image of the CRT controller (and present it when there is a window).
 *   priv_regs: 19 values, in this order: PMODE, SMODE1, SMODE2, SRFSH, SYNCH1, SYNCH2, SYNCV,
 *              DISPFB1, DISPLAY1, DISPFB2, DISPLAY2, EXTBUF, EXTDATA, EXTWRITE, BGCOLOR, CSR,
 *              IMR, BUSDIR, SIGLBLID. They overwrite the GS's copy, CSR and SIGLBLID included.
 *   field:     0 or 1, the value of CSR.FIELD for this vertical sync (it overrides bit 13 of the
 *              CSR value in priv_regs). Alternate it on every vsync of an interlaced picture. */
DON_GS_API void don_gs_vsync(const uint64_t priv_regs[19], int field);

/* Reads the CSR and SIGLBLID values the GS holds: those passed to the last don_gs_vsync (zero
 * before the first). PCSX2's GS does not interpret the SIGNAL, FINISH and LABEL registers (its
 * GIF unit does, which is not part of this library): the caller must track them. Outputs may
 * be NULL. */
DON_GS_API void don_gs_read_signals(uint64_t* csr, uint64_t* siglblid);

/* GS to CPU transfer (TRXDIR = 1, set up by a previous don_gs_gif_transfer): reads `bytes` bytes
 * of the image selected by BITBLTBUF/TRXPOS/TRXREG into `dst`, which must hold `bytes` bytes.
 * `bytes` must be a non-zero multiple of 16. Repeated calls continue the same transfer.
 * Returns the number of bytes written to `dst`; 0 on error (no read transfer active, bad
 * arguments, GS not open). */
DON_GS_API uint32_t don_gs_read_fifo(uint8_t* dst, uint32_t bytes);

/* Replaces the GS state with a saved one: `data` is the "state" block of a PCSX2 GS dump (.gs),
 * the bytes that PCSX2 hands to GSfreeze(Load): a 32-bit version number (PCSX2 v2.8.2 writes
 * 9, accepts up to 9) followed by the drawing registers, the 4 MiB of local memory and the
 * transfer state. It does NOT contain the 0x2000 bytes of privileged registers, which a dump
 * stores in a separate block: pass those through don_gs_vsync. Returns 0 on success, negative
 * if `data` is too short, the version is unknown or the GS is not open. */
DON_GS_API int don_gs_load_state(const uint8_t* data, uint32_t bytes);

/* Counterpart of don_gs_load_state (an addition to the card's API, used to test it): writes the
 * GS state in the same format into `dst`. With dst == NULL returns the size needed in bytes.
 * Otherwise returns the bytes written, or a negative value on error (GS not open, or
 * max_bytes smaller than the size needed). */
DON_GS_API int don_gs_save_state(uint8_t* dst, int max_bytes);

/* Copies the picture the GS composed at the last don_gs_vsync, at the GS's internal resolution
 * (the aspect ratio is not corrected), as 8-bit RGBA, rows top to bottom, 4 bytes per pixel,
 * alpha 255.
 *   dst:       destination, or NULL to only get the size.
 *   width, height: receive the size in pixels (may be NULL).
 *   max_bytes: size of dst in bytes.
 * Returns 0 on success; -1 GS not open; -2 nothing to show (no vsync yet with the display
 * circuits enabled); -3 dst too small (width and height are still filled in). This call waits for
 * the GPU, it is meant for tools and tests, not for every frame. */
DON_GS_API int don_gs_snapshot_rgba(uint8_t* dst, int* width, int* height, int max_bytes);

/* Tells the GS that the window surface changed size, in pixels. Does nothing when headless or
 * not open. */
DON_GS_API void don_gs_resize(int width, int height);

/* Describes the renderer and the Vulkan physical devices, for the host's [don] start block.
 * Writes a NUL-terminated text to `buf` (at most buf_len bytes, always terminated). The first
 * line is the renderer actually in use ("vulkan" or "software"); when it is Vulkan, one line
 * follows per physical device:
 *   <name>|<type>|0x<vendor>|<driver maj.min.patch>|<api maj.min.patch>|<1 if the GS picked it>
 * with <type> one of integrated, discrete, virtual, cpu, other. Device names never contain '|'.
 * Returns 0 on success; negative if `buf` is NULL/too small, the GS is not open, or Vulkan is
 * not loaded. May be called only from the thread that opened the GS (threading: see the top). */
DON_GS_API int don_gs_device_info(char* buf, int buf_len);

#ifdef __cplusplus
}
#endif

#endif /* DON_GS_H */
