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
        m_PrefetchItems = items;
        // The project's pictures stay asked for: Request replaces what is waiting, and the folder must not
        // push them out of the queue (nor the other way round). What the cache already holds is not handed
        // over at all (THM1n-13): the splash made the project resident, so entering a folder decodes nothing.
        items.insert( items.end(), m_ProjectPrefetchItems.begin(), m_ProjectPrefetchItems.end() );
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

    std::size_t AssetThumbnailPool::WarmProjectThumbnails( const std::vector<ThumbnailWarmup::WarmItem>& scene,
                                                           const std::vector<ThumbnailWarmup::WarmItem>& project )
    {
        using ThumbnailWarmup::WarmItem;
        using ThumbnailWarmup::WarmKind;
        if ( m_AssetManager == nullptr )
            return 0;

        // A mesh is judged and filed by its COOKED form, as its tile does (DrawRenderedMeshThumbnail): the
        // picture is under the .stmesh and its freshness source is MeshFreshnessSource of it. A foliage type
        // is its mesh's picture (MeshSourceFor); a .skmesh is its own cooked form; a skinned source is the
        // pose of the asset its import wrote (MeshPictureFor), resolved as a Pose.
        const auto pictureOf = [this]( const WarmItem& item ) -> std::optional<MeshPicture>
        {
            if ( item.Kind == WarmKind::Pose )
                return MeshPicture{ item.Path, true };
            return MeshPictureFor( item.Path, ThumbnailWarmup::FileTypeOfPath( item.Path ) );
        };
        const auto cookedOf = [&pictureOf]( const WarmItem& item ) -> std::optional<std::string>
        {
            const std::optional<MeshPicture> picture = pictureOf( item );
            if ( !picture )
                return std::nullopt;
            return picture->Cooked;
        };
        const auto verdictOf = [&]( const WarmItem& item )
        {
            if ( item.Kind == WarmKind::Decoded )
                return ThumbnailFreshness::Verdict::Show; // the file is its own picture
            if ( item.Kind == WarmKind::Mesh || item.Kind == WarmKind::Pose )
            {
                const std::optional<std::string> cooked = cookedOf( item );
                if ( !cooked )
                    return ThumbnailFreshness::Verdict::Show; // refused and named by MeshSourceFor: no capture
                return ThumbnailService::JudgeMeshPicture( *cooked ); // the tile's and the enqueue gate's verdict
            }
            if ( item.Kind == WarmKind::Sky )
                return ThumbnailService::JudgeSkyboxPicture( item.Path ); // the tile's and RequestSkybox's verdict
            return ThumbnailFreshness::Judge(
                 ThumbnailFreshness::Observe( ThumbnailPngFor( item.Path ), item.Path ) );
        };
        const auto needsCapture = [&]( const WarmItem& item ) {
            return !m_FailedThumbs.contains( item.Path ) &&
                   verdictOf( item ) == ThumbnailFreshness::Verdict::Capture;
        };

        // EVERY PICTURE OF THE PROJECT IS ASKED FOR — the one the disk has goes to a worker decode now, the one
        // a capture below writes is asked for again when the captures have landed (RequestProjectPictures).
        m_ProjectPrefetchItems.clear();
        for ( const std::vector<WarmItem>* list : { &scene, &project } )
        {
            for ( const WarmItem& item : *list )
            {
                if ( item.Kind == WarmKind::Decoded )
                    m_ProjectPrefetchItems.push_back( { item.Path, {} } );
                else if ( item.Kind == WarmKind::Mesh || item.Kind == WarmKind::Pose )
                {
                    if ( const std::optional<std::string> cooked = cookedOf( item ) )
                        m_ProjectPrefetchItems.push_back( { ThumbnailKey::DiskPath( *cooked ), *cooked } );
                }
                else
                    m_ProjectPrefetchItems.push_back( { ThumbnailPngFor( item.Path ), item.Path } );
            }
        }

        const std::vector<WarmItem> warm = ThumbnailWarmup::SplashWarmList( scene, project, needsCapture );
        m_WarmMeshesPending.clear();
        for ( const WarmItem& item : warm )
        {
            if ( !needsCapture( item ) )
                continue; // a fresh scene subject: its picture is decoded with the rest
            switch ( item.Kind )
            {
                case WarmKind::Mesh:
                case WarmKind::Pose:
                {
                    // Resolved by TickWarmMeshes with the path each resolver takes, as the tile resolves it: a
                    // pose by its cooked asset (ResolvePoseSubject), a static mesh by the FILE ITS PICTURE IS OF
                    // (MeshFreshnessSource: a hand-authored .stmesh itself, else the raw source beside it).
                    // ResolveMesh asks the DDC for the source's import; handed the cooked path of an import
                    // (a scene root names base.stmesh, which an import never writes) it hashed a file that is
                    // not on disk and logged "Could not read file" for every imported mesh the scene used.
                    const std::optional<MeshPicture> picture = pictureOf( item );
                    if ( !picture )
                        break;
                    if ( picture->Pose )
                        m_WarmMeshesPending.push_back( { picture->Cooked, WarmKind::Pose } );
                    else
                        m_WarmMeshesPending.push_back(
                             { ThumbnailFreshness::MeshFreshnessSource( picture->Cooked ).generic_string(),
                               WarmKind::Mesh } );
                    break;
                }
                case WarmKind::Painted:
                    ThumbnailService::Get().WarmPainted( item.Path );
                    break;
                case WarmKind::Sky:
                {
                    // The row the registry filed it under names the handle, as the tile's request does.
                    const Assets::AssetHandle skybox = Runtime::SkyboxHandleAtPath( item.Path );
                    if ( static_cast<uint64_t>( skybox ) == 0 )
                    {
                        LOG_WARN( "[Thumbnails] the splash cannot warm '{}': the registry has no skybox row at it",
                                  item.Path );
                        m_FailedThumbs.insert( item.Path );
                        break;
                    }
                    ThumbnailService::Get().WarmSkybox( skybox, item.Path );
                    break;
                }
                case WarmKind::Material:
                {
                    // Resolved on a worker when it is not read yet; the arrival queues it as the tile's would.
                    const auto subject = ThumbnailSubject::ResolveMaterial(
                         *m_AssetManager, item.Path,
                         []( const std::string&                                   assetPath,
                             const Common::ResultStr<ThumbnailSubject::Material>& resolved )
                         {
                             if ( resolved )
                                 ThumbnailService::Get().WarmMaterial( resolved.GetValue(), assetPath );
                         } );
                    if ( !subject )
                    {
                        LOG_WARN( "[Thumbnails] the splash cannot warm '{}': {}", item.Path, subject.GetError() );
                        m_FailedThumbs.insert( item.Path );
                        break;
                    }
                    if ( const auto& material = subject.GetValue() )
                        ThumbnailService::Get().WarmMaterial( *material, item.Path );
                    break;
                }
                case WarmKind::Decoded:
                    break;
            }
        }
        (void)TickWarmMeshes();
        RequestProjectPictures();
        return ThumbnailService::Get().SceneWarmPending() + m_WarmMeshesPending.size();
    }

    void AssetThumbnailPool::RequestProjectPictures()
    {
        std::vector<ThumbnailPrefetch::Item> items = m_PrefetchItems;
        items.insert( items.end(), m_ProjectPrefetchItems.begin(), m_ProjectPrefetchItems.end() );
        ThumbnailPrefetch::Get().Request(
             ThumbnailPrefetch::Unresident( std::move( items ), [this]( const std::string& picture )
                                            { return m_Thumbnails->Holds( picture ); } ) );
    }

    std::size_t AssetThumbnailPool::ResidentThumbnails() const
    {
        return m_Thumbnails->ResidentCount();
    }

    std::size_t AssetThumbnailPool::TickWarmMeshes()
    {
        if ( m_AssetManager == nullptr || m_WarmMeshesPending.empty() )
            return 0;
        std::erase_if( m_WarmMeshesPending,
                       [this]( const ThumbnailWarmup::WarmItem& item )
                       {
                           const bool         pose = item.Kind == ThumbnailWarmup::WarmKind::Pose;
                           const std::string& path = item.Path;
                           const auto subject = pose ? ThumbnailPose::ResolvePoseSubject( *m_AssetManager, path )
                                                     : ThumbnailSubject::ResolveMesh( *m_AssetManager, path );
                           if ( !subject )
                           {
                               // The same refusal the tile would log and blacklist; once, here, instead.
                               LOG_WARN( "[Thumbnails] the splash cannot warm '{}': {}", path,
                                         subject.GetError() );
                               m_FailedThumbs.insert( path );
                               return true;
                           }
                           if ( subject.GetValue().Pending )
                               return false; // read in flight: asked again next frame
                           if ( pose )
                               ThumbnailService::Get().WarmPose( subject.GetValue() );
                           else
                               ThumbnailService::Get().WarmMesh( subject.GetValue() );
                           return true;
                       } );
        return m_WarmMeshesPending.size();
    }

    std::size_t AssetThumbnailPool::UploadPrefetchedThumbnails()
    {
        std::vector<ThumbnailPrefetch::Item> items = m_PrefetchItems;
        items.insert( items.end(), m_ProjectPrefetchItems.begin(), m_ProjectPrefetchItems.end() );
        const ThumbnailPrefetch::Survey survey = ThumbnailPrefetch::Get().SurveyOf( items );
        for ( const std::string& picture : survey.Ready )
            (void)m_Thumbnails->Get( picture );
        return survey.Pending;
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
