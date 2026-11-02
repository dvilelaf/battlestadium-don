// SPDX-License-Identifier: GPL-3.0-or-later
// Windows only: the display-driver resets and unexpected power events since the previous start, from the
// System event log (see crash_report.h). A friend's PC went fully black once while playing and nobody will
// be asked for details: the events that explain it (a driver reset, an unexpected reboot) are what the log
// must say. wevtapi is loaded dynamically, so nothing new enters the link.
//
// The events that count: the provider nvlddmkm (NVIDIA), amdkmdag (AMD), any igfx* provider (Intel),
// "Display" event 4101 (the generic "driver stopped responding and has recovered" reset) and Kernel-Power
// event 41 (the system restarted without a clean shutdown). At most 10 are printed, newest first.
#ifdef _WIN32

#include "crash_report.h"
#include "log_redirect.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winevt.h>

namespace
{
    std::string wide_to_utf8(const wchar_t *text)
    {
        if (text == nullptr || *text == L'\0')
            return {};
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0)
            return {};
        std::string out(static_cast<std::size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
        out.resize(static_cast<std::size_t>(n - 1)); // drop the trailing NUL
        return out;
    }

    std::string ascii_lower(const std::string &text)
    {
        std::string out = text;
        for (char &c : out)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return out;
    }

    // FILETIME value -> "YYYY-MM-DD HH:MM:SS" in local time.
    void format_local_time(std::int64_t filetime, char *out, std::size_t cap)
    {
        FILETIME utc{};
        utc.dwLowDateTime = static_cast<DWORD>(filetime & 0xFFFFFFFFull);
        utc.dwHighDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(filetime) >> 32);
        FILETIME local{};
        SYSTEMTIME st{};
        if (!::FileTimeToLocalFileTime(&utc, &local) || !::FileTimeToSystemTime(&local, &st))
        {
            std::snprintf(out, cap, "?");
            return;
        }
        std::snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u", static_cast<unsigned>(st.wYear),
                      static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay),
                      static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
                      static_cast<unsigned>(st.wSecond));
    }

    // FILETIME value -> "YYYY-MM-DDTHH:MM:SS.mmmZ" (UTC), the event-log XPath form.
    void format_utc_iso(std::int64_t filetime, wchar_t *out, std::size_t cap)
    {
        FILETIME ft{};
        ft.dwLowDateTime = static_cast<DWORD>(filetime & 0xFFFFFFFFull);
        ft.dwHighDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(filetime) >> 32);
        SYSTEMTIME st{};
        if (!::FileTimeToSystemTime(&ft, &st))
        {
            out[0] = L'\0';
            return;
        }
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", static_cast<unsigned>(st.wYear),
                      static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay),
                      static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
                      static_cast<unsigned>(st.wSecond), static_cast<unsigned>(st.wMilliseconds));
        std::size_t i = 0;
        for (; i + 1 < cap && tmp[i] != '\0'; ++i)
            out[i] = static_cast<wchar_t>(tmp[i]);
        out[i] = L'\0';
    }

    // wevtapi, loaded dynamically so the build needs no import library for it.
    struct WevtApi
    {
        bool ok = false;
        decltype(&::EvtQuery) query = nullptr;
        decltype(&::EvtNext) next = nullptr;
        decltype(&::EvtRender) render = nullptr;
        decltype(&::EvtClose) close = nullptr;
        decltype(&::EvtCreateRenderContext) create_context = nullptr;
        decltype(&::EvtFormatMessage) format_message = nullptr;
        decltype(&::EvtOpenPublisherMetadata) open_metadata = nullptr;

        WevtApi()
        {
            HMODULE lib = ::LoadLibraryW(L"wevtapi.dll");
            if (lib == nullptr)
                return;
            query = reinterpret_cast<decltype(query)>(::GetProcAddress(lib, "EvtQuery"));
            next = reinterpret_cast<decltype(next)>(::GetProcAddress(lib, "EvtNext"));
            render = reinterpret_cast<decltype(render)>(::GetProcAddress(lib, "EvtRender"));
            close = reinterpret_cast<decltype(close)>(::GetProcAddress(lib, "EvtClose"));
            create_context = reinterpret_cast<decltype(create_context)>(::GetProcAddress(lib, "EvtCreateRenderContext"));
            format_message = reinterpret_cast<decltype(format_message)>(::GetProcAddress(lib, "EvtFormatMessage"));
            open_metadata =
                reinterpret_cast<decltype(open_metadata)>(::GetProcAddress(lib, "EvtOpenPublisherMetadata"));
            // open_metadata is optional: without it the events print without their message text.
            ok = query && next && render && close && create_context && format_message;
        }
    };

    bool provider_matches(const std::string &provider, unsigned short id)
    {
        const std::string lower = ascii_lower(provider);
        if (lower == "nvlddmkm" || lower == "amdkmdag")
            return true;
        if (lower.size() >= 4 && lower.compare(0, 4, "igfx") == 0)
            return true;
        if (lower == "display" && id == 4101)
            return true;
        if (lower == "kernel-power" && id == 41)
            return true;
        return false;
    }

    // The message's first line, truncated to 200 characters. Empty when the message cannot be rendered.
    std::string event_message_first_line(const WevtApi &api, EVT_HANDLE event, const std::wstring &provider)
    {
        // EvtFormatMessage needs the provider's metadata; without it the call faults or fails.
        if (api.open_metadata == nullptr || provider.empty())
            return {};
        EVT_HANDLE metadata = api.open_metadata(nullptr, provider.c_str(), nullptr, 0, 0);
        if (metadata == nullptr)
            return {};
        DWORD used = 0;
        (void)api.format_message(metadata, event, 0, 0, nullptr, EvtFormatMessageEvent, 0, nullptr, &used);
        if (used == 0)
        {
            api.close(metadata);
            return {};
        }
        std::wstring buffer(used, L'\0');
        const BOOL ok =
            api.format_message(metadata, event, 0, 0, nullptr, EvtFormatMessageEvent, used, buffer.data(), &used);
        api.close(metadata);
        if (!ok)
            return {};
        const std::size_t cut = buffer.find_first_of(L"\r\n");
        if (cut != std::wstring::npos)
            buffer.resize(cut);
        if (buffer.size() > 200)
            buffer.resize(200);
        return wide_to_utf8(buffer.c_str());
    }
} // namespace

