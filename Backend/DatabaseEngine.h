#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <winrt/Windows.Storage.h>
#include "TrackInfo.h"
#include "ThirdParty/sqlite/sqlite3.h"

namespace LastMusicPlayer::Backend
{
    struct LibraryStats
    {
        int SongCount{ 0 };
        int AlbumCount{ 0 };
        int ArtistCount{ 0 };
        int GenreCount{ 0 };
        double TotalSeconds{ 0.0 };
    };

    struct TrackQuery
    {
        std::wstring Filter{ L"All" };
        std::wstring Sort{ L"DateAdded" };
        std::wstring GroupKind;
        std::wstring GroupKey;
        bool IncludeRemote{ true };
        bool ActiveOnly{ true };
        int Offset{ 0 };
        int Limit{ 0 };
    };

    struct TrackPage
    {
        std::vector<TrackInfo> Tracks;
        int TotalCount{ 0 };
        double TotalSeconds{ 0.0 };
    };

    class DatabaseEngine
    {
    public:
        DatabaseEngine() = default;
        ~DatabaseEngine();

        bool Initialize();
        void Close();
        bool IsInitialized() const { return m_db != nullptr; }
        bool ClearAllUserData();

        void BeginLocalScan();
        void CompleteLocalScan(std::vector<std::wstring> const& activeSourceKeys);
        int64_t UpsertLocalTrack(TrackInfo const& track, std::wstring const& sourceKey);
        int64_t UpsertRemoteTrack(TrackInfo const& track, std::wstring const& sourceKey);
        void RecordPlayback(std::wstring const& sourceKey, uint64_t playOrder);
        void MergePlaybackStats(std::wstring const& sourceKey, uint32_t playCount, uint64_t lastPlayedOrder);
        uint64_t MaxLastPlayedOrder() const;
        void SetLiked(std::wstring const& sourceKey, bool liked);
        bool IsLiked(std::wstring const& sourceKey) const;
        int64_t CreateAlbumCollection(std::wstring const& title);
        int64_t UpsertAlbumCollection(TrackInfo const& album, std::wstring const& albumKey);
        void ReplaceAlbumCollectionTracks(int64_t albumId, std::vector<int64_t> const& trackIds);
        bool DeleteAlbumCollection(std::wstring const& albumKey);
        int64_t CreatePlaylist(std::wstring const& title);
        int64_t UpsertPlaylist(TrackInfo const& playlist, std::wstring const& playlistKey);
        void ReplacePlaylistTracks(int64_t playlistId, std::vector<int64_t> const& trackIds);
        bool AddTrackToPlaylist(std::wstring const& playlistKey, int64_t trackId);
        bool RemoveTrackFromPlaylist(std::wstring const& playlistKey, int64_t trackId);
        bool RenamePlaylist(std::wstring const& playlistKey, std::wstring const& title);
        bool DeletePlaylist(std::wstring const& playlistKey);

        std::vector<TrackInfo> LoadTracks(bool includeRemote = true, bool activeOnly = true) const;
        std::vector<TrackInfo> LoadHistoryTracks(bool includeRemote = true, bool activeOnly = true) const;
        std::vector<TrackInfo> LoadMostPlayedTracks(bool includeRemote = true, bool activeOnly = true) const;
        TrackPage LoadTrackPage(TrackQuery const& query) const;
        std::vector<TrackInfo> LoadTracksForQuery(TrackQuery const& query) const;
        std::vector<TrackInfo> LoadRecentlyAddedTracks(int limit) const;
        std::vector<TrackInfo> LoadAlbums() const;
        std::vector<TrackInfo> LoadPlaylists() const;
        std::vector<TrackInfo> LoadRecentPlaylists(int limit) const;
        std::vector<TrackInfo> LoadArtists() const;
        std::vector<TrackInfo> LoadGenres() const;
        // Genre names ranked by total listening (SUM(PlayCount)), falling back
        // to genre size when the user has no play history yet. Drives the
        // genre-clustered "Made for you" Daily Mixes. Excludes untagged tracks.
        std::vector<std::wstring> LoadTopGenres(int limit) const;
        std::vector<TrackInfo> LoadTracksForGroup(std::wstring const& groupKind, std::wstring const& groupKey) const;
        LibraryStats GetLibraryStats() const;

    private:
        bool EnsureSchema();
        bool MigrateImportedAlbumCollectionsToPlaylists();
        int64_t UpsertTrack(TrackInfo const& track, std::wstring const& sourceKind, std::wstring const& provider, std::wstring const& sourceKey, bool active);
        sqlite3* m_db = nullptr;
        mutable std::recursive_mutex m_mutex;
    };
}
