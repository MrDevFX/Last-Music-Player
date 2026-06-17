#include "pch.h"
#include "Backend/DiscordPresence.h"
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <cmath>

// The Discord application client id is build-private. Official builds define it
// in a gitignored Backend/AppSecrets.local.h; public builds compile with the
// empty default below, which disables Discord Rich Presence. Copy
// Backend/AppSecrets.example.h to AppSecrets.local.h and fill it in to enable.
#if __has_include("Backend/AppSecrets.local.h")
#include "Backend/AppSecrets.local.h"
#endif
#ifndef LMP_DISCORD_CLIENT_ID
#define LMP_DISCORD_CLIENT_ID ""
#endif

namespace
{
    // Empty id (public builds) → Rich Presence disabled: Connect() short-circuits
    // below, so the toggle and persisted preference still work but have no
    // visible effect.
    constexpr char kDiscordClientId[] = LMP_DISCORD_CLIENT_ID;

    // Minimum gap between SET_ACTIVITY sends. Discord throttles to roughly
    // five updates per 20 seconds; staying above ~1.5 s lets seek scrubs
    // and rapid play/pause coalesce without ever tripping the limit.
    constexpr std::chrono::milliseconds kMinSendGap{ 1500 };

    std::string ToUtf8(std::wstring const& text)
    {
        if (text.empty())
        {
            return {};
        }
        int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (required <= 1)
        {
            return {};
        }
        std::string out(static_cast<size_t>(required - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), required, nullptr, nullptr);
        return out;
    }

    std::string JsonEscape(std::wstring const& text)
    {
        auto utf8 = ToUtf8(text);
        std::string out;
        out.reserve(utf8.size() + 8);
        for (unsigned char ch : utf8)
        {
            switch (ch)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (ch < 0x20)
                {
                    char buf[8];
                    sprintf_s(buf, "\\u%04x", ch);
                    out += buf;
                }
                else
                {
                    out += static_cast<char>(ch);
                }
                break;
            }
        }
        return out;
    }

    // Used to suppress the banner-hover "album" tooltip when the provider
    // hands back album == title (the common singles case). Compare case-
    // insensitively and treat any whitespace run as a single space so
    // "Piya  Tose - Qawwali Version" and "piya tose – qawwali version"
    // still match.
    bool EquivalentText(std::wstring const& a, std::wstring const& b)
    {
        auto normalize = [](std::wstring const& s) {
            std::wstring out;
            out.reserve(s.size());
            for (wchar_t ch : s) {
                if (iswspace(ch)) {
                    if (!out.empty() && out.back() != L' ') out += L' ';
                    continue;
                }
                out += static_cast<wchar_t>(towlower(ch));
            }
            while (!out.empty() && out.back() == L' ') out.pop_back();
            return out;
        };
        return normalize(a) == normalize(b);
    }
}

namespace LastMusicPlayer::Backend
{
    DiscordPresence::~DiscordPresence()
    {
        {
            std::scoped_lock lock{ m_mutex };
            if (m_pendingTimer)
            {
                SetThreadpoolTimer(m_pendingTimer, nullptr, 0, 0);
                WaitForThreadpoolTimerCallbacks(m_pendingTimer, TRUE);
                CloseThreadpoolTimer(m_pendingTimer);
                m_pendingTimer = nullptr;
            }
        }
        Disconnect();
    }

