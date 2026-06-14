#pragma once

#include <winrt/Windows.Foundation.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace LastMusicPlayer::Backend
{
    struct LyricLine
    {
        int64_t TimeMs{ 0 };
        std::wstring Text;
    };

    struct LyricsResult
    {
        bool Found{ false };
        bool Instrumental{ false };
        std::wstring Source;
        std::wstring Plain;
        std::vector<LyricLine> Synced;
        std::wstring TrackName;
        std::wstring ArtistName;
    };

    class LyricsService
    {
    public:
        LyricsService();

        void SetProviderEndpoint(winrt::hstring const& baseUrl, winrt::hstring const& bearerToken);

        // Returns the raw JSON payload from /v1/lyrics. Returns empty hstring on
        // any network failure so callers can render the "No lyrics found" state
        // without having to catch exceptions.
        winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> FetchPayloadAsync(
            winrt::hstring artist,
            winrt::hstring title,
            winrt::hstring album,
            int64_t durationMs,
            winrt::hstring sourceUrl);

        // Pure: parses the /v1/lyrics JSON shape into a LyricsResult. Returns
        // an empty result for empty or malformed input.
        static LyricsResult ParseLyrics(winrt::hstring const& payload);

        // Pure: returns the index in `synced` whose TimeMs is the greatest value
        // not exceeding positionMs. Returns -1 before the first stamp or when
        // `synced` is empty.
        static int32_t ActiveLineIndex(std::vector<LyricLine> const& synced, int64_t positionMs);

    private:
        static std::wstring MakeCacheKey(
            winrt::hstring const& artist,
            winrt::hstring const& title,
            int64_t durationMs,
            winrt::hstring const& sourceUrl);

        bool TryGetCachedPayload(std::wstring const& key, winrt::hstring& outPayload) const;
        void StoreCachedPayload(std::wstring const& key, winrt::hstring const& payload);

        winrt::hstring m_baseUrl{ L"http://127.0.0.1:4527" };
        winrt::hstring m_token;
        mutable std::unordered_map<std::wstring, winrt::hstring> m_cache;
        mutable std::deque<std::wstring> m_cacheOrder;
        static constexpr size_t kCacheCapacity = 32;
    };
}
