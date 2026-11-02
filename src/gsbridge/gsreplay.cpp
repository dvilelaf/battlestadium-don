// SPDX-License-Identifier: GPL-3.0-or-later
//
// gsreplay: command line tool that drives libdon_gs through its C API only (no PCSX2 headers).
//
//   gsreplay --synthetic <out.png> [--renderer software|vulkan] [--width W --height H]
//            [--state-roundtrip] [--reopen]
//
// --synthetic builds a tiny GIF stream by hand (no game data): it clears the frame buffer with a
// one-colour sprite, draws a rectangle of another colour, sends a vsync, takes the snapshot
// through the API and writes it as a PNG. The two colours and the rectangle are the constants
// below; tests/gs/test_gs_synthetic.py checks them.
//
//   --state-roundtrip  also tests don_gs_save_state / don_gs_load_state: saves the state, paints
//                      the whole screen another colour, loads the state back and checks that the
//                      picture is the first one again.
//   --reopen           also closes the GS and opens it again, checks that the picture is the same.
//
//   gsreplay --dump <file.gs|file.gs.zst> <out.png> [--renderer software|vulkan] [--frame N]
//
// --dump replays a PCSX2 GS dump (format: docs/knowledge/15-gs-dumps.md): loads its state block
// with don_gs_load_state, feeds the privileged registers and the packets through the API, and
// writes the picture after the dump's last vertical sync (after the N-th with --frame N, counted
// from 1). Prints one summary line. A truncated or malformed file is reported on stderr, exit 1.
//
// --write-dump <file.gs> (with --synthetic, not documented in the usage text) writes the synthetic
// scene as an uncompressed dump: the state of the freshly opened GS, the privileged registers, the
// scene's GIF data in two transfers, a GS-to-CPU read, and a vertical sync. Replaying that file with
// --dump must give the same picture as --synthetic (tests/gs does it).
//
// Exit codes: 0 = done; 1 = the GS failed (message on stderr, prefix [don-gs] or [gsreplay]);
// 2 = bad command line.
#include "don_gs.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <zstd.h>

