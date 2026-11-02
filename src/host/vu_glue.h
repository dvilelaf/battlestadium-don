// SPDX-License-Identifier: GPL-3.0-or-later
// The VU1 library (libdon_vu, the C API of src/vubridge/don_vu.h) loaded at run time, and the
// Ps2HostVu1 table (patches/ps2recomp/0020-host-vu1-hook.patch, runtime/ps2_host_vu1.h) with which
// the runtime's VIF1 delegates microcode upload and microprogram execution to it. The table is what
// PS2Runtime::setHostVu1() copies; VIF1 itself stays in the runtime.
//
// Why it is loaded at run time (SDL_LoadObject) and not linked: the same reasons as the GS and SPU2
// glues (gs_glue.h, spu2_glue.h): a missing or unloadable library is an ordinary error the caller
// turns into "use the runtime's own VU1 interpreter" instead of a loader failure before main() runs;
// on Windows there is no import library to build or ship; and the library sits in a folder of its own
// (build/vu) that the host may point at with --vu-dir or DON_VU_DIR without a relink. The cost is a
// table of function pointers (DonVuApi), filled once.
//
// Threads. microVU is one VU1 per process and every call must come from the thread that opened it, so
// the library is opened in thread_begin and closed in thread_end, both on the game thread, and every
// other callback of the table just forwards. The main thread only reads state() / error() / summary()
// (atomics and counters written on the game thread; read them when the game thread is not running).
//
// Floating point. The library switches the calling thread's MXCSR to the VU's own value for the
// duration of execute / continue_ and restores it on return, and the XGKICK sink runs with the
// caller's value (don_vu.h). The glue does not touch MXCSR; test_vu_glue.cpp checks that it holds
// through the table.
//
// Engine selection (the card's step 3). DON_VU1 chooses which VU1 runs:
//   microvu      (also unset) PCSX2's microVU, through this glue. If the library is not found the host
//                prints one [don-vu] warning and the runtime's own interpreter runs.
//   runtime      (also "interp"): the runtime's own VU1 interpreter. This is "do not install the
//                table at all", the reference for debugging; it gives up on the menus (journal 008).
//   pcsx2interp  the VU interpreter of the same library (slow; for comparing the two engines).
// don_vu1_selection_from_string() is the pure parser (tests/vuglue/test_vu_select.cpp); the host
// program calls don_vu1_selection_from_env() once, before the game thread starts, and when the answer
// is not the runtime's interpreter it loads the library and installs HostVu::iface() (with a
// std::unique_ptr<HostVu> that outlives the runtime; `options.vu_dir` is --vu-dir):
//
//     const int selection = don_vu1_selection_from_env();      // prints its own [don-vu] line
//     std::unique_ptr<HostVu> vu;
//     if (selection != DON_VU1_RUNTIME)
//     {
//         DonVuApi vu_api;
//         std::string vu_error;
//         if (!don_vu_load(options.vu_dir, vu_api, vu_error))
//             std::fprintf(stderr, "[don] %s\n", vu_error.c_str()); // no library: the runtime's
//                                                                   // interpreter runs instead
//         else
//         {
//             vu = std::make_unique<HostVu>(vu_api, don_vu1_engine_for_selection(selection));
//             runtime->setHostVu1(&vu->iface());               // before startGameThread()
//         }
//     }
//     ... after stopGameThread(): runtime->setHostVu1(nullptr); vu->print_summary(); vu.reset();
//
// A missing library (don_vu_load false) or a failed open (don_vu1_open_ex nonzero: no executable
// memory, or a VU1 already open) leaves the runtime's interpreter in charge: with a failed open
// data() answers null and every other callback is a no-op, and the runtime keeps its own VU1 (see
// beginHostVu1 in ps2_memory.cpp). Neither case crashes and neither needs a fallback path of its own.
#pragma once

#include "don_vu.h"
#include "runtime/ps2_host_vu1.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

// What DON_VU1 selected. DON_VU1_RUNTIME means "the runtime's own interpreter": the host program must
// not install the table at all.
#define DON_VU1_RUNTIME 0
#define DON_VU1_MICROVU 1
#define DON_VU1_PCSX2INTERP 2
#define DON_VU1_UNKNOWN (-1)

