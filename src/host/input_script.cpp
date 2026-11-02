// SPDX-License-Identifier: GPL-3.0-or-later
#include "input_script.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>

namespace input
{

std::string ScriptError::to_string() const
{
    if (line <= 0)
        return message;
    return "line " + std::to_string(line) + ": " + message;
}

uint64_t InputScript::last_frame() const { return m_commands.empty() ? 0 : m_commands.back().frame; }

struct ScriptParser
{
    ParseResult result;
    uint64_t previous_frame = 0;

    void error(int line, std::string message) { result.errors.push_back({line, std::move(message)}); }

    // After the last line: with errors nothing is returned to execute; otherwise order by frame.
    ParseResult finish()
    {
        if (!result.errors.empty())
        {
            result.script = InputScript{};
            result.ok = false;
            return std::move(result);
        }
        auto &cmds = result.script.m_commands;
        std::stable_sort(cmds.begin(), cmds.end(),
                         [](const ScriptCommand &a, const ScriptCommand &b) { return a.frame < b.frame; });
        for (const auto &c : cmds)
            if (c.kind != ScriptCommand::Kind::Shot && c.kind != ScriptCommand::Kind::Quit)
                result.script.m_players_used |= static_cast<uint8_t>(1u << c.player);
        result.ok = true;
        return std::move(result);
    }

