#include "AssetThumbnailPool.hpp"

#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailFoliage.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailKey.hpp>
#include <Editor/Widgets/ThumbnailPose.hpp>
#include <Editor/Widgets/ThumbnailProducers.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/ThumbnailSubject.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace Desert::Editor
{
    AssetThumbnailPool::AssetThumbnailPool( Assets::AssetManager* assetManager )
         : m_AssetManager( assetManager ), m_Thumbnails( std::make_unique<ThumbnailCache>() )
    {
        ThumbnailCache::PurgeOldVersions(); // drop stale-renderer thumbnails so they regenerate cleanly
    }

    AssetThumbnailPool::~AssetThumbnailPool() = default;

    void AssetThumbnailPool::OnCaptured( const std::string& png, const std::string& assetPath )
    {
        m_Thumbnails->Invalidate( png );   // drop the cached decode: the grid reloads
        m_FailedThumbs.erase( assetPath ); // a refused tile shows the captured picture
    }

    void AssetThumbnailPool::PrefetchFolder( const DirectoryInformation& folder )
    {
        // The same picture each AssetTileThumbnail::Draw* will pass to ThumbnailCache::Get, and the same
        // freshness subject it judges — a mismatch here would only cost a wasted decode (Get takes nothing
        // it was not asked for), never a wrong picture.
        std::vector<ThumbnailPrefetch::Item> items;
        for ( const DirectoryInformation* entry : folder.Children )
        {
            if ( entry == nullptr || !entry->IsFile || entry->Hidden )
                continue;
            using ThumbnailProducers::Producer;
            switch ( ThumbnailProducers::ProducerOf( entry->Type ).value_or( Producer::TypeIcon ) )
            {
                case Producer::Decoded:
                    items.push_back( { entry->AssetPath, {} } );
                    break;
                case Producer::RenderedMaterial:
                case Producer::RenderedPose: // a .skmesh is its own cooked form and freshness source
                case Producer::RenderedSky:  // keyed and judged on the skybox's own file
                case Producer::Painted:
                    items.push_back( { ThumbnailPngFor( entry->AssetPath ), entry->AssetPath } );
                    break;
                case Producer::RenderedMesh:
                {
                    const std::optional<MeshPicture> picture = MeshPictureFor( entry->AssetPath, entry->Type );
                    if ( !picture )
                        break;
                    items.push_back( { ThumbnailKey::DiskPath( picture->Cooked ), picture->Cooked } );
                    break;
                }
                case Producer::NotYetProduced:
                case Producer::TypeIcon:
                    break;
            }
        }
        // THE FOLDER BEING SHOWN, AND ONLY IT (THUMB-LAZY; UE FAssetThumbnailPool decodes what a visible tile
        // asks for): Request replaces what is waiting, so leaving a folder drops its undecoded pictures. What
        // the cache already holds is not handed over at all. Nothing waits on this: the workers decode, the
        // tile uploads through ThumbnailCache::Get when it is drawn.
        ThumbnailPrefetch::Get().Request(
             ThumbnailPrefetch::Unresident( std::move( items ), [this]( const std::string& picture )
                                            { return m_Thumbnails->Holds( picture ); } ) );
    }

    const std::string& AssetThumbnailPool::ThumbnailPngFor( const std::string& assetPath )
    {
        auto it = m_ThumbnailPngOf.find( assetPath );
        if ( it == m_ThumbnailPngOf.end() )
            it = m_ThumbnailPngOf.emplace( assetPath, ThumbnailKey::DiskPath( assetPath ) ).first;
        return it->second;
    }

    std::size_t AssetThumbnailPool::ResidentThumbnails() const
    {
        return m_Thumbnails->ResidentCount();
    }

    std::optional<std::string> AssetThumbnailPool::MeshSourceFor( const std::string& assetPath,
                                                                  const FileType     type )
    {
        if ( type != FileType::FoliageType )
            return assetPath;
        if ( m_FailedThumbs.contains( assetPath ) )
            return std::nullopt;
        std::error_code                       ec;
        const std::filesystem::file_time_type written = std::filesystem::last_write_time( assetPath, ec );
        if ( const auto it = m_MeshSourceOf.find( assetPath );
             it != m_MeshSourceOf.end() && !ec && it->second.Written == written )
            return it->second.Source;
        const auto source = ThumbnailFoliage::ReadMeshSource( assetPath, Common::Constants::Path::ASSETS_PATH );
        if ( !source )
        {
            LOG_WARN( "[Thumbnail] '{}': {}", assetPath, source.GetError() );
            m_FailedThumbs.insert( assetPath );
            m_MeshSourceOf.erase( assetPath );
            return std::nullopt;
        }
        std::string mesh          = source.GetValue().generic_string();
        m_MeshSourceOf[assetPath] = { written, mesh };
        return mesh;
    }

    std::optional<AssetThumbnailPool::MeshPicture>
    AssetThumbnailPool::MeshPictureFor( const std::string& assetPath, const FileType type )
    {
        const std::optional<std::string> source = MeshSourceFor( assetPath, type );
        if ( !source )
            return std::nullopt;
        std::string extension = std::filesystem::path( *source ).extension().string();
        std::ranges::transform( extension, extension.begin(),
                                []( const unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        if ( std::ranges::find( Common::Content::kRawMeshSourceExtensions, extension ) ==
             Common::Content::kRawMeshSourceExtensions.end() )
            return MeshPicture{ CookPaths::MeshAsset( *source ).generic_string(), false }; // its own cooked form

        std::error_code                       ec;
        const std::filesystem::file_time_type written =
             std::filesystem::last_write_time( Common::Content::ImportRecordPathFor( *source ), ec );
        if ( ec )
            return std::nullopt; // not imported yet: a source is not an asset, its tile is the type icon
        if ( const auto it = m_SourcePictureOf.find( *source );
             it != m_SourcePictureOf.end() && it->second.Written == written )
            return it->second.Picture;

        std::optional<MeshPicture> picture;
        const auto                 kind = Assets::Serialization::ReadImportRecordKind( *source );
        if ( !kind )
        {
            LOG_WARN( "[Thumbnail] '{}': {}", assetPath, kind.GetError() ); // once per record version (cached)
        }
        else
        {
            using Common::Content::ContentKind;
            switch ( kind.GetValue() )
            {
                case ContentKind::StaticMesh:
                    picture = MeshPicture{ CookPaths::MeshAsset( *source ).generic_string(), false };
                    break;
                case ContentKind::SkinnedMesh:
                    picture = MeshPicture{ CookPaths::SkinnedAsset( *source, ".skmesh" ).generic_string(), true };
                    break;
                case ContentKind::Skeleton:
                    picture =
                         MeshPicture{ CookPaths::SkinnedAsset( *source, ".skeleton" ).generic_string(), true };
                    break;
                default:
                    break; // clips only: each clip has its own tile; the source keeps its type icon
            }
        }
        m_SourcePictureOf[*source] = { written, picture };
        return picture;
    }
} // namespace Desert::Editor