// Pure parser of the value of DON_VU1: one of the constants above, DON_VU1_UNKNOWN for a value that is
// not one of the names. NULL or "" is "unset" and means DON_VU1_MICROVU (the default since T-113/T-205). The empty
// string is accepted as unset because an exported-but-empty variable is common. "interp" is accepted as
// a second name for "runtime": it is the name the T-205 card uses for the reference mode. The names are
// case-insensitive.
int don_vu1_selection_from_string(const char *value);

// "runtime", "microvu", "pcsx2interp", or "unknown".
const char *don_vu1_selection_name(int selection);

// The value of DON_VU1 in the environment through don_vu1_selection_from_string(). An unknown value
// prints one [don-vu] line to stderr (it is a misconfiguration worth telling about) and returns
// DON_VU1_RUNTIME, so that a typo cannot make a run fail.
int don_vu1_selection_from_env();

// The engine to pass to HostVu for a selection: DON_VU_ENGINE_MICROVU, DON_VU_ENGINE_INTERPRETER, or
// -1 for DON_VU1_RUNTIME / DON_VU1_UNKNOWN (no engine: do not install the table).
int don_vu1_engine_for_selection(int selection);

// The functions of libdon_vu that the glue uses, resolved once. All of them are non-null once
// don_vu_load returned true; the signatures are the ones of don_vu.h. `continue_` is not `continue`
// because that is a C++ keyword.
struct DonVuApi
{
    void *library = nullptr; // SDL_SharedObject*
    std::filesystem::path path;

    int (*version)() = nullptr;
    int (*open_ex)(int engine, DonVuXgkick cb, void *user) = nullptr;
    void (*close)() = nullptr;
    void (*reset)() = nullptr;
    uint8_t *(*data)() = nullptr;
    void (*write_micro)(uint32_t addr_bytes, const uint8_t *data, uint32_t bytes) = nullptr;
    void (*set_top)(uint32_t top, uint32_t itop) = nullptr;
    uint32_t (*execute)(uint32_t start_pc_bytes, uint32_t max_cycles) = nullptr;
    uint32_t (*continue_)(uint32_t max_cycles) = nullptr;
    int (*running)() = nullptr;
    void (*set_regs)(const void *vf, const void *vi) = nullptr;
    void (*get_regs)(void *vf, void *vi) = nullptr;
    uint64_t (*total_cycles)() = nullptr;
};

// Looks for the library in `vu_dir` if given, else in $DON_VU_DIR if set (an explicit folder is the
// only one searched), else next to the executable, in a "vu" folder next to it and in a sibling
// "../vu" folder (the layout of build/). Fills `api` and returns true, or returns false with a
// complete message (starting with "[don-vu] ", prefix included) in `err`.
bool don_vu_load(const std::string &vu_dir, DonVuApi &api, std::string &err);

// Counters for the summary line. starts, continues, packets and bytes are counted by the glue: starts
// and continues are the execute / continue_ calls, and packets and bytes are counted by the sink
// trampoline that DON_VU_LOG=1 installs (the library has no byte counter, and its own packet counter
// don_vu1_xgkick_count is zeroed by don_vu1_reset, so it cannot serve as a session total). cycles comes
// from the library (don_vu1_total_cycles). Game thread writes them; read them when the game thread is
// not running. When DON_VU_LOG is off there is no trampoline and packets and bytes stay 0: nothing
// prints them and the sink goes to the runtime without a call of ours in between.
struct DonVuSummary
{
    uint64_t starts = 0;    // execute() calls (MSCAL, MSCALF, MSCNT)
    uint64_t continues = 0; // continue_() calls
    uint64_t cycles = 0;    // don_vu1_total_cycles() at thread_end
    uint64_t packets = 0;   // XGKICK packets the sink received (DON_VU_LOG=1)
    uint64_t bytes = 0;     // their total size in bytes (DON_VU_LOG=1)
};

// One instance per process, owned by the host program. It owns the library (opened in thread_begin,
// closed in thread_end, on the game thread) and hands out the Ps2HostVu1 table that forwards to
// DonVuApi. Nothing here decides whether the runtime's interpreter or the library runs: that is the
// caller's choice of installing iface() or not.
//
// thread_begin opens the library once: a second call while one is open (or after a failure) only
// prints one [don-vu] line and changes nothing, so the VU1 the first call opened keeps working.
class HostVu
{
public:
    enum State : int
    {
        Pending, // thread_begin has not run yet
        Open,
        Failed, // don_vu1_open_ex refused on the game thread; error() has the message
        Closed, // thread_end ran
    };

