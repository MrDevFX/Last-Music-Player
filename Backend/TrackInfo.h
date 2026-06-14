#pragma once
#include "TrackInfo.g.h"

namespace winrt::Last_Music_Player::implementation
{
    struct TrackInfo : TrackInfoT<TrackInfo>
    {
        TrackInfo() = default;

        hstring Title() { return m_title; }
        void Title(hstring const& value) { m_title = value; }

        hstring Artist() { return m_artist; }
        void Artist(hstring const& value) { m_artist = value; }

        winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage AlbumArt() { return m_albumArt; }
        void AlbumArt(winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage const& value) { m_albumArt = value; }

        hstring ArtworkUrl() { return m_artworkUrl; }
        void ArtworkUrl(hstring const& value) { m_artworkUrl = value; }

        hstring ArtworkMode() { return m_artworkMode; }
        void ArtworkMode(hstring const& value) { m_artworkMode = value; }

        hstring ArtworkTitle() { return m_artworkTitle; }
        void ArtworkTitle(hstring const& value) { m_artworkTitle = value; }

        hstring ArtworkCaption() { return m_artworkCaption; }
        void ArtworkCaption(hstring const& value) { m_artworkCaption = value; }

        hstring ArtworkGlyph() { return m_artworkGlyph; }
        void ArtworkGlyph(hstring const& value) { m_artworkGlyph = value; }

        double ImageArtworkOpacity() { return m_imageArtworkOpacity; }
        void ImageArtworkOpacity(double value) { m_imageArtworkOpacity = value; }

        double GeneratedArtworkOpacity() { return m_generatedArtworkOpacity; }
        void GeneratedArtworkOpacity(double value) { m_generatedArtworkOpacity = value; }

        winrt::Windows::Storage::StorageFile File() { return m_file; }
        void File(winrt::Windows::Storage::StorageFile const& value) { m_file = value; }

        hstring FilePath() { return m_filePath; }
        void FilePath(hstring const& value) { m_filePath = value; }

        double DurationSeconds() { return m_durationSeconds; }
        void DurationSeconds(double value) { m_durationSeconds = value; }

        hstring Album() { return m_album; }
        void Album(hstring const& value) { m_album = value; }

        hstring Genre() { return m_genre; }
        void Genre(hstring const& value) { m_genre = value; }

        hstring DateAdded() { return m_dateAdded; }
        void DateAdded(hstring const& value) { m_dateAdded = value; }

        double DateAddedSortKey() { return m_dateAddedSortKey; }
        void DateAddedSortKey(double value) { m_dateAddedSortKey = value; }

        hstring Duration() { return m_duration; }
        void Duration(hstring const& value) { m_duration = value; }

        int32_t Index() { return m_index; }
        void Index(int32_t value) { m_index = value; }

        int64_t CatalogId() { return m_catalogId; }
        void CatalogId(int64_t value) { m_catalogId = value; }

        hstring SourceKind() { return m_sourceKind; }
        void SourceKind(hstring const& value) { m_sourceKind = value; }

        hstring Provider() { return m_provider; }
        void Provider(hstring const& value) { m_provider = value; }

        hstring SourceUrl() { return m_sourceUrl; }
        void SourceUrl(hstring const& value) { m_sourceUrl = value; }

        hstring SourceLabel()
        {
            if (UsesExternalProvider())
            {
                return L"Music API";
            }
            return m_sourceLabel;
        }
        void SourceLabel(hstring const& value) { m_sourceLabel = value; }

        bool IsLiked() { return m_isLiked; }
        void IsLiked(bool value) { m_isLiked = value; }

        hstring LikeActionText() { return m_isLiked ? L"Unlike" : L"Like"; }

        int32_t TrackCount() { return m_trackCount; }
        void TrackCount(int32_t value) { m_trackCount = value; }

    private:
        bool UsesExternalProvider() const
        {
            return m_sourceKind == L"remote"
                || (!m_provider.empty() && m_provider != L"manual" && m_provider != L"auto");
        }

        hstring m_title;
        hstring m_artist;
        winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage m_albumArt{ nullptr };
        hstring m_artworkUrl;
        hstring m_artworkMode{ L"track-placeholder" };
        hstring m_artworkTitle;
        hstring m_artworkCaption;
        hstring m_artworkGlyph{ L"\xE8D6" };
        double m_imageArtworkOpacity{ 0.0 };
        double m_generatedArtworkOpacity{ 1.0 };
        winrt::Windows::Storage::StorageFile m_file{ nullptr };
        hstring m_filePath;
        double m_durationSeconds{ 0.0 };
        hstring m_album;
        hstring m_genre;
        hstring m_dateAdded;
        double m_dateAddedSortKey{ 0.0 };
        hstring m_duration;
        int32_t m_index{ 0 };
        int64_t m_catalogId{ 0 };
        hstring m_sourceKind;
        hstring m_provider;
        hstring m_sourceUrl;
        hstring m_sourceLabel;
        bool m_isLiked{ false };
        int32_t m_trackCount{ 0 };
    };
}

namespace winrt::Last_Music_Player::factory_implementation
{
    struct TrackInfo : TrackInfoT<TrackInfo, implementation::TrackInfo>
    {
    };
}

namespace LastMusicPlayer::Backend
{
    using TrackInfo = winrt::Last_Music_Player::TrackInfo;
}
