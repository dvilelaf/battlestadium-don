// SPDX-License-Identifier: GPL-3.0-or-later
//
// The C API of libdon_gs (see don_gs.h) on top of PCSX2's GS (v2.8.2).
//
// The model (how the privileged registers reach the GS, how a vsync is delivered, the GIF
// being fed synchronously) follows the PCSX2 GS bridge of the Ridge Racer V recompilation
// project (https://github.com/Fukami-SoftwareHub/RRV-Recomp, tools/pcsx2-gs-bridge/bridge.cpp,
// GPL-3.0-or-later). The code is written for this project and carries nothing specific to
// that game.
#include "don_gs.h"

#include "don_gs_host.h"

#include "common/FPControl.h"
#include "common/WindowInfo.h"
#include "pcsx2/Config.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GS/Renderers/Common/GSFunctionMap.h"
#include "pcsx2/GS/Renderers/Common/GSRenderer.h"
#include "pcsx2/GS/Renderers/Vulkan/GSDeviceVK.h"
#include "pcsx2/SaveState.h"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
// Size of the block of privileged registers the GS reads through its `basemem` pointer.
constexpr size_t kPrivBytes = 0x2000;
constexpr int kSlotCount = DON_GS_PRIV_REG_COUNT;
// Offset of each register of don_gs_vsync's array inside that block (PCSX2's PS2MEM_GS layout).
constexpr std::array<size_t, kSlotCount> kPrivOffsets = {
	0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0,
	0x1000, 0x1010, 0x1040, 0x1080,
};
constexpr int kCsrSlot = 15;
constexpr int kSiglblidSlot = 18;
constexpr uint64_t kCsrField = 1ull << 13;
// Highest GS state version PCSX2 v2.8.2 reads and writes (STATE_VERSION).
constexpr uint32_t kMaxStateVersion = 9;
constexpr int kMaxUpscale = 8;

struct Bridge
{
	bool open = false;
	std::thread::id owner;
	bool has_window = false;
	std::vector<u8> priv = std::vector<u8>(kPrivBytes);
	uint64_t csr = 0;
	uint64_t siglblid = 0;
	bool displayed = false; // a vsync with a display circuit enabled happened
	std::vector<u8> scratch; // read_fifo staging buffer
	// Staging buffer for GIF data that arrives at an address that is not a multiple of 16: the GS
	// reads PACKED registers with aligned loads (GSVector4i::loadnt) and crashes otherwise.
	struct alignas(16) Quad
	{
		u64 lo, hi;
	};
	std::vector<Quad> aligned;
};

Bridge s_bridge;
std::atomic<uint32_t> s_log_count{0};