namespace
{
// ---- The synthetic scene (documented constants; the test reads the same numbers) --------------
// Frame buffer: PSM 32-bit RGBA at address 0, kWidth x kHeight (kWidth must be a multiple of 64).
// Background: the whole frame. Rectangle: kRectX0..kRectX1-1 by kRectY0..kRectY1-1 (pixels,
// exclusive right and bottom edges).
constexpr int kDefaultWidth = 640;
constexpr int kDefaultHeight = 448;
constexpr uint8_t kBackground[3] = {0x20, 0x40, 0x80};
constexpr uint8_t kRectangle[3] = {0xE0, 0xC0, 0x10};
constexpr int kRectX0 = 100, kRectY0 = 50, kRectX1 = 300, kRectY1 = 200;
// Colour used by --state-roundtrip to overwrite the whole screen.
constexpr uint8_t kOverwrite[3] = {0x10, 0xD0, 0x30};

// ---- GIF building ------------------------------------------------------------------------------
struct Qword
{
	uint64_t lo, hi;
};

// GS register addresses (A+D mode).
constexpr uint64_t REG_PRIM = 0x00, REG_RGBAQ = 0x01, REG_XYZ2 = 0x05, REG_XYOFFSET_1 = 0x18,
				   REG_PRMODECONT = 0x1A, REG_SCISSOR_1 = 0x40, REG_TEST_1 = 0x47, REG_FRAME_1 = 0x4C,
				   REG_ZBUF_1 = 0x4E;
constexpr uint64_t PRIM_SPRITE = 6;

struct Gif
{
	std::vector<Qword> q;
	// Packed GIF tag: NLOOP = count of A+D qwords that follow, EOP = 1, FLG = 0 (PACKED), NREG = 1,
	// REGS = 0xE (A+D).
	void Tag(uint64_t count) { q.push_back({count | (1ull << 15) | (1ull << 60), 0xE}); }
	void Ad(uint64_t reg, uint64_t value) { q.push_back({value, reg}); }
};

uint64_t Xyz(int x, int y)
{
	// 12.4 fixed point, relative to the XYOFFSET of 2048.
	return static_cast<uint64_t>((2048 + x) * 16) | (static_cast<uint64_t>((2048 + y) * 16) << 16);
}

uint64_t Rgbaq(const uint8_t c[3])
{
	// Q = 1.0f in the high half.
	return c[0] | (c[1] << 8) | (c[2] << 16) | (0x80ull << 24) | (0x3F800000ull << 32);
}

void Sprite(Gif& gif, const uint8_t colour[3], int x0, int y0, int x1, int y1)
{
	gif.Tag(4);
	gif.Ad(REG_PRIM, PRIM_SPRITE);
	gif.Ad(REG_RGBAQ, Rgbaq(colour));
	gif.Ad(REG_XYZ2, Xyz(x0, y0));
	gif.Ad(REG_XYZ2, Xyz(x1, y1));
}

Gif SetupStream(int width, int height)
{
	Gif gif;
	gif.Tag(7);
	// FBP 0, FBW in 64-pixel units, PSMCT32, no write mask.
	gif.Ad(REG_FRAME_1, 0ull | (static_cast<uint64_t>(width / 64) << 16));
	// Z buffer right after the frame (both in 8 KiB pages: 64x32 pixels each), writes masked (ZMSK)
	// because the scene does not use depth.
	const uint64_t zbp = static_cast<uint64_t>(width / 64) * ((height + 31) / 32);
	gif.Ad(REG_ZBUF_1, zbp | (1ull << 32));
	gif.Ad(REG_XYOFFSET_1, (2048ull << 4) | ((2048ull << 4) << 32));
	gif.Ad(REG_SCISSOR_1, 0ull | (static_cast<uint64_t>(width - 1) << 16) | (0ull << 32) |
							  (static_cast<uint64_t>(height - 1) << 48));
	gif.Ad(REG_TEST_1, (1ull << 16) | (1ull << 17)); // ZTE on, ZTST = ALWAYS
	gif.Ad(REG_PRMODECONT, 1);
	gif.Ad(REG_PRIM, PRIM_SPRITE);
	return gif;
}

// 19 privileged registers for don_gs_vsync, NTSC interlaced picture of width x height.
void PrivRegs(uint64_t regs[DON_GS_PRIV_REG_COUNT], int width, int height)
{
	std::memset(regs, 0, sizeof(uint64_t) * DON_GS_PRIV_REG_COUNT);
	const uint64_t magh = static_cast<uint64_t>(2560 / width - 1);
	regs[0] = 0xFF65;        // PMODE: EN1, ALP = 0xFF
	regs[1] = 2ull << 13;    // SMODE1: CMOD = NTSC
	regs[2] = 1;             // SMODE2: INT = 1, FFMD = 0 (frame mode: every line of the frame buffer is shown)
	regs[7] = static_cast<uint64_t>(width / 64) << 9; // DISPFB1: FBP 0, FBW, PSMCT32
	// DISPLAY1: DX, DY, MAGH, MAGV = 0, DW, DH
	regs[8] = 636ull | (50ull << 12) | (magh << 23) | ((static_cast<uint64_t>(width) * (magh + 1) - 1) << 32) |
			  (static_cast<uint64_t>(height - 1) << 44);
}

// ---- PNG writer: stored (uncompressed) deflate blocks, CRC-32 and Adler-32 done by hand --------
uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0)
{
	static uint32_t table[256];
	static bool ready = false;
	if (!ready)
	{
		for (uint32_t n = 0; n < 256; n++)
		{
			uint32_t c = n;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[n] = c;
		}
		ready = true;
	}
	crc = ~crc;
	for (size_t i = 0; i < size; i++)
		crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

void Put32(std::vector<uint8_t>& out, uint32_t v)
{
	for (int shift = 24; shift >= 0; shift -= 8)
		out.push_back(static_cast<uint8_t>(v >> shift));
}

void Chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& body)
{
	Put32(out, static_cast<uint32_t>(body.size()));
	std::vector<uint8_t> typed(type, type + 4);
	typed.insert(typed.end(), body.begin(), body.end());
	out.insert(out.end(), typed.begin(), typed.end());
	Put32(out, Crc32(typed.data(), typed.size()));
}

bool WritePng(const std::string& path, const uint8_t* rgba, int width, int height)
{
	// Raw image: each row = filter byte 0 + RGB.
	std::vector<uint8_t> raw;
	raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 3));
	for (int y = 0; y < height; y++)
	{
		raw.push_back(0);
		for (int x = 0; x < width; x++)
		{
			const uint8_t* p = rgba + (static_cast<size_t>(y) * width + x) * 4;
			raw.push_back(p[0]);
			raw.push_back(p[1]);
			raw.push_back(p[2]);
		}
	}
	// zlib stream: header, stored blocks of up to 65535 bytes, Adler-32.
	std::vector<uint8_t> z = {0x78, 0x01};
	for (size_t pos = 0; pos < raw.size() || pos == 0;)
	{
		const size_t n = std::min<size_t>(65535, raw.size() - pos);
		const bool last = pos + n >= raw.size();
		z.push_back(last ? 1 : 0);
		z.push_back(n & 0xFF);
		z.push_back(n >> 8);
		z.push_back(~n & 0xFF);
		z.push_back((~n >> 8) & 0xFF);
		z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
		pos += n;
		if (last)
			break;
	}
	uint32_t a = 1, b = 0;
	for (uint8_t byte : raw)
	{
		a = (a + byte) % 65521;
		b = (b + a) % 65521;
	}
	Put32(z, (b << 16) | a);

	std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	std::vector<uint8_t> ihdr;
	Put32(ihdr, static_cast<uint32_t>(width));
	Put32(ihdr, static_cast<uint32_t>(height));
	ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8 bit, RGB, deflate, no filter method, no interlace
	Chunk(png, "IHDR", ihdr);
	Chunk(png, "IDAT", z);
	Chunk(png, "IEND", {});

	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
	return std::fclose(f) == 0 && ok;
}

// ---- helpers -----------------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }

