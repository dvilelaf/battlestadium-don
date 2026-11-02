// Adapted from https://github.com/Fukami-SoftwareHub/RRV-Recomp (commit e8818121566cfaf3d361e38745860b0a9438e27b),
// tools/pcsx2-spu2/shim/common/Console.h, GPL-3.0-or-later. Renamed rrv_ -> don_ and RRV -> DON; comments adjusted accordingly.
// SPDX-License-Identifier: GPL-3.0+
// DON shim for PCSX2 common/Console.h (2.8.2): logging sinks are no-ops.
#pragma once

struct DonNullConsole
{
	template <typename... A> void WriteLn(A&&...) const {}
	template <typename... A> void Warning(A&&...) const {}
	template <typename... A> void Error(A&&...) const {}
	template <typename... A> void Write(A&&...) const {}
	template <typename... A> void WriteLnFmt(A&&...) const {}
	template <typename... A> void WarningFmt(A&&...) const {}
	template <typename... A> void ErrorFmt(A&&...) const {}
};

inline constexpr DonNullConsole Console{};
inline constexpr DonNullConsole DevCon{};
