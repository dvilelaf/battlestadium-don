// SPDX-License-Identifier: GPL-3.0-or-later
#include "spu2_glue.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
#if defined(_WIN32)
    constexpr const char *kLibraryName = "don_spu2.dll";
#elif defined(__APPLE__)
    constexpr const char *kLibraryName = "libdon_spu2.dylib";
#else
    constexpr const char *kLibraryName = "libdon_spu2.so";
#endif

    template <typename Fn>
    bool load_symbol(void *library, const char *name, Fn &out, std::string &err)
    {
        SDL_FunctionPointer fn = SDL_LoadFunction(static_cast<SDL_SharedObject *>(library), name);
        if (!fn)
        {
            err = std::string("the SPU2 library has no function ") + name;
            return false;
        }
        out = reinterpret_cast<Fn>(fn);
        return true;
    }

} // namespace

bool don_spu2_load(const std::string &spu2_dir, DonSpu2Api &api, std::string &err)
{
    std::vector<fs::path> dirs;
    const char *env_dir = std::getenv("DON_SPU2_DIR");
    if (!spu2_dir.empty())
        dirs.emplace_back(spu2_dir);
    else if (env_dir && env_dir[0])
        dirs.emplace_back(env_dir);
    else
    {
        const char *base_text = SDL_GetBasePath();
        const fs::path base = base_text ? fs::path(base_text) : fs::path(".");
        dirs.push_back(base);
        dirs.push_back(base / "spu2");
        dirs.push_back((base / ".." / "spu2").lexically_normal());
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

        DonSpu2Api loaded;
        loaded.library = library;
        loaded.path = absolute;
        std::string bind_error;
        const bool ok = load_symbol(library, "don_spu2_version", loaded.version, bind_error) &&
                        load_symbol(library, "don_spu2_open", loaded.open, bind_error) &&
                        load_symbol(library, "don_spu2_close", loaded.close, bind_error) &&
                        load_symbol(library, "don_spu2_reset", loaded.reset, bind_error) &&
                        load_symbol(library, "don_spu2_write16", loaded.write16, bind_error) &&
                        load_symbol(library, "don_spu2_read16", loaded.read16, bind_error) &&
                        load_symbol(library, "don_spu2_dma_write", loaded.dma_write, bind_error) &&
                        load_symbol(library, "don_spu2_dma_read", loaded.dma_read, bind_error) &&
                        load_symbol(library, "don_spu2_advance", loaded.advance, bind_error) &&
                        load_symbol(library, "don_spu2_cycle", loaded.cycle, bind_error) &&
                        load_symbol(library, "don_spu2_poll_event", loaded.poll_event, bind_error) &&
                        load_symbol(library, "don_spu2_pull_output", loaded.pull_output, bind_error);
        if (!ok || loaded.version() != DON_SPU2_API_VERSION)
        {
            tried += "  " + absolute.string() + ": " + (ok ? "unsupported API version" : bind_error) + "\n";
            SDL_UnloadObject(library);
            continue;
        }
        api = loaded;
        return true;
    }
    err = "[don-spu2] cannot find the SPU2 library (" + std::string(kLibraryName) + "); tried:\n" + tried +
          "[don-spu2] set --spu2-dir or DON_SPU2_DIR to the folder that holds it";
    return false;
}

namespace
{
    bool env_flag(const char *name)
    {
        const char *value = std::getenv(name);
        return value != nullptr && value[0] == '1';
    }
}

HostSpu2::HostSpu2(const DonSpu2Api &api) : m_api(api)
{
    m_trace = env_flag("DON_TRACE_SPU2");
    if (const char *log = std::getenv("DON_SPU2_CALLLOG"); log != nullptr && log[0] != '\0')
        m_callLog = std::fopen(log, "w");
    m_iface.user = this;
    m_iface.write16 = &HostSpu2::write16;
    m_iface.read16 = &HostSpu2::read16;
    m_iface.dma_write = &HostSpu2::dma_write;
    m_iface.dma_read = &HostSpu2::dma_read;
    m_iface.advance = &HostSpu2::advance;
    m_iface.poll_event = &HostSpu2::poll_event;
    m_iface.reset = &HostSpu2::reset;
    m_iface.thread_begin = &HostSpu2::thread_begin;
    m_iface.thread_end = &HostSpu2::thread_end;
}

