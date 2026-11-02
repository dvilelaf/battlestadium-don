// SPDX-License-Identifier: GPL-3.0-or-later
// Stands in for ps2xIOP/src/emulator/core/iop_cpu.h when the translated IOP module is built into a game module: the
// same name in the same namespace, backed by the ABI struct (src/game_module/don_iop_abi.h).
#pragma once
#include "../don_iop_abi.h"

namespace ps2x::iop::detail
{
using IopCpuState = DonIopCpuState;
}