bool Open(int renderer, int width, int height, double* open_ms)
{
	(void)width;
	(void)height;
	DonGsConfig cfg{};
	cfg.renderer = renderer;
	cfg.upscale = 1;
	char err[512];
	const auto start = Clock::now();
	if (don_gs_open(&cfg, err, sizeof(err)) != 0)
	{
		std::fprintf(stderr, "[gsreplay] open failed: %s\n", err);
		return false;
	}
	*open_ms = Ms(start);
	return true;
}

void Send(const Gif& gif)
{
	don_gs_gif_transfer(1, reinterpret_cast<const uint8_t*>(gif.q.data()), static_cast<uint32_t>(gif.q.size() * 16));
}

bool Snapshot(std::vector<uint8_t>& pixels, int* width, int* height, bool quiet = false)
{
	int w = 0, h = 0;
	int rc = don_gs_snapshot_rgba(nullptr, &w, &h, 0);
	if (rc == -3)
	{
		pixels.assign(static_cast<size_t>(w) * h * 4, 0);
		rc = don_gs_snapshot_rgba(pixels.data(), &w, &h, static_cast<int>(pixels.size()));
	}
	if (rc != 0)
	{
		if (!quiet)
			std::fprintf(stderr, "[gsreplay] snapshot failed: %d\n", rc);
		return false;
	}
	*width = w;
	*height = h;
	return true;
}

constexpr uint64_t REG_BITBLTBUF = 0x50, REG_TRXPOS = 0x51, REG_TRXREG = 0x52, REG_TRXDIR = 0x53;

// Reads `count` pixels (32 bit, 4 pixels per quadword: count % 4 == 0) of row `y` from x0 of the
// frame buffer (FBP 0) back from the GS local memory with a GS to CPU transfer.
bool ReadRow(int width, int x0, int y, int count, std::vector<uint32_t>& out)
{
	Gif gif;
	gif.Tag(4);
	gif.Ad(REG_BITBLTBUF, (0ull << 0) | (static_cast<uint64_t>(width / 64) << 16) | (0ull << 24)); // SBP 0, SBW, SPSM 32
	gif.Ad(REG_TRXPOS, static_cast<uint64_t>(x0) | (static_cast<uint64_t>(y) << 16)); // SSAX, SSAY
	gif.Ad(REG_TRXREG, static_cast<uint64_t>(count) | (1ull << 32));                  // RRW, RRH = 1
	gif.Ad(REG_TRXDIR, 1);                                                            // GS to CPU
	Send(gif);
	out.assign(count, 0);
	const uint32_t bytes = static_cast<uint32_t>(count) * 4;
	return don_gs_read_fifo(reinterpret_cast<uint8_t*>(out.data()), bytes) == bytes;
}

// True when the pixels of `row` (starting at x0 on row y) are the scene's colours (alpha ignored).
bool RowIsScene(const std::vector<uint32_t>& row, int x0, int y)
{
	for (size_t i = 0; i < row.size(); i++)
	{
		const int x = x0 + static_cast<int>(i);
		const uint8_t* c = (x >= kRectX0 && x < kRectX1 && y >= kRectY0 && y < kRectY1) ? kRectangle : kBackground;
		if ((row[i] & 0xFFFFFF) != (c[0] | (c[1] << 8) | (c[2] << 16)))
			return false;
	}
	return true;
}

bool RowIsColour(const std::vector<uint32_t>& row, const uint8_t c[3])
{
	for (uint32_t v : row)
		if ((v & 0xFFFFFF) != (c[0] | (c[1] << 8) | (c[2] << 16)))
			return false;
	return true;
}

// Draws the scene and a vsync, returns the snapshot. `frame_ms` gets the time of the draw + vsync.
bool RenderScene(int width, int height, std::vector<uint8_t>& pixels, int* out_w, int* out_h, double* frame_ms)
{
	uint64_t regs[DON_GS_PRIV_REG_COUNT];
	PrivRegs(regs, width, height);
	const auto start = Clock::now();
	Gif gif = SetupStream(width, height);
	Sprite(gif, kBackground, 0, 0, width, height);
	Sprite(gif, kRectangle, kRectX0, kRectY0, kRectX1, kRectY1);
	Send(gif);
	don_gs_vsync(regs, 0);
	if (!Snapshot(pixels, out_w, out_h))
		return false;
	if (frame_ms)
		*frame_ms = Ms(start);
	return true;
}

bool SamePicture(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
	return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}


// ---- GS dump files (.gs, .gs.zst) --------------------------------------------------------------
// Format: docs/knowledge/15-gs-dumps.md. Read from PCSX2's GS/GSDump.{h,cpp} and GS/GSLzma.cpp.
constexpr size_t kPrivBlockBytes = 0x2000;
constexpr size_t kDumpHeaderBytes = 36; // GSDumpHeader: nine 32-bit words
constexpr size_t kMaxDumpBytes = static_cast<size_t>(1) << 31; // decompressed size cap
// Offset of each of the 19 values of don_gs_vsync inside the 0x2000-byte privileged block: PMODE,
// SMODE1, SMODE2, SRFSH, SYNCH1, SYNCH2, SYNCV, DISPFB1, DISPLAY1, DISPFB2, DISPLAY2, EXTBUF,
// EXTDATA, EXTWRITE, BGCOLOR (0x00..0xE0), CSR (0x1000), IMR (0x1010), BUSDIR (0x1040),
// SIGLBLID (0x1080). Same table as src/gsbridge/don_gs.cpp.
constexpr size_t kPrivOffsets[DON_GS_PRIV_REG_COUNT] = {
	0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0,
	0x1000, 0x1010, 0x1040, 0x1080};
