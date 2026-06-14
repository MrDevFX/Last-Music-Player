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

    winrt::Last_Music_Player::TrackInfo MainWindow::TrackFromActionSender(winrt::Windows::Foundation::IInspectable const& sender)
    {
        if (auto item = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem>())
        {
            if (auto track = item.CommandParameter().try_as<winrt::Last_Music_Player::TrackInfo>())
            {
                return track;
            }
            if (auto track = item.Tag().try_as<winrt::Last_Music_Player::TrackInfo>())
            {
                return track;
            }
        }

        if (auto element = sender.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
        {
            if (auto track = element.Tag().try_as<winrt::Last_Music_Player::TrackInfo>())
            {
                return track;
            }
            if (auto track = element.DataContext().try_as<winrt::Last_Music_Player::TrackInfo>())
            {
                return track;
            }
        }

        return nullptr;
    }


    int64_t MainWindow::PersistTrackForPlaylist(winrt::Last_Music_Player::TrackInfo const& track)
    {
        if (!track || !DatabaseService().IsInitialized())
        {
            return 0;
        }

        if (track.CatalogId() > 0)
        {
            return track.CatalogId();
        }

        auto key = CatalogSourceKey(track);
        if (key.empty())
        {
            return 0;
        }

        auto remote = ToLowerCopy(track.SourceKind()) == L"remote" || (!track.File() && IsHttpUrl(track.FilePath()));
        return remote
            ? DatabaseService().UpsertRemoteTrack(track, key)
            : DatabaseService().UpsertLocalTrack(track, key);
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::AddTrackToPlaylistAsync(winrt::Last_Music_Player::TrackInfo track)
    {
        if (!track || !IsPlayableHomeTrack(track) || !DatabaseService().IsInitialized())
        {
            co_return;
        }

        if (m_manualPlaylists.Size() == 0)
        {
            co_await HydrateLibraryTabAsync(L"Playlists", false);
        }

        winrt::Microsoft::UI::Xaml::Controls::StackPanel content;
        content.Spacing(12);

        winrt::Microsoft::UI::Xaml::Controls::ComboBox playlistPicker;
        playlistPicker.Header(winrt::box_value(winrt::hstring(L"Playlist")));
        playlistPicker.PlaceholderText(L"Choose a playlist");
        for (uint32_t i = 0; i < m_manualPlaylists.Size(); ++i)
        {
            auto playlist = m_manualPlaylists.GetAt(i);
            winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem item;
            item.Content(winrt::box_value(playlist.Title()));
            item.Tag(winrt::box_value(playlist.SourceUrl()));
            playlistPicker.Items().Append(item);
        }
        if (m_manualPlaylists.Size() > 0)
        {
            playlistPicker.SelectedIndex(0);
        }
        content.Children().Append(playlistPicker);

        winrt::Microsoft::UI::Xaml::Controls::TextBox newNameBox;
        newNameBox.Header(winrt::box_value(winrt::hstring(L"Or create new")));
        newNameBox.PlaceholderText(L"New playlist name");
        content.Children().Append(newNameBox);

        winrt::Microsoft::UI::Xaml::Controls::ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring(L"Add to playlist")));
        dlg.Content(content);
        dlg.PrimaryButtonText(L"Add");
        dlg.CloseButtonText(L"Cancel");
        dlg.DefaultButton(winrt::Microsoft::UI::Xaml::Controls::ContentDialogButton::Primary);
        dlg.XamlRoot(this->Content().XamlRoot());

        auto result = co_await dlg.ShowAsync();
        if (result != winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary)
        {
            co_return;
        }

        std::wstring playlistKey;
        auto newName = TrimQuery(newNameBox.Text());
        if (!newName.empty())
        {
            auto playlistId = DatabaseService().CreatePlaylist(std::wstring(newName.c_str()));
            co_await HydrateLibraryTabAsync(L"Playlists", true);
            for (uint32_t i = 0; i < m_manualPlaylists.Size(); ++i)
            {
                auto playlist = m_manualPlaylists.GetAt(i);
                if (playlist.CatalogId() == playlistId)
                {
                    playlistKey = std::wstring(playlist.SourceUrl().c_str());
                    break;
                }
            }
        }
        else if (auto selected = playlistPicker.SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem>())
        {
            playlistKey = std::wstring(winrt::unbox_value_or<winrt::hstring>(selected.Tag(), L"").c_str());
        }

        if (playlistKey.empty())
        {
            auto playlistId = DatabaseService().CreatePlaylist(L"New Playlist");
            co_await HydrateLibraryTabAsync(L"Playlists", true);
            for (uint32_t i = 0; i < m_manualPlaylists.Size(); ++i)
            {
                auto playlist = m_manualPlaylists.GetAt(i);
                if (playlist.CatalogId() == playlistId)
                {
                    playlistKey = std::wstring(playlist.SourceUrl().c_str());
                    break;
                }
            }
        }

        auto trackId = PersistTrackForPlaylist(track);
        if (playlistKey.empty() || trackId <= 0 || !DatabaseService().AddTrackToPlaylist(playlistKey, trackId))
        {
            LibraryImportStatusText().Text(L"Could not add to playlist");
            co_return;
        }

        auto detailTitle = LibraryDetailTitleText() ? LibraryDetailTitleText().Text() : winrt::hstring{};
        auto detailSubtitle = m_libraryDetailSubtitle;
        auto inSamePlaylist = m_libraryDetailKind == L"playlist" && m_libraryDetailKey == playlistKey;
        MarkLibraryViewsDirty();
        co_await HydrateLibraryTabAsync(L"Playlists", true);
        if (inSamePlaylist)
        {
            ShowLibraryDetail(L"playlist", winrt::hstring(playlistKey), detailTitle.empty() ? winrt::hstring(L"Playlist") : detailTitle, winrt::hstring(detailSubtitle));
        }
        LibraryImportStatusText().Text(L"Added to playlist");
    }

    void MainWindow::ToggleTrackLiked(winrt::Last_Music_Player::TrackInfo const& track)
    {
        if (!track)
        {
            return;
        }

        auto key = CatalogSourceKey(track);
        if (key.empty())
        {
            return;
        }

        bool liked = !track.IsLiked();
        if (DatabaseService().IsInitialized())
        {
            auto remote = ToLowerCopy(track.SourceKind()) == L"remote" || (!track.File() && IsHttpUrl(track.FilePath()));
            if (remote)
            {
                DatabaseService().UpsertRemoteTrack(track, key);
            }
            else
            {
                DatabaseService().UpsertLocalTrack(track, key);
            }

            liked = !DatabaseService().IsLiked(key);
            DatabaseService().SetLiked(key, liked);
        }

        auto patchLike = [&](winrt::Last_Music_Player::TrackInfo const& candidate)
        {
            if (candidate && CatalogSourceKey(candidate) == key)
            {
                candidate.IsLiked(liked);
            }
        };

        patchLike(track);
        if (auto current = AudioPlayerService().GetCurrentTrack())
        {
            patchLike(current);
            if (CatalogSourceKey(current) == key)
            {
                UpdateLikeButton(current);
            }
        }

        auto patchObservable = [&](winrt::Windows::Foundation::Collections::IObservableVector<winrt::Last_Music_Player::TrackInfo> const& items)
        {
            for (uint32_t i = 0; i < items.Size(); ++i)
            {
                patchLike(items.GetAt(i));
            }
        };

        for (auto& item : m_queue.CurrentPlaylist) { patchLike(item); }
        for (auto& item : m_queue.Queue) { patchLike(item); }
        for (auto& item : m_homeRecentHistory) { patchLike(item); }
        for (auto& item : m_catalogTracks) { patchLike(item); }
        for (auto& item : m_browseAllResults) { patchLike(item); }
        for (auto& item : m_librarySongAllResults) { patchLike(item); }
        for (auto& item : m_libraryDetailAllResults) { patchLike(item); }
        for (auto& mix : m_homeMixes)
        {
            for (auto& item : mix.second)
            {
                patchLike(item);
            }
        }
        patchObservable(m_browseTracks);
        patchObservable(m_homeTracks);
        patchObservable(m_recentlyAddedTracks);
        patchObservable(m_searchTracks);
        patchObservable(m_librarySongs);
        patchObservable(m_libraryDetailTracks);
        patchObservable(m_upNextQueue);

        auto detailKind = m_libraryDetailKind;
        auto detailKey = m_libraryDetailKey;
        auto detailTitle = LibraryDetailTitleText() ? LibraryDetailTitleText().Text() : winrt::hstring{};
        auto detailSubtitle = m_libraryDetailSubtitle;

        MarkLibraryViewsDirty();
        RunDetached(HydrateHomeAsync(false));

        if (!detailKind.empty() && !detailKey.empty())
        {
            ShowLibraryDetail(winrt::hstring(detailKind), winrt::hstring(detailKey), detailTitle.empty() ? winrt::hstring(detailKey) : detailTitle, winrt::hstring(detailSubtitle));
        }

        // The Explore "Favourites" tab is a liked-filtered DB query, not a
        // plain projection, so patching IsLiked above does not add/remove
        // rows. Rebuild it when it is the active filter so a like/unlike
        // from anywhere (e.g. the bottom bar) shows up immediately.
        if (m_browseFilter == L"Fav")
        {
            ApplyBrowseFilterSort();
        }
    }

    void MainWindow::OpenBrowseTrackArtist(winrt::Last_Music_Player::TrackInfo const& track)
    {
        if (!track)
        {
            return;
        }

        auto artist = track.Artist().empty() ? winrt::hstring{ L"Unknown Artist" } : track.Artist();
        HomeViewContainer().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        SettingsViewContainer().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        BrowseViewContainer().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        LibraryViewContainer().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        ExitSearchMode();
        UpdateNavSelection(L"Library");
        ShowLibraryDetail(L"artist", artist, artist, L"");
    }

    void MainWindow::OpenBrowseTrackAlbum(winrt::Last_Music_Player::TrackInfo const& track)
    {
        if (!track)
        {
            return;
        }

        auto remote = ToLowerCopy(track.SourceKind()) == L"remote" || (!track.File() && IsHttpUrl(track.FilePath()));
        auto album = track.Album().empty()
            ? (remote ? winrt::hstring{ L"Remote Singles" } : winrt::hstring{ L"Unknown Album" })
            : track.Album();

        HomeViewContainer().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        SettingsViewContainer().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        BrowseViewContainer().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        LibraryViewContainer().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        ExitSearchMode();
        UpdateNavSelection(L"Library");
        ShowLibraryDetail(L"album", album, album, track.Artist(), ApprovedDetailArtwork(track, L"album"));
    }


    void MainWindow::LikedSongsButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        BrowseButton_Click(sender, args);
        SelectBrowseFilter(L"Fav");
    }

    void MainWindow::LikeCurrentTrack_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        (void)sender;
        (void)args;
        auto current = AudioPlayerService().GetCurrentTrack();
        ToggleTrackLiked(current);
    }

}
