// SPDX-License-Identifier: GPL-3.0-or-later
// The pure part of the VU1 glue: the parser of DON_VU1 (src/host/vu_glue.h). No library, no game.
//
// The names are the ones the card of T-205 proposes, plus "interp", which the card itself uses for the
// reference mode (the Done when of T-205 runs the game with DON_VU1=interp): runtime and interp both
// mean "do not install a host VU1, use the runtime's own interpreter".
#include "check.h"
#include "vu_glue.h"

#include <cstdio>
#include <cstring>

int main()
{
    // Unset and empty: microVU is the default (the runtime's interpreter gives up on the menus).
    CHECK_EQ(don_vu1_selection_from_string(nullptr), DON_VU1_MICROVU);
    CHECK_EQ(don_vu1_selection_from_string(""), DON_VU1_MICROVU);

    // The three names.
    CHECK_EQ(don_vu1_selection_from_string("runtime"), DON_VU1_RUNTIME);
    CHECK_EQ(don_vu1_selection_from_string("microvu"), DON_VU1_MICROVU);
    CHECK_EQ(don_vu1_selection_from_string("pcsx2interp"), DON_VU1_PCSX2INTERP);

    // "interp" is the name the card uses for the runtime's interpreter, not for PCSX2's.
    CHECK_EQ(don_vu1_selection_from_string("interp"), DON_VU1_RUNTIME);

    // Case-insensitive: an environment variable typed in capitals still works.
    CHECK_EQ(don_vu1_selection_from_string("MicroVU"), DON_VU1_MICROVU);
    CHECK_EQ(don_vu1_selection_from_string("RUNTIME"), DON_VU1_RUNTIME);
    CHECK_EQ(don_vu1_selection_from_string("PCSX2Interp"), DON_VU1_PCSX2INTERP);

    // A prefix, a suffix, a space and a wrong name are unknown, not silently something else.
    CHECK_EQ(don_vu1_selection_from_string("micro"), DON_VU1_UNKNOWN);
    CHECK_EQ(don_vu1_selection_from_string("microvux"), DON_VU1_UNKNOWN);
    CHECK_EQ(don_vu1_selection_from_string(" microvu"), DON_VU1_UNKNOWN);
    CHECK_EQ(don_vu1_selection_from_string("interp2"), DON_VU1_UNKNOWN);

    // Names for the messages.
    CHECK(std::strcmp(don_vu1_selection_name(DON_VU1_RUNTIME), "runtime") == 0);
    CHECK(std::strcmp(don_vu1_selection_name(DON_VU1_MICROVU), "microvu") == 0);
    CHECK(std::strcmp(don_vu1_selection_name(DON_VU1_PCSX2INTERP), "pcsx2interp") == 0);
    CHECK(std::strcmp(don_vu1_selection_name(DON_VU1_UNKNOWN), "unknown") == 0);

    // The engine of the library, or -1 when the runtime keeps its own interpreter.
    CHECK_EQ(don_vu1_engine_for_selection(DON_VU1_MICROVU), DON_VU_ENGINE_MICROVU);
    CHECK_EQ(don_vu1_engine_for_selection(DON_VU1_PCSX2INTERP), DON_VU_ENGINE_INTERPRETER);
    CHECK_EQ(don_vu1_engine_for_selection(DON_VU1_RUNTIME), -1);
    CHECK_EQ(don_vu1_engine_for_selection(DON_VU1_UNKNOWN), -1);
    CHECK(don_vu1_engine_for_selection(DON_VU1_MICROVU) != don_vu1_engine_for_selection(DON_VU1_PCSX2INTERP));

    return check_result("test_vu_select");
}
