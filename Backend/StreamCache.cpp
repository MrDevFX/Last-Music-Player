#include "pch.h"
#include "Backend/StreamCache.h"

#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <system_error>
#include <vector>

namespace LastMusicPlayer::Backend
{
    namespace
    {
        // Map the upstream Content-Type to a container extension the
        // MediaPlayer recognises. A wrong/unknown type falls back to .bin —
        // the player still sniffs the byte signature for the common formats,
        // but a correct extension avoids ambiguity.
        std::wstring ExtensionForContentType(winrt::hstring const& mediaType)
        {
            std::wstring t{ mediaType.c_str() };
            std::transform(t.begin(), t.end(), t.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (t.find(L"mpeg") != std::wstring::npos || t.find(L"mp3") != std::wstring::npos) return L".mp3";
            if (t.find(L"mp4") != std::wstring::npos || t.find(L"m4a") != std::wstring::npos || t.find(L"aac") != std::wstring::npos) return L".m4a";
            if (t.find(L"webm") != std::wstring::npos) return L".webm";
            if (t.find(L"ogg") != std::wstring::npos || t.find(L"opus") != std::wstring::npos) return L".ogg";
            if (t.find(L"wav") != std::wstring::npos) return L".wav";
            if (t.find(L"flac") != std::wstring::npos) return L".flac";
            return L".bin";
        }
    }

    std::filesystem::path StreamCache::CacheDir()
    {
        std::filesystem::path base;
        wchar_t* localAppData{};
        size_t length{};
        if (_wdupenv_s(&localAppData, &length, L"LOCALAPPDATA") == 0 && localAppData && *localAppData)
        {
            base = std::filesystem::path{ localAppData } / L"Last Music Player";
        }
        else
        {
            base = std::filesystem::current_path() / L"Last Music Player";
        }
        std::free(localAppData);

        auto dir = base / L"stream-cache";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    // Same FNV-1a-over-UTF16 as Backend::StableFnv1a64 (ProviderHelpers) so the
    // filename is stable and collision-resistant across sessions.
    std::wstring StreamCache::HashKey(std::wstring const& sourceKey)
    {
        uint64_t hash = 1469598103934665603ull;
        for (wchar_t ch : sourceKey)
        {
            auto code = static_cast<uint32_t>(ch);
            for (int shift = 0; shift < 16; shift += 8)
            {
                hash ^= static_cast<uint8_t>((code >> shift) & 0xFF);
                hash *= 1099511628211ull;
            }
        }
        wchar_t buf[17]{};
        swprintf_s(buf, L"%016llx", static_cast<unsigned long long>(hash));
        return buf;
    }

    std::wstring StreamCache::FindOnDisk(std::wstring const& hash)
    {
        std::error_code ec;
        auto dir = CacheDir();
        for (auto const& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (ec) break;
            std::error_code fe;
            if (!entry.is_regular_file(fe)) continue;
            if (entry.path().stem().wstring() == hash && entry.path().extension() != L".part")
            {
                return entry.path().wstring();
            }
        }
        return {};
    }

    std::wstring StreamCache::ReadyPath(std::wstring const& sourceKey)
    {
        if (sourceKey.empty()) return {};
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_entries.find(sourceKey);
            if (it != m_entries.end())
            {
                if (it->second.status == Status::Ready) return it->second.path;
                if (it->second.status == Status::InFlight) return {};  // still downloading
                // Failed -> fall through and re-check disk (a prior session may
                // have a file even though this session's attempt failed).
            }
        }
        auto onDisk = FindOnDisk(HashKey(sourceKey));
        if (!onDisk.empty())
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entries[sourceKey] = Entry{ Status::Ready, onDisk };
            return onDisk;
        }
        return {};
    }