bool don_parse_previous_start_header(const char *line, std::int64_t *out_utc)
{
    if (line == nullptr || out_utc == nullptr)
        return false;
    const char *marker = std::strstr(line, " started ");
    if (marker == nullptr)
        return false;
    marker += std::strlen(" started ");
    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(marker, "%u-%u-%u %u:%u:%u", &year, &month, &day, &hour, &minute, &second) != 6)
        return false;
    if (month == 0 || month > 12 || day == 0 || day > 31 || hour > 23 || minute > 59 || second > 59)
        return false;
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(year);
    st.wMonth = static_cast<WORD>(month);
    st.wDay = static_cast<WORD>(day);
    st.wHour = static_cast<WORD>(hour);
    st.wMinute = static_cast<WORD>(minute);
    st.wSecond = static_cast<WORD>(second);
    SYSTEMTIME utc{};
    if (!::TzSpecificLocalTimeToSystemTime(nullptr, &st, &utc))
        return false;
    FILETIME ft{};
    if (!::SystemTimeToFileTime(&utc, &ft))
        return false;
    *out_utc = (static_cast<std::int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return true;
}

bool don_previous_start_from_log(std::int64_t *out_utc)
{
    if (out_utc == nullptr)
        return false;
    const std::filesystem::path path = don_log_path();
    if (path.empty())
        return false;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    const char *marker = "===== Battle Stadium D.O.N started ";
    const std::size_t marker_len = std::strlen(marker);
    std::vector<const char *> headers;
    std::size_t pos = 0;
    while ((pos = text.find(marker, pos)) != std::string::npos)
    {
        headers.push_back(text.data() + pos);
        pos += marker_len;
    }
    // The current run's header was just appended by don_log_redirect(); the previous run is the one before.
    if (headers.size() < 2)
        return false;
    return don_parse_previous_start_header(headers[headers.size() - 2], out_utc);
}

namespace
{
    void driver_events_report_impl(std::int64_t previous_start_utc);