void LogLine(const char* format, ...)
{
	char text[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	std::fprintf(stderr, "[don-gs] %s\n", text);
}

// For void functions and per-frame calls: the first 16 messages, then only the power-of-two ones.
void LogLimited(const char* format, const char* function, const char* detail)
{
	const uint32_t n = ++s_log_count;
	if (n <= 16 || (n & (n - 1)) == 0)
		LogLine(format, function, detail);
}

int Fail(char* err, int err_len, int code, const char* format, ...)
{
	char text[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	LogLine("%s", text);
	if (err && err_len > 0)
	{
		std::strncpy(err, text, static_cast<size_t>(err_len) - 1);
		err[err_len - 1] = '\0';
	}
	return code;
}

// True when the GS is open and the caller is the thread that opened it.
bool Ready(const char* function)
{
	if (!s_bridge.open)
	{
		LogLimited("%s: the GS is not open%s", function, "");
		return false;
	}
	if (s_bridge.owner != std::this_thread::get_id())
	{
		LogLimited("%s: called from a thread other than the one that opened the GS%s", function, "");
		return false;
	}
	return true;
}

std::optional<WindowInfo> MakeWindow(const DonGsConfig& cfg)
{
	WindowInfo wi;
	switch (cfg.window_type)
	{
		case DON_GS_WINDOW_X11:
			wi.type = WindowInfo::Type::X11;
			wi.display_connection = cfg.native_display;
			wi.window_handle = cfg.native_window;
			break;
		case DON_GS_WINDOW_WAYLAND:
			wi.type = WindowInfo::Type::Wayland;
			wi.display_connection = cfg.native_display;
			wi.window_handle = cfg.native_window;
			break;
		case DON_GS_WINDOW_WIN32:
			wi.type = WindowInfo::Type::Win32;
			wi.window_handle = cfg.native_window;
			break;
		default:
			return std::nullopt;
	}
	wi.surface_width = static_cast<u32>(cfg.width);
	wi.surface_height = static_cast<u32>(cfg.height);
	wi.surface_scale = 1.0f;
	return wi;
}

// Validates the configuration; returns an empty string when it is fine.
std::string ValidateConfig(const DonGsConfig& cfg)
{
	if (cfg.renderer != DON_GS_RENDERER_VULKAN && cfg.renderer != DON_GS_RENDERER_SOFTWARE)
		return "invalid renderer " + std::to_string(cfg.renderer) + " (0 = Vulkan, 1 = software)";
	if (cfg.upscale < 1 || cfg.upscale > kMaxUpscale)
		return "invalid upscale " + std::to_string(cfg.upscale) + " (valid 1.." + std::to_string(kMaxUpscale) + ")";
	if (cfg.window_type < DON_GS_WINDOW_NONE || cfg.window_type > DON_GS_WINDOW_WIN32)
		return "invalid window_type " + std::to_string(cfg.window_type);
	if (cfg.window_type != DON_GS_WINDOW_NONE)
	{
		if (!cfg.native_window)
			return "window_type is set but native_window is NULL";
		if (cfg.window_type != DON_GS_WINDOW_WIN32 && !cfg.native_display)
			return "X11 and Wayland windows need native_display";
		if (cfg.width <= 0 || cfg.height <= 0)
			return "window width and height must be positive";
	}
	return {};
}
} // namespace

extern "C" int don_gs_version(void)
{
	return 1;
}

extern "C" int don_gs_open(const DonGsConfig* cfg_in, char* err, int err_len)
{
	if (err && err_len > 0)
		err[0] = '\0';
	if (s_bridge.open)
		return Fail(err, err_len, -1, "don_gs_open: the GS is already open");

	DonGsConfig cfg{};
	if (cfg_in)
		cfg = *cfg_in;
	else
		cfg.upscale = 1;
	if (cfg.upscale == 0)
		cfg.upscale = 1; // a zeroed struct means native resolution
	const std::string invalid = ValidateConfig(cfg);
	if (!invalid.empty())
		return Fail(err, err_len, -2, "don_gs_open: %s", invalid.c_str());

	std::string host_error;
	if (!DonGsHost::Initialize(host_error))
		return Fail(err, err_len, -3, "don_gs_open: %s", host_error.c_str());

	const std::optional<WindowInfo> window = MakeWindow(cfg);
	DonGsHost::SetWindow(window);
	DonGsHost::TakeLastError();

	const GSRendererType renderer =
		cfg.renderer == DON_GS_RENDERER_SOFTWARE ? GSRendererType::SW : GSRendererType::VK;
	// PCSX2's defaults, with nothing but the renderer, the scale and the vsync changed. The game has
	// no gsHWFixes in GameIndex.yaml (SLPS-25675), so there is no per-game hack to apply.
	Pcsx2Config::GSOptions options{};
	options.Renderer = renderer;
	options.UpscaleMultiplier = renderer == GSRendererType::SW ? 1.0f : static_cast<float>(cfg.upscale);
	options.VsyncEnable = window.has_value() && cfg.vsync != 0;

	std::memset(s_bridge.priv.data(), 0, kPrivBytes);
	s_bridge.csr = 0;
	s_bridge.siglblid = 0;
	s_bridge.displayed = false;

	bool ok;
	{
		FPControlRegisterBackup backup(FPControlRegister::GetDefault());
		ok = GSopen(options, renderer, s_bridge.priv.data(),
			window.has_value() && cfg.vsync ? GSVSyncMode::FIFO : GSVSyncMode::Disabled, false);
	}
	if (!ok)
	{
		DonGsHost::SetWindow(std::nullopt);
		const std::string reason = DonGsHost::TakeLastError();
		return Fail(err, err_len, -4, "don_gs_open: GSopen(%s) failed%s%s",
			renderer == GSRendererType::SW ? "software" : "Vulkan", reason.empty() ? "" : ": ", reason.c_str());
	}

	s_bridge.open = true;
	s_bridge.owner = std::this_thread::get_id();
	s_bridge.has_window = window.has_value();
	return 0;
}

extern "C" void don_gs_close(void)
{
	if (!s_bridge.open)
		return;
	if (s_bridge.owner != std::this_thread::get_id())
	{
		LogLimited("%s: called from a thread other than the one that opened the GS%s", "don_gs_close", "");
		return;
	}
	{
		FPControlRegisterBackup backup(FPControlRegister::GetDefault());
		GSclose();
	}
	DonGsHost::ReleaseCacheLock();
	GSCodeReserve::ReleaseMemory();
	DonGsHost::SetWindow(std::nullopt);
	s_bridge.open = false;
	s_bridge.has_window = false;
	s_bridge.displayed = false;
	s_bridge.scratch.clear();
	s_bridge.scratch.shrink_to_fit();
	s_bridge.aligned.clear();
	s_bridge.aligned.shrink_to_fit();
}

extern "C" void don_gs_reset(void)
{
	if (!Ready("don_gs_reset"))
		return;
	FPControlRegisterBackup backup(FPControlRegister::GetDefault());
	GSreset(false);
}

extern "C" void don_gs_gif_transfer(int path, const uint8_t* data, uint32_t bytes)
{
	if (!Ready("don_gs_gif_transfer"))
		return;
	if (path < 1 || path > 3)
	{
		LogLimited("%s: invalid GIF path %s", "don_gs_gif_transfer", std::to_string(path).c_str());
		return;
	}
	if (bytes == 0)
		return;
	if (!data || (bytes & 15) != 0)
	{
		LogLimited("%s: %s", "don_gs_gif_transfer", !data ? "NULL data" : "size is not a multiple of 16");
		return;
	}
	if (reinterpret_cast<uintptr_t>(data) & 15)
	{
		s_bridge.aligned.resize(bytes / 16);
		std::memcpy(s_bridge.aligned.data(), data, bytes);
		data = reinterpret_cast<const uint8_t*>(s_bridge.aligned.data());
	}
	FPControlRegisterBackup backup(FPControlRegister::GetDefault());
	GSgifTransfer(data, bytes / 16);
}

extern "C" void don_gs_vsync(const uint64_t priv_regs[19], int field)
{
	if (!Ready("don_gs_vsync"))
		return;
	if (!priv_regs)
	{
		LogLimited("%s: %s", "don_gs_vsync", "NULL priv_regs");
		return;
	}
	for (int i = 0; i < kSlotCount; i++)
		std::memcpy(s_bridge.priv.data() + kPrivOffsets[i], &priv_regs[i], sizeof(uint64_t));

	// CSR.FIELD is the caller's field argument; PCSX2's GSvsync takes the inverse of it.
	uint64_t csr = priv_regs[kCsrSlot] & ~kCsrField;
	if (field & 1)
		csr |= kCsrField;
	std::memcpy(s_bridge.priv.data() + kPrivOffsets[kCsrSlot], &csr, sizeof(csr));
	s_bridge.csr = csr;
	s_bridge.siglblid = priv_regs[kSiglblidSlot];
	// PMODE.EN1 | PMODE.EN2: at least one display circuit reads a frame buffer.
	if (priv_regs[0] & 3)
		s_bridge.displayed = true;

	FPControlRegisterBackup backup(FPControlRegister::GetDefault());
	GSvsync((csr & kCsrField) ? 0 : 1, true);
}

extern "C" void don_gs_read_signals(uint64_t* csr, uint64_t* siglblid)
{
	if (csr)
		*csr = s_bridge.open ? s_bridge.csr : 0;
	if (siglblid)
		*siglblid = s_bridge.open ? s_bridge.siglblid : 0;
}

extern "C" uint32_t don_gs_read_fifo(uint8_t* dst, uint32_t bytes)
{
	if (!Ready("don_gs_read_fifo"))
		return 0;
	if (!dst || bytes == 0 || (bytes & 15) != 0)
	{
		LogLimited("%s: %s", "don_gs_read_fifo", "bad arguments (dst NULL or bytes not a non-zero multiple of 16)");
		return 0;
	}
	if (!g_gs_renderer || g_gs_renderer->m_env.TRXDIR.XDIR != 1)
	{
		LogLimited("%s: %s", "don_gs_read_fifo", "no GS to CPU transfer is active (TRXDIR.XDIR != 1)");
		return 0;
	}
	// PCSX2 may write one quadword more than asked for at the end of a transfer.
	s_bridge.scratch.assign(static_cast<size_t>(bytes) + 16, 0);
	FPControlRegisterBackup backup(FPControlRegister::GetDefault());
	GSInitAndReadFIFO(s_bridge.scratch.data(), bytes / 16);
	std::memcpy(dst, s_bridge.scratch.data(), bytes);
	return bytes;
}

extern "C" int don_gs_load_state(const uint8_t* data, uint32_t bytes)
{
	if (!Ready("don_gs_load_state"))
		return -1;
	if (!data || bytes < 4)
	{
		LogLimited("%s: %s", "don_gs_load_state", "NULL data or fewer than 4 bytes");
		return -2;
	}
	uint32_t version;
	std::memcpy(&version, data, sizeof(version));
	if (version == 0 || version > kMaxStateVersion)
	{
		LogLimited("%s: %s", "don_gs_load_state", "unknown state version");
		return -3;
	}
	// Defrost reads in place and never writes, but its interface wants a mutable pointer.
	freezeData fd{static_cast<int>(bytes), const_cast<u8*>(data)};
	FPControlRegisterBackup backup(FPControlRegister::GetDefault());
	const int result = GSfreeze(FreezeAction::Load, &fd);
	if (result != 0)
	{
		LogLimited("%s: %s", "don_gs_load_state", "the GS rejected the state (too short for its version?)");
		return -4;
	}
	return 0;
}

extern "C" int don_gs_save_state(uint8_t* dst, int max_bytes)
{
	if (!Ready("don_gs_save_state"))
		return -1;
	freezeData fd{0, nullptr};
	FPControlRegisterBackup backup(FPControlRegister::GetDefault());
	GSfreeze(FreezeAction::Size, &fd);
	if (!dst)
		return fd.size;
	if (max_bytes < fd.size)
	{
		LogLimited("%s: %s", "don_gs_save_state", "buffer smaller than the state");
		return -2;
	}
	fd.data = dst;
	// The hardware renderer keeps drawn pixels in GPU targets; a state is only complete once they are
	// written back into the local memory that is saved (PCSX2 does the same for GS dumps).
	if (g_gs_renderer)
		g_gs_renderer->ReadbackTextureCache();
	if (GSfreeze(FreezeAction::Save, &fd) != 0)
		return -3;
	return fd.size;
}

extern "C" int don_gs_snapshot_rgba(uint8_t* dst, int* width, int* height, int max_bytes)
{
	if (!Ready("don_gs_snapshot_rgba"))
		return -1;
	if (!s_bridge.displayed)
		return -2;
	u32 w = 0, h = 0;
	std::vector<u32> pixels;
	{
		FPControlRegisterBackup backup(FPControlRegister::GetDefault());
		if (!GSSaveSnapshotToMemory(0, 0, false, true, &w, &h, &pixels) || w == 0 || h == 0)
			return -2;
	}
	if (width)
		*width = static_cast<int>(w);
	if (height)
		*height = static_cast<int>(h);
	const size_t size = static_cast<size_t>(w) * h * 4;
	if (!dst || max_bytes < 0 || size > static_cast<size_t>(max_bytes))
		return -3;
	// PCSX2 hands back little-endian 0xAABBGGRR words, that is R, G, B, A in memory.
	std::memcpy(dst, pixels.data(), size);
	for (size_t i = 3; i < size; i += 4)
		dst[i] = 255;
	return 0;
}

extern "C" void don_gs_resize(int width, int height)
{
	if (!Ready("don_gs_resize") || !s_bridge.has_window)
		return;
	if (width <= 0 || height <= 0)
	{
		LogLimited("%s: %s", "don_gs_resize", "width and height must be positive");
		return;
	}
	FPControlRegisterBackup backup(FPControlRegister::GetDefault());
	GSResizeDisplayWindow(static_cast<u32>(width), static_cast<u32>(height), 1.0f);
}

namespace
{
	const char* DeviceTypeWord(VkPhysicalDeviceType type)
	{
		switch (type)
		{
			case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
			case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
			case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
			case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
			default: return "other";
		}
	}
} // namespace

extern "C" int don_gs_device_info(char* buf, int buf_len)
{
	if (!buf || buf_len <= 0)
		return -1;
	buf[0] = '\0';
	if (!s_bridge.open)
		return -2;
	if (s_bridge.owner != std::this_thread::get_id())
		return -3;

	if (GSGetCurrentRenderer() != GSRendererType::VK || !g_gs_device ||
		g_gs_device->GetRenderAPI() != RenderAPI::Vulkan)
	{
		std::snprintf(buf, static_cast<size_t>(buf_len), "software");
		return 0;
	}

	const std::string picked = g_gs_device->GetName();
	std::string out = "vulkan\n";
	const GSDeviceVK::GPUList gpus = GSDeviceVK::EnumerateGPUs();
	for (const auto& [device, ai] : gpus)
	{
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(device, &props);
		char line[512];
		std::snprintf(line, sizeof(line), "%s|%s|0x%04X|%u.%u.%u|%u.%u.%u|%d\n",
			ai.name.c_str(), DeviceTypeWord(props.deviceType), props.vendorID,
			VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion),
			VK_VERSION_PATCH(props.driverVersion),
			VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
			VK_API_VERSION_PATCH(props.apiVersion), ai.name == picked ? 1 : 0);
		out += line;
	}
	std::strncpy(buf, out.c_str(), static_cast<size_t>(buf_len) - 1);
	buf[buf_len - 1] = '\0';
	return 0;
}
