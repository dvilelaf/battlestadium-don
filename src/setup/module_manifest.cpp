// SPDX-License-Identifier: GPL-3.0-or-later
// The one reader of module/<id>/manifest.json (T-616; T-683 made it the only one; fields: module_manifest.h). One JSON
// object, no duplicate keys among the fields we read, every field we read with the right type. Unknown fields are
// skipped (but must still be valid JSON).
#include "module_manifest.h"
#include "path_text.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
class Reader
{
public:
    explicit Reader(const std::string &text) : t_(text) {}

    bool object(DonModuleManifest &m, std::string &error)
    {
        ws();
        if (!eat('{'))
            return fail(error, "not a JSON object");
        ws();
        unsigned seen = 0;
        if (!eat('}'))
        {
            for (;;)
            {
                ws();
                std::string key;
                if (!str(key))
                    return fail(error, "bad key");
                ws();
                if (!eat(':'))
                    return fail(error, "missing ':' after \"" + key + "\"");
                ws();
                if (!field(key, m, seen, error))
                    return false;
                ws();
                if (eat(','))
                    continue;
                if (eat('}'))
                    break;
                return fail(error, "expected ',' or '}' after \"" + key + "\"");
            }
        }
        ws();
        if (p_ != t_.size())
            return fail(error, "text after the object");
        static const char *const kNames[] = {"program_version", "abi_version", "module_id", "toolchain_hash",
                                             "module_sha256",   "verified",    "variant"};
        for (unsigned i = 0; i < 7; ++i)
            if ((seen & (1u << i)) == 0)
                return fail(error, std::string("missing field \"") + kNames[i] + "\"");
        return true;
    }

private:
    const std::string &t_;
    size_t p_ = 0;

    static bool fail(std::string &error, const std::string &why)
    {
        error = why;
        return false;
    }
    void ws()
    {
        while (p_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[p_])))
            ++p_;
    }
    bool eat(char c)
    {
        if (p_ < t_.size() && t_[p_] == c)
        {
            ++p_;
            return true;
        }
        return false;
    }
    bool lit(const char *word)
    {
        const std::string w(word);
        if (t_.compare(p_, w.size(), w) != 0)
            return false;
        p_ += w.size();
        return true;
    }
    bool str(std::string &out)
    {
        out.clear();
        if (!eat('"'))
            return false;
        while (p_ < t_.size())
        {
            const char c = t_[p_++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }
            if (p_ >= t_.size())
                return false;
            const char e = t_[p_++];
            switch (e)
            {
            case '"': case '\\': case '/': out.push_back(e); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
                if (p_ + 4 > t_.size())
                    return false;
                for (int i = 0; i < 4; ++i)
                    if (!std::isxdigit(static_cast<unsigned char>(t_[p_ + i])))
                        return false;
                out.push_back('?');  // no field we read holds non-ASCII text
                p_ += 4;
                break;
            default: return false;
            }
        }
        return false;
    }
    bool uint(uint64_t &out)
    {
        const size_t start = p_;
        while (p_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[p_])))
            ++p_;
        if (p_ == start || p_ - start > 9)
            return false;
        out = std::strtoull(t_.substr(start, p_ - start).c_str(), nullptr, 10);
        return true;
    }
    // Any JSON value, skipped (numbers are checked loosely: sign, digits, '.', exponent).
    bool value(int depth)
    {
        if (depth > 16)
            return false;
        ws();
        if (p_ >= t_.size())
            return false;
        const char c = t_[p_];
        if (c == '"')
        {
            std::string s;
            return str(s);
        }
        if (c == '{' || c == '[')
        {
            const char close = c == '{' ? '}' : ']';
            ++p_;
            ws();
            if (eat(close))
                return true;
            for (;;)
            {
                ws();
                if (c == '{')
                {
                    std::string k;
                    ws();
                    if (!str(k))
                        return false;
                    ws();
                    if (!eat(':'))
                        return false;
                }
                if (!value(depth + 1))
                    return false;
                ws();
                if (eat(','))
                    continue;
                return eat(close);
            }
        }
        if (lit("true") || lit("false") || lit("null"))
            return true;
        const size_t start = p_;
        while (p_ < t_.size() && (std::isdigit(static_cast<unsigned char>(t_[p_])) || std::strchr("+-.eE", t_[p_]) != nullptr))
            ++p_;
        return p_ > start;
    }
    bool field(const std::string &key, DonModuleManifest &m, unsigned &seen, std::string &error)
    {
        static const char *const kNames[] = {"program_version", "abi_version", "module_id", "toolchain_hash",
                                             "module_sha256",   "verified",    "variant"};
        for (unsigned i = 0; i < 7; ++i)
        {
            if (key != kNames[i])
                continue;
            if (seen & (1u << i))
                return fail(error, "duplicate field \"" + key + "\"");
            seen |= 1u << i;
            bool ok = false;
            switch (i)
            {
            case 0: ok = str(m.program_version); break;
            case 1: { uint64_t v = 0; ok = uint(v); m.abi_version = static_cast<uint32_t>(v); break; }
            case 2: ok = str(m.module_id); break;
            case 3: ok = str(m.toolchain_hash); break;
            case 4: ok = str(m.module_sha256); break;
            case 5:
                ok = true;
                if (lit("true"))
                    m.verified = true;
                else if (lit("false"))
                    m.verified = false;
                else
                    ok = false;
                break;
            default: ok = str(m.variant); break;
            }
            return ok || fail(error, "field \"" + key + "\" has the wrong type");
        }
        return value(0) || fail(error, "bad value of \"" + key + "\"");
    }
};
}  // namespace

