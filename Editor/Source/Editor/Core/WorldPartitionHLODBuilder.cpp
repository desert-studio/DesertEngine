// Ported from UE 5.5 (pattern)
// Engine/Source/Editor/WorldPartitionEditor/Private/WorldPartition/WorldPartitionHLODsBuilder.cpp (SetupHLODActors
// / BuildHLODActors / DeleteHLODActors: an HLOD actor whose hash is unchanged is skipped unless -ForceBuild) and
// Engine/Source/Editor/WorldPartitionEditor/Private/WorldPartition/HLOD/Utilities/WorldPartitionHLODUtilities.cpp
// (ComputeHLODHash: the input actors, the assets they use, the HLOD layer's settings and the builder's base key),
// adapted: the HLOD actors are rows of BuiltHLODIndex saved beside the world (WorldCells.hpp), the hash is
// WorldCells::HLODBuildChecksum, and the landscape BaseColor bake runs over editor frames (LandscapeHLODBake).

#include "WorldPartitionHLODBuilder.hpp"

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

        // The world saved at a scene file, read exactly as WorldCook reads it, so the checksums a build stores are
        // the ones a cook of the same files computes.
        struct SavedWorld
        {
            ::Desert::Core::LoadableScene                        Loadable;
            std::vector<::Desert::Core::Rules::EntityDescriptor> Descriptors;
        };

        Common::ResultStr<SavedWorld> ReadSavedWorld( const std::filesystem::path& path )
        {
            auto text = ::Desert::Core::ExternalEntities::ReadSceneFileText( path );
            if ( !text )
                return Common::MakeError<SavedWorld>( text.GetError() );
            auto scene = ::Desert::Core::ParseLoadableScene( path.string(), text.GetValue() );
            if ( !scene )
                return Common::MakeError<SavedWorld>( scene.GetError() );
            auto descriptors = ::Desert::Core::DescriptorIndex::Refresh( path );
            if ( !descriptors )
                return Common::MakeError<SavedWorld>( descriptors.GetError() );
            SavedWorld world{ scene.ExtractValue(),
                              ::Desert::Core::DescriptorIndex::Descriptors( descriptors.GetValue().Index ) };
            return Common::MakeSuccess( std::move( world ) );
        }

        bool HasSavedIndex( const std::filesystem::path& scenePath )
        {
            std::error_code ec;
            return std::filesystem::exists( WC::BuiltHLODDirectory( scenePath ) / WC::kBuiltHLODIndexName, ec );
        }

        // The rows a build may keep: the saved index, unless every row is to be built - forced, never built, or an
        // index that no longer reads (a format raised since it was saved), which the log names.
        std::optional<WC::StoredHLODs> PreviousBuild( const std::filesystem::path& scenePath,
                                                      HLODBuildOptions             options )
        {
            if ( options.ForceAll || !HasSavedIndex( scenePath ) )
                return std::nullopt;
            auto stored = WC::ReadBuiltHLODs( scenePath );
            if ( !stored )
            {
                LOG_WARN( "[Build HLODs] the saved HLOD index does not read, so every HLOD is built again: {0}",
                          stored.GetError() );
                return std::nullopt;
            }
            return stored.ExtractValue();
        }

        std::span<const Common::Utils::AssetRegistry> Registries()
        {
            return std::span( &::Desert::Assets::ContentRegistry::Get(), 1 );
        }
    } // namespace

    WorldPartitionHLODBuilder::WorldPartitionHLODBuilder( std::filesystem::path scenePath,
                                                          HLODBuildOptions      options )
         : m_ScenePath( std::move( scenePath ) ), m_Options( options )
    {
    }

    WorldPartitionHLODBuilder::~WorldPartitionHLODBuilder() = default;

    Common::BoolResultStr WorldPartitionHLODBuilder::Start( const Assets::AssetManager* assets )
    {
        auto saved = ReadSavedWorld( m_ScenePath );
        if ( !saved )
            return Common::MakeError( "Build HLODs: " + saved.GetError() );
        SavedWorld                           world      = saved.ExtractValue();
        const std::optional<WC::StoredHLODs> previous   = PreviousBuild( m_ScenePath, m_Options );
        const auto                           registries = Registries();
        const ::Desert::Core::HLODBuilders   builders   = ::Desert::Core::MakeHLODBuilders( registries );
        // Phase 1: the landscape HLODs' areas of the rows to build, from the same request the build makes for
        // each.
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
                  world.Loadable.Scene, world.Descriptors, registries, WC::PrefabFileReader(), builders.Mesh,
                  collect, previous ? &*previous : nullptr,
                  // The probe only gathers the landscape tiles to bake: its files are dropped.
                  []( WC::CookedFile ) -> Common::BoolResultStr { return BOOLSUCCESS; } );
             !probe )
            return Common::MakeError( "Build HLODs: " + probe.GetError() );
        if ( areas.empty() )
        {
            m_Done = true;
            return Finish( {} );
        }
        LOG_INFO( "[Build HLODs] '{0}': baking the BaseColor of {1} landscape tile(s)",
                  m_ScenePath.stem().string(), areas.size() );
        m_Bake = std::make_unique<LandscapeHLODBake>( m_ScenePath, std::move( world.Loadable ), std::move( areas ),
                                                      assets );
        return BOOLSUCCESS;
    }

    bool WorldPartitionHLODBuilder::Tick()
    {
        if ( m_Done )
            return true;
        if ( !m_Bake || !m_Bake->Tick() )
            return false;
        const std::unique_ptr<LandscapeHLODBake> bake = std::move( m_Bake );
        m_Done                                        = true;
        if ( bake->Error() )
        {
            m_Error = "Build HLODs: the landscape bake was refused: " + *bake->Error();
            return true;
        }
        if ( auto done = Finish( bake->Baked() ); !done )
            m_Error = done.GetError();
        return true;
    }

    Common::BoolResultStr WorldPartitionHLODBuilder::Finish( const std::vector<LandscapeHLODBaked>& baked )
    {
        auto saved = ReadSavedWorld( m_ScenePath );
        if ( !saved )
            return Common::MakeError( "Build HLODs: " + saved.GetError() );
        const SavedWorld&                    world      = saved.GetValue();
        const std::optional<WC::StoredHLODs> previous   = PreviousBuild( m_ScenePath, m_Options );
        const auto                           registries = Registries();
        const ::Desert::Core::HLODBuilders   builders   = ::Desert::Core::MakeHLODBuilders( registries );
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
        // actors cell by cell), so the world's HLODs are never in memory at once. A kept row's file stays.
        const std::filesystem::path dir    = WC::BuiltHLODDirectory( m_ScenePath );
        const WC::BuiltHLODSink     writer = WC::BuiltHLODWriter( m_ScenePath );
        std::size_t                 bytes  = 0;
        std::size_t                 files  = 0;
        auto                        built  = WC::BuildWorldHLODs(
             world.Loadable.Scene, world.Descriptors, registries, WC::PrefabFileReader(), builders.Mesh, landscape,
             previous ? &*previous : nullptr,
             [&]( WC::CookedFile file ) -> Common::BoolResultStr
             {
                 bytes += file.Bytes.size();
                 ++files;
                 LOG_INFO( "[Build HLODs] {0}: {1} bytes", ( dir / file.Name ).string(), file.Bytes.size() );
                 return writer( std::move( file ) );
             } );
        if ( !built )
            return Common::MakeError( "Build HLODs: " + built.GetError() );
        const WC::HLODBuild& result = built.GetValue();
        if ( auto written = WC::WriteBuiltHLODIndex( m_ScenePath, result.Index ); !written )
            return Common::MakeError( written.GetError() );
        // Read back what is saved now - the rows built and the rows kept - and check it against the world.
        auto stored = WC::ReadBuiltHLODs( m_ScenePath );
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
        auto cut = WC::ExpandWorldForCook( world.Loadable.Scene, world.Descriptors, WC::PrefabFileReader() );
        if ( !cut )
            return Common::MakeError( "Build HLODs: read back: " + cut.GetError() );
        auto stale = WC::StaleHLODCells( stored.GetValue().Index, cut.GetValue().World.Scene,
                                         cut.GetValue().Descriptors, registries );
        if ( !stale )
            return Common::MakeError( "Build HLODs: read back: " + stale.GetError() );
        for ( const std::string& unit : stale.GetValue() )
            LOG_ERROR( "[Build HLODs] {0}: the HLOD read back is stale", unit );
        for ( const std::string& unit : result.Built )
            LOG_INFO( "[Build HLODs] built {0}", unit );
        LOG_INFO(
             "[Build HLODs] '{0}': built {1} of {2} HLOD cells ({3}); {4} file(s), {5} bytes written, {6} HLOD "
             "record(s) in {7}; read back: {8} stale cell(s)",
             m_ScenePath.stem().string(), result.Built.size(), result.Index.Cells.size(),
             m_Options.ForceAll ? "force all" : "others up to date", files, bytes, records, dir.string(),
             stale.GetValue().size() );
        if ( !stale.GetValue().empty() )
            return Common::MakeError(
                 std::format( "Build HLODs: {} cell(s) read back stale", stale.GetValue().size() ) );
        return BOOLSUCCESS;
    }

    Common::ResultStr<HLODStatus> HLODStatusOf( const std::filesystem::path& scenePath )
    {
        auto saved = ReadSavedWorld( scenePath );
        if ( !saved )
            return Common::MakeError<HLODStatus>( saved.GetError() );
        const SavedWorld&  world = saved.GetValue();
        HLODStatus         status;
        WC::BuiltHLODIndex index; // no saved index: every row is missing
        if ( HasSavedIndex( scenePath ) )
        {
            auto stored = WC::ReadBuiltHLODs( scenePath );
            if ( !stored )
                return Common::MakeError<HLODStatus>( stored.GetError() );
            index        = std::move( stored.GetValue().Index );
            status.Built = true;
        }
        // The world the build cuts and the cook checks: prefab instances expanded (WP21).
        auto cut = WC::ExpandWorldForCook( world.Loadable.Scene, world.Descriptors, WC::PrefabFileReader() );
        if ( !cut )
            return Common::MakeError<HLODStatus>( cut.GetError() );
        auto stale =
             WC::StaleHLODCells( index, cut.GetValue().World.Scene, cut.GetValue().Descriptors, Registries() );
        if ( !stale )
            return Common::MakeError<HLODStatus>( stale.GetError() );
        status.Stale = stale.ExtractValue();
        return Common::MakeSuccess( std::move( status ) );
    }

    Common::ResultStr<std::size_t> DeleteHLODs( const std::filesystem::path& scenePath )
    {
        namespace fs              = std::filesystem;
        const fs::path        dir = WC::BuiltHLODDirectory( scenePath );
        std::error_code       ec;
        std::vector<fs::path> saved;
        if ( !fs::exists( dir, ec ) )
            return Common::MakeSuccess( std::size_t{ 0 } );
        for ( const fs::directory_entry& entry : fs::recursive_directory_iterator( dir, ec ) )
            if ( entry.is_regular_file() )
                saved.push_back( entry.path() );
        if ( ec )
            return Common::MakeError<std::size_t>( "Delete HLODs: cannot list '" + dir.string() +
                                                   "': " + ec.message() );
        // The index first: a delete cut short leaves no index naming files that are gone.
        std::ranges::stable_partition( saved, []( const fs::path& file )
                                       { return file.filename().string() == WC::kBuiltHLODIndexName; } );
        for ( const fs::path& file : saved )
        {
            if ( !fs::remove( file, ec ) && ec )
                return Common::MakeError<std::size_t>( "Delete HLODs: cannot remove '" + file.string() +
                                                       "': " + ec.message() );
            // A baked landscape texture or material had a registry row: the file is gone, so is the row.
            ::Desert::Assets::ContentRegistry::Update( file );
        }
        fs::remove_all( dir, ec );
        if ( ec )
            return Common::MakeError<std::size_t>( "Delete HLODs: cannot remove '" + dir.string() +
                                                   "': " + ec.message() );
        LOG_INFO( "[Delete HLODs] '{0}': {1} file(s) removed from {2}", scenePath.stem().string(), saved.size(),
                  dir.string() );
        return Common::MakeSuccess( saved.size() );
    }
} // namespace Desert::Editor