    // SEH wrapper: no C++ object with a destructor lives in this function (a requirement of __try).
    bool driver_events_report_guarded(std::int64_t previous_start_utc)
    {
        __try
        {
            driver_events_report_impl(previous_start_utc);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
} // namespace

void don_driver_events_report(std::int64_t previous_start_utc)
{
    // A diagnostic: it must never stop the game.
    if (!driver_events_report_guarded(previous_start_utc))
        std::fprintf(stderr, "[don] windows event report faulted and was skipped\n");
}

namespace
{
void driver_events_report_impl(std::int64_t previous_start_utc)
{
    const WevtApi api;
    if (!api.ok)
        return;

    wchar_t iso[64];
    format_utc_iso(previous_start_utc, iso, 64);
    if (iso[0] == L'\0')
        return;

    wchar_t query[512];
    std::size_t i = 0;
    const wchar_t *sel = L"*[System[TimeCreated[@SystemTime >= '";
    while (*sel != L'\0' && i + 1 < 512)
        query[i++] = *sel++;
    for (const wchar_t *p = iso; *p != L'\0' && i + 1 < 512; ++p)
        query[i++] = *p;
    const wchar_t *tail = L"']]]";
    while (*tail != L'\0' && i + 1 < 512)
        query[i++] = *tail++;
    query[i] = L'\0';

    EVT_HANDLE results = api.query(nullptr, L"System", query, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (results == nullptr)
        return;

    // The system context renders every System property; they are read by index (EVT_SYSTEM_PROPERTY_ID).
    EVT_HANDLE context = api.create_context(0, nullptr, EvtRenderContextSystem);
    if (context == nullptr)
    {
        // Without the values context there is no provider/id to filter on: say nothing rather than a
        // wrong "no resets".
        api.close(results);
        return;
    }

    unsigned printed = 0;
    std::vector<std::uint64_t> values_buffer(64); // 8-byte aligned storage for the EVT_VARIANT array
    for (unsigned examined = 0; examined < 2000 && printed < 10; ++examined)
    {
        EVT_HANDLE event = nullptr;
        DWORD returned = 0;
        if (!api.next(results, 1, &event, 1000, 0, &returned) || returned == 0)
            break;

        std::string provider;
        std::wstring provider_wide;
        unsigned short id = 0;
        std::int64_t time_utc = 0;
        {
            DWORD used = 0, count = 0;
            BOOL rendered = api.render(context, event, EvtRenderEventValues,
                                       static_cast<DWORD>(values_buffer.size() * sizeof(std::uint64_t)),
                                       values_buffer.data(), &used, &count);
            if (!rendered && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER && used > 0 && used <= (1u << 20))
            {
                values_buffer.resize((used + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
                rendered = api.render(context, event, EvtRenderEventValues,
                                      static_cast<DWORD>(values_buffer.size() * sizeof(std::uint64_t)),
                                      values_buffer.data(), &used, &count);
            }
            const EVT_VARIANT *values = reinterpret_cast<const EVT_VARIANT *>(values_buffer.data());
            if (rendered && count > EvtSystemTimeCreated)
            {
                const EVT_VARIANT &p = values[EvtSystemProviderName];
                if (p.Type == EvtVarTypeString && p.StringVal != nullptr)
                {
                    provider_wide = p.StringVal;
                    provider = wide_to_utf8(p.StringVal);
                }
                if (values[EvtSystemEventID].Type == EvtVarTypeUInt16)
                    id = values[EvtSystemEventID].UInt16Val;
                // FileTimeVal is the FILETIME as one ULONGLONG (low 32 bits first on little-endian).
                if (values[EvtSystemTimeCreated].Type == EvtVarTypeFileTime)
                    time_utc = static_cast<std::int64_t>(values[EvtSystemTimeCreated].FileTimeVal);
            }
        }

        if (!provider_matches(provider, id))
        {
            api.close(event);
            continue;
        }

        const std::string message = event_message_first_line(api, event, provider_wide);
        char when[64];
        format_local_time(time_utc, when, sizeof(when));
        if (message.empty())
            std::fprintf(stderr, "[don] windows event: %s %s %u\n", when, provider.c_str(),
                         static_cast<unsigned>(id));
        else
            std::fprintf(stderr, "[don] windows event: %s %s %u: %s\n", when, provider.c_str(),
                         static_cast<unsigned>(id), message.c_str());
        ++printed;
        api.close(event);
    }

    api.close(context);
    api.close(results);

    if (printed == 0)
    {
        char when[64];
        format_local_time(previous_start_utc, when, sizeof(when));
        std::fprintf(stderr, "[don] windows event: no display driver resets since %s\n", when);
    }
}
} // namespace

#endif // _WIN32