HostSpu2::~HostSpu2()
{
    if (m_trace)
        trace_summary();
    close();
}

bool HostSpu2::open(std::string &err)
{
    if (is_open())
        return true;
    const int rc = m_api.open ? m_api.open() : -1;
    if (rc != 0)
    {
        err = "[don-spu2] the SPU2 library could not be opened (" + std::to_string(rc) + ")";
        return false;
    }
    m_owner = std::this_thread::get_id();
    m_open.store(true, std::memory_order_release);
    return true;
}

void HostSpu2::thread_begin(void *user)
{
    HostSpu2 *const self = static_cast<HostSpu2 *>(user);
    if (!self->m_workerOwned)
        return;
    std::string err;
    if (!self->open(err))
        std::fprintf(stderr, "%s\n", err.c_str());
}

void HostSpu2::thread_end(void *user)
{
    HostSpu2 *const self = static_cast<HostSpu2 *>(user);
    if (self->m_workerOwned)
        self->close();
}

void HostSpu2::close()
{
    if (m_workerOwned && is_open() && m_owner != std::this_thread::get_id())
        return; // the IOP worker closes it (thread_end)
    if (m_callLog)
    {
        std::fclose(m_callLog);
        m_callLog = nullptr;
    }
    if (!m_open.exchange(false, std::memory_order_acq_rel))
        return;
    if (m_api.close)
        m_api.close();
}

uint32_t HostSpu2::pull(float *stereo, uint32_t frames)
{
    if (!is_open() || !stereo || frames == 0)
        return 0;
    const uint32_t got = m_api.pull_output(stereo, frames);
    m_frames += got;
    return got;
}

// ---- DON_TRACE_SPU2 (T-304) ------------------------------------------------------------------------
// Register groups, by offset inside the chip's 0x800 bytes (core 1 is +0x400; the offsets below are the
// ones of third_party/pcsx2/pcsx2/SPU2/regs.h):
//   0 voices          0x000-0x17F per core (0x10 per voice: VOLL/VOLR/PITCH/ADSR1/ADSR2/ENVX/VOLXL/VOLXR)
//   1 core control    0x180-0x19F and 0x1A8-0x2DF: mixes, ATTR (0x19A), IRQA (0x19C), TSA (0x1A8),
//                     ADMAS (0x1B0), the per-voice addresses (0x1C0+) and the effects block
//   2 key on/off      0x1A0-0x1A7: KON (0x1A0/0x1A2), KOFF (0x1A4/0x1A6)
//   3 reverb area     0x2E0-0x33F: ESA (0x2E0), EEA (0x33C) and the effect parameters
//   4 post block      0x760-0x7FF: the master volumes (0x760/0x762) and the rest of the "P" block
// The same five groups exist for core 1 (offsets 0x400-0x7FF); groups 5..9 are those.
static int spu2_group(uint32_t addr)
{
    const uint32_t core = (addr >= 0x400u) ? 1u : 0u;
    const uint32_t off = addr & 0x3FFu;
    int group = 1;
    if (off < 0x180u)
        group = 0;
    else if (off < 0x1A0u)
        group = 1;
    else if (off < 0x1A8u)
        group = 2;
    else if (off < 0x2E0u)
        group = 1;
    else if (off < 0x340u)
        group = 3;
    else if (off >= 0x360u && off < 0x400u)
        group = 4;
    else
        group = 1;
    return static_cast<int>(core * 5u) + group;
}

static const char *spu2_group_name(int group)
{
    static const char *const names[10] = {"v0", "c0", "k0", "r0", "p0", "v1", "c1", "k1", "r1", "p1"};
    return names[group];
}

