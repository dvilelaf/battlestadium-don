// SPDX-License-Identifier: GPL-3.0-or-later
// "The port is skipping a video" (T-113): the third source of player 1's pad, next to the devices and the
// input script. The game's own video player (0x36A820, docs/knowledge/19-videos.md section 1) loops until the
// "pressed this frame" word has Start (opening) or Circle (ending). The game side (src/game/don_overrides.cpp)
// installs a probe that says whether the player is running; the host (host.cpp, at each vertical blank) holds
// Start and Circle for the game while it is, from a few blanks after it began. Nothing is written into the
// game's memory; the probe only reads it.
#pragma once

namespace don_video
{
// What the port does with the video player when DON_MOVIE_SKIP_AFTER is not set. With an FFmpeg build (the
// game's own player decodes and ends the movies, journal 034) the port does nothing: the movies play and the
// player's own Start (or Circle, in the ending) skips them. Without FFmpeg the player has no stream to read, so
// the port holds Start after 240 vertical blanks. DON_MOVIE_SKIP_AFTER=<blanks> holds it after that many
// blanks in either build; DON_PLAY_MOVIES=1 turns the pressing off altogether.
#if defined(DON_WITH_FFMPEG)
inline constexpr bool kAutoSkipByDefault = false;
#else
inline constexpr bool kAutoSkipByDefault = true;
#endif
inline constexpr unsigned long long kDefaultSkipAfterBlanks = 240;

// Returns non-zero while the game is inside its video player. Null: no skipping (DON_PLAY_MOVIES=1, or a
// program that is not the game). Called on the game thread, at each vertical blank.
inline bool (*&player_probe())()
{
    static bool (*probe)() = nullptr;
    return probe;
}
} // namespace don_video
