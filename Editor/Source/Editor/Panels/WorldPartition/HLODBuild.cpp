#include "HLODBuild.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Core/HLODMeshBuilder.hpp>
#include <Engine/Core/Serialize/EntityDescriptorIndex.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <system_error>
#include <utility>

namespace Desert::Editor
{
    namespace
    {
        namespace WC = ::Desert::Core::WorldCells;

        // The saved world, read exactly as WorldCook reads it: the scene file's text parsed, and its descriptor
        // index refreshed from the entity files.
        struct SavedWorld
        {
            ::Desert::Core::LoadableScene                        Scene;
            std::vector<::Desert::Core::Rules::EntityDescriptor> Planned;
        };

        Common::ResultStr<SavedWorld> ReadSavedWorld( const std::filesystem::path& path )
        {
            auto text = ::Desert::Core::ExternalEntities::ReadSceneFileText( path );
            if ( !text )
                return Common::MakeError<SavedWorld>( "Build HLODs: " + text.GetError() );
            auto scene = ::Desert::Core::ParseLoadableScene( path.string(), text.GetValue() );
            if ( !scene )
                return Common::MakeError<SavedWorld>( "Build HLODs: " + scene.GetError() );
            auto descriptors = ::Desert::Core::DescriptorIndex::Refresh( path );
            if ( !descriptors )
                return Common::MakeError<SavedWorld>( "Build HLODs: " + descriptors.GetError() );
            SavedWorld world{ std::move( scene.GetValue() ),
                              ::Desert::Core::DescriptorIndex::Descriptors( descriptors.GetValue().Index ) };
            return Common::MakeSuccess( std::move( world ) );
        }
    } // namespace

    HLODBuild::HLODBuild( std::filesystem::path scenePath, std::string sceneName )
         : m_ScenePath( std::move( scenePath ) ), m_SceneName( std::move( sceneName ) )
    {
    }

    HLODBuild::~HLODBuild() = default;

    Common::ResultStr<std::unique_ptr<HLODBuild>>
    HLODBuild::Start( const std::filesystem::path& scenePath, std::string sceneName, Assets::AssetManager* assets )
    {
        auto world = ReadSavedWorld( scenePath );
        if ( !world )
            return Common::MakeError<std::unique_ptr<HLODBuild>>( world.GetError() );
        const std::span                    registries( &::Desert::Assets::ContentRegistry::Get(), 1 );
        const ::Desert::Core::HLODBuilders builders = ::Desert::Core::MakeHLODBuilders( registries );
        // Phase 1: the landscape HLODs' areas, from the same request the build makes for each.
        std::vector<LandscapeHLODBakeTile> areas;
        decltype( builders.Landscape )     collect;
        if ( builders.Landscape )
            collect = [&]( const auto& request ) -> Common::ResultStr<::Desert::Assets::StaticMeshComponentSer>
            {
                auto area = ::Desert::Core::LandscapeHLODTileAreaOf( request );
                if ( !area )
                    return Common::MakeError<::Desert::Assets::StaticMeshComponentSer>( area.GetError() );
                auto mesh = builders.Landscape( request );
                if ( !mesh )
                    return mesh;
                const bool known = std::any_of( areas.begin(), areas.end(),
                                                [&]( const auto& seen ) {
                                                    return static_cast<uint64_t>( seen.Area.Tile ) ==
                                                           static_cast<uint64_t>( area.GetValue().Tile );
                                                } );
                if ( known )
                    return mesh;
                // The bake states the normal map in the tangent frame of this very mesh.
                if ( !mesh.GetValue().EditMesh )
                    return Common::MakeError<::Desert::Assets::StaticMeshComponentSer>(
                         std::format( "the landscape HLOD of tile {} carries no mesh",
                                      static_cast<uint64_t>( area.GetValue().Tile ) ) );
                areas.push_back( LandscapeHLODBakeTile{ area.GetValue(), *mesh.GetValue().EditMesh } );
                return mesh;
            };
        if ( auto probe = WC::BuildWorldHLODs(
                  world.GetValue().Scene.Scene, world.GetValue().Planned, registries, WC::PrefabFileReader(),
                  builders.Mesh, collect,
                  // The probe only gathers the landscape tiles to bake: its files are dropped.
                  []( WC::CookedFile ) -> Common::BoolResultStr { return BOOLSUCCESS; } );
             !probe )
            return Common::MakeError<std::unique_ptr<HLODBuild>>( "Build HLODs: " + probe.GetError() );

        std::unique_ptr<HLODBuild> build( new HLODBuild( scenePath, std::move( sceneName ) ) );
        if ( areas.empty() )
        {
            build->m_Outcome = build->Finish( {} );
            return Common::MakeSuccess( std::move( build ) );
        }
        LOG_INFO( "[Build HLODs] '{0}': baking the BaseColor of {1} landscape tile(s)", build->m_SceneName,
                  areas.size() );
        build->m_Bake = std::make_unique<LandscapeHLODBake>( scenePath, std::move( world.GetValue().Scene ),
                                                             std::move( areas ), assets );
        return Common::MakeSuccess( std::move( build ) );
    }