bool don_read_module_manifest(const std::string &file, DonModuleManifest &out, std::string &error)
{
    return don_read_module_manifest(std::filesystem::path(file), out, error);
}

bool don_read_module_manifest(const std::filesystem::path &file, DonModuleManifest &out, std::string &error)
{
    const std::string name = don_setup::path_text(file);
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        error = "manifest " + name + " does not exist or cannot be read";
        return false;
    }
    std::error_code size_error;
    const uintmax_t size = std::filesystem::file_size(file, size_error);
    if (!size_error && size > kDonModuleManifestMaxBytes)
    {
        error = "manifest " + name + " is corrupt: larger than " + std::to_string(kDonModuleManifestMaxBytes) + " bytes";
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    const std::string body = text.str();
    out = DonModuleManifest();
    std::string why;
    if (!Reader(body).object(out, why))
    {
        error = "manifest " + name + " is corrupt: " + why;
        return false;
    }
    return true;
}

namespace
{
std::string json_string(const std::string &s)
{
    std::string o = "\"";
    for (const char c : s)
    {
        if (c == '"' || c == '\\')
            o += '\\';
        if (static_cast<unsigned char>(c) < 0x20)
            o += ' ';  // no field we write holds control characters
        else
            o += c;
    }
    return o + "\"";
}
} // namespace

bool don_write_module_manifest(const std::string &file, const DonModuleManifest &m, std::string &error)
{
    return don_write_module_manifest(std::filesystem::path(file), m, error);
}

bool don_write_module_manifest(const std::filesystem::path &file, const DonModuleManifest &m, std::string &error)
{
    const std::string name = don_setup::path_text(file);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "{\n  \"program_version\": " << json_string(m.program_version) << ",\n  \"abi_version\": " << m.abi_version
        << ",\n  \"module_id\": " << json_string(m.module_id) << ",\n  \"toolchain_hash\": "
        << json_string(m.toolchain_hash) << ",\n  \"module_sha256\": " << json_string(m.module_sha256)
        << ",\n  \"verified\": " << (m.verified ? "true" : "false") << ",\n  \"variant\": " << json_string(m.variant)
        << "\n}\n";
    out.flush();
    if (!out)
    {
        error = "cannot write manifest " + name;
        return false;
    }
    return true;
}