constexpr int kCsrSlot = 15;
constexpr uint64_t kCsrField = 1ull << 13;

enum PacketId : uint8_t { kTransfer = 0, kVSync = 1, kReadFifo = 2, kRegisters = 3 };
// Transfer path field: 0 = PATH1 (old, 16 KiB buffer), 1 = PATH2, 2 = PATH3, 3 = PATH1 (new)
constexpr const char* kPathNames[4] = {"path1old", "path2", "path3", "path1"};

struct Packet
{
	uint8_t id;
	uint8_t path;      // transfers only
	size_t offset;     // payload offset in the decompressed file
	uint32_t length;   // payload bytes
};

struct DumpFile
{
	std::vector<uint8_t> bytes;
	uint32_t crc = 0;
	std::string serial;
	bool new_header = false;
	uint32_t screenshot_w = 0, screenshot_h = 0;
	size_t state_offset = 0, state_size = 0;
	size_t regs_offset = 0;
	std::vector<Packet> packets;
};

bool Fail(std::string& err, const char* format, ...)
{
	char text[400];
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	err = text;
	return false;
}

// Reads a file; when it starts with the Zstandard magic, decompresses it.
bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out, std::string& err)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return Fail(err, "cannot open %s", path.c_str());
	std::vector<uint8_t> raw;
	uint8_t chunk[1 << 16];
	size_t n;
	while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
		raw.insert(raw.end(), chunk, chunk + n);
	const bool read_error = std::ferror(f) != 0;
	std::fclose(f);
	if (read_error)
		return Fail(err, "error reading %s", path.c_str());
	if (raw.size() >= 4 && raw[0] == 0x28 && raw[1] == 0xB5 && raw[2] == 0x2F && raw[3] == 0xFD)
	{
		ZSTD_DStream* ds = ZSTD_createDStream();
		if (!ds)
			return Fail(err, "cannot create the zstd decoder");
		std::vector<uint8_t> buf(1 << 20);
		ZSTD_inBuffer in = {raw.data(), raw.size(), 0};
		size_t ret = 1;
		bool ok = true;
		while (ok && ret != 0)
		{
			if (in.pos == in.size)
			{
				ok = Fail(err, "%s: the Zstandard stream is truncated (%zu bytes decompressed)", path.c_str(), out.size());
				break;
			}
			ZSTD_outBuffer ob = {buf.data(), buf.size(), 0};
			ret = ZSTD_decompressStream(ds, &ob, &in);
			if (ZSTD_isError(ret))
			{
				ok = Fail(err, "%s: Zstandard error: %s", path.c_str(), ZSTD_getErrorName(ret));
				break;
			}
			if (out.size() + ob.pos > kMaxDumpBytes)
			{
				ok = Fail(err, "%s: decompressed size over %zu bytes", path.c_str(), kMaxDumpBytes);
				break;
			}
			out.insert(out.end(), buf.begin(), buf.begin() + ob.pos);
		}
		ZSTD_freeDStream(ds);
		return ok;
	}
	if (raw.size() >= 6 && raw[0] == 0xFD && raw[1] == '7' && raw[2] == 'z' && raw[3] == 'X' && raw[4] == 'Z')
		return Fail(err, "%s is an .xz dump; decompress it first (xz -d) or use .gs or .gs.zst", path.c_str());
	out = std::move(raw);
	return true;
}

