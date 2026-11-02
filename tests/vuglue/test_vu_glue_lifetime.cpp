// SPDX-License-Identifier: GPL-3.0-or-later
// T-304: the HostVu table must not alias the DonVuApi it was built from.
//
// What happened: the host program (src/host/host.cpp) declares its DonVuApi inside a block that ends
// before the game thread starts, and HostVu held `const DonVuApi &m_api`. Every callback therefore read
// function pointers from a dead stack slot. It worked by accident until something reused that stack:
// with --sound on the main thread's audio path did, the slot read as zero, and the glue's write_micro
// trampoline jumped to address 0 (SIGSEGV with DON_VU1=microvu --sound on, ~0.85 s in).
//
// The check here is the contract that fixes it: once HostVu is built, the storage of the table it was
// built from can be destroyed and reused, and every callback of iface() must still reach the functions
// of the original table.
//
// It needs no library: the table is fake, so the test also runs where build/vu is not there.
// Before the fix this program exits 1 with failed CHECKs (with the old aliasing code the callbacks
// reach the clobber's stubs, which answer different values); after it, it passes.

#include "check.h"
#include "vu_glue.h"

#include <cstdint>
#include <new>

namespace
{

// ---- the table the object is built from ----------------------------------------------------------
int original_open = 0;
int original_close = 0;
int original_micro = 0;
uint32_t original_micro_bytes = 0;
uint8_t original_data[64];

int original_version() { return DON_VU_VERSION; }
int original_open_ex(int, DonVuXgkick, void *)
{
    ++original_open;
    return 0;
}
void original_close_fn() { ++original_close; }
void original_reset() {}
uint8_t *original_data_fn() { return original_data; }
void original_write_micro(uint32_t, const uint8_t *, uint32_t bytes)
{
    ++original_micro;
    original_micro_bytes = bytes;
}
void original_set_top(uint32_t, uint32_t) {}
uint32_t original_execute(uint32_t, uint32_t) { return 7; }
uint32_t original_continue(uint32_t) { return 7; }
int original_running() { return 0; }
void original_set_regs(const void *, const void *) {}
void original_get_regs(void *, void *) {}
uint64_t original_total_cycles() { return 1234; }

// ---- the table that overwrites the same bytes afterwards ------------------------------------------
// Valid functions with different answers, so that the aliasing build fails a CHECK instead of jumping
// through a null pointer: a test has to report the defect, not crash on it.
int clobber_calls = 0;
int clobber_version() { return 0; } // not DON_VU_VERSION: unmistakable
int clobber_open_ex(int, DonVuXgkick, void *)
{
    ++clobber_calls;
    return 0;
}
void clobber_close() { ++clobber_calls; }
void clobber_reset() { ++clobber_calls; }
uint8_t *clobber_data()
{
    ++clobber_calls;
    return nullptr;
}
void clobber_write_micro(uint32_t, const uint8_t *, uint32_t) { ++clobber_calls; }
void clobber_set_top(uint32_t, uint32_t) { ++clobber_calls; }
uint32_t clobber_execute(uint32_t, uint32_t)
{
    ++clobber_calls;
    return 0;
}
uint32_t clobber_continue(uint32_t)
{
    ++clobber_calls;
    return 0;
}
int clobber_running()
{
    ++clobber_calls;
    return 0;
}
void clobber_set_regs(const void *, const void *) { ++clobber_calls; }
void clobber_get_regs(void *, void *) { ++clobber_calls; }
uint64_t clobber_total_cycles()
{
    ++clobber_calls;
    return 0;
}

// Only the function pointers are set: `library` and `path` keep their default state (no memset over a
// std::filesystem::path).
void fill_original(DonVuApi &api)
{
    api.version = &original_version;
    api.open_ex = &original_open_ex;
    api.close = &original_close_fn;
    api.reset = &original_reset;
    api.data = &original_data_fn;
    api.write_micro = &original_write_micro;
    api.set_top = &original_set_top;
    api.execute = &original_execute;
    api.continue_ = &original_continue;
    api.running = &original_running;
    api.set_regs = &original_set_regs;
    api.get_regs = &original_get_regs;
    api.total_cycles = &original_total_cycles;
}

void fill_clobber(DonVuApi &api)
{
    api.version = &clobber_version;
    api.open_ex = &clobber_open_ex;
    api.close = &clobber_close;
    api.reset = &clobber_reset;
    api.data = &clobber_data;
    api.write_micro = &clobber_write_micro;
    api.set_top = &clobber_set_top;
    api.execute = &clobber_execute;
    api.continue_ = &clobber_continue;
    api.running = &clobber_running;
    api.set_regs = &clobber_set_regs;
    api.get_regs = &clobber_get_regs;
    api.total_cycles = &clobber_total_cycles;
}

void sink(void *, const uint8_t *, uint32_t) {}

} // namespace

int main()
{
    // The storage the host program's block would leave behind, and the table built from it.
    alignas(DonVuApi) unsigned char storage[sizeof(DonVuApi)];
    DonVuApi *table = new (storage) DonVuApi();
    fill_original(*table);

    HostVu vu(*table, DON_VU_ENGINE_MICROVU);

    // The block ends: the table's lifetime is over, and its bytes are reused (exactly what the audio
    // path did to the host program's stack).
    table->~DonVuApi();
    DonVuApi *clobber = new (storage) DonVuApi();
    fill_clobber(*clobber);

    // The whole runtime sequence, from the table the object holds.
    const Ps2HostVu1 &iface = vu.iface();
    iface.thread_begin(iface.user, &sink, nullptr);
    CHECK_EQ(original_open, 1);
    CHECK_EQ(clobber_calls, 0);
    CHECK(vu.is_open());
    CHECK_EQ(vu.error().empty(), true);

    CHECK(iface.data(iface.user) == original_data);
    CHECK_EQ(clobber_calls, 0);

    const uint8_t payload[16] = {};
    iface.write_micro(iface.user, 8, payload, sizeof(payload));
    CHECK_EQ(original_micro, 1);
    CHECK_EQ(original_micro_bytes, static_cast<uint32_t>(sizeof(payload)));
    CHECK_EQ(clobber_calls, 0);

    iface.set_top(iface.user, 3, 4);
    CHECK_EQ(iface.execute(iface.user, 0, 65536), 7u);
    CHECK_EQ(iface.continue_(iface.user, 65536), 7u);
    CHECK_EQ(iface.running(iface.user), 0);
    iface.reset(iface.user);
    iface.set_regs(iface.user, nullptr, nullptr);
    iface.get_regs(iface.user, nullptr, nullptr);
    CHECK_EQ(clobber_calls, 0);

    iface.thread_end(iface.user);
    CHECK_EQ(original_close, 1);
    CHECK_EQ(clobber_calls, 0);
    CHECK(!vu.is_open());

    // The source table is still intact and unused: the object answered from its own copy.
    clobber->~DonVuApi();

    return check_result("test_vu_glue_lifetime");
}
