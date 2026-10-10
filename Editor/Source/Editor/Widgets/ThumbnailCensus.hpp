#pragma once

#include <Common/Content/ContentScan.hpp>

#include <Common/Content/ContentKinds.hpp>

#include <Editor/Panels/FileExplorer/FileType.hpp>
#include <Editor/Widgets/ThumbnailProducers.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// WHICH KINDS OF THE PROJECT GET NO PICTURE, said once (THM-FIXB), free of the device so a suite can hold it.
//
// Nothing here makes a picture or decides when one is made: a picture is made lazily, when a shower draws it
// (ThumbnailService::TickCapture, THUMB-LAZY — UE FAssetThumbnailPool). The project's splash warm-up that
// lived in this header (ProjectWarmList / SceneWarmList / SplashWarmList) held the hand-over for every
// uncaptured picture of the project (1297 on GI_Bistro, > 8 min) and is gone.
namespace Desert::Editor::ThumbnailCensus
{
    /// The file type of a file of content kind @p kind, as the browser types it (FileTypeOfContent: the
    /// extension without the dot, and the kind — a skybox `.detex` is a Skybox, not a texture).
    [[nodiscard]] inline FileType FileTypeOfRow( const std::filesystem::path&                path,
                                                 std::optional<Common::Content::ContentKind> kind )
    {
        std::string extension = path.extension().string();
        if ( !extension.empty() )
            extension.erase( 0, 1 );
        std::transform( extension.begin(), extension.end(), extension.begin(),
                        []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        return FileTypeOfContent( extension, kind );
    }

    /// A CONTENT KIND WITH FILES AND NO PICTURE PRODUCER (THM-FIXB): files the browser shows only a
    /// type icon for, counted by kind, so the editor SAYS it (one line per kind, with the producer table's reason)
    /// instead of skipping silently. TypeIcon kinds are icons by design and not listed; NotYetProduced kinds are
    /// the debt the register names.
    struct Unproduced
    {
        Common::Content::ContentKind Kind  = Common::Content::ContentKind::Scene;
        std::size_t                  Files = 0;
        std::string_view             Why;
    };

    [[nodiscard]] inline std::vector<Unproduced> UnproducedKinds(
         const std::function<std::vector<std::filesystem::path>( Common::Content::ContentKind )>& filesOf )
    {
        using Common::Content::ContentKind;
        std::vector<Unproduced> out;
        for ( std::size_t index = 0; index < Common::Content::CONTENT_KIND_COUNT; ++index )
        {
            const auto kind = static_cast<ContentKind>( index );
            if ( kind == ContentKind::Redirector )
                continue;
            for ( const std::filesystem::path& file : filesOf( kind ) )
            {
                const FileType type = FileTypeOfRow( file, kind );
                if ( ThumbnailProducers::ProducerOf( type ) != ThumbnailProducers::Producer::NotYetProduced )
                    continue;
                if ( out.empty() || out.back().Kind != kind )
                {
                    std::string_view why;
                    for ( const ThumbnailProducers::Row& row : ThumbnailProducers::kTable )
                        if ( row.Type == type )
                            why = row.Why;
                    out.push_back( { kind, 0, why } );
                }
                ++out.back().Files;
            }
        }
        return out;
    }
} // namespace Desert::Editor::ThumbnailCensus
