#pragma once
#include <winrt/Windows.Foundation.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace LastMusicPlayer::Backend
{
    // Disk prefetch cache for remote (provider-streamed) tracks.
    //
    // Progressive HTTP streams rebuffer on any network jitter — a fraction of
    // a second of silence, then playback resumes. We can't deepen the
    // MediaPlayer's progressive buffer, but we CAN download the *upcoming*
    // tracks to local files while the current one plays: by the time the user
    // reaches them they are fully on disk and play with no live network at all,
    // so jitter can't stall them. The currently-streaming track is handled
    // separately by MediaFailed auto-recovery in the UI layer.
    //
    // A cache file is published (made visible to ReadyPath) only after a
    // complete, successful download + atomic rename, so playback always reuses
    // the well-tested local-file path on a *whole* file — never a partial one.
    class StreamCache
    {
    public:
        // Start a background download of `streamUrl`, identified by the stable
        // per-track `sourceKey` (the track's upstream SourceUrl). No-op if the
        // track is already cached, a download for this key is already running,
        // or the global in-flight cap is hit. Fire-and-forget; safe to call
        // repeatedly (e.g. every time the Up Next queue is rebuilt).
        void Prefetch(std::wstring const& sourceKey, std::wstring const& streamUrl);

        // Full path of a completed cache file for `sourceKey`, or empty if not
        // cached. Survives restarts: on a cold cache it also checks disk for a
        // previously-downloaded file matching the key.
        std::wstring ReadyPath(std::wstring const& sourceKey);

        // Delete leftover *.part files and trim the cache to the size/count cap.
        // Call once at startup.
        void PruneOnStartup();

        // Drop all completed and in-flight cache entries owned by this app.
        void Clear();

    private:
        enum class Status { InFlight, Ready, Failed };
        struct Entry
        {
            Status status{ Status::InFlight };
            std::wstring path;             // populated when Ready
            unsigned long long startTick{ 0 };  // GetTickCount64 when InFlight began
        };

        std::filesystem::path CacheDir();
        static std::wstring HashKey(std::wstring const& sourceKey);
        std::wstring FindOnDisk(std::wstring const& hash);
        void TrimToCap();
        winrt::Windows::Foundation::IAsyncAction DownloadAsync(std::wstring sourceKey, std::wstring streamUrl, uint64_t generation);

        std::mutex m_mutex;
        std::unordered_map<std::wstring, Entry> m_entries;  // sourceKey -> entry
        uint64_t m_generation{ 0 };

        // Keep concurrent prefetch downloads (and total disk footprint) modest:
        // we only need a couple of tracks of lookahead, and the box is a music
        // player, not a downloader.
        static constexpr int kMaxInFlight = 2;
        static constexpr int kMaxFiles = 24;
        static constexpr unsigned long long kMaxBytes = 256ull * 1024 * 1024;  // 256 MiB
        // A download older than this no longer counts against the concurrency
        // cap, so a stalled connection can't permanently disable prefetch. A
        // provider may deliver only slightly faster than realtime, so even a
        // long track finishes well under this; a download still running past it
        // is treated as hung.
        static constexpr unsigned long long kMaxInFlightAgeMs = 6ull * 60 * 1000;  // 6 min
    };
}
