// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host:: functions that PCSX2's GS and device code need at link time. The minimal
// implementation of PCSX2's own `pcsx2-gsrunner/Main.cpp` (GPL-3.0-or-later) is compiled into
// this library, with its render window functions and `main` renamed away; the window the GS
// draws into is supplied by this project instead (see Host::AcquireRenderWindow below).
#include <optional>

// Keep the original declarations visible: Main.cpp includes these headers too, and their include
// guards then stop the renaming macros below from changing the declarations.
#include "pcsx2/Host.h"
#include "pcsx2/GS/GS.h"

#define AcquireRenderWindow don_gsrunner_unused_acquire_render_window
#define ReleaseRenderWindow don_gsrunner_unused_release_render_window
#define main don_gsrunner_unused_main
namespace Host
{
std::optional<WindowInfo> don_gsrunner_unused_acquire_render_window(bool recreate_window);
void don_gsrunner_unused_release_render_window();
} // namespace Host
#include "pcsx2-gsrunner/Main.cpp"
#undef main
#undef ReleaseRenderWindow
#undef AcquireRenderWindow

// ---------------------------------------------------------------------------------------------
// The part that belongs to this project: Host::AcquireRenderWindow returns the window the API
// set, and DonGsHost::Initialize is GSRunner::InitializeConfig with the folders redirected to the
// library's own directory (nothing is read from or written to the user's PCSX2 profile).
// ---------------------------------------------------------------------------------------------
#include "don_gs_host.h"

#if defined(_WIN32)
#include "common/RedtapeWindows.h"
#include "common/StringUtil.h"
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace
{
std::optional<WindowInfo> s_don_window;
std::string s_don_last_error;
bool s_don_stderr_log = false;
bool s_don_initialized = false;
MemorySettingsInterface s_don_settings;

// Only one process may own the shared Vulkan shader/pipeline cache at a time
// (DON_GS_DATA_DIR/cache). Two instances started together opened the same
// vulkan_shaders.{bin,idx} and vulkan_pipelines.bin at once: PCSX2 truncates and rewrites those
// files, and two GSDeviceVKs raced on one GPU (journal 040). The first process to reach here keeps
// the shared cache; every other process falls back to a private per-process cache that is removed
// when it closes the GS.
#if defined(_WIN32)
static HANDLE s_cache_lock_handle = INVALID_HANDLE_VALUE;
#else
static int s_cache_lock_fd = -1;
#endif
static bool s_private_cache = false;
static std::string s_shared_cache_dir;

std::string ProcessIdString()
{
#if defined(_WIN32)
	return std::to_string(::GetCurrentProcessId());
#else
	return std::to_string(::getpid());
#endif
}

bool HoldsSharedCacheLock()
{
#if defined(_WIN32)
	return s_cache_lock_handle != INVALID_HANDLE_VALUE;
#else
	return s_cache_lock_fd >= 0;
#endif
}

bool TryTakeSharedCacheLock(const std::string& dir)
{
	const std::string lock_path = Path::Combine(dir, ".don-gs.lock");
#if defined(_WIN32)
	const std::wstring wide = StringUtil::UTF8StringToWideString(lock_path);
	HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	OVERLAPPED ov = {};
	if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov))
	{
		CloseHandle(h);
		return false;
	}
	s_cache_lock_handle = h;
	return true;
#else
	const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;
	if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
	{
		::close(fd);
		return false;
	}
	s_cache_lock_fd = fd;
	return true;
#endif
}

void ReleaseSharedCacheLock()
{
#if defined(_WIN32)
	if (s_cache_lock_handle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(s_cache_lock_handle);
		s_cache_lock_handle = INVALID_HANDLE_VALUE;
	}
#else
	if (s_cache_lock_fd >= 0)
	{
		::close(s_cache_lock_fd);
		s_cache_lock_fd = -1;
	}
#endif
}

bool EnsureOwnCache(std::string& err)
{
	// Remember the real shared cache before it is possibly redirected, so a reopen re-contends.
	if (s_shared_cache_dir.empty())
		s_shared_cache_dir = EmuFolders::Cache;

	if (!HoldsSharedCacheLock() && !s_private_cache)
	{
		if (!FileSystem::EnsureDirectoryExists(s_shared_cache_dir.c_str(), true))
		{
			err = "GS cache directory setup failed: " + s_shared_cache_dir;
			return false;
		}
		if (TryTakeSharedCacheLock(s_shared_cache_dir))
		{
			EmuFolders::Cache = s_shared_cache_dir;
		}
		else
		{
			s_private_cache = true;
			EmuFolders::Cache = s_shared_cache_dir + ".don-gs-" + ProcessIdString();
		}
	}

	if (!FileSystem::EnsureDirectoryExists(EmuFolders::Cache.c_str(), true))
	{
		err = "GS cache directory setup failed: " + EmuFolders::Cache;
		return false;
	}
	return true;
}

