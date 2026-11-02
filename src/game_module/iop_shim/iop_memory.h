// SPDX-License-Identifier: GPL-3.0-or-later
// Stands in for ps2xIOP/src/emulator/core/iop_memory.h in a game module: ram().data() and write32() are all the
// translated code uses (src/game_module/don_iop_abi.h).
#pragma once
#include "../don_iop_abi.h"

namespace ps2x::iop::detail
{
using IopMemory = DonIopMemory;
}
