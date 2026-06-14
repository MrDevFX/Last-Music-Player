#include "pch.h"

#include <winrt/Windows.Foundation.h>

#include "Backend/ProviderHelpers.h"

#include <exception>
#include <iostream>
#include <string>

namespace provider = LastMusicPlayer::Backend;

namespace
{
    std::wstring ToWide(winrt::hstring const& value)
    {
        return std::wstring{ value.c_str() };
    }

    bool Contains(winrt::hstring const& value, wchar_t const* expected)
    {
        return ToWide(value).find(expected) != std::wstring::npos;
    }

    bool StartsWith(winrt::hstring const& value, wchar_t const* expected)
    {
        return ToWide(value).rfind(expected, 0) == 0;
    }

    void Expect(bool condition, char const* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void TestNormalizeBaseUrl()
    {
        Expect(provider::NormalizeProviderBaseUrl(L"") == L"http://127.0.0.1:4527", "empty base URL should use the local default");
        Expect(provider::NormalizeProviderBaseUrl(L"https://music.example.test///") == L"https://music.example.test", "base URL should trim trailing slashes");
        Expect(provider::NormalizeProviderBaseUrl(L"https://music.example.test\\") == L"https://music.example.test", "base URL should trim trailing backslashes");
    }

    void TestExistingStreamUrlIsStable()
    {
        auto existing = winrt::hstring{ L"https://provider.example.test/v1/stream/direct%3A1?url=https%3A%2F%2Faudio.example.test%2Fa.mp3&media_token=short" };
        auto stream = provider::BuildProviderStreamUrl(existing, L"", L"", L"", L"http://127.0.0.1:4527", L"");
        Expect(!Contains(stream, L"media_token="), "existing provider stream URLs should drop stale media tokens");
        Expect(Contains(stream, L"?url=https%3A%2F%2Faudio.example.test%2Fa.mp3"), "existing provider stream URLs should keep the source URL");

        auto legacy = winrt::hstring{ L"https://provider.example.test/v1/stream/direct%3A1?access_token=full-key&url=https%3A%2F%2Faudio.example.test%2Fa.mp3&media_token=short" };
        auto sanitized = provider::BuildProviderStreamUrl(legacy, L"", L"", L"", L"http://127.0.0.1:4527", L"");
        Expect(!Contains(sanitized, L"access_token="), "existing media-token stream URLs should drop legacy access tokens");
        Expect(!Contains(sanitized, L"media_token="), "existing media-token stream URLs should drop stale media tokens");
    }

    void TestProviderStreamUrl()
    {
        auto stream = provider::BuildProviderStreamUrl(
            L"",
            L"https://catalog.example.test/tracks/abc 123",
            L"internal-provider",
            L"",
            L"https://provider.example.test/",
            L"key value");

        Expect(StartsWith(stream, L"https://provider.example.test/v1/stream/remote%3A"), "remote tracks should use a generic stream id");
        Expect(Contains(stream, L"?url=https%3A%2F%2Fcatalog.example.test%2Ftracks%2Fabc%20123"), "source URL should be escaped into the stream query");
        Expect(Contains(stream, L"access_token=key%20value"), "provider stream URLs should include the escaped API token");
    }

    void TestReturnedStreamUrlWins()
    {
        auto stream = provider::BuildProviderStreamUrl(
            L"http://old-provider.example.test/v1/stream/service%3Aissued-id?url=https%3A%2F%2Fcatalog.example.test%2Ftracks%2Fabc&media_token=short",
            L"https://catalog.example.test/tracks/abc",
            L"service",
            L"",
            L"https://provider.example.test/",
            L"fresh key");

        Expect(StartsWith(stream, L"https://provider.example.test/v1/stream/service%3Aissued-id"), "returned stream ids should be preserved");
        Expect(Contains(stream, L"?url=https%3A%2F%2Fcatalog.example.test%2Ftracks%2Fabc"), "returned stream URLs should keep their source query");
        Expect(!Contains(stream, L"media_token="), "returned stream URLs should drop stale media tokens");
        Expect(Contains(stream, L"access_token=fresh%20key"), "returned stream URLs should get a fresh access token");
    }

    void TestPersistedImportedStreamUrl()
    {
        auto stream = provider::BuildProviderStreamUrl(
            L"http://old-provider.example.test/v1/stream/service%3Aissued-id?url=https%3A%2F%2Fcatalog.example.test%2Ftracks%2Fabc&media_token=short#lmp=12345",
            L"https://catalog.example.test/tracks/abc",
            L"service",
            L"",
            L"https://provider.example.test/",
            L"fresh key");

        Expect(StartsWith(stream, L"https://provider.example.test/v1/stream/service%3Aissued-id"), "persisted stream ids should be preserved");
        Expect(Contains(stream, L"?url=https%3A%2F%2Fcatalog.example.test%2Ftracks%2Fabc"), "persisted stream URLs should keep their source query");
        Expect(!Contains(stream, L"media_token="), "persisted stream URLs should drop stale media tokens");
        Expect(Contains(stream, L"access_token=fresh%20key"), "persisted stream URLs should send the fresh access token");
        Expect(!Contains(stream, L"#lmp="), "internal storage fragments should not reach playback");
    }

    void TestDirectStreamUrl()
    {
        auto stream = provider::BuildProviderStreamUrl(
            L"",
            L"https://cdn.example.test/audio/song.mp3",
            L"",
            L"",
            L"http://127.0.0.1:4527/",
            L"");

        Expect(StartsWith(stream, L"http://127.0.0.1:4527/v1/stream/direct%3A"), "empty provider with an HTTP source should use the direct stream provider");
        Expect(Contains(stream, L"?url=https%3A%2F%2Fcdn.example.test%2Faudio%2Fsong.mp3"), "direct source URL should be escaped into the stream query");
        Expect(!Contains(stream, L"access_token="), "direct stream URL should omit access token when no token is available");
    }

    void TestFallbackTokenAndUnsupportedSources()
    {
        auto fallback = provider::BuildProviderStreamUrl(
            L"https://provider.example.test/v1/track/1?access_token=legacy%20key",
            L"https://cdn.example.test/audio/song.mp3",
            L"direct",
            L"",
            L"http://127.0.0.1:4527",
            L"");

        Expect(!Contains(fallback, L"access_token="), "legacy access token from file path should not be reused in new fallback stream URLs");
        Expect(provider::BuildProviderStreamUrl(L"", L"custom-scheme://track/1", L"unsupported", L"", L"", L"").empty(), "unsupported sources should not build stream URLs");
        Expect(provider::BuildProviderStreamUrl(L"", L"file:///C:/Music/a.mp3", L"direct", L"", L"", L"").empty(), "non-HTTP sources should not build stream URLs");
    }
}

int wmain()
{
    try
    {
        winrt::init_apartment();
        TestNormalizeBaseUrl();
        TestExistingStreamUrlIsStable();
        TestProviderStreamUrl();
        TestReturnedStreamUrlWins();
        TestPersistedImportedStreamUrl();
        TestDirectStreamUrl();
        TestFallbackTokenAndUnsupportedSources();
        std::wcout << L"ProviderHelpersTests passed" << std::endl;
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "ProviderHelpersTests failed: " << error.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "ProviderHelpersTests failed with an unknown exception" << std::endl;
        return 1;
    }
}