void HostSpu2::trace_register(uint32_t addr, bool write, uint16_t value)
{
    if (!m_trace)
        return;
    ++m_writes;
    // The emulator passes the absolute IOP address of the chip's window (0x1F900000-0x1F9007FF); the
    // tables below are indexed by the offset inside the window.
    addr &= 0x7FFu;
    m_last[addr] = value;
    m_touched[addr] = 1;
    const int group = spu2_group(addr);
    ++m_group[group];
    ++m_groupTotal[group];
    if (group == 0 || group == 5)
        ++m_voiceWrites;
    if (addr == 0x1A0u || addr == 0x1A2u)
        m_konEver = static_cast<uint16_t>(m_konEver | value);
    if (addr == 0x1A4u || addr == 0x1A6u)
        m_koffEver = static_cast<uint16_t>(m_koffEver | value);
    if (addr == 0x5A0u || addr == 0x5A2u)
        m_konEver1 = static_cast<uint16_t>(m_konEver1 | value);
    if (addr == 0x5A4u || addr == 0x5A6u)
        m_koffEver1 = static_cast<uint16_t>(m_koffEver1 | value);
    // T-304 Part 2.3: the control registers whose late writes say how the driver stops and restarts the
    // music: ATTR (0x19A/0x59A), IRQA (0x19C/0x59C) and the automatic-transfer control ADMAS
    // (0x1B0/0x5B0). The ring keeps the last kSpecialWrites with their emulated time.
    if (addr == 0x19Au || addr == 0x59Au || addr == 0x19Cu || addr == 0x59Cu || addr == 0x1B0u ||
        addr == 0x5B0u)
    {
        SpecialWrite &w = m_special[m_specialCount % kSpecialWrites];
        w.cycle = m_traceTotalCycles + m_traceCycles;
        w.addr = static_cast<uint16_t>(addr);
        w.value = value;
        ++m_specialCount;
    }
}

