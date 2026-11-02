// Network-folder check on Windows, twin of fs_linux.cpp (task T-733): a UNC path, or a path on a drive letter that
// GetDriveTypeW reports as DRIVE_REMOTE (a mapped network drive).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwctype>
#include <string>
#include <system_error>

#include "../discover.h"

namespace don_setup {

bool is_network_filesystem(const std::filesystem::path& dir) {
    std::error_code ec;
    std::wstring p = std::filesystem::absolute(dir, ec).native();
    if (ec) return false;
    // Extended-length prefixes: \\?\UNC\server\share is a network path, \\?\C:\... is the local drive C:.
    const std::wstring kExtUnc = L"\\\\?\\UNC\\", kExtended = L"\\\\?\\", kDevice = L"\\\\.\\";
    if (p.rfind(kExtUnc, 0) == 0) return true;
    if (p.rfind(kExtended, 0) == 0) p.erase(0, kExtended.size());
    else if (p.rfind(kDevice, 0) == 0) p.erase(0, kDevice.size());
    else if (p.rfind(L"\\\\", 0) == 0 || p.rfind(L"//", 0) == 0) return true;  // \\server\share\...
    if (p.size() >= 2 && p[1] == L':' && std::iswalpha(p[0])) {
        const wchar_t root[4] = {p[0], L':', L'\\', 0};
        return GetDriveTypeW(root) == DRIVE_REMOTE;
    }
    return false;
}

}  // namespace don_setup