void DonLogCallback(LOGLEVEL level, ConsoleColors color, std::string_view message)
{
	if (level == LOGLEVEL_ERROR)
		s_don_last_error.assign(message.data(), message.size());
	// PCSX2's errors and warnings always reach the log (a texture that could not be made, a lost device):
	// a player's don.log is the only report there is. At most 200 of them per run unless DON_GS_LOG=1, which
	// also adds everything else; a warning repeated every frame must not fill don.log.
	static int s_reported = 0;
	if (s_don_stderr_log)
		std::fprintf(stderr, "[don-gs] pcsx2: %.*s\n", static_cast<int>(message.size()), message.data());
	else if (level <= LOGLEVEL_WARNING && s_reported < 200)
	{
		std::fprintf(stderr, "[don-gs] pcsx2 %s: %.*s\n", level == LOGLEVEL_ERROR ? "error" : "warning",
			static_cast<int>(message.size()), message.data());
		if (++s_reported == 200)
			std::fprintf(stderr, "[don-gs] pcsx2: 200 errors and warnings reported; the rest of this run's are not\n");
	}
}
} // namespace

std::string DonGsHost::LibraryDirectory()
{
#if defined(_WIN32)
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&DonGsHost::LibraryDirectory), &module))
		return {};
	wchar_t path[32768];
	const DWORD length = GetModuleFileNameW(module, path, 32768);
	if (length == 0 || length >= 32768)
		return {};
	return std::string(Path::GetDirectory(Path::Canonicalize(StringUtil::WideStringToUTF8String(path))));
#else
	Dl_info info{};
	if (dladdr(reinterpret_cast<const void*>(&DonGsHost::LibraryDirectory), &info) == 0 || !info.dli_fname)
		return {};
	return std::string(Path::GetDirectory(Path::Canonicalize(info.dli_fname)));
#endif
}

bool DonGsHost::Initialize(std::string& err)
{
	// The log sink is (re)installed on every open; the rest of the setup happens once.
	const char* log_env = std::getenv("DON_GS_LOG");
	s_don_stderr_log = log_env && log_env[0] == '1';
	Log::SetHostOutputLevel(s_don_stderr_log ? LOGLEVEL_DEBUG : LOGLEVEL_WARNING, &DonLogCallback);
	if (s_don_initialized)
		return EnsureOwnCache(err); // reopen: re-contend for the cache and recreate the directory

	const std::string dir = LibraryDirectory();
	if (dir.empty())
	{
		err = "cannot find the directory of libdon_gs";
		return false;
	}

	EmuFolders::SetAppRoot();
	EmuFolders::Resources = Path::Combine(dir, "resources");
	if (!FileSystem::DirectoryExists(EmuFolders::Resources.c_str()))
	{
		err = "PCSX2 resources directory not found: " + EmuFolders::Resources;
		return false;
	}
	const char* data_env = std::getenv("DON_GS_DATA_DIR");
	EmuConfig.CustomDataPath = (data_env && data_env[0]) ? std::string(data_env) : Path::Combine(dir, "data");
	if (!FileSystem::EnsureDirectoryExists(EmuConfig.CustomDataPath.c_str(), true) ||
		!EmuFolders::SetDataDirectory(nullptr))
	{
		err = "PCSX2 data directory setup failed: " + EmuConfig.CustomDataPath;
		return false;
	}
	EmuFolders::UserResources = EmuFolders::Resources;

	const char* hardware_error = nullptr;
	if (!VMManager::PerformEarlyHardwareChecks(&hardware_error))
	{
		err = hardware_error ? hardware_error : "PCSX2 early hardware checks failed";
		return false;
	}

	Host::Internal::SetBaseSettingsLayer(&s_don_settings);
	VMManager::SetDefaultSettings(s_don_settings, true, true, true, true, true);
	s_don_settings.SetBoolValue("Logging", "EnableFileLogging", false);
	s_don_settings.SetBoolValue("Logging", "EnableSystemConsole", false);
	VMManager::Internal::LoadStartupSettings();
	if (!EnsureOwnCache(err))
		return false;
	Log::SetConsoleOutputLevel(LOGLEVEL_NONE);
	Log::SetDebugOutputLevel(LOGLEVEL_NONE);
	Log::SetFileOutputLevel(LOGLEVEL_NONE, std::string());
	Log::SetHostOutputLevel(s_don_stderr_log ? LOGLEVEL_DEBUG : LOGLEVEL_WARNING, &DonLogCallback);

	const std::string roboto_path =
		EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
	const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
	if (roboto_data.empty())
	{
		err = "PCSX2 font not found: " + roboto_path;
		return false;
	}
	std::vector<ImGuiManager::FontInfo> fonts;
	ImGuiManager::FontInfo font{};
	font.data = roboto_data;
	fonts.push_back(font);
	ImGuiManager::SetFonts(std::move(fonts));

	s_don_initialized = true;
	return true;
}

void DonGsHost::SetWindow(const std::optional<WindowInfo>& window)
{
	s_don_window = window;
}

std::string DonGsHost::TakeLastError()
{
	std::string out;
	out.swap(s_don_last_error);
	return out;
}

void DonGsHost::ReleaseCacheLock()
{
	if (s_private_cache && !s_shared_cache_dir.empty())
	{
		// EmuFolders::Cache points at the private fallback cache; remove it so the next concurrent
		// instance's leftovers do not accumulate. A later reopen re-contends for the shared cache.
		FileSystem::RecursiveDeleteDirectory(EmuFolders::Cache.c_str());
		s_private_cache = false;
	}
	ReleaseSharedCacheLock();
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	if (s_don_window.has_value())
		return s_don_window;
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void Host::ReleaseRenderWindow()
{
	// The window belongs to the caller of don_gs_open.
}