uint32_t Le32(const uint8_t* p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool ParseDump(const std::string& path, DumpFile& d, std::string& err)
{
	if (!ReadWholeFile(path, d.bytes, err))
		return false;
	const std::vector<uint8_t>& b = d.bytes;
	size_t pos = 0;
	if (b.size() < 8)
		return Fail(err, "%s: too short to be a GS dump (%zu bytes)", path.c_str(), b.size());
	d.crc = Le32(&b[0]);
	const uint32_t second = Le32(&b[4]);
	pos = 8;
	if (d.crc == 0xFFFFFFFFu)
	{
		// New header: `second` is the size of the header block (header + serial + screenshot).
		d.new_header = true;
		if (second < kDumpHeaderBytes || second > b.size() - pos)
			return Fail(err, "%s: the header block (%u bytes) does not fit in the file", path.c_str(), second);
		const uint8_t* h = &b[pos];
		const uint32_t state_version = Le32(h), state_size = Le32(h + 4), serial_off = Le32(h + 8),
					   serial_size = Le32(h + 12), crc = Le32(h + 16);
		d.screenshot_w = Le32(h + 20);
		d.screenshot_h = Le32(h + 24);
		(void)state_version;
		d.crc = crc;
		if (static_cast<uint64_t>(serial_off) + serial_size > second)
			return Fail(err, "%s: the serial lies outside the header block", path.c_str());
		d.serial.assign(reinterpret_cast<const char*>(h) + serial_off, serial_size);
		pos += second;
		if (state_size > b.size() - pos)
			return Fail(err, "%s: truncated: the state block needs %u bytes, %zu remain", path.c_str(), state_size,
				b.size() - pos);
		d.state_offset = pos;
		d.state_size = state_size;
		pos += state_size;
	}
	else
	{
		// Old dumps: crc word, state size, state.
		if (second > b.size() - pos)
			return Fail(err, "%s: truncated: the state block needs %u bytes, %zu remain", path.c_str(), second,
				b.size() - pos);
		d.state_offset = pos;
		d.state_size = second;
		pos += second;
	}
	if (d.state_size < 4)
		return Fail(err, "%s: the state block is empty", path.c_str());
	if (kPrivBlockBytes > b.size() - pos)
		return Fail(err, "%s: truncated: no privileged-register block", path.c_str());
	d.regs_offset = pos;
	pos += kPrivBlockBytes;

	while (pos < b.size())
	{
		Packet p{};
		p.id = b[pos++];
		switch (p.id)
		{
			case kTransfer:
				if (b.size() - pos < 5)
					return Fail(err, "%s: truncated transfer packet header at offset %zu", path.c_str(), pos - 1);
				p.path = b[pos];
				p.length = Le32(&b[pos + 1]);
				pos += 5;
				if (p.path > 3)
					return Fail(err, "%s: unknown transfer path %u at offset %zu", path.c_str(), p.path, pos - 6);
				if (p.length % 16 != 0)
					return Fail(err, "%s: transfer of %u bytes (not a multiple of 16) at offset %zu", path.c_str(),
						p.length, pos - 6);
				break;
			case kVSync:
				p.length = 1;
				break;
			case kReadFifo:
				p.length = 4;
				break;
			case kRegisters:
				p.length = kPrivBlockBytes;
				break;
			default:
				return Fail(err, "%s: unknown packet type %u at offset %zu", path.c_str(), p.id, pos - 1);
		}
		if (p.length > b.size() - pos)
			return Fail(err, "%s: truncated: packet type %u at offset %zu needs %u bytes, %zu remain", path.c_str(),
				p.id, pos - 1, p.length, b.size() - pos);
		p.offset = pos;
		pos += p.length;
		d.packets.push_back(p);
	}
	return true;
}

void ExtractPriv(const uint8_t* block, uint64_t regs[DON_GS_PRIV_REG_COUNT])
{
	for (int i = 0; i < DON_GS_PRIV_REG_COUNT; i++)
		std::memcpy(&regs[i], block + kPrivOffsets[i], sizeof(uint64_t));
}

struct ReplayStats
{
	uint32_t count[4] = {0, 0, 0, 0};            // packets by id
	uint64_t gif_bytes[4] = {0, 0, 0, 0};        // by transfer path field
	uint64_t fifo_qwords = 0;
	uint32_t vsyncs = 0;
};

// Replays the packets; stops after the stop_frame-th vsync (0 = replay all). Returns false on error.
bool Replay(const DumpFile& d, uint32_t stop_frame, ReplayStats& st, std::string& err)
{
	const uint8_t* base = d.bytes.data();
	uint64_t regs[DON_GS_PRIV_REG_COUNT];
	ExtractPriv(base + d.regs_offset, regs);
	for (const Packet& p : d.packets)
	{
		st.count[p.id]++;
		switch (p.id)
		{
			case kTransfer:
			{
				// The bridge uses one parser for all paths; the path only matters for validation.
				const int path = p.path == 1 ? 2 : (p.path == 2 ? 3 : 1);
				st.gif_bytes[p.path] += p.length;
				if (p.length)
					don_gs_gif_transfer(path, base + p.offset, p.length);
				break;
			}
			case kRegisters:
				ExtractPriv(base + p.offset, regs);
				break;
			case kReadFifo:
			{
				const uint32_t qwc = Le32(base + p.offset);
				if (qwc == 0 || qwc > (1u << 24))
					return Fail(err, "read-FIFO packet with a bad size of %u quadwords", qwc);
				std::vector<uint8_t> sink(static_cast<size_t>(qwc) * 16);
				// A failed read is not fatal: PCSX2 ignores the same case. It is counted by the caller of
				// the API in its log ("[don-gs]"), which is on stderr.
				don_gs_read_fifo(sink.data(), static_cast<uint32_t>(sink.size()));
				st.fifo_qwords += qwc;
				break;
			}
			case kVSync:
				// The field is the one of the CSR in the registers block, as when PCSX2 replays the dump
				// (it ignores the field byte of the packet).
				don_gs_vsync(regs, (regs[kCsrSlot] & kCsrField) ? 1 : 0);
				st.vsyncs++;
				if (stop_frame && st.vsyncs >= stop_frame)
					return true;
				break;
		}
	}
	return true;
}

// ---- writing a dump (hidden --write-dump, used by the test that needs no game data) -------------
void Put32le(std::vector<uint8_t>& out, uint32_t v)
{
	for (int s = 0; s < 32; s += 8)
		out.push_back(static_cast<uint8_t>(v >> s));
}

void PutTransfer(std::vector<uint8_t>& out, uint8_t path_field, const Gif& gif)
{
	out.push_back(kTransfer);
	out.push_back(path_field);
	Put32le(out, static_cast<uint32_t>(gif.q.size() * 16));
	const uint8_t* p = reinterpret_cast<const uint8_t*>(gif.q.data());
	out.insert(out.end(), p, p + gif.q.size() * 16);
}

void PutRegs(std::vector<uint8_t>& out, const uint64_t regs[DON_GS_PRIV_REG_COUNT])
{
	std::vector<uint8_t> block(kPrivBlockBytes, 0);
	for (int i = 0; i < DON_GS_PRIV_REG_COUNT; i++)
		std::memcpy(block.data() + kPrivOffsets[i], &regs[i], sizeof(uint64_t));
	out.insert(out.end(), block.begin(), block.end());
}

// The rectangle again, but in the PACKED format with RGBAQ and XYZ2 as the two registers of the tag
// (no A+D): PCSX2 reads those with aligned loads, which exercises the bridge's handling of data
// that does not start on a 16-byte boundary (a dump's packets rarely do).
void PackedSprite(Gif& gif, const uint8_t colour[3], int x0, int y0, int x1, int y1)
{
	// NLOOP = 2 vertices, EOP, PRE = 1 with PRIM = sprite, FLG = PACKED, NREG = 2, REGS = RGBAQ, XYZ2.
	gif.q.push_back({2ull | (1ull << 15) | (1ull << 46) | (PRIM_SPRITE << 47) | (2ull << 60), 0x51ull});
	const int xs[2] = {x0, x1}, ys[2] = {y0, y1};
	for (int i = 0; i < 2; i++)
	{
		gif.q.push_back({static_cast<uint64_t>(colour[0]) | (static_cast<uint64_t>(colour[1]) << 32),
			static_cast<uint64_t>(colour[2]) | (0x80ull << 32)});
		gif.q.push_back({(static_cast<uint64_t>((2048 + xs[i]) * 16)) | (static_cast<uint64_t>((2048 + ys[i]) * 16) << 32), 0});
	}
}

// Writes the synthetic scene as an uncompressed dump. Needs the GS to be open and still untouched.
bool WriteSceneDump(const std::string& path, int width, int height)
{
	const int size = don_gs_save_state(nullptr, 0);
	std::vector<uint8_t> state(size > 0 ? size : 0);
	if (size <= 0 || don_gs_save_state(state.data(), size) != size)
	{
		std::fprintf(stderr, "[gsreplay] save_state failed\n");
		return false;
	}
	uint64_t regs[DON_GS_PRIV_REG_COUNT];
	PrivRegs(regs, width, height);

	const std::string serial = "SYNTH-00000";
	std::vector<uint8_t> out;
	Put32le(out, 0xFFFFFFFFu);
	Put32le(out, static_cast<uint32_t>(kDumpHeaderBytes + serial.size())); // no screenshot
	Put32le(out, 9);                                                       // state_version
	Put32le(out, static_cast<uint32_t>(state.size()));
	Put32le(out, static_cast<uint32_t>(kDumpHeaderBytes)); // serial_offset
	Put32le(out, static_cast<uint32_t>(serial.size()));
	Put32le(out, 0);                                       // crc
	Put32le(out, 0);                                       // screenshot_width
	Put32le(out, 0);                                       // screenshot_height
	Put32le(out, static_cast<uint32_t>(kDumpHeaderBytes + serial.size())); // screenshot_offset
	Put32le(out, 0);                                       // screenshot_size
	out.insert(out.end(), serial.begin(), serial.end());
	out.insert(out.end(), state.begin(), state.end());
	PutRegs(out, regs);

	Gif setup = SetupStream(width, height);
	PutTransfer(out, 3, setup);
	Gif scene;
	Sprite(scene, kBackground, 0, 0, width, height);
	PutTransfer(out, 3, scene);
	Gif rect;
	PackedSprite(rect, kRectangle, kRectX0, kRectY0, kRectX1, kRectY1);
	PutTransfer(out, 3, rect);
	// A GS-to-CPU read of 20 pixels (5 quadwords) of a row of the rectangle.
	Gif read;
	read.Tag(4);
	read.Ad(REG_BITBLTBUF, static_cast<uint64_t>(width / 64) << 16);
	read.Ad(REG_TRXPOS, static_cast<uint64_t>(kRectX0 - 10) | (static_cast<uint64_t>(kRectY0 + 10) << 16));
	read.Ad(REG_TRXREG, 20ull | (1ull << 32));
	read.Ad(REG_TRXDIR, 1);
	PutTransfer(out, 3, read);
	out.push_back(kReadFifo);
	Put32le(out, 5);
	out.push_back(kRegisters);
	PutRegs(out, regs);
	out.push_back(kVSync);
	out.push_back(0);

	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
	{
		std::fprintf(stderr, "[gsreplay] cannot write %s\n", path.c_str());
		return false;
	}
	const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
	if (std::fclose(f) != 0 || !ok)
	{
		std::fprintf(stderr, "[gsreplay] cannot write %s\n", path.c_str());
		return false;
	}
	std::printf("[gsreplay] wrote dump %s (%zu bytes)\n", path.c_str(), out.size());
	return true;
}

int RunDump(const std::string& dump_path, const std::string& out_path, int renderer, uint32_t stop_frame)
{
	const auto t0 = Clock::now();
	DumpFile dump;
	std::string err;
	if (!ParseDump(dump_path, dump, err))
	{
		std::fprintf(stderr, "[gsreplay] %s\n", err.c_str());
		return 1;
	}
	const double parse_ms = Ms(t0);
	uint32_t total_vsyncs = 0;
	for (const Packet& p : dump.packets)
		total_vsyncs += p.id == kVSync;
	if (total_vsyncs == 0)
	{
		std::fprintf(stderr, "[gsreplay] %s has no vertical sync: nothing to show\n", dump_path.c_str());
		return 1;
	}
	if (stop_frame > total_vsyncs)
	{
		std::fprintf(stderr, "[gsreplay] --frame %u: the dump has only %u vertical syncs\n", stop_frame, total_vsyncs);
		return 1;
	}

	double open_ms = 0;
	if (!Open(renderer, 0, 0, &open_ms))
		return 1;
	const auto t1 = Clock::now();
	if (don_gs_load_state(dump.bytes.data() + dump.state_offset, static_cast<uint32_t>(dump.state_size)) != 0)
	{
		std::fprintf(stderr, "[gsreplay] the GS rejected the state block of %s (%zu bytes, version word %u)\n",
			dump_path.c_str(), dump.state_size, Le32(dump.bytes.data() + dump.state_offset));
		don_gs_close();
		return 1;
	}
	ReplayStats st;
	if (!Replay(dump, stop_frame, st, err))
	{
		std::fprintf(stderr, "[gsreplay] %s\n", err.c_str());
		don_gs_close();
		return 1;
	}
	std::vector<uint8_t> pixels;
	int pw = 0, ph = 0;
	const bool have_picture = Snapshot(pixels, &pw, &ph);
	const double replay_ms = Ms(t1);
	int status = 0;
	if (!have_picture)
		status = 1;
	else if (!WritePng(out_path, pixels.data(), pw, ph))
	{
		std::fprintf(stderr, "[gsreplay] cannot write %s\n", out_path.c_str());
		status = 1;
	}
	don_gs_close();
	std::printf("[gsreplay] dump=%s serial=%s crc=%08x packets: transfer=%u vsync=%u read_fifo=%u regs=%u | gif_bytes: "
				"%s=%llu %s=%llu %s=%llu %s=%llu | fifo_qwords=%llu | renderer=%s picture=%dx%d replayed_vsyncs=%u "
				"parse_ms=%.1f open_ms=%.1f replay_ms=%.1f\n",
		dump_path.c_str(), dump.serial.c_str(), dump.crc, st.count[kTransfer], st.count[kVSync], st.count[kReadFifo],
		st.count[kRegisters], kPathNames[0], static_cast<unsigned long long>(st.gif_bytes[0]), kPathNames[1],
		static_cast<unsigned long long>(st.gif_bytes[1]), kPathNames[2], static_cast<unsigned long long>(st.gif_bytes[2]),
		kPathNames[3], static_cast<unsigned long long>(st.gif_bytes[3]), static_cast<unsigned long long>(st.fifo_qwords),
		renderer == DON_GS_RENDERER_SOFTWARE ? "software" : "vulkan", pw, ph, st.vsyncs, parse_ms, open_ms, replay_ms);
	return status;
}

int Usage()
{
	std::fprintf(stderr,
		"usage: gsreplay --synthetic <out.png> [--renderer software|vulkan] [--width W --height H]\n"
		"                [--state-roundtrip] [--reopen]\n"
		"       gsreplay --dump <file.gs|file.gs.zst> <out.png> [--renderer software|vulkan] [--frame N]\n");
	return 2;
}
} // namespace

