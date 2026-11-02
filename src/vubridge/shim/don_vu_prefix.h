// SPDX-License-Identifier: GPL-3.0-or-later
//
// Forced include for PCSX2's VU sources (x86/microVU.cpp, VUops.cpp, VUflags.cpp,
// VU1microInterp.cpp, VU0microInterp.cpp) and for this library's own sources.
//
// It first includes every PCSX2 header those files use (all of them are #pragma once, so the
// sources' own #includes become no-ops), then redirects three emulator subsystems to the shims in
// don_vu_shim.h by macro:
//   gifUnit   -> don_vu_gifUnit   (XGKICK: GIF packets go to the host callback, not to the GIF unit / MTGS)
//   EmuConfig -> don_vu_EmuConfig (only the VU settings; filled from the open() arguments)
//   vu1Thread -> don_vu_vu1Thread (MTVU is compiled out: THREAD_VU1 is forced to false)
// Because the macros are defined after the headers were parsed, the PCSX2 declarations keep their
// original types and only the code in the VU sources (and microVU_*.inl) is redirected.
//
// Adapted from KillzoneRecomp, https://github.com/drakolordx7/KillzoneRecomp, commit
// 5f0700079f9ac4e14799d66d01747dbcdcec55f6, ext/kzvu/shim/include/kzvu_prefix.h (GPL-3.0-or-later).
// Dropped: the global renames (this library hides every symbol and shares a process with the GS
// library only through its C API) and the per-thread VU0 alias (one thread, no VU0 here).
#pragma once

#include "PrecompiledHeader.h"

#include "Common.h"
#include "Config.h"
#include "GS.h"
#include "Gif.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"
#include "VU.h"
#include "VUflags.h"
#include "VUmicro.h"
#include "VUops.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "x86/iR5900.h"
#include "GS/MultiISA.h"

#include "common/AlignedMalloc.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Perf.h"
#include "common/StringUtil.h"
#include "common/emitter/x86emitter.h"

#include "don_vu_shim.h"

#undef THREAD_VU1
#define THREAD_VU1 false

#define EmuConfig don_vu_EmuConfig
#define gifUnit don_vu_gifUnit
#define vu1Thread don_vu_vu1Thread
