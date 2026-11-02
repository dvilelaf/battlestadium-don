// SPDX-License-Identifier: GPL-3.0-or-later
// Scripted input (T-112): a text file that says which buttons are held at which frame, which screenshots to
// take and when to quit. Format and usage: docs/INPUT_SCRIPTS.md. Frames count presented frames (vertical
// blanks), never time. The parser and the player are pure code; the host program connects them (see the
// comments on ScriptPlayer).
#pragma once

#include "input.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace input
{

constexpr int kScriptPlayers = 4;

struct ScriptCommand
{
    enum class Kind : uint8_t
    {
        Tap,     // button held for `frames` frames starting at `frame`
        Press,   // button held from `frame` until a Release
        Release, // ends a Press
        Stick,   // sets a stick (value stays until the next Stick command)
        Shot,    // screenshot `name`
        Quit,
    };

    Kind kind = Kind::Quit;
    uint64_t frame = 0; // absolute (relative frames are resolved by the parser)
    int player = 0;     // 0..3 (P1..P4)
    PadButton button = PadButton::Cross;
    uint32_t frames = 0; // Tap
    bool right_stick = false;
    float x = 0.f, y = 0.f; // Stick, -1..1, y positive down
    std::string name;       // Shot
    int line = 0;           // line in the file, for messages
};

struct ScriptError
{
    int line = 0;
    std::string message;
    std::string to_string() const; // "line 12: unknown button 'FOO'"
};

class InputScript
{
public:
    // Commands ordered by frame (stable: the order of the file for equal frames).
    const std::vector<ScriptCommand> &commands() const { return m_commands; }
    // Bit p set if any command is for player p (a pad is "connected" only for those players).
    uint8_t players_used() const { return m_players_used; }
    // Frame of the last command (0 if empty).
    uint64_t last_frame() const;
    bool empty() const { return m_commands.empty(); }

private:
    friend struct ScriptParser;
    std::vector<ScriptCommand> m_commands;
    uint8_t m_players_used = 0;
};

struct ParseResult
{
    bool ok = false;
    InputScript script;             // empty unless ok
    std::vector<ScriptError> errors; // all the errors found; empty if ok
};

// Parses a script. Nothing is returned for execution if any line is wrong.
ParseResult parse_script(const std::string &text);
// Reads the file (std::filesystem path) and parses it; an unreadable file is one error with line 0.
ParseResult parse_script_file(const std::filesystem::path &path);

// What must happen at one frame.
struct FrameActions
{
    // The buttons the script holds, per player, as pad states built with the same bit layout and pressure
    // rules as the devices (set_pad_button). connected is 1 only for the players the script mentions
    // (InputScript::players_used), so that a script for P1 does not plug in three more pads.
    Ps2HostPadState pads[kScriptPlayers];
    std::vector<std::string> shots; // screenshots whose frame was reached since the last step
    bool quit = false;              // true from the frame of the first `quit` on
};

// Plays a script. The host calls step(n) once per presented frame with n = 1, 2, 3... (the number of the
// vertical blank), before the guest reads the pads for that frame; "frame N" in the script is the value
// passed here. Then:
//   - pads[] -> merge_pad_states(pads[p], device state of player p) -> publish to the pad table;
//   - shots  -> take a screenshot with that name after presenting frame n;
//   - quit   -> end the run (exit code 0).
// step() must be called with non-decreasing frames; a lower one is treated as the same frame as the last.
// A frame that is skipped still applies its commands (a shot at a skipped frame is reported by the next step).
class ScriptPlayer
{
public:
    explicit ScriptPlayer(const InputScript &script);

    FrameActions step(uint64_t frame);

private:
    const InputScript &m_script;
    size_t m_next = 0;
    uint64_t m_last = 0;
    bool m_quit = false;
    uint16_t m_held[kScriptPlayers] = {};                    // bit of button set = Press without Release
    uint64_t m_tap_end[kScriptPlayers][kButtonCount] = {};   // held while frame < end
    float m_stick[kScriptPlayers][4] = {};                   // lx ly rx ry
};

} // namespace input
