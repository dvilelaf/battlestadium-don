// The one place that says, per platform, how the game module and the toolchain that builds it are called (M7, T-730):
// the module recipe (module_build.*) uses it now; the loader, startup and the manifest switch to it later, so that
// "libdon_game.so" and "don_game.dll" are never spelled out anywhere else.
// The platform is a parameter (defaulting to the one this program runs on) so that both twins are testable on any host.
#pragma once

namespace don_setup {

enum class ModulePlatform { Linux, Windows };

#ifdef _WIN32
constexpr ModulePlatform kHostModulePlatform = ModulePlatform::Windows;
#else
constexpr ModulePlatform kHostModulePlatform = ModulePlatform::Linux;
#endif

// File name of the game module inside module/<id>/.
constexpr const char* module_file_name(ModulePlatform p = kHostModulePlatform) {
    return p == ModulePlatform::Windows ? "don_game.dll" : "libdon_game.so";
}

// File names under <toolchain>/bin/. On Windows the linker is lld in its MSVC-link flavour, which LLVM picks from the
// program name (lld-link), exactly as ld.lld selects the ELF flavour on Linux.
constexpr const char* compiler_file_name(ModulePlatform p = kHostModulePlatform) {
    return p == ModulePlatform::Windows ? "clang++.exe" : "clang++";
}
constexpr const char* linker_file_name(ModulePlatform p = kHostModulePlatform) {
    return p == ModulePlatform::Windows ? "lld-link.exe" : "ld.lld";
}

// The target triple the compiler gets for a Windows module; Linux builds for the host and passes none.
constexpr const char* module_target_triple(ModulePlatform p = kHostModulePlatform) {
    return p == ModulePlatform::Windows ? "x86_64-pc-windows-msvc" : "";
}

// The symbol the loader looks up in the module (src/host/game_module.h).
constexpr const char* kModuleEntrySymbol = "don_game_module_v1";

}  // namespace don_setup
