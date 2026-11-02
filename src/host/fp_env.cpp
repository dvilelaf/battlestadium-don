// SPDX-License-Identifier: GPL-3.0-or-later
// See fp_env.h: the floating-point mode of the game thread (T-206).
#include "fp_env.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#define DON_FP_ENV_X86 1
#include <xmmintrin.h>
#else
#define DON_FP_ENV_X86 0
#endif

namespace
{
    // The SSE control register the console's mode corresponds to: exceptions masked (bits 7-12 = 0x1F80),
    // round toward zero (bits 13-14 = 0b11 = 0x6000), flush-to-zero (bit 15 = 0x8000) and
    // denormals-are-zero (bit 6 = 0x40) on -> 0xFFC0. The C default is 0x1F80 (round to nearest).
    // (The first version of this file had 0x9FC0 here, which has bits 13-14 clear, i.e. round to nearest:
    // the probe work/orchestrator/t3d/fp_probe.cpp showed it changed nothing, which is how it was found.)
    constexpr uint32_t kConsoleCsr = 0xFFC0u;

    // What the game thread had before enter_game() (the host's value), and whether it was set.
    uint32_t g_host_csr = 0;
    bool g_entered = false;
    bool g_warned = false;

    void warn_once()
    {
        if (g_warned)
            return;
        g_warned = true;
        std::fprintf(stderr, "[don] this build cannot set the floating-point mode on this architecture: "
                             "the game runs with the host's rounding (T-206)\n");
    }
} // namespace

fp_env::Mode fp_env::mode_from_string(const char *value)
{
    if (value == nullptr || value[0] == '\0')
        return Mode::Console;
    if (std::strcmp(value, "host") == 0)
        return Mode::Host;
    if (std::strcmp(value, "console") == 0)
        return Mode::Console;
    std::fprintf(stderr, "[don] DON_FP_MODE=\"%s\" is not console or host; using console\n", value);
    return Mode::Console;
}

fp_env::Mode fp_env::mode_from_env() { return mode_from_string(std::getenv("DON_FP_MODE")); }

const char *fp_env::mode_name(Mode mode) { return mode == Mode::Host ? "host" : "console"; }

bool fp_env::available()
{
#if DON_FP_ENV_X86
    return true;
#else
    return false;
#endif
}

uint32_t fp_env::capture()
{
#if DON_FP_ENV_X86
    return static_cast<uint32_t>(_mm_getcsr());
#else
    return 0;
#endif
}

void fp_env::restore(uint32_t value)
{
#if DON_FP_ENV_X86
    _mm_setcsr(value);
#else
    (void)value;
#endif
}

void fp_env::apply(Mode mode, uint32_t *previous)
{
    if (previous != nullptr)
        *previous = capture();
#if DON_FP_ENV_X86
    _mm_setcsr(mode == Mode::Console ? kConsoleCsr : capture());
#else
    (void)mode;
    warn_once();
#endif
}

void fp_env::enter_game(Mode mode)
{
    g_host_csr = capture();
    g_entered = true;
    if (mode == Mode::Console)
    {
        apply(Mode::Console, nullptr);
        std::fprintf(stderr, "[don] floating point: the game thread runs with the console's mode "
                             "(round toward zero, FTZ and DAZ on), the host had 0x%04x\n",
                     g_host_csr & 0xFFFFu);
    }
    else
    {
        std::fprintf(stderr, "[don] floating point: DON_FP_MODE=host, the game thread keeps the host's "
                             "mode (0x%04x)\n",
                     g_host_csr & 0xFFFFu);
    }
}

void fp_env::leave_game()
{
    if (!g_entered)
        return;
    restore(g_host_csr);
    g_entered = false;
}

fp_env::HostCsrScope::HostCsrScope()
{
    if (!g_entered)
        return;
    m_active = true;
    m_saved = capture();
    restore(g_host_csr);
}

fp_env::HostCsrScope::~HostCsrScope()
{
    if (m_active)
        restore(m_saved);
}
