#include "pch.h"
#include "Backend/ProviderHelpers.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace LastMusicPlayer::Backend
{
    namespace
    {
        std::wstring ToLowerCopy(winrt::hstring const& value)
        {
            std::wstring lowered{ value.c_str() };
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch)
            {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            return lowered;
        }

        bool IsHttpUrl(winrt::hstring const& value)
        {
            auto lower = ToLowerCopy(value);
            return lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0;
        }

        winrt::hstring EscapeUrlComponent(winrt::hstring const& value)
        {
            return winrt::Windows::Foundation::Uri::EscapeComponent(value);
        }

        // (UnescapeUrlComponent removed — dead code with a latent
        // failure-mode bug: catch-all returned the still-encoded input,
        // silently conflating "already decoded" with "could not decode".
        // If URL-unescaping is needed later, prefer std::optional return
        // so callers must handle decode failure explicitly.)

        uint64_t StableFnv1a64(std::wstring const& value)
        {
            uint64_t hash = 1469598103934665603ull;
            for (wchar_t ch : value)
            {
                auto code = static_cast<uint32_t>(ch);
                for (int shift = 0; shift < 16; shift += 8)
                {
                    hash ^= static_cast<uint8_t>((code >> shift) & 0xFF);
                    hash *= 1099511628211ull;
                }
            }
            return hash;
        }

        std::wstring Hex64(uint64_t value)
        {
            wchar_t buffer[17]{};
            swprintf_s(buffer, L"%016llx", static_cast<unsigned long long>(value));
            return buffer;
        }

        winrt::hstring StableStreamId(winrt::hstring const& streamProvider, winrt::hstring const& sourceUrl)
        {
            auto idSeed = std::wstring(sourceUrl.c_str());
            return streamProvider + L":" + winrt::hstring{ Hex64(StableFnv1a64(idSeed)) };
        }

        // Strips named query params from `url` before reattaching a fresh
        // access token derived from the user's current ProviderApiKey setting.
        winrt::hstring StripQueryParams(winrt::hstring const& url, std::initializer_list<wchar_t const*> names)
        {
            std::wstring text{ url.c_str() };
            auto queryStart = text.find(L'?');
            if (queryStart == std::wstring::npos)
            {
                return url;
            }

            auto fragmentStart = text.find(L'#', queryStart + 1);
            auto queryEnd = fragmentStart == std::wstring::npos ? text.size() : fragmentStart;
            auto query = text.substr(queryStart + 1, queryEnd - queryStart - 1);
            std::vector<std::wstring> kept;
            size_t start = 0;
            while (start <= query.size())
            {
                auto amp = query.find(L'&', start);
                auto part = query.substr(start, amp == std::wstring::npos ? std::wstring::npos : amp - start);
                bool drop = false;
                for (auto name : names)
                {
                    std::wstring needle{ name };
                    needle += L"=";
                    if (part.rfind(needle, 0) == 0)
                    {
                        drop = true;
                        break;
                    }
                }
                if (!drop && !part.empty())
                {
                    kept.push_back(part);
                }
                if (amp == std::wstring::npos)
                {
                    break;
                }
                start = amp + 1;
            }

            std::wstring rebuilt = text.substr(0, queryStart);
            if (!kept.empty())
            {
                rebuilt += L"?";
                for (size_t i = 0; i < kept.size(); ++i)
                {
                    if (i > 0)
                    {
                        rebuilt += L"&";
                    }
                    rebuilt += kept[i];
                }
            }
            if (fragmentStart != std::wstring::npos)
            {
                rebuilt += text.substr(fragmentStart);
            }
            return winrt::hstring{ rebuilt };
        }

        winrt::hstring AppendAccessToken(winrt::hstring const& url, winrt::hstring const& apiToken)
        {
            if (apiToken.empty()) return url;
            std::wstring text{ url.c_str() };
            std::wstring fragment;
            if (auto fragmentStart = text.find(L'#'); fragmentStart != std::wstring::npos)
            {
                fragment = text.substr(fragmentStart);
                text.resize(fragmentStart);
            }
            wchar_t separator = (text.find(L'?') == std::wstring::npos) ? L'?' : L'&';
            text += separator;
            text += L"access_token=";
            text += EscapeUrlComponent(apiToken).c_str();
            text += fragment;
            return winrt::hstring{ text };
        }

        winrt::hstring DetermineStreamProvider(winrt::hstring const& provider, winrt::hstring const& sourceUrl)
        {
            (void)sourceUrl;
            // The provider segment of the stream id is opaque to the server,
            // which resolves the stream from the ?url= query param. Keep this
            // generic so runtime URLs never expose the provider's internal name.
            auto normalized = ToLowerCopy(provider);
            return (normalized.empty() || normalized == L"direct")
                ? winrt::hstring{ L"direct" }
                : winrt::hstring{ L"remote" };
        }

        winrt::hstring RebaseProviderStreamUrl(
            winrt::hstring const& streamUrl,
            winrt::hstring const& providerBaseUrl,
            winrt::hstring const& apiToken)
        {
            if (!IsHttpUrl(streamUrl))
            {
                return {};
            }

            std::wstring text{ streamUrl.c_str() };
            if (auto fragmentStart = text.find(L"#lmp="); fragmentStart != std::wstring::npos)
            {
                text.resize(fragmentStart);
            }
            auto lowered = ToLowerCopy(winrt::hstring{ text });
            auto pathIdx = lowered.find(L"/v1/stream/");
            if (pathIdx == std::wstring::npos)
            {
                return {};
            }

            auto baseUrl = NormalizeProviderBaseUrl(providerBaseUrl);
            std::wstring rebuilt{ baseUrl.c_str() };
            rebuilt += text.substr(pathIdx);
            auto cleaned = StripQueryParams(winrt::hstring{ rebuilt }, { L"media_token", L"access_token" });
            return AppendAccessToken(cleaned, apiToken);
        }
    }

    winrt::hstring NormalizeProviderBaseUrl(winrt::hstring const& savedBase)
    {
        std::wstring base{ (savedBase.empty() ? winrt::hstring{ L"http://127.0.0.1:4527" } : savedBase).c_str() };
        while (!base.empty() && (base.back() == L'/' || base.back() == L'\\'))
        {
            base.pop_back();
        }
        return winrt::hstring{ base };
    }

    winrt::hstring BuildProviderStreamUrl(
        winrt::hstring const& filePath,
        winrt::hstring const& sourceUrl,
        winrt::hstring const& provider,
        winrt::hstring const& artworkUrl,
        winrt::hstring const& providerBaseUrl,
        winrt::hstring const& apiToken)
    {
        (void)artworkUrl;

        // Prefer an existing Music API stream endpoint when one was returned by
        // search/import. Some services use the stream id as server-side state;
        // preserving the path keeps playback aligned while still refreshing
        // host/settings and auth tokens.
        if (auto providerStreamUrl = RebaseProviderStreamUrl(filePath, providerBaseUrl, apiToken);
            !providerStreamUrl.empty())
        {
            return providerStreamUrl;
        }

        // Prefer rebuilding from sourceUrl. Stored stream URLs may carry
        // expired media tokens or point at a previously configured provider
        // base. Rebuilding from sourceUrl gives us a URL bound to the user's
        // current provider settings and a fresh `access_token`.
        if (IsHttpUrl(sourceUrl))
        {
            auto streamProvider = DetermineStreamProvider(provider, sourceUrl);
            if (!streamProvider.empty())
            {
                auto baseUrl = NormalizeProviderBaseUrl(providerBaseUrl);
                auto streamId = StableStreamId(streamProvider, sourceUrl);
                std::wstring streamUrl{ baseUrl.c_str() };
                streamUrl += L"/v1/stream/";
                streamUrl += EscapeUrlComponent(streamId).c_str();
                streamUrl += L"?url=";
                streamUrl += EscapeUrlComponent(sourceUrl).c_str();
                return AppendAccessToken(winrt::hstring{ streamUrl }, apiToken);
            }
        }

        // Fallback for tracks where sourceUrl wasn't captured (legacy rows).
        // Strip any stale media_token / access_token (they were signed
        // with the provider's secret at save time and almost certainly
        // don't verify any more) and reattach a fresh access_token.
        if (IsHttpUrl(filePath) && ToLowerCopy(filePath).find(L"/v1/stream/") != std::wstring::npos)
        {
            auto cleaned = StripQueryParams(filePath, { L"media_token", L"access_token" });
            return AppendAccessToken(cleaned, apiToken);
        }

        return {};
    }

    winrt::hstring BuildProviderArtworkUrl(
        winrt::hstring const& artworkUrl,
        winrt::hstring const& providerBaseUrl,
        winrt::hstring const& apiToken)
    {
        // Provider-proxied artwork URLs can go stale when their media token
        // expires or their host points at a previously configured provider
        // base. Both cause the card to fall back to the placeholder gradient.
        // Mirror BuildProviderStreamUrl: rebuild the host from the
        // current ProviderBaseUrl, then strip stale tokens and reattach
        // a fresh long-lived `access_token` from ProviderApiKey.
        if (!IsHttpUrl(artworkUrl))
        {
            return artworkUrl;
        }
        std::wstring text{ artworkUrl.c_str() };
        auto lowered = ToLowerCopy(artworkUrl);
        auto pathIdx = lowered.find(L"/v1/artwork");
        if (pathIdx == std::wstring::npos)
        {
            return artworkUrl;
        }

        auto baseUrl = NormalizeProviderBaseUrl(providerBaseUrl);
        std::wstring rebuilt{ baseUrl.c_str() };
        rebuilt += text.substr(pathIdx);
        auto cleaned = StripQueryParams(winrt::hstring{ rebuilt }, { L"media_token", L"access_token" });
        return AppendAccessToken(cleaned, apiToken);
    }
}
