#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.Internal.h"

#include "Backend/ProviderClient.h"
#include "Backend/SettingsManager.h"
#include "Backend/TrayIcon.h"
#include "Backend/DiscordPresence.h"

#include <windows.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Text.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::Last_Music_Player::implementation
{
    using namespace detail;

    winrt::Windows::Foundation::IAsyncAction MainWindow::MusicListView_ItemClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args)
    {
        auto clickedTrack = args.ClickedItem().try_as<winrt::Last_Music_Player::TrackInfo>();
        if (!clickedTrack)
        {
            co_return;
        }

        MusicListView().SelectedItem(clickedTrack);
        co_await LoadBrowseQueueAndPlayAsync(clickedTrack);
        co_return;
    }

    // ---- Shared queue navigation (shuffle + repeat aware) -------------------

    // ---- Transport routing (Local MediaPlayer vs Chromecast) ---------------

    // ---- Full-screen Now Playing view --------------------------------------

    // ---- Chromecast --------------------------------------------------------

    void MainWindow::AppendTrackPage(
        std::vector<winrt::Last_Music_Player::TrackInfo> const& source,
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::Last_Music_Player::TrackInfo> target,
        uint32_t pageSize)
    {
        if (!target || pageSize == 0)
        {
            return;
        }

        auto start = target.Size();
        auto total = static_cast<uint32_t>((std::min)(source.size(), static_cast<size_t>((std::numeric_limits<uint32_t>::max)())));
        auto end = (std::min)(total, start + pageSize);
        for (uint32_t i = start; i < end; ++i)
        {
            auto copy = source[i];
            copy.Index(static_cast<int32_t>(i + 1));
            ResolveArtworkPresentation(copy, L"track");
            target.Append(copy);
        }
    }

    void MainWindow::AppendBrowsePage()
    {
        AppendTrackPage(
            m_browseAllResults,
            m_browseTracks,
            m_browseGridMode ? kBrowseGridPageSize : kBrowseListPageSize);
        UpdateBrowseStats();
    }

    void MainWindow::MaybeAppendBrowsePage(uint32_t itemIndex)
    {
        auto total = m_browseMatchedCount > 0 ? static_cast<uint32_t>(m_browseMatchedCount) : static_cast<uint32_t>(m_browseAllResults.size());
        if (m_browseTracks.Size() < total &&
            itemIndex + kPageAppendThreshold >= m_browseTracks.Size())
        {
            if (DatabaseService().IsInitialized())
            {
                RunDetached(AppendBrowsePageAsync());
            }
            else
            {
                AppendBrowsePage();
            }
        }
    }

    LastMusicPlayer::Backend::TrackQuery MainWindow::CurrentBrowseQuery(uint32_t offset, uint32_t limit) const
    {
        LastMusicPlayer::Backend::TrackQuery query;
        query.Filter = m_browseFilter;
        query.Sort = m_browseSort;
        query.Offset = static_cast<int>(offset);
        query.Limit = static_cast<int>(limit);
        query.IncludeRemote = true;
        query.ActiveOnly = true;
        return query;
    }

    void MainWindow::ApplyBrowseFilterSort()
    {
        BrowseSortButton().IsEnabled(m_browseFilter != L"History" && m_browseFilter != L"Most");
        m_browseAllResults.clear();
        m_browseTracks.Clear();
        m_browseMatchedCount = 0;
        m_browseMatchedSeconds = 0.0;
        m_browsePageLoading = false;
        ++m_browsePageLoadId;
        m_browseResultsValid = false;
        m_browseLoadState = LoadState::Loading;
        BrowseSubtitle().Text(L"Loading tracks...");
        UpdateBrowseStats();
        RunDetached(EnsureBrowseHydratedAsync(true));
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::EnsureBrowseHydratedAsync(bool reset)
    {
        auto lifetime = get_strong();
        auto dispatcher = this->DispatcherQueue();
        auto epoch = reset ? ++m_browseHydrationEpoch : m_browseHydrationEpoch;

        if (reset)
        {
        }

        if (!DatabaseService().IsInitialized())
        {
            m_browseAllResults.clear();
            m_browseTracks.Clear();
            m_browseMatchedCount = static_cast<int>(m_queue.CurrentPlaylist.size());
            m_browseMatchedSeconds = 0.0;
            for (auto const& track : m_queue.CurrentPlaylist)
            {
                m_browseMatchedSeconds += track.DurationSeconds();
                auto copy = track;
                copy.Index(static_cast<int32_t>(m_browseAllResults.size() + 1));
                m_browseAllResults.push_back(copy);
            }
            AppendBrowsePage();
            m_browseLoadState = LoadState::Loaded;
            m_browseResultsValid = true;
            UpdateBrowseStats();
            co_return;
        }

        co_await AppendBrowsePageAsync();
        if (epoch == m_browseHydrationEpoch)
        {
            m_browseLoadState = LoadState::Loaded;
            m_browseResultsValid = true;
        }
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::AppendBrowsePageAsync()
    {
        auto lifetime = get_strong();
        if (m_browsePageLoading)
        {
            co_return;
        }
        if (m_browseMatchedCount > 0 && static_cast<int>(m_browseAllResults.size()) >= m_browseMatchedCount)
        {
            co_return;
        }

        auto dispatcher = this->DispatcherQueue();
        auto epoch = m_browseHydrationEpoch;
        auto offset = static_cast<uint32_t>(m_browseAllResults.size());
        auto limit = m_browseGridMode ? kBrowseGridPageSize : kBrowseListPageSize;
        auto query = CurrentBrowseQuery(offset, limit);
        m_browsePageLoading = true;
        auto pageLoadId = ++m_browsePageLoadId;

        co_await winrt::resume_background();
        auto page = DatabaseService().LoadTrackPage(query);

        co_await wil::resume_foreground(dispatcher);
        if (epoch != m_browseHydrationEpoch || pageLoadId != m_browsePageLoadId)
        {
            if (pageLoadId == m_browsePageLoadId)
            {
                m_browsePageLoading = false;
            }
            co_return;
        }

        m_browseMatchedCount = page.TotalCount;
        m_browseMatchedSeconds = page.TotalSeconds;
        for (auto const& source : page.Tracks)
        {
            auto copy = source;
            copy.Index(static_cast<int32_t>(m_browseAllResults.size() + 1));
            ResolveArtworkPresentation(copy, L"track");
            m_browseAllResults.push_back(copy);
            m_browseTracks.Append(copy);
        }
        if (pageLoadId == m_browsePageLoadId)
        {
            m_browsePageLoading = false;
        }
        UpdateBrowseStats();
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::LoadBrowseQueueAndPlayAsync(winrt::Last_Music_Player::TrackInfo clickedTrack)
    {
        auto lifetime = get_strong();
        auto dispatcher = this->DispatcherQueue();

        std::vector<winrt::Last_Music_Player::TrackInfo> queue;
        if (DatabaseService().IsInitialized())
        {
            auto query = CurrentBrowseQuery(0, 0);
            co_await winrt::resume_background();
            queue = DatabaseService().LoadTracksForQuery(query);
            co_await wil::resume_foreground(dispatcher);
        }
        else
        {
            queue = m_browseAllResults;
        }

        if (queue.empty())
        {
            co_return;
        }
        if (!clickedTrack)
        {
            clickedTrack = queue.front();
        }
        QueueAndPlayVisible(queue, clickedTrack);
    }

    void MainWindow::SetBrowseGridMode(bool gridMode)
    {
        m_browseGridMode = gridMode;
        EnsureAccentBrushes();
        using V = winrt::Microsoft::UI::Xaml::Visibility;
        MusicListView().Visibility(gridMode ? V::Collapsed : V::Visible);
        BrowseListHeader().Visibility(gridMode ? V::Collapsed : V::Visible);
        BrowseGridView().Visibility(gridMode ? V::Visible : V::Collapsed);

        BrowseListViewButton().Background(gridMode ? m_brushTransparent : m_brushAccentSoft);
        BrowseGridViewButton().Background(gridMode ? m_brushAccentSoft : m_brushTransparent);

        auto total = m_browseMatchedCount > 0 ? static_cast<size_t>(m_browseMatchedCount) : m_browseAllResults.size();
        if (!gridMode && m_browseTracks.Size() < (std::min)(static_cast<size_t>(kBrowseListPageSize), total))
        {
            if (DatabaseService().IsInitialized())
            {
                RunDetached(AppendBrowsePageAsync());
            }
            else
            {
                AppendBrowsePage();
            }
        }
    }

    void MainWindow::UpdateBrowseScopeLabel()
    {
        if (!m_xamlReadyForEvents)
        {
            return;
        }

        bool hasRemote = !ReadAppSettingString(L"ProviderBaseUrl").empty();
        if (!hasRemote)
        {
            for (auto const& track : m_catalogTracks)
            {
                if (ToLowerCopy(track.SourceKind()) == L"remote")
                {
                    hasRemote = true;
                    break;
                }
            }
        }

        BrowseScopeLabel().Text(hasRemote ? L"Local and Remote Library" : L"Local Library");
    }

    void MainWindow::UpdateBrowseStats()
    {
        UpdateBrowseScopeLabel();
        size_t count = DatabaseService().IsInitialized()
            ? static_cast<size_t>(m_libraryStats.SongCount)
            : m_queue.CurrentPlaylist.size();
        size_t loadedCount = m_browseTracks.Size();
        size_t matchedCount = m_browseMatchedCount > 0 ? static_cast<size_t>(m_browseMatchedCount) : m_browseAllResults.size();
        double totalSeconds = m_browseMatchedSeconds;
        if (totalSeconds <= 0.0 && !DatabaseService().IsInitialized())
        {
            for (auto const& t : m_queue.CurrentPlaylist)
            {
                totalSeconds += t.DurationSeconds();
            }
        }
        long long mins = static_cast<long long>(totalSeconds / 60.0);
        std::wstring s;
        if (count == 0)
        {
            s = L"No tracks scanned yet";
        }
        else
        {
            s = std::to_wstring(loadedCount) + L" loaded / "
                + std::to_wstring(matchedCount) + L" shown / "
                + std::to_wstring(count) + L" tracks \x00B7 "
                + std::to_wstring(mins) + L" min total";
        }
        BrowseSubtitle().Text(winrt::hstring(s));
    }

    void MainWindow::BrowsePlayAll_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)sender;
        (void)args;
        if (m_browseAllResults.empty() && m_browseMatchedCount == 0)
        {
            return;
        }

        RunDetached(LoadBrowseQueueAndPlayAsync(winrt::Last_Music_Player::TrackInfo{ nullptr }));
    }

    void MainWindow::SelectBrowseFilter(std::wstring const& filter)
    {
        auto nextFilter = filter.empty() ? std::wstring{ L"All" } : filter;
        if (m_browseFilter == nextFilter && m_browseResultsValid)
        {
            return;
        }

        m_browseFilter = nextFilter;
        m_updatingBrowseChips = true;

        winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton chips[] = {
            ChipAll(), ChipHistory(), ChipMost(), ChipFav()
        };

        for (auto const& chip : chips)
        {
            auto tag = ReadTagString(chip.Tag());
            auto selected = std::wstring(tag.c_str()) == m_browseFilter;
            chip.IsChecked(selected);
        }

        m_updatingBrowseChips = false;
        ApplyBrowseFilterSort();
    }

    void MainWindow::BrowseChip_Checked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)args;
        if (!m_xamlReadyForEvents)
        {
            return;
        }

        if (m_updatingBrowseChips)
        {
            return;
        }

        auto clicked = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton>();
        if (!clicked)
        {
            return;
        }

        auto tag = ReadTagString(clicked.Tag());
        SelectBrowseFilter(tag.empty() ? L"All" : std::wstring(tag.c_str()));
    }

    void MainWindow::BrowseChip_Unchecked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)sender;
        (void)args;
        if (!m_xamlReadyForEvents || m_updatingBrowseChips)
        {
            return;
        }

        winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton chips[] = {
            ChipAll(), ChipHistory(), ChipMost(), ChipFav()
        };
        for (auto const& chip : chips)
        {
            if (chip)
            {
                auto checked = chip.IsChecked();
                if (checked && checked.Value())
                {
                    return;
                }
            }
        }

        SelectBrowseFilter(m_browseFilter);
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::BrowseGridView_ItemClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args)
    {
        (void)sender;
        auto clickedTrack = args.ClickedItem().try_as<winrt::Last_Music_Player::TrackInfo>();
        if (clickedTrack)
        {
            RunDetached(LoadBrowseQueueAndPlayAsync(clickedTrack));
        }
        co_return;
    }

    void MainWindow::BrowseList_ContainerContentChanging(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ContainerContentChangingEventArgs const& args)
    {
        (void)sender;
        if (!args.InRecycleQueue())
        {
            MaybeAppendBrowsePage(args.ItemIndex());
        }
    }

    void MainWindow::BrowseGrid_ContainerContentChanging(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ContainerContentChangingEventArgs const& args)
    {
        (void)sender;
        if (!args.InRecycleQueue())
        {
            MaybeAppendBrowsePage(args.ItemIndex());
        }
    }

    void MainWindow::PlayBrowseTrack(winrt::Last_Music_Player::TrackInfo const& track)
    {
        if (!track)
        {
            return;
        }

        MusicListView().SelectedItem(track);
        RunDetached(LoadBrowseQueueAndPlayAsync(track));
    }

    void MainWindow::PlayNextFromBrowse(winrt::Last_Music_Player::TrackInfo const& track)
    {
        if (!track || !IsPlayableHomeTrack(track))
        {
            return;
        }

        auto key = CatalogSourceKey(track);
        if (key.empty())
        {
            return;
        }

        // Nothing playing yet → "Play next" is functionally "Play now".
        auto current = AudioPlayerService().GetCurrentTrack();
        if (!current)
        {
            PlayBrowseTrack(track);
            return;
        }

        // Skip if it's already queued by the user (avoid stacking dupes from
        // accidental double right-clicks).
        for (auto const& queued : m_queue.UserQueue)
        {
            if (CatalogSourceKey(queued) == key)
            {
                RebuildUpNextQueue();
                return;
            }
        }

        // Push to the front of UserQueue so this track plays as soon as the
        // current one ends. UserQueue is the user's explicit Up Next; it
        // survives tile clicks elsewhere in the app (context replaces,
        // user queue persists).
        m_queue.UserQueue.insert(m_queue.UserQueue.begin(), track);
        RebuildUpNextQueue();
    }

    void MainWindow::AddBrowseTrackToQueue(winrt::Last_Music_Player::TrackInfo const& track)
    {
        if (!track || !IsPlayableHomeTrack(track))
        {
            return;
        }

        auto key = CatalogSourceKey(track);
        if (key.empty())
        {
            return;
        }

        // Skip if it's already queued by the user.
        for (auto const& queued : m_queue.UserQueue)
        {
            if (CatalogSourceKey(queued) == key)
            {
                RebuildUpNextQueue();
                return;
            }
        }

        // Append to the end of UserQueue. Plays after any earlier explicit
        // queue items and after the current track / context items finish.
        m_queue.UserQueue.push_back(track);
        RebuildUpNextQueue();
    }

    void MainWindow::PlayNextFromBrowseBulk(std::vector<winrt::Last_Music_Player::TrackInfo> const& tracks)
    {
        if (tracks.empty())
        {
            return;
        }

        std::unordered_set<std::wstring> existing;
        existing.reserve(m_queue.UserQueue.size() + tracks.size());
        for (auto const& queued : m_queue.UserQueue)
        {
            auto k = CatalogSourceKey(queued);
            if (!k.empty()) existing.insert(std::move(k));
        }

        // Build the prefix in input order, dedup'd against UserQueue and
        // itself, then insert as a single block so the original ordering
        // survives. Per-track PlayNextFromBrowse would reverse the list.
        std::vector<winrt::Last_Music_Player::TrackInfo> toInsert;
        toInsert.reserve(tracks.size());
        for (auto const& track : tracks)
        {
            if (!track || !IsPlayableHomeTrack(track)) continue;
            auto key = CatalogSourceKey(track);
            if (key.empty()) continue;
            if (existing.find(key) != existing.end()) continue;
            existing.insert(key);
            toInsert.push_back(track);
        }

        if (toInsert.empty())
        {
            RebuildUpNextQueue();
            return;
        }

        m_queue.UserQueue.insert(m_queue.UserQueue.begin(), toInsert.begin(), toInsert.end());
        RebuildUpNextQueue();
    }

    void MainWindow::AddBrowseTracksToQueueBulk(std::vector<winrt::Last_Music_Player::TrackInfo> const& tracks)
    {
        if (tracks.empty())
        {
            return;
        }

        std::unordered_set<std::wstring> existing;
        existing.reserve(m_queue.UserQueue.size() + tracks.size());
        for (auto const& queued : m_queue.UserQueue)
        {
            auto k = CatalogSourceKey(queued);
            if (!k.empty()) existing.insert(std::move(k));
        }

        bool anyAppended = false;
        for (auto const& track : tracks)
        {
            if (!track || !IsPlayableHomeTrack(track)) continue;
            auto key = CatalogSourceKey(track);
            if (key.empty()) continue;
            if (existing.find(key) != existing.end()) continue;
            existing.insert(key);
            m_queue.UserQueue.push_back(track);
            anyAppended = true;
        }

        if (anyAppended)
        {
            RebuildUpNextQueue();
        }
    }

    void MainWindow::BrowseRowPlay_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)args;
        PlayBrowseTrack(TrackFromActionSender(sender));
    }

    void MainWindow::BrowseRowMenuPlayNow_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)args;
        PlayBrowseTrack(TrackFromActionSender(sender));
    }

    void MainWindow::BrowseRowMenuPlayNext_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)args;
        PlayNextFromBrowse(TrackFromActionSender(sender));
    }

    void MainWindow::BrowseRowMenuAddToQueue_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)args;
        AddBrowseTrackToQueue(TrackFromActionSender(sender));
    }

    void MainWindow::BrowseRowMenuLike_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)args;
        ToggleTrackLiked(TrackFromActionSender(sender));
    }

    void MainWindow::BrowseRowMenuArtist_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)args;
        OpenBrowseTrackArtist(TrackFromActionSender(sender));
    }

    void MainWindow::BrowseRowMenuAlbum_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)args;
        OpenBrowseTrackAlbum(TrackFromActionSender(sender));
    }

    void MainWindow::BrowseListView_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)sender;
        (void)args;
        SetBrowseGridMode(false);
    }

    void MainWindow::BrowseGridView_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)sender;
        (void)args;
        SetBrowseGridMode(true);
    }

    void MainWindow::BrowseSort_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)args;
        auto item = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem>();
        if (!item)
        {
            return;
        }

        auto tag = ReadTagString(item.Tag());
        if (!tag.empty())
        {
            m_browseSort = std::wstring(tag.c_str());
            BrowseSortLabel().Text(item.Text());
            ApplyBrowseFilterSort();
        }
    }

    void MainWindow::Row_PointerEntered(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
    {
        (void)e;
        if (auto fe = sender.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
        {
            if (auto n = fe.FindName(L"RowNum").try_as<winrt::Microsoft::UI::Xaml::UIElement>())
            {
                n.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            }
            if (auto p = fe.FindName(L"RowPlay").try_as<winrt::Microsoft::UI::Xaml::UIElement>())
            {
                p.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            }
        }
    }

    void MainWindow::Row_PointerExited(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
    {
        (void)e;
        if (auto fe = sender.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
        {
            if (auto n = fe.FindName(L"RowNum").try_as<winrt::Microsoft::UI::Xaml::UIElement>())
            {
                n.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            }
            if (auto p = fe.FindName(L"RowPlay").try_as<winrt::Microsoft::UI::Xaml::UIElement>())
            {
                p.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            }
        }
    }

}
