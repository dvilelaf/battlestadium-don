// SPDX-License-Identifier: GPL-3.0-or-later
#include "vu_glue.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
#if defined(_WIN32)
    constexpr const char *kLibraryName = "don_vu.dll";
#elif defined(__APPLE__)
    constexpr const char *kLibraryName = "libdon_vu.dylib";
#else
    constexpr const char *kLibraryName = "libdon_vu.so";
#endif

    template <typename Fn>
    bool load_symbol(void *library, const char *name, Fn &out, std::string &err)
    {
        SDL_FunctionPointer fn = SDL_LoadFunction(static_cast<SDL_SharedObject *>(library), name);
        if (!fn)
        {
            err = std::string("the VU library has no function ") + name;
            return false;
        }
        out = reinterpret_cast<Fn>(fn);
        return true;
    }

    bool env_flag(const char *name)
    {
        const char *value = std::getenv(name);
        return value && value[0] == '1';
    }

    // Case-insensitive comparison of an ASCII name.
    bool name_is(const char *value, const char *name)
    {
        for (; *value && *name; ++value, ++name)
        {
            const char a = (*value >= 'A' && *value <= 'Z') ? static_cast<char>(*value - 'A' + 'a') : *value;
            if (a != *name)
                return false;
        }
        return *value == '\0' && *name == '\0';
    }

} // namespace

int don_vu1_selection_from_string(const char *value)
{
    if (value == nullptr || value[0] == '\0')
        return DON_VU1_MICROVU; // unset: microVU (T-205); the host falls back to the runtime interpreter if the library is missing
    if (name_is(value, "runtime") || name_is(value, "interp"))
        return DON_VU1_RUNTIME;
    if (name_is(value, "microvu"))
        return DON_VU1_MICROVU;
    if (name_is(value, "pcsx2interp"))
        return DON_VU1_PCSX2INTERP;
    return DON_VU1_UNKNOWN;
}

const char *don_vu1_selection_name(int selection)
{
    switch (selection)
    {
    case DON_VU1_RUNTIME: return "runtime";
    case DON_VU1_MICROVU: return "microvu";
    case DON_VU1_PCSX2INTERP: return "pcsx2interp";
    default: return "unknown";
    }
}

int don_vu1_selection_from_env()
{
    const char *value = std::getenv("DON_VU1");
    const int selection = don_vu1_selection_from_string(value);
    if (selection == DON_VU1_UNKNOWN)
    {
        std::fprintf(stderr, "[don-vu] DON_VU1=\"%s\" is not one of runtime, microvu, pcsx2interp; "
                             "using the runtime's interpreter\n",
                     value);
        return DON_VU1_RUNTIME;
    }
    return selection;
}

int don_vu1_engine_for_selection(int selection)
{
    switch (selection)
    {
    case DON_VU1_MICROVU: return DON_VU_ENGINE_MICROVU;
    case DON_VU1_PCSX2INTERP: return DON_VU_ENGINE_INTERPRETER;
    default: return -1; // runtime (no table) or unknown: nothing to open
    }
}