void HostSpu2::trace_report(uint64_t cycles)
{
    static uint64_t lastWrites = 0, lastReads = 0, lastFrames = 0;
    static uint64_t lastDma[2] = {}, lastDmaBytes[2] = {}, lastEvents[8] = {}, lastDmaReads = 0;
    // The group that was written most since the last line.
    int top = 0;
    for (int i = 1; i < 10; ++i)
        if (m_group[i] > m_group[top])
            top = i;
    // T-304 Part 2.3: the register read most often since the last line, and the value it last returned.
    int topRead = 0;
    uint64_t topReadDelta = 0;
    for (uint32_t i = 0; i < 0x800u; ++i)
    {
        const uint64_t delta = m_readsByReg[i] - m_readsByRegLast[i];
        if (delta > topReadDelta)
        {
            topReadDelta = delta;
            topRead = static_cast<int>(i);
        }
        m_readsByRegLast[i] = m_readsByReg[i];
    }
    std::fprintf(stderr,
                 "[don-spu2] t=%.1fs writes=%llu (+%llu) reads=%llu (+%llu) dma0=%llu/%lluB (+%llu/%lluB) "
                 "dma1=%llu/%lluB (+%llu/%lluB) dmaReads=%llu events1=%llu events4=%llu events7=%llu "
                 "frames=%llu (+%llu) topRegs=%s:%llu dmapeak0=%d dmapeak1=%d topRead=0x%03X:%04X(%llu)\n",
                 static_cast<double>(m_traceTotalCycles) / 36864000.0,
                 static_cast<unsigned long long>(m_writes),
                 static_cast<unsigned long long>(m_writes - lastWrites),
                 static_cast<unsigned long long>(m_reads), static_cast<unsigned long long>(m_reads - lastReads),
                 static_cast<unsigned long long>(m_dmaWrites[0]),
                 static_cast<unsigned long long>(m_dmaBytes[0]),
                 static_cast<unsigned long long>(m_dmaWrites[0] - lastDma[0]),
                 static_cast<unsigned long long>(m_dmaBytes[0] - lastDmaBytes[0]),
                 static_cast<unsigned long long>(m_dmaWrites[1]),
                 static_cast<unsigned long long>(m_dmaBytes[1]),
                 static_cast<unsigned long long>(m_dmaWrites[1] - lastDma[1]),
                 static_cast<unsigned long long>(m_dmaBytes[1] - lastDmaBytes[1]),
                 static_cast<unsigned long long>(m_dmaReads - lastDmaReads),
                 static_cast<unsigned long long>(m_events[1]), static_cast<unsigned long long>(m_events[4]),
                 static_cast<unsigned long long>(m_events[7]), static_cast<unsigned long long>(m_frames),
                 static_cast<unsigned long long>(m_frames - lastFrames), spu2_group_name(top),
                 static_cast<unsigned long long>(m_group[top]),
                 m_dmaPeak[0], m_dmaPeak[1], static_cast<unsigned>(topRead), m_lastRead[topRead],
                 static_cast<unsigned long long>(topReadDelta));
    for (int i = 0; i < 2; ++i)
    {
        if (m_dmaPeak[i] > m_dmaPeakMax[i])
            m_dmaPeakMax[i] = m_dmaPeak[i];
        m_dmaPeak[i] = 0;
    }
    // The control registers, every line: what decides whether anything can be heard.
    for (int core = 0; core < 2; ++core)
    {
        const uint32_t b = static_cast<uint32_t>(core) * 0x400u;
        std::fprintf(stderr,
                     "[don-spu2]   core%d ATTR(0x%03X)=0x%04X%s%s MVOL=0x%04X/0x%04X KONever=0x%06X "
                     "KOFFever=0x%06X TSA(0x1A8)=0x%04X words[0x1B0..0x1BE]=%04X %04X %04X %04X %04X %04X %04X %04X "
                     "ESA(0x2E0)=0x%04X EEA(0x33C)=0x%04X voiceWrites=%llu\n",
                     core, 0x19A + b, m_last[0x19A + b], (m_last[0x19A + b] & 0x1u) ? " DMA=on" : " DMA=off",
                     (m_last[0x19A + b] & 0x2u) ? " MUTED" : "",
                     core == 0 ? m_last[0x760] : m_last[0x764], core == 0 ? m_last[0x762] : m_last[0x766],
                     core == 0 ? (unsigned)m_konEver : (unsigned)m_konEver1,
                     core == 0 ? (unsigned)m_koffEver : (unsigned)m_koffEver1, m_last[0x1A8 + b],
                     m_last[0x1B0 + b], m_last[0x1B2 + b], m_last[0x1B4 + b], m_last[0x1B6 + b], m_last[0x1B8 + b],
                     m_last[0x1BA + b], m_last[0x1BC + b], m_last[0x1BE + b], m_last[0x2E0 + b], m_last[0x33C + b],
                     static_cast<unsigned long long>(m_voiceWrites));
    }
    lastWrites = m_writes;
    lastReads = m_reads;
    lastFrames = m_frames;
    for (int i = 0; i < 2; ++i)
    {
        lastDma[i] = m_dmaWrites[i];
        lastDmaBytes[i] = m_dmaBytes[i];
    }
    for (int i = 0; i < 8; ++i)
        lastEvents[i] = m_events[i];
    lastDmaReads = m_dmaReads;
    for (int i = 0; i < 10; ++i)
        m_group[i] = 0;
}

