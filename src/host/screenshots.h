// SPDX-License-Identifier: GPL-3.0-or-later
// PNG screenshots of the GS picture (--shots / --shot-every). Written with SDL3's own PNG writer
// (SDL_SavePNG), so there is no PNG or zlib dependency of ours. Game thread only.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

struct DonGsApi;

class ScreenshotWriter
{
public:
    // dir is created if it does not exist. every: one screenshot each `every` vertical blanks.
    ScreenshotWriter(const DonGsApi &api, std::filesystem::path dir, uint32_t every);

    bool enabled() const { return m_every != 0; }

    // Call once per vertical blank, after don_gs_vsync, with the number of this blank (1, 2, ...).
    void on_vsync(uint64_t frame);

    // Writes one PNG of the current picture under an arbitrary name (the script's `shot NAME`), from the game
    // thread at a vertical blank. false (with one warning line) if there is nothing to show or the write failed.
    bool write_named(const std::string &name);

    uint32_t written() const { return m_written; }

private:
    // Fills m_pixels with the current picture; 0 on success, the snapshot return code otherwise.
    int snapshot();
    // Writes m_pixels (m_width x m_height) as `file_name` in the folder; true when it was written.
    bool write(const std::string &file_name);

    const DonGsApi &m_api;
    std::filesystem::path m_dir;
    uint32_t m_every;
    uint32_t m_written = 0;
    int m_width = 0;
    int m_height = 0;
    std::vector<uint8_t> m_pixels;
};