bool don_vu_load(const std::string &vu_dir, DonVuApi &api, std::string &err)
{
    std::vector<fs::path> dirs;
    const char *env_dir = std::getenv("DON_VU_DIR");
    if (!vu_dir.empty())
        dirs.emplace_back(vu_dir);
    else if (env_dir && env_dir[0])
        dirs.emplace_back(env_dir);
    else
    {
        const char *base_text = SDL_GetBasePath();
        const fs::path base = base_text ? fs::path(base_text) : fs::path(".");
        dirs.push_back(base);
        dirs.push_back(base / "vu");
        dirs.push_back((base / ".." / "vu").lexically_normal());
    }

    std::string tried;
    for (const fs::path &dir : dirs)
    {
        const fs::path candidate = dir / kLibraryName;
        std::error_code ec;
        if (!fs::exists(candidate, ec))
        {
            tried += "  " + candidate.string() + ": not found\n";
            continue;
        }
        // An absolute path: the library finds its own dependencies relative to itself.
        const fs::path absolute = fs::absolute(candidate, ec);
        SDL_SharedObject *library = SDL_LoadObject(absolute.string().c_str());
        if (!library)
        {
            tried += "  " + absolute.string() + ": " + SDL_GetError() + "\n";
            continue;
        }

        DonVuApi loaded;
        loaded.library = library;
        loaded.path = absolute;
        std::string bind_error;
        const bool ok = load_symbol(library, "don_vu_version", loaded.version, bind_error) &&
                        load_symbol(library, "don_vu1_open_ex", loaded.open_ex, bind_error) &&
                        load_symbol(library, "don_vu1_close", loaded.close, bind_error) &&
                        load_symbol(library, "don_vu1_reset", loaded.reset, bind_error) &&
                        load_symbol(library, "don_vu1_data", loaded.data, bind_error) &&
                        load_symbol(library, "don_vu1_write_micro", loaded.write_micro, bind_error) &&
                        load_symbol(library, "don_vu1_set_top", loaded.set_top, bind_error) &&
                        load_symbol(library, "don_vu1_execute", loaded.execute, bind_error) &&
                        load_symbol(library, "don_vu1_continue", loaded.continue_, bind_error) &&
                        load_symbol(library, "don_vu1_running", loaded.running, bind_error) &&
                        load_symbol(library, "don_vu1_set_regs", loaded.set_regs, bind_error) &&
                        load_symbol(library, "don_vu1_get_regs", loaded.get_regs, bind_error) &&
                        load_symbol(library, "don_vu1_total_cycles", loaded.total_cycles, bind_error);
        if (!ok || loaded.version() != DON_VU_VERSION)
        {
            tried += "  " + absolute.string() + ": " + (ok ? "unsupported API version" : bind_error) + "\n";
            SDL_UnloadObject(library);
            continue;
        }
        api = loaded;
        return true;
    }
    err = "[don-vu] cannot find the VU library (" + std::string(kLibraryName) + "); tried:\n" + tried +
          "[don-vu] set --vu-dir or DON_VU_DIR to the folder that holds it";
    return false;
}

HostVu::HostVu(const DonVuApi &api, int engine)
    : m_api(api), m_engine(engine), m_log(env_flag("DON_VU_LOG"))
{
    m_iface.user = this;
    m_iface.thread_begin = &HostVu::thread_begin;
    m_iface.thread_end = &HostVu::thread_end;
    m_iface.data = &HostVu::data;
    m_iface.write_micro = &HostVu::write_micro;
    m_iface.set_top = &HostVu::set_top;
    m_iface.execute = &HostVu::execute;
    m_iface.continue_ = &HostVu::continue_;
    m_iface.running = &HostVu::running;
    m_iface.reset = &HostVu::reset;
    m_iface.set_regs = &HostVu::set_regs;
    m_iface.get_regs = &HostVu::get_regs;
}

HostVu::~HostVu()
{
    // The runtime must have called thread_end (the library has to be closed on the thread that opened
    // it); closing from here would be too late or from the wrong thread.
    if (is_open())
        std::fprintf(stderr, "[don-vu] the VU1 was still open: thread_end was not called before the "
                             "glue was destroyed\n");
}

void HostVu::thread_begin(void *self, Ps2HostVu1XgkickFn xgkick, void *xgkick_user)
{
    static_cast<HostVu *>(self)->on_thread_begin(xgkick, xgkick_user);
}

void HostVu::thread_end(void *self) { static_cast<HostVu *>(self)->on_thread_end(); }

void HostVu::on_thread_begin(Ps2HostVu1XgkickFn xgkick, void *xgkick_user)
{
    if (m_state.load(std::memory_order_acquire) != Pending)
    {
        // The runtime calls thread_begin once. A second call (a test, or a restarted game thread) must
        // not open a second VU1: the library refuses it anyway, and this way the open one keeps working.
        std::fprintf(stderr, "[don-vu] thread_begin called again (%s): the VU1 that is already open is kept\n",
                     m_state.load() == Open ? "open" : "not open");
        return;
    }

    m_sink = xgkick;
    m_sink_user = xgkick_user;

    // The two signatures match exactly (void *user, const uint8_t *packet, uint32_t bytes), so the
    // runtime's sink goes to the library as it is. With DON_VU_LOG=1 a counting trampoline is used
    // instead: the library has no byte counter and its packet counter is zeroed by don_vu1_reset.
    DonVuXgkick cb = m_sink;
    void *user = m_sink_user;
    if (m_log)
    {
        cb = &HostVu::xgkick;
        user = this;
    }

    const int rc = m_api.open_ex(m_engine, cb, user);
    if (rc != 0)
    {
        m_error = "[don-vu] the VU1 could not be opened (don_vu1_open_ex returned " + std::to_string(rc) +
                  "); the runtime's interpreter will run the microprograms";
        m_state.store(Failed, std::memory_order_release);
        std::fprintf(stderr, "%s\n", m_error.c_str());
        return;
    }
    m_open.store(true, std::memory_order_release);
    m_opened = true;
    m_state.store(Open, std::memory_order_release);
    if (m_log)
        std::fprintf(stderr, "[don-vu] VU1 opened on %s\n",
                     m_engine == DON_VU_ENGINE_MICROVU ? "microVU" : "the PCSX2 interpreter");
}