    static std::string lower(std::string s)
    {
        for (char &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static bool parse_uint(const std::string &t, uint64_t &out)
    {
        if (t.empty())
            return false;
        const char *end = t.data() + t.size();
        auto r = std::from_chars(t.data(), end, out);
        return r.ec == std::errc() && r.ptr == end;
    }

    static bool parse_float(const std::string &t, float &out)
    {
        if (t.empty())
            return false;
        const char *begin = t.data();
        const char *end = begin + t.size();
        if (*begin == '+')
            ++begin;
        auto r = std::from_chars(begin, end, out);
        return r.ec == std::errc() && r.ptr == end;
    }

    static bool valid_shot_name(const std::string &n)
    {
        if (n.empty() || n[0] == '.')
            return false;
        for (char c : n)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
                return false;
        return true;
    }

    void parse_line(int line, std::string text)
    {
        const size_t hash = text.find('#');
        if (hash != std::string::npos)
            text.erase(hash);
        std::vector<std::string> tok;
        {
            std::istringstream in(text);
            std::string t;
            while (in >> t)
                tok.push_back(t);
        }
        if (tok.empty())
            return;

        // Frame: "600" absolute, "+30" relative to the frame of the previous command.
        uint64_t frame = 0;
        {
            const std::string &f = tok[0];
            const bool relative = !f.empty() && f[0] == '+';
            uint64_t n = 0;
            if (!parse_uint(relative ? f.substr(1) : f, n))
            {
                error(line, "expected a frame number (such as 600, or +30 for 30 frames after the previous command), got '" + f + "'");
                return;
            }
            frame = relative ? previous_frame + n : n;
        }
        if (tok.size() < 2)
        {
            error(line, "missing command after the frame");
            return;
        }

        // Optional player prefix, glued to the command ("P2:tap") or alone ("P2: tap").
        int player = 0;
        bool has_prefix = false;
        size_t at = 1;
        {
            std::string t = tok[1];
            if (t.size() >= 2 && (t[0] == 'P' || t[0] == 'p') && std::isdigit(static_cast<unsigned char>(t[1])))
            {
                const size_t colon = t.find(':');
                if (colon != std::string::npos)
                {
                    const std::string digits = t.substr(1, colon - 1);
                    if (digits.size() != 1 || digits[0] < '1' || digits[0] > '4')
                    {
                        error(line, "unknown player prefix '" + t.substr(0, colon + 1) + "' (only P1: to P4:)");
                        return;
                    }
                    player = digits[0] - '1';
                    has_prefix = true;
                    const std::string rest = t.substr(colon + 1);
                    if (rest.empty())
                        ++at;
                    else
                        tok[1] = rest;
                    if (at >= tok.size())
                    {
                        error(line, "missing command after the player prefix");
                        return;
                    }
                }
            }
        }

        const std::string cmd = lower(tok[at]);
        const size_t nargs = tok.size() - at - 1;
        auto args = [&](size_t i) -> const std::string & { return tok[at + 1 + i]; };

        ScriptCommand c;
        c.frame = frame;
        c.player = player;
        c.line = line;

        auto need = [&](size_t n, const char *usage) -> bool {
            if (nargs < n)
            {
                error(line, std::string("'") + cmd + "' needs " + usage);
                return false;
            }
            if (nargs > n)
            {
                error(line, "unexpected extra argument '" + args(n) + "' after '" + cmd + " " + usage + "'");
                return false;
            }
            return true;
        };
        auto button = [&](const std::string &name) -> bool {
            if (!parse_button_name(name, c.button))
            {
                error(line, "unknown button '" + name + "' (UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE TRIANGLE L1 L2 R1 R2 START SELECT L3 R3)");
                return false;
            }
            return true;
        };

        if (cmd == "tap")
        {
            if (!need(2, "a button and a number of frames"))
                return;
            if (!button(args(0)))
                return;
            uint64_t n = 0;
            if (!parse_uint(args(1), n) || n < 1 || n > 0xFFFFFFFFull)
            {
                error(line, "the number of frames of 'tap' must be a whole number of at least 1, got '" + args(1) + "'");
                return;
            }
            c.kind = ScriptCommand::Kind::Tap;
            c.frames = static_cast<uint32_t>(n);
        }
        else if (cmd == "press" || cmd == "release")
        {
            if (!need(1, "a button"))
                return;
            if (!button(args(0)))
                return;
            c.kind = cmd == "press" ? ScriptCommand::Kind::Press : ScriptCommand::Kind::Release;
        }
        else if (cmd == "stick")
        {
            if (!need(3, "LEFT or RIGHT and two numbers between -1 and 1"))
                return;
            const std::string which = lower(args(0));
            if (which != "left" && which != "right")
            {
                error(line, "the first argument of 'stick' must be LEFT or RIGHT, got '" + args(0) + "'");
                return;
            }
            float x = 0, y = 0;
            if (!parse_float(args(1), x) || !parse_float(args(2), y) || x < -1.f || x > 1.f || y < -1.f || y > 1.f)
            {
                error(line, "the position of 'stick' must be two numbers between -1 and 1, got '" + args(1) + " " + args(2) + "'");
                return;
            }
            c.kind = ScriptCommand::Kind::Stick;
            c.right_stick = which == "right";
            c.x = x;
            c.y = y;
        }
        else if (cmd == "shot")
        {
            if (has_prefix)
            {
                error(line, "'shot' is not per player: remove the player prefix");
                return;
            }
            if (!need(1, "a name"))
                return;
            if (!valid_shot_name(args(0)))
            {
                error(line, "invalid shot name '" + args(0) + "' (letters, digits, '_', '-' and '.', not starting with '.')");
                return;
            }
            c.kind = ScriptCommand::Kind::Shot;
            c.name = args(0);
        }
        else if (cmd == "quit")
        {
            if (has_prefix)
            {
                error(line, "'quit' is not per player: remove the player prefix");
                return;
            }
            if (!need(0, ""))
                return;
            c.kind = ScriptCommand::Kind::Quit;
        }
        else
        {
            error(line, "unknown command '" + tok[at] + "' (tap press release stick shot quit)");
            return;
        }

        previous_frame = frame;
        result.script.m_commands.push_back(std::move(c));
    }
};

ParseResult parse_script(const std::string &text)
{
    ScriptParser p;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line))
    {
        ++n;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        p.parse_line(n, line);
    }
    return p.finish();
}

ParseResult parse_script_file(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        ParseResult r;
        r.errors.push_back({0, "cannot read the input script '" + path.string() + "'"});
        return r;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_script(ss.str());
}

ScriptPlayer::ScriptPlayer(const InputScript &script) : m_script(script) {}

FrameActions ScriptPlayer::step(uint64_t frame)
{
    frame = std::max(frame, m_last);
    m_last = frame;

    FrameActions out;
    const auto &cmds = m_script.commands();
    while (m_next < cmds.size() && cmds[m_next].frame <= frame)
    {
        const ScriptCommand &c = cmds[m_next++];
        const unsigned b = static_cast<unsigned>(c.button);
        switch (c.kind)
        {
        case ScriptCommand::Kind::Tap:
            m_tap_end[c.player][b] = std::max(m_tap_end[c.player][b], c.frame + c.frames);
            break;
        case ScriptCommand::Kind::Press:
            m_held[c.player] = static_cast<uint16_t>(m_held[c.player] | (1u << b));
            break;
        case ScriptCommand::Kind::Release:
            m_held[c.player] = static_cast<uint16_t>(m_held[c.player] & ~(1u << b));
            break;
        case ScriptCommand::Kind::Stick:
            m_stick[c.player][c.right_stick ? 2 : 0] = c.x;
            m_stick[c.player][c.right_stick ? 3 : 1] = c.y;
            break;
        case ScriptCommand::Kind::Shot:
            out.shots.push_back(c.name);
            break;
        case ScriptCommand::Kind::Quit:
            m_quit = true;
            break;
        }
    }
    out.quit = m_quit;

    for (int p = 0; p < kScriptPlayers; ++p)
    {
        Ps2HostPadState s = neutral_pad_state((m_script.players_used() >> p) & 1u);
        for (int b = 0; b < kButtonCount; ++b)
            if ((m_held[p] >> b) & 1u || frame < m_tap_end[p][b])
                set_pad_button(s, static_cast<PadButton>(b), true);
        s.lx = stick_axis_to_byte(m_stick[p][0]);
        s.ly = stick_axis_to_byte(m_stick[p][1]);
        s.rx = stick_axis_to_byte(m_stick[p][2]);
        s.ry = stick_axis_to_byte(m_stick[p][3]);
        out.pads[p] = s;
    }
    return out;
}

} // namespace input
