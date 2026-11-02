// SPDX-License-Identifier: GPL-3.0-or-later
// Private interface between don_gs.cpp and don_gs_host.cpp (which compiles PCSX2's gsrunner Host
// implementation into the library). Not part of the public API.
#pragma once

#include <optional>
#include <string>

#include "common/WindowInfo.h"

namespace DonGsHost
{
/// Sets up PCSX2's folders, settings layers, fonts and logging once per process. Returns false
/// and fills `err` on failure. Safe to call again after the GS was closed.
bool Initialize(std::string& err);

/// The window the GS will draw into when it asks the host for one. nullopt = headless.
void SetWindow(const std::optional<WindowInfo>& window);

/// Last error-level message PCSX2 logged since the previous call (cleared on read).
std::string TakeLastError();

/// Releases the shared cache lock and removes the private fallback cache, if one was used. Call
/// when the GS closes so a reopen re-contends for the shared cache (and nothing accumulates).
void ReleaseCacheLock();

/// Directory of libdon_gs, empty if it cannot be found.
std::string LibraryDirectory();
} // namespace DonGsHost
