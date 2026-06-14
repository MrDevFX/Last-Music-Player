#pragma once

#include <winrt/Windows.Foundation.h>

namespace LastMusicPlayer::Backend
{
    winrt::hstring NormalizeProviderBaseUrl(winrt::hstring const& savedBase);
    winrt::hstring BuildProviderStreamUrl(
        winrt::hstring const& filePath,
        winrt::hstring const& sourceUrl,
        winrt::hstring const& provider,
        winrt::hstring const& artworkUrl,
        winrt::hstring const& providerBaseUrl,
        winrt::hstring const& apiToken);

    winrt::hstring BuildProviderArtworkUrl(
        winrt::hstring const& artworkUrl,
        winrt::hstring const& providerBaseUrl,
        winrt::hstring const& apiToken);
}