int main(int argc, char** argv)
{
	std::string out_path;
	int renderer = DON_GS_RENDERER_SOFTWARE;
	int width = kDefaultWidth, height = kDefaultHeight;
	bool synthetic = false, roundtrip = false, reopen = false;
	std::string dump_path, write_dump_path;
	uint32_t stop_frame = 0;
	for (int i = 1; i < argc; i++)
	{
		const std::string arg = argv[i];
		if (arg == "--dump" && i + 2 < argc)
		{
			dump_path = argv[++i];
			out_path = argv[++i];
			continue;
		}
		if (arg == "--write-dump" && i + 1 < argc)
		{
			write_dump_path = argv[++i];
			continue;
		}
		if (arg == "--frame" && i + 1 < argc)
		{
			const long n = std::atol(argv[++i]);
			if (n < 1)
				return Usage();
			stop_frame = static_cast<uint32_t>(n);
			continue;
		}
		if (arg == "--synthetic" && i + 1 < argc)
		{
			synthetic = true;
			out_path = argv[++i];
		}
		else if (arg == "--renderer" && i + 1 < argc)
		{
			const std::string name = argv[++i];
			if (name == "software")
				renderer = DON_GS_RENDERER_SOFTWARE;
			else if (name == "vulkan")
				renderer = DON_GS_RENDERER_VULKAN;
			else
				return Usage();
		}
		else if (arg == "--width" && i + 1 < argc)
			width = std::atoi(argv[++i]);
		else if (arg == "--height" && i + 1 < argc)
			height = std::atoi(argv[++i]);
		else if (arg == "--state-roundtrip")
			roundtrip = true;
		else if (arg == "--reopen")
			reopen = true;
		else
			return Usage();
	}
	if (!dump_path.empty())
	{
		if (synthetic || !write_dump_path.empty() || roundtrip || reopen)
			return Usage();
		return RunDump(dump_path, out_path, renderer, stop_frame);
	}
	if (!write_dump_path.empty() && !synthetic)
		return Usage();
	if (stop_frame)
		return Usage();
	if (!synthetic || width < 64 || width > 1024 || width % 64 != 0 || 2560 % width != 0 || height < 32 ||
		height > 576 || kRectX1 > width || kRectY1 > height)
	{
		std::fprintf(stderr, "[gsreplay] need --synthetic, a width that is a multiple of 64 and divides 2560 "
							 "(e.g. 640) and a height of 32..576 that holds the rectangle (%dx%d)\n", kRectX1, kRectY1);
		return Usage();
	}

	double open_ms = 0, frame_ms = 0;
	if (!Open(renderer, width, height, &open_ms))
		return 1;

	if (!write_dump_path.empty() && !WriteSceneDump(write_dump_path, width, height))
	{
		don_gs_close();
		return 1;
	}

	std::vector<uint8_t> pixels;
	int pw = 0, ph = 0;
	if (!RenderScene(width, height, pixels, &pw, &ph, &frame_ms))
	{
		don_gs_close();
		return 1;
	}
	std::printf("[gsreplay] renderer=%s picture=%dx%d open_ms=%.1f frame_ms=%.1f\n",
		renderer == DON_GS_RENDERER_SOFTWARE ? "software" : "vulkan", pw, ph, open_ms, frame_ms);

	int status = 0;
	if (roundtrip)
	{
		const int size = don_gs_save_state(nullptr, 0);
		std::vector<uint8_t> state(size > 0 ? size : 0);
		if (size <= 0 || don_gs_save_state(state.data(), size) != size)
		{
			std::fprintf(stderr, "[gsreplay] save_state failed\n");
			status = 1;
		}
		else
		{
			uint64_t regs[DON_GS_PRIV_REG_COUNT];
			PrivRegs(regs, width, height);
			// The pixels are checked in local memory with don_gs_read_fifo, which works the same on both
			// renderers; the hardware renderer shows nothing after a state load until something is drawn
			// (it does not build its display target from local memory), so the picture is compared
			// only when there is one.
			const int row_y = kRectY0 + 10, row_x = kRectX0 - 10, row_n = 20;
			std::vector<uint32_t> before, during, after;
			int w2, h2;
			bool ok = ReadRow(width, row_x, row_y, row_n, before) && RowIsScene(before, row_x, row_y);
			if (!ok)
				{
				std::fprintf(stderr, "[gsreplay] state test: the scene is not in local memory before the save\n");
				for (uint32_t v : before)
					std::fprintf(stderr, " %08x", v);
				std::fprintf(stderr, "\n");
			}
			if (ok)
			{
				Gif gif = SetupStream(width, height);
				Sprite(gif, kOverwrite, 0, 0, width, height);
				Send(gif);
				don_gs_vsync(regs, 0);
				ok = ReadRow(width, row_x, row_y, row_n, during) && RowIsColour(during, kOverwrite);
				if (!ok)
					std::fprintf(stderr, "[gsreplay] state test: the overwrite did not reach local memory\n");
			}
			ok = ok && don_gs_load_state(state.data(), static_cast<uint32_t>(size)) == 0;
			if (ok)
			{
				ok = ReadRow(width, row_x, row_y, row_n, after) && RowIsScene(after, row_x, row_y);
				if (!ok)
					std::fprintf(stderr, "[gsreplay] state test: the loaded state does not give back the scene\n");
			}
			const char* picture = "not-checked";
			if (ok)
			{
				std::vector<uint8_t> restored;
				don_gs_vsync(regs, 0);
				if (!Snapshot(restored, &w2, &h2, true))
					picture = "none";
				else if (SamePicture(restored, pixels))
					picture = "same";
				else
				{
					picture = "DIFFERENT";
					ok = false;
				}
			}
			std::printf("[gsreplay] state-roundtrip: picture-after-load=%s\n", picture);
			std::printf("[gsreplay] state-roundtrip: size=%d %s\n", size, ok ? "ok" : "FAILED");
			status = ok ? status : 1;
		}
	}

	if (!WritePng(out_path, pixels.data(), pw, ph))
	{
		std::fprintf(stderr, "[gsreplay] cannot write %s\n", out_path.c_str());
		status = 1;
	}
	don_gs_close();

	if (reopen)
	{
		double reopen_open_ms = 0;
		std::vector<uint8_t> again;
		int w2, h2;
		const bool ok = Open(renderer, width, height, &reopen_open_ms) && RenderScene(width, height, again, &w2, &h2, nullptr) &&
						SamePicture(again, pixels);
		don_gs_close();
		std::printf("[gsreplay] reopen: open_ms=%.1f %s\n", reopen_open_ms, ok ? "ok" : "FAILED");
		status = ok ? status : 1;
	}
	return status;
}