void HostVu::on_thread_end()
{
    if (m_open.exchange(false, std::memory_order_acq_rel))
    {
        // The library's own cycle counter, while it is still open.
        m_cycles = m_api.total_cycles();
        m_api.close();
    }
    const State previous = m_state.load(std::memory_order_acquire);
    m_state.store(previous == Failed ? Failed : Closed, std::memory_order_release);
    // DON_VU_LOG=1: the one summary line, here, where the counters are complete and stderr is still
    // ours (the game thread is on its way out).
    emit_summary();
}

uint8_t *HostVu::data(void *self)
{
    auto *vu = static_cast<HostVu *>(self);
    // null before thread_begin, after a failed open and after thread_end. The runtime reads this right
    // after thread_begin: null means "stay on your own interpreter" (ps2_memory.cpp, beginHostVu1).
    return vu->is_open() ? vu->m_api.data() : nullptr;
}

void HostVu::write_micro(void *self, uint32_t addr_bytes, const uint8_t *data, uint32_t bytes)
{
    auto *vu = static_cast<HostVu *>(self);
    if (vu->is_open())
        vu->m_api.write_micro(addr_bytes, data, bytes);
}

void HostVu::set_top(void *self, uint32_t top, uint32_t itop)
{
    auto *vu = static_cast<HostVu *>(self);
    if (vu->is_open())
        vu->m_api.set_top(top, itop);
}

uint32_t HostVu::execute(void *self, uint32_t start_pc_bytes, uint32_t max_cycles)
{
    auto *vu = static_cast<HostVu *>(self);
    if (!vu->is_open())
        return 0;
    ++vu->m_starts;
    return vu->m_api.execute(start_pc_bytes, max_cycles);
}

uint32_t HostVu::continue_(void *self, uint32_t max_cycles)
{
    auto *vu = static_cast<HostVu *>(self);
    if (!vu->is_open())
        return 0;
    ++vu->m_continues;
    return vu->m_api.continue_(max_cycles);
}

int HostVu::running(void *self)
{
    auto *vu = static_cast<HostVu *>(self);
    return vu->is_open() ? vu->m_api.running() : 0;
}

void HostVu::reset(void *self)
{
    auto *vu = static_cast<HostVu *>(self);
    if (vu->is_open())
        vu->m_api.reset();
}

void HostVu::set_regs(void *self, const void *vf, const void *vi)
{
    auto *vu = static_cast<HostVu *>(self);
    if (vu->is_open())
        vu->m_api.set_regs(vf, vi);
}

void HostVu::get_regs(void *self, void *vf, void *vi)
{
    auto *vu = static_cast<HostVu *>(self);
    // Nothing is open: leave the caller's buffers alone (a debugger then reports "not available"
    // instead of a plausible-looking zero state).
    if (vu->is_open())
        vu->m_api.get_regs(vf, vi);
}

void HostVu::xgkick(void *user, const uint8_t *packet, uint32_t bytes)
{
    auto *vu = static_cast<HostVu *>(user);
    ++vu->m_packets;
    vu->m_bytes += bytes;
    if (vu->m_sink)
        vu->m_sink(vu->m_sink_user, packet, bytes);
}

DonVuSummary HostVu::summary() const
{
    DonVuSummary out;
    out.starts = m_starts;
    out.continues = m_continues;
    out.cycles = m_cycles;
    out.packets = m_packets;
    out.bytes = m_bytes;
    return out;
}

void HostVu::print_summary() const { emit_summary(); }

void HostVu::emit_summary() const
{
    // Nothing to report for an instance that never opened the library (its refusal printed its own
    // [don-vu] line), and never twice.
    if (!m_log || !m_opened || m_summary_printed)
        return;
    m_summary_printed = true;
    const DonVuSummary s = summary();
    std::fprintf(stderr, "[don-vu] VU1 (%s): %llu programs started, %llu continuations, %llu cycles, "
                         "%llu XGKICK packets, %llu bytes\n",
                 m_engine == DON_VU_ENGINE_MICROVU ? "microvu" : "pcsx2interp",
                 static_cast<unsigned long long>(s.starts), static_cast<unsigned long long>(s.continues),
                 static_cast<unsigned long long>(s.cycles), static_cast<unsigned long long>(s.packets),
                 static_cast<unsigned long long>(s.bytes));
}