    void StreamCache::Prefetch(std::wstring const& sourceKey, std::wstring const& streamUrl)
    {
        if (sourceKey.empty() || streamUrl.empty()) return;

        // A previously-downloaded file (this or a prior session) means nothing
        // to do — adopt it and bail before reserving an in-flight slot.
        auto existing = FindOnDisk(HashKey(sourceKey));
        if (!existing.empty())
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entries[sourceKey] = Entry{ Status::Ready, existing };
            return;
        }

        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_entries.find(sourceKey);
            if (it != m_entries.end() && (it->second.status == Status::Ready || it->second.status == Status::InFlight))
            {
                return;  // already cached or downloading (this exact key)
            }
            // Concurrency cap counts only downloads that are still fresh, so a
            // hung connection ages out and can't permanently block new ones.
            auto now = ::GetTickCount64();
            int active = 0;
            for (auto const& kv : m_entries)
            {
                if (kv.second.status == Status::InFlight && now - kv.second.startTick < kMaxInFlightAgeMs)
                {
                    ++active;
                }
            }
            if (active >= kMaxInFlight) return;  // back off; a later Up Next rebuild retries
            m_entries[sourceKey] = Entry{ Status::InFlight, {}, now };
            generation = m_generation;
        }

        DownloadAsync(sourceKey, streamUrl, generation);
    }

    winrt::Windows::Foundation::IAsyncAction StreamCache::DownloadAsync(std::wstring sourceKey, std::wstring streamUrl, uint64_t generation)
    {
        co_await winrt::resume_background();

        auto hash = HashKey(sourceKey);
        auto dir = CacheDir();
        auto partPath = (dir / (hash + L".part")).wstring();
        bool ok = false;
        std::wstring finalPath;

        try
        {
            namespace WWH = winrt::Windows::Web::Http;
            WWH::HttpClient client;
            auto uri = winrt::Windows::Foundation::Uri{ winrt::hstring{ streamUrl } };
            auto response = co_await client.GetAsync(uri, WWH::HttpCompletionOption::ResponseHeadersRead);
            if (response.IsSuccessStatusCode())
            {
                std::wstring ext = L".bin";
                try
                {
                    auto ct = response.Content().Headers().ContentType();
                    if (ct) ext = ExtensionForContentType(ct.MediaType());
                }
                catch (...) {}

                auto inStream = co_await response.Content().ReadAsInputStreamAsync();
                auto folder = co_await winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(winrt::hstring{ dir.wstring() });
                auto file = co_await folder.CreateFileAsync(
                    winrt::hstring{ hash + L".part" },
                    winrt::Windows::Storage::CreationCollisionOption::ReplaceExisting);

                auto outStream = co_await file.OpenAsync(winrt::Windows::Storage::FileAccessMode::ReadWrite);
                co_await winrt::Windows::Storage::Streams::RandomAccessStream::CopyAsync(inStream, outStream.GetOutputStreamAt(0));
                co_await outStream.FlushAsync();
                outStream.Close();
                inStream.Close();

                finalPath = (dir / (hash + ext)).wstring();
                std::error_code ec;
                std::filesystem::rename(partPath, finalPath, ec);
                if (ec)
                {
                    // A stale final file can block the rename; replace it.
                    std::filesystem::remove(finalPath, ec);
                    std::filesystem::rename(partPath, finalPath, ec);
                }
                ok = !ec;
            }
        }
        catch (...)
        {
            ok = false;
        }

        if (!ok)
        {
            std::error_code ec;
            std::filesystem::remove(partPath, ec);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (generation != m_generation)
            {
                std::error_code ec;
                std::filesystem::remove(partPath, ec);
                if (!finalPath.empty())
                {
                    std::filesystem::remove(finalPath, ec);
                }
                co_return;
            }
            m_entries[sourceKey] = ok ? Entry{ Status::Ready, finalPath } : Entry{ Status::Failed, {} };
        }

        if (ok)
        {
            TrimToCap();
        }
    }

    void StreamCache::TrimToCap()
    {
        std::error_code ec;
        auto dir = CacheDir();

        struct FileInfo { std::filesystem::path path; unsigned long long size; std::filesystem::file_time_type writeTime; };
        std::vector<FileInfo> files;
        unsigned long long total = 0;
        for (auto const& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (ec) break;
            std::error_code fe;
            if (!entry.is_regular_file(fe)) continue;
            if (entry.path().extension() == L".part") continue;
            auto sz = static_cast<unsigned long long>(entry.file_size(fe));
            if (fe) continue;
            auto wt = entry.last_write_time(fe);
            if (fe) continue;
            files.push_back({ entry.path(), sz, wt });
            total += sz;
        }

        if (files.size() <= kMaxFiles && total <= kMaxBytes) return;

        // Evict oldest-first until back under both the file-count and byte caps.
        std::sort(files.begin(), files.end(),
            [](FileInfo const& a, FileInfo const& b) { return a.writeTime < b.writeTime; });

        size_t count = files.size();
        for (auto const& f : files)
        {
            if (count <= kMaxFiles && total <= kMaxBytes) break;
            std::error_code re;
            std::filesystem::remove(f.path, re);
            if (re) continue;
            --count;
            total -= (f.size <= total ? f.size : total);

            // Forget any in-memory entry that pointed at this file so a later
            // ReadyPath misses and the track gets re-prefetched if needed.
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_entries.begin(); it != m_entries.end(); )
            {
                if (it->second.path == f.path.wstring()) it = m_entries.erase(it);
                else ++it;
            }
        }
    }

    void StreamCache::PruneOnStartup()
    {
        std::error_code ec;
        auto dir = CacheDir();
        for (auto const& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (ec) break;
            std::error_code fe;
            if (entry.is_regular_file(fe) && entry.path().extension() == L".part")
            {
                std::error_code re;
                std::filesystem::remove(entry.path(), re);
            }
        }
        TrimToCap();
    }

    void StreamCache::Clear()
    {
        std::filesystem::path dir;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_generation;
            m_entries.clear();
            dir = CacheDir();
        }

        std::error_code ec;
        for (auto const& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (ec)
            {
                break;
            }
            std::error_code removeError;
            std::filesystem::remove_all(entry.path(), removeError);
        }
    }
}