void HostSpu2::trace_summary()
{
    const auto reg = [this](uint32_t a) { return static_cast<unsigned>(m_last[a]); };
    std::fprintf(stderr,
                 "[don-spu2] SUMMARY writes=%llu reads=%llu voiceWrites=%llu dma0=%llu/%lluB dma1=%llu/%lluB "
                 "dmaReads=%llu/%lluB events1=%llu events4=%llu events7=%llu frames=%llu "
                 "dmapeak0max=%d dmapeak1max=%d\n",
                 static_cast<unsigned long long>(m_writes), static_cast<unsigned long long>(m_reads),
                 static_cast<unsigned long long>(m_voiceWrites), static_cast<unsigned long long>(m_dmaWrites[0]),
                 static_cast<unsigned long long>(m_dmaBytes[0]), static_cast<unsigned long long>(m_dmaWrites[1]),
                 static_cast<unsigned long long>(m_dmaBytes[1]), static_cast<unsigned long long>(m_dmaReads),
                 static_cast<unsigned long long>(m_dmaReadBytes), static_cast<unsigned long long>(m_events[1]),
                 static_cast<unsigned long long>(m_events[4]), static_cast<unsigned long long>(m_events[7]),
                 static_cast<unsigned long long>(m_frames), m_dmaPeakMax[0], m_dmaPeakMax[1]);
    for (int core = 0; core < 2; ++core)
    {
        const uint32_t base = static_cast<uint32_t>(core) * 0x400u;
        // The volume registers live in the "different" area (regs.h): core 0 at 0x760, core 1 at 0x788
        // (+0x28, NOT +0x400). MVOL = Master Volume, MVOLX = its current (slid) value, EVOL = effect,
        // AVOL = external input (core 1 only), BVOL = "Sound Data Volume" (InpVol): the one that scales
        // the data the DMA writes, i.e. what the level question is about.
        const uint32_t m = core ? 0x28u : 0u;
        std::fprintf(stderr,
                     "[don-spu2] core%d ATTR(0x%03X)=0x%04X%s%s%s MVOL=0x%04X/0x%04X MVOLX=0x%04X/0x%04X "
                     "EVOL=0x%04X/0x%04X AVOL=0x%04X/0x%04X BVOL=0x%04X/0x%04X "
                     "KONever=0x%06X KOFFever=0x%06X TSA(0x1A8)=0x%04X ADMAS(0x1B0)=0x%04X ESA=0x%04X "
                     "IRQen=%d IRQA(0x19C)=0x%04X\n",
                     core, 0x19A + base, reg(0x19A + base), (reg(0x19A + base) & 0x1u) ? " DMA=on" : " DMA=off",
                     (reg(0x19A + base) & 0x2u) ? " MUTED" : "", (reg(0x19A + base) & 0x8000u) ? " SPU2=on" : "",
                     reg(0x760 + m), reg(0x762 + m), reg(0x770 + m), reg(0x772 + m), reg(0x764 + m), reg(0x766 + m),
                     reg(0x768 + m), reg(0x76A + m), reg(0x76C + m), reg(0x76E + m),
                     core == 0 ? (unsigned)m_konEver : (unsigned)m_konEver1,
                     core == 0 ? (unsigned)m_koffEver : (unsigned)m_koffEver1, reg(0x1A8 + base),
                     reg(0x1B0 + base), reg(0x2E0 + base), (reg(0x19A + base) & 0x40u) ? 1 : 0,
                     reg(0x19C + base));
        for (int v = 0; v < 24; ++v)
        {
            const uint16_t bit = static_cast<uint16_t>(1u << v);
            const unsigned kon = core == 0 ? m_konEver : m_konEver1;
            if ((kon & bit) == 0)
                continue;
            const uint32_t vo = base + static_cast<uint32_t>(v) * 0x10u;
            std::fprintf(stderr, "[don-spu2] core%d voice %2d started: VOLL=0x%04X VOLR=0x%04X PITCH=0x%04X "
                                 "ADSR1=0x%04X SSA=0x%04X\n",
                         core, v, reg(vo), reg(vo + 2), reg(vo + 4), reg(vo + 6), reg(0x1C0 + base + v * 0xCu));
        }
    }
    const char *names[10] = {"v0", "c0", "k0", "r0", "p0", "v1", "c1", "k1", "r1", "p1"};
    std::fprintf(stderr, "[don-spu2] writes by group:");
    for (int i = 0; i < 10; ++i)
        std::fprintf(stderr, " %s=%llu", names[i], static_cast<unsigned long long>(m_groupTotal[i]));
    std::fprintf(stderr, "\n");
    // T-304 Part 2.3: the control-register writes kept in the ring, oldest first, with their time.
    const int first = m_specialCount > kSpecialWrites ? m_specialCount - kSpecialWrites : 0;
    for (int i = first; i < m_specialCount; ++i)
    {
        const SpecialWrite &w = m_special[i % kSpecialWrites];
        std::fprintf(stderr, "[don-spu2] write t=%.3fs 0x%03X=0x%04X\n",
                     static_cast<double>(w.cycle) / 36864000.0, w.addr, w.value);
    }
}