    bool DiscordPresence::WriteFrame(int opcode, std::string const& payload)
    {
        if (m_pipe == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        std::vector<char> frame;
        frame.resize(8 + payload.size());
        std::int32_t op = opcode;
        std::int32_t len = static_cast<std::int32_t>(payload.size());
        std::memcpy(frame.data(), &op, 4);
        std::memcpy(frame.data() + 4, &len, 4);
        std::memcpy(frame.data() + 8, payload.data(), payload.size());

        DWORD written = 0;
        if (!WriteFile(m_pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr))
        {
            Disconnect();
            return false;
        }
        return true;
    }

    bool DiscordPresence::Connect()
    {
        if (IsConnected())
        {
            return true;
        }

        // No client id compiled in (public builds) → don't attempt the IPC
        // handshake at all; Rich Presence stays disabled.
        if (kDiscordClientId[0] == '\0')
        {
            return false;
        }

        for (int i = 0; i < 10; ++i)
        {
            std::wstring path = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
            HANDLE pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE)
            {
                m_pipe = pipe;
                std::string handshake =
                    std::string("{\"v\":1,\"client_id\":\"") + kDiscordClientId + "\"}";
                if (!WriteFrame(0, handshake))
                {
                    return false;
                }
                return true;
            }
        }
        return false;
    }

    void DiscordPresence::Disconnect()
    {
        if (m_pipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }
    }

    std::string DiscordPresence::BuildActivityJson(PresencePayload const& p) const
    {
        // Wall-clock timestamps for the progress bar. Discord renders the
        // bar locally between start and end; we only re-send when the
        // anchor needs to move (seek, pause/resume, track change).
        auto now = std::chrono::system_clock::now();
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        double position = (std::max)(0.0, p.positionSeconds);
        double duration = (std::max)(0.0, p.durationSeconds);
        long long startMs = nowMs - static_cast<long long>(position * 1000.0);
        long long endMs   = nowMs + static_cast<long long>((std::max)(0.0, duration - position) * 1000.0);

        std::ostringstream os;
        os << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"
           << GetCurrentProcessId()
           << ",\"activity\":{\"type\":2";

        // `name` is what Discord renders as the activity header
        // ("Listening to <name>"). Without it, Discord falls back to the
        // dev-portal app name, which works for the basic card, but
        // including an explicit name matches common music-app behaviour.
        os << ",\"name\":\"Last Music\"";

        if (!p.title.empty())
        {
            os << ",\"details\":\"" << JsonEscape(p.title) << "\"";
        }

        // State line: music-app style: just the artist, with " · Paused"
        // appended on pause. The album lives only in the banner-hover
        // tooltip below (and only when it's actually meaningful), so the
        // state line stays clean even for singles where provider returns
        // album == title.
        std::wstring state = p.artist;
        if (!p.isPlaying)
        {
            if (!state.empty()) state += L" · ";
            state += L"Paused";
        }
        if (!state.empty())
        {
            os << ",\"state\":\"" << JsonEscape(state) << "\"";
        }

        // Timestamps: only emit while playing so the bar freezes on pause.
        // Need a positive duration too, else Discord renders an
        // open-ended timer instead of a progress bar.
        if (p.isPlaying && duration > 0.5)
        {
            os << ",\"timestamps\":{\"start\":" << startMs
               << ",\"end\":" << endMs << "}";
        }

        // Discord's RPC IPC `large_image` only accepts URLs from a small CDN
        // whitelist; arbitrary HTTPS URLs are silently rejected — Discord still
        // shows the activity card, but no banner renders. The provider supplies
        // a whitelist-compatible cover URL asynchronously via SetArtworkProxyUrl.
        // Until that resolves (or if none is found), p.artworkUrl is empty here
        // and the assets field is omitted entirely.
        //
        // Also accepts bare asset keys (no slashes) for forward-compat
        // with a future dev-portal-uploaded fallback.
        bool isHttpsUrl = p.artworkUrl.rfind(L"https://", 0) == 0;
        bool isDiscordExternalAsset = p.artworkUrl.rfind(L"mp:external/", 0) == 0;
        bool isBareAssetKey = !p.artworkUrl.empty()
            && p.artworkUrl.find(L'/') == std::wstring::npos;
        if (isHttpsUrl || isDiscordExternalAsset || isBareAssetKey)
        {
            os << ",\"assets\":{\"large_image\":\"" << JsonEscape(p.artworkUrl) << "\"";
            // Third-line label on the activity card. Always advertise the
            // source ("Source: Local" or "Source: Remote") instead of an
            // album — providers commonly return album == title or generic
            // placeholders ("Imported Playlist", "Unknown Album"), so the
            // source string is uniformly more informative.
            std::wstring sourceLabel = p.isLocal ? L"Source: Local" : L"Source: Remote Music API";
            os << ",\"large_text\":\"" << JsonEscape(sourceLabel) << "\"";
            os << "}";
        }

        os << "}},\"nonce\":\"" << GetTickCount64() << "\"}";
        return os.str();
    }