    std::optional<Common::BoolResultStr> HLODBuild::Tick()
    {
        if ( m_Outcome )
            return m_Outcome;
        if ( !m_Bake || !m_Bake->Tick() )
            return std::nullopt;
        const std::unique_ptr<LandscapeHLODBake> bake = std::move( m_Bake );
        if ( bake->Error() )
            m_Outcome.emplace( Common::MakeError( "Build HLODs: the landscape bake was refused: " + *bake->Error() ) );
        else
            m_Outcome = Finish( bake->Baked() );
        return m_Outcome;
    }

    Common::BoolResultStr HLODBuild::Finish( const std::vector<LandscapeHLODBaked>& baked ) const
    {
        const std::filesystem::path& path  = m_ScenePath;
        auto                         world = ReadSavedWorld( path );
        if ( !world )
            return Common::MakeError( world.GetError() );
        const ::Desert::Core::SceneSerialized& scene   = world.GetValue().Scene.Scene;
        const auto&                            planned = world.GetValue().Planned;
        const std::span                        registries( &::Desert::Assets::ContentRegistry::Get(), 1 );
        const ::Desert::Core::HLODBuilders     builders = ::Desert::Core::MakeHLODBuilders( registries );
        // Each landscape HLOD bound to the material its tile's bake wrote (UE ULandscapeHLODBuilder). The slot
        // does not enter HLODBuildChecksum (it hashes the member records), so binding changes no staleness.
        decltype( builders.Landscape ) landscape;
        if ( builders.Landscape )
            landscape = [&]( const auto& request ) -> Common::ResultStr<::Desert::Assets::StaticMeshComponentSer>
            {
                auto mesh = builders.Landscape( request );
                if ( !mesh )
                    return mesh;
                const auto& id = request.Records[request.Tile].id;
                for ( const LandscapeHLODBaked& tile : baked )
                    if ( id && static_cast<uint64_t>( tile.Tile ) == static_cast<uint64_t>( *id ) )
                    {
                        ::Desert::Assets::StaticMeshComponentSer block = mesh.GetValue();
                        block.MaterialSlots = std::vector<::Desert::Assets::AssetGuidRef>{
                             ::Desert::Assets::AssetGuidRef{ tile.MaterialGuid, tile.MaterialPath } };
                        return Common::MakeSuccess( std::move( block ) );
                    }
                return mesh;
            };
        // Each cell's file is written the moment it is built and freed with it (UE builds and saves its HLOD
        // actors cell by cell), so the world's HLODs are never in memory at once.
        const std::filesystem::path dir    = WC::BuiltHLODDirectory( path );
        const WC::BuiltHLODSink     writer = WC::BuiltHLODWriter( path );
        std::size_t                 bytes  = 0;
        std::size_t                 files  = 0;
        auto                        built  = WC::BuildWorldHLODs(
             scene, planned, registries, WC::PrefabFileReader(), builders.Mesh, landscape,
             [&]( WC::CookedFile file ) -> Common::BoolResultStr
             {
                 bytes += file.Bytes.size();
                 ++files;
                 LOG_INFO( "[Build HLODs] {0}: {1} bytes", ( dir / file.Name ).string(), file.Bytes.size() );
                 return writer( std::move( file ) );
             } );
        if ( !built )
            return Common::MakeError( "Build HLODs: " + built.GetError() );
        if ( auto written = WC::WriteBuiltHLODIndex( path, built.GetValue() ); !written )
            return Common::MakeError( written.GetError() );
        // Read back what was written and check it against the world it was built from.
        auto stored = WC::ReadBuiltHLODs( path );
        if ( !stored )
            return Common::MakeError( "Build HLODs: " + stored.GetError() );
        std::size_t records = 0;
        for ( const WC::BuiltHLODCell& cell : stored.GetValue().Index.Cells )
        {
            auto cellRecords = WC::StoredCellRecords( stored.GetValue(), cell );
            if ( !cellRecords )
                return Common::MakeError( "Build HLODs: read back: " + cellRecords.GetError() );
            records += cellRecords.GetValue().size();
        }
        // Against the world the build cut: prefab instances expanded as the cook expands them (WP21).
        auto cut = WC::ExpandWorldForCook( scene, planned, WC::PrefabFileReader() );
        if ( !cut )
            return Common::MakeError( "Build HLODs: read back: " + cut.GetError() );
        auto stale = WC::StaleHLODCells( stored.GetValue().Index, cut.GetValue().World.Scene,
                                         cut.GetValue().Descriptors, registries );
        if ( !stale )
            return Common::MakeError( "Build HLODs: read back: " + stale.GetError() );
        for ( const std::string& unit : stale.GetValue() )
            LOG_ERROR( "[Build HLODs] {0}: the HLOD read back is stale", unit );
        LOG_INFO(
             "[Build HLODs] '{0}': {1} cell(s), {2} file(s), {3} bytes, {4} HLOD record(s) in {5}; read back: "
             "{6} stale cell(s)",
             m_SceneName, stored.GetValue().Index.Cells.size(), files, bytes, records, dir.string(),
             stale.GetValue().size() );
        if ( !stale.GetValue().empty() )
            return Common::MakeError(
                 std::format( "Build HLODs: {} cell(s) read back stale", stale.GetValue().size() ) );
        return BOOLSUCCESS;
    }

    Common::ResultStr<std::size_t> DeleteBuiltHLODs( const std::filesystem::path& scenePath )
    {
        const std::filesystem::path dir = WC::BuiltHLODDirectory( scenePath );
        std::error_code             error;
        if ( !std::filesystem::exists( dir, error ) )
        {
            if ( error )
                return Common::MakeError<std::size_t>( std::format(
                     "Delete HLODs: '{}' could not be looked at: {}", dir.string(), error.message() ) );
            LOG_INFO( "[Delete HLODs] '{}': no HLODs are built for this world", scenePath.string() );
            return Common::MakeSuccess( std::size_t{ 0 } );
        }
        const std::uintmax_t removed = std::filesystem::remove_all( dir, error );
        if ( error )
            return Common::MakeError<std::size_t>(
                 std::format( "Delete HLODs: '{}' was not removed: {}", dir.string(), error.message() ) );
        // remove_all counts the directory itself among what it removed.
        const std::size_t files = removed > 0 ? static_cast<std::size_t>( removed - 1 ) : 0;
        LOG_INFO( "[Delete HLODs] '{}': {} file(s) removed with {}", scenePath.string(), files, dir.string() );
        return Common::MakeSuccess( files );
    }
} // namespace Desert::Editor