// The callbacks below are called by the IOP emulator on the game thread. While the SPU2 is not open
// they do nothing (don_spu2.h: the library itself behaves the same before open and after close).
void HostSpu2::write16(void *user, uint32_t addr, uint16_t value)
{
    auto *self = static_cast<HostSpu2 *>(user);
    if (self->is_open())
    {
        if (self->m_callLog)
            std::fprintf(self->m_callLog, "W %llu %x %x\n", (unsigned long long)self->m_chipCycles, addr, value);
        self->trace_register(addr, true, value);
        self->m_api.write16(addr, value);
    }
}

uint16_t HostSpu2::read16(void *user, uint32_t addr)
{
    auto *self = static_cast<HostSpu2 *>(user);
    if (!self->is_open())
        return 0;
    ++self->m_reads;
    const uint16_t value = self->m_api.read16(addr);
    if (self->m_trace)
    {
        const uint32_t off = addr & 0x7FFu;
        ++self->m_readsByReg[off];
        self->m_lastRead[off] = value;
    }
    return value;
}

void HostSpu2::dma_write(void *user, int core, const uint8_t *data, uint32_t bytes)
{
    auto *self = static_cast<HostSpu2 *>(user);
    if (self->is_open())
    {
        if (core >= 0 && core < 2)
        {
            ++self->m_dmaWrites[core];
            if (self->m_callLog)
                std::fprintf(self->m_callLog, "D %llu %d %u\n", (unsigned long long)self->m_chipCycles, core, bytes);
            self->m_dmaBytes[core] += bytes;
            if (self->m_trace)
            {
                // T-304 point 5: the level the game sends, not the level that comes out. Samples are
                // little-endian int16 (the SPU2's own format); the peak is reset by trace_report.
                for (uint32_t i = 0; i + 1 < bytes; i += 2)
                {
                    const int sample = static_cast<int16_t>(
                        static_cast<uint16_t>(data[i] | (static_cast<uint32_t>(data[i + 1]) << 8)));
                    const int magnitude = sample < 0 ? -sample : sample;
                    if (magnitude > self->m_dmaPeak[core])
                        self->m_dmaPeak[core] = magnitude;
                }
            }
        }
        self->m_api.dma_write(core, data, bytes);
    }
}

void HostSpu2::dma_read(void *user, int core, uint8_t *dst, uint32_t bytes)
{
    auto *self = static_cast<HostSpu2 *>(user);
    if (self->is_open())
    {
        ++self->m_dmaReads;
        self->m_dmaReadBytes += bytes;
        self->m_api.dma_read(core, dst, bytes);
    }
}

void HostSpu2::advance(void *user, uint64_t iop_cycles)
{
    auto *self = static_cast<HostSpu2 *>(user);
    if (self->is_open())
    {
        self->m_chipCycles += iop_cycles;
        if (self->m_callLog)
            std::fprintf(self->m_callLog, "A %llu %llu\n", (unsigned long long)self->m_chipCycles, (unsigned long long)iop_cycles);
        self->m_api.advance(iop_cycles);
        if (self->m_trace)
        {
            self->m_traceCycles += iop_cycles;
            self->m_traceTotalCycles += iop_cycles;
            // One emulated second of the IOP is 36,864,000 cycles (don_spu2.h: 768 cycles = one 48 kHz frame).
            while (self->m_traceCycles >= 36864000ull)
            {
                self->m_traceCycles -= 36864000ull;
                self->trace_report(36864000ull);
            }
        }
    }
}

int HostSpu2::poll_event(void *user, int *type, uint64_t *iop_cycle)
{
    auto *self = static_cast<HostSpu2 *>(user);
    if (!self->is_open())
        return 0;
    const int rc = self->m_api.poll_event(type, iop_cycle);
    if (rc != 0 && type != nullptr && *type >= 0 && *type < 8)
        ++self->m_events[*type];
    return rc;
}

void HostSpu2::reset(void *user)
{
    auto *self = static_cast<HostSpu2 *>(user);
    if (self->is_open())
        self->m_api.reset();
}