    void CALLBACK DiscordPresence::FlushCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER)
    {
        auto self = static_cast<DiscordPresence*>(context);
        self->FlushPending();
    }

    void DiscordPresence::FlushPending()
    {
        std::string json;
        {
            std::scoped_lock lock{ m_mutex };
            m_pendingScheduled = false;
            if (m_pendingJson.empty())
            {
                return;
            }
            json.swap(m_pendingJson);
        }
        // Use bypassGate=true: the timer already waited out the window.
        SendOrDefer(json, /*bypassGate*/ true);
    }

    void DiscordPresence::SendOrDefer(std::string const& json, bool bypassGate)
    {
        if (!IsConnected())
        {
            // Lazy reconnect: when Discord is restarted while Last Music
            // is running, the pipe drops and every subsequent send used
            // to be silently lost forever (no resume path until the user
            // toggled the integration off + on). Attempt a single
            // Connect() here, throttled so a missing-Discord state can't
            // hammer the OS with CreateFile calls on every state change.
            constexpr auto kReconnectBackoff = std::chrono::seconds(5);
            auto now = std::chrono::steady_clock::now();
            bool throttled =
                m_lastReconnectAttempt.time_since_epoch().count() != 0 &&
                (now - m_lastReconnectAttempt) < kReconnectBackoff;
            if (throttled)
            {
                return;
            }
            m_lastReconnectAttempt = now;
            if (!Connect())
            {
                return;
            }
        }

        std::size_t hash = std::hash<std::string>{}(json);

        std::scoped_lock lock{ m_mutex };

        // Dedup: identical activity to the last successful send is a no-op.
        if (hash == m_lastSentHash && !m_lastSentJson.empty())
        {
            return;
        }

        if (!bypassGate)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = now - m_lastSendAt;
            if (m_lastSendAt.time_since_epoch().count() != 0 && elapsed < kMinSendGap)
            {
                // Defer: stash latest json; schedule a one-shot threadpool
                // timer if one isn't already running. A newer call simply
                // overwrites m_pendingJson without rescheduling.
                m_pendingJson = json;
                if (!m_pendingScheduled)
                {
                    if (!m_pendingTimer)
                    {
                        m_pendingTimer = CreateThreadpoolTimer(&DiscordPresence::FlushCallback, this, nullptr);
                    }
                    if (m_pendingTimer)
                    {
                        auto wait = kMinSendGap - std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                        if (wait.count() < 0) wait = std::chrono::milliseconds(0);
                        FILETIME ft{};
                        ULARGE_INTEGER ul{};
                        // SetThreadpoolTimer takes a negative relative time
                        // in 100-ns units when used as a FILETIME pointer.
                        long long relative = -static_cast<long long>(wait.count()) * 10000LL;
                        ul.QuadPart = static_cast<ULONGLONG>(relative);
                        ft.dwLowDateTime  = ul.LowPart;
                        ft.dwHighDateTime = ul.HighPart;
                        SetThreadpoolTimer(m_pendingTimer, &ft, 0, 0);
                        m_pendingScheduled = true;
                    }
                }
                return;
            }
        }

        if (WriteFrame(1, json))
        {
            m_lastSendAt   = std::chrono::steady_clock::now();
            m_lastSentJson = json;
            m_lastSentHash = hash;
        }
        else
        {
        }
    }

    void DiscordPresence::SetNowPlaying(PresencePayload const& payload)
    {
        if (payload.title.empty())
        {
            Clear();
            return;
        }

        bool titleChanged = false;
        {
            std::scoped_lock lock{ m_mutex };
            titleChanged = !m_last
                || m_last->title != payload.title
                || m_last->artist != payload.artist;
            m_last = payload;
        }

        std::string json = BuildActivityJson(payload);
        // A new track is the one update the user notices most — let it
        // jump the gate so it shows immediately.
        SendOrDefer(json, /*bypassGate*/ titleChanged);
    }

    void DiscordPresence::SetPlaybackState(bool isPlaying, double positionSeconds, double durationSeconds)
    {
        PresencePayload snapshot;
        {
            std::scoped_lock lock{ m_mutex };
            if (!m_last) return;
            m_last->isPlaying = isPlaying;
            m_last->positionSeconds = positionSeconds;
            // Only overwrite the cached duration when the caller actually
            // has one — many sources (MediaPlaybackSession on Opening /
            // Buffering states, tracks loaded from the queue with stale
            // DB metadata) return 0 here. A late NaturalDurationChanged
            // calls SetDuration separately.
            if (durationSeconds > 0.5) m_last->durationSeconds = durationSeconds;
            snapshot = *m_last;
        }
        std::string json = BuildActivityJson(snapshot);
        SendOrDefer(json, /*bypassGate*/ false);
    }

    void DiscordPresence::SetDuration(double durationSeconds)
    {
        if (durationSeconds <= 0.5) return;
        PresencePayload snapshot;
        {
            std::scoped_lock lock{ m_mutex };
            if (!m_last) return;
            // Don't churn if the cache already has the same duration to
            // within one second (NaturalDurationChanged occasionally
            // fires twice with the same value).
            if (std::abs(m_last->durationSeconds - durationSeconds) < 1.0) return;
            m_last->durationSeconds = durationSeconds;
            snapshot = *m_last;
        }
        std::string json = BuildActivityJson(snapshot);
        SendOrDefer(json, /*bypassGate*/ false);
    }

    void DiscordPresence::SetArtworkProxyUrl(
        std::wstring const& proxyUrl,
        std::wstring const& originalTitle,
        std::wstring const& originalArtist)
    {
        if (proxyUrl.empty())
        {
            return;
        }
        PresencePayload snapshot;
        {
            std::scoped_lock lock{ m_mutex };
            // Drop the update if the user has since skipped — Discord
            // should never end up showing track B with track A's art.
            if (!m_last)
            {
                return;
            }
            if (m_last->title != originalTitle)
            {
                return;
            }
            if (m_last->artist != originalArtist)
            {
                return;
            }
            m_last->artworkUrl = proxyUrl;
            snapshot = *m_last;
        }
        std::string json = BuildActivityJson(snapshot);
        SendOrDefer(json, /*bypassGate*/ false);
    }

    void DiscordPresence::SetPosition(double positionSeconds)
    {
        PresencePayload snapshot;
        {
            std::scoped_lock lock{ m_mutex };
            if (!m_last) return;
            m_last->positionSeconds = positionSeconds;
            snapshot = *m_last;
        }
        std::string json = BuildActivityJson(snapshot);
        SendOrDefer(json, /*bypassGate*/ false);
    }

    void DiscordPresence::Clear()
    {
        {
            std::scoped_lock lock{ m_mutex };
            m_last.reset();
            m_pendingJson.clear();
            m_pendingScheduled = false;
        }
        if (!IsConnected())
        {
            return;
        }
        std::string payload =
            "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) +
            ",\"activity\":null},\"nonce\":\"" + std::to_string(GetTickCount64()) + "\"}";
        if (WriteFrame(1, payload))
        {
            std::scoped_lock lock{ m_mutex };
            m_lastSendAt = std::chrono::steady_clock::now();
            m_lastSentJson.clear();
            m_lastSentHash = 0;
        }
    }
}
