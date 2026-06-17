#pragma once
#include <windows.h>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>

namespace LastMusicPlayer::Backend
{
    // Rich-presence payload built per track. Empty title clears the activity.
    struct PresencePayload
    {
        std::wstring title;
        std::wstring artist;
        std::wstring album;          // kept for future use; not currently emitted
        std::wstring artworkUrl;     // HTTPS only; empty -> omit large_image
        double durationSeconds{ 0.0 };
        double positionSeconds{ 0.0 };
        bool isPlaying{ true };
        bool isLocal{ false };       // drives the "Source: Local/Remote" label
    };

    // Discord Rich Presence client over the IPC named pipe
    // (\\.\pipe\discord-ipc-N). Supports a music timeline with play/pause
    // and artwork. SET_ACTIVITY sends are debounced (Discord throttles to
    // ~5 / 20 s) and deduped.
    class DiscordPresence
    {
    public:
        DiscordPresence() = default;
        ~DiscordPresence();

        DiscordPresence(DiscordPresence const&) = delete;
        DiscordPresence& operator=(DiscordPresence const&) = delete;

        bool Connect();
        void Disconnect();
        bool IsConnected() const { return m_pipe != INVALID_HANDLE_VALUE; }

        void SetNowPlaying(PresencePayload const& payload);
        // durationSeconds <= 0 keeps the cached value unchanged. Pass the
        // live MediaPlaybackSession.NaturalDuration when you have it so
        // late-resolved durations (the track was still opening when
        // SetNowPlaying ran) flow into the next activity send.
        void SetPlaybackState(bool isPlaying, double positionSeconds, double durationSeconds = -1.0);
        void SetPosition(double positionSeconds);
        // Late duration update — fires from MediaPlaybackSession's
        // NaturalDurationChanged event. Triggers a re-send so the
        // progress bar can finally appear.
        void SetDuration(double durationSeconds);
        // Updates the artwork field of the cached payload (set asynchronously
        // after the provider service has converted an HTTPS URL to a Discord
        // External Asset `mp:external/...` proxy URL) and re-emits the activity.
        // `proxyUrl` must already be in the form Discord accepts.
        void SetArtworkProxyUrl(
            std::wstring const& proxyUrl,
            std::wstring const& originalTitle,
            std::wstring const& originalArtist);
        void Clear();

    private:
        bool WriteFrame(int opcode, std::string const& payload);
        std::string BuildActivityJson(PresencePayload const& p) const;
        void SendOrDefer(std::string const& json, bool bypassGate);
        void FlushPending();
        static void CALLBACK FlushCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER);

        HANDLE m_pipe{ INVALID_HANDLE_VALUE };

        std::mutex m_mutex;
        std::optional<PresencePayload> m_last;
        std::string m_lastSentJson;
        std::size_t m_lastSentHash{ 0 };
        std::chrono::steady_clock::time_point m_lastSendAt{};
        std::string m_pendingJson;
        PTP_TIMER m_pendingTimer{ nullptr };
        bool m_pendingScheduled{ false };
        // Throttles lazy-reconnect attempts inside SendOrDefer so a
        // Discord-not-running state doesn't spam CreateFile calls on
        // every playback-state change.
        std::chrono::steady_clock::time_point m_lastReconnectAttempt{};
    };
}