    // `engine` is DON_VU_ENGINE_MICROVU or DON_VU_ENGINE_INTERPRETER (don_vu1_engine_for_selection).
    HostVu(const DonVuApi &api, int engine);
    ~HostVu();
    HostVu(const HostVu &) = delete;
    HostVu &operator=(const HostVu &) = delete;

    // The table to install with PS2Runtime::setHostVu1() before the game thread starts (the runtime
    // copies it; `this` must outlive the runtime). Safe to call before thread_begin, after a failed
    // open and after thread_end: every callback then does nothing (data() answers null, execute() 0,
    // running() 0), which is what makes the runtime fall back to its own interpreter.
    const Ps2HostVu1 &iface() const { return m_iface; }

    // DON_VU_ENGINE_* given to the constructor.
    int engine() const { return m_engine; }

    // Any thread.
    State state() const { return m_state.load(std::memory_order_acquire); }
    const std::string &error() const { return m_error; } // valid once state() is Failed
    bool is_open() const { return m_open.load(std::memory_order_acquire); }

    // Counters of the library session (after thread_end).
    DonVuSummary summary() const;

    // DON_VU_LOG=1: the one summary line of the session, to stderr. thread_end already printed it on
    // the game thread (the counters are read there, the last moment the library can be asked); a call
    // from the main thread after stopGameThread then prints nothing more but is harmless. Off by
    // default: nothing is printed when DON_VU_LOG is not 1.
    void print_summary() const;

private:
    static void thread_begin(void *self, Ps2HostVu1XgkickFn xgkick, void *xgkick_user);
    static void thread_end(void *self);
    static uint8_t *data(void *self);
    static void write_micro(void *self, uint32_t addr_bytes, const uint8_t *data, uint32_t bytes);
    static void set_top(void *self, uint32_t top, uint32_t itop);
    static uint32_t execute(void *self, uint32_t start_pc_bytes, uint32_t max_cycles);
    static uint32_t continue_(void *self, uint32_t max_cycles);
    static int running(void *self);
    static void reset(void *self);
    static void set_regs(void *self, const void *vf, const void *vi);
    static void get_regs(void *self, void *vf, void *vi);
    // The XGKICK sink handed to the library. With DON_VU_LOG=1 it counts the packets and the bytes for
    // the summary and forwards to the runtime's sink; with the log off the runtime's sink is passed to
    // the library directly (no trampoline, no cost when off: AGENTS.md, "Diagnostics").
    static void xgkick(void *user, const uint8_t *packet, uint32_t bytes);

    void on_thread_begin(Ps2HostVu1XgkickFn xgkick, void *xgkick_user);
    void on_thread_end();
    // The one summary line (DON_VU_LOG=1), at most once per session.
    void emit_summary() const;

    // A copy, not a reference: the caller's table is often a local (host.cpp declares its DonVuApi
    // inside a block that ends before the game thread starts, off the stack of a function that has
    // already returned). Holding a reference there left every callback reading a dead stack slot, and
    // once any later call reused that slot the table went null and the glue jumped to address 0
    // (T-304). The copy is a handful of pointers and the library path; the library handle in it is
    // borrowed, not owned, exactly as before.
    DonVuApi m_api;
    const int m_engine;
    Ps2HostVu1 m_iface;
    std::atomic<State> m_state{Pending};
    // Written on the game thread before the state becomes Open, read by the other threads afterwards.
    std::atomic<bool> m_open{false};
    std::string m_error;

    // Game thread only.
    bool m_log = false;
    bool m_opened = false; // this instance opened the library (a refused open has nothing to summarise)
    Ps2HostVu1XgkickFn m_sink = nullptr; // the runtime's sink, from thread_begin
    void *m_sink_user = nullptr;
    uint64_t m_starts = 0;
    uint64_t m_continues = 0;
    uint64_t m_packets = 0; // counted by xgkick(); only with DON_VU_LOG=1
    uint64_t m_bytes = 0;   // the same, in bytes
    uint64_t m_cycles = 0;  // library counter at thread_end
    mutable bool m_summary_printed = false;
};
