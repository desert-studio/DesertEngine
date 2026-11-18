// Ported from UE 5.5 (pattern)
// Engine/Source/Editor/WorldPartitionEditor/Private/WorldPartition/WorldPartitionHLODsBuilder.cpp (SetupHLODActors
// / BuildHLODActors / DeleteHLODActors: an HLOD actor whose hash is unchanged is skipped unless -ForceBuild) and
// Engine/Source/Editor/WorldPartitionEditor/Private/WorldPartition/HLOD/Utilities/WorldPartitionHLODUtilities.cpp
// (ComputeHLODHash: the input actors, the assets they use, the HLOD layer's settings and the builder's base key),
// adapted: the HLOD actors are rows of BuiltHLODIndex saved beside the world (WorldCells.hpp), the hash is
// WorldCells::HLODBuildChecksum, and the landscape BaseColor bake runs over editor frames (LandscapeHLODBake).

#include "WorldPartitionHLODBuilder.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Core/HLODApproximate.hpp>
#include <Engine/Core/HLODMeshBuilder.hpp>
#include <Engine/Core/Serialize/EntityDescriptorIndex.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <chrono>
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
            std::string                                          Text;
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
            SavedWorld world{ text.GetValue(), scene.ExtractValue(),
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

    // What Plan gathered for the bakes, handed to the editor's thread by StartBakes.
    struct WorldPartitionHLODBuilder::Planned
    {
        std::string                                  Text;     // the saved scene text the material bake reads
        std::optional<::Desert::Core::LoadableScene> Loadable; // the landscape bake's world
        std::optional<::Desert::Core::LoadableScene> Captured; // the approximation capture's own parse of Text
        std::vector<LandscapeHLODBakeTile>           Areas;
        std::vector<HLODMaterialBakeJob>             MeshJobs;
        std::vector<HLODApproximateBakeCell>         Approximations;
    };

    WorldPartitionHLODBuilder::~WorldPartitionHLODBuilder()
    {
        m_Cancel = true;
        if ( m_Work.valid() )
            m_Work.wait();
    }

    Common::BoolResultStr WorldPartitionHLODBuilder::Start( const Assets::AssetManager* assets )
    {
        std::error_code ec;
        if ( !std::filesystem::is_regular_file( m_ScenePath, ec ) )
            return Common::MakeError( "Build HLODs: '" + m_ScenePath.string() + "' is not a saved scene file" );
        m_Assets = assets;
        m_Stage  = Stage::Planning;
        RunOnWorker( &WorldPartitionHLODBuilder::Plan );
        LOG_INFO( "[Build HLODs] '{0}': started (reading the saved world on a worker)",
                  m_ScenePath.stem().string() );
        return BOOLSUCCESS;
    }

    void WorldPartitionHLODBuilder::RunOnWorker( Common::BoolResultStr ( WorldPartitionHLODBuilder::*phase )() )
    {
        m_CellsDone = 0;
        m_Work      = Common::JobSystem::Get().Async( [this, phase]() noexcept { return ( this->*phase )(); },
                                                 Common::JobPriority::Background );
    }

    std::string WorldPartitionHLODBuilder::Progress() const
    {
        switch ( m_Stage )
        {
            case Stage::Planning:
                return std::format( "reading the saved world: {} HLOD cell(s) checked", m_CellsDone.load() );
            case Stage::Baking:
                if ( m_Bake )
                    return "baking the landscape tiles' BaseColor";
                if ( m_MeshBake )
                    return "baking the mesh HLODs' merged materials";
                if ( m_ApproxBake )
                    return "capturing the approximated cells' textures";
                return "starting to write the HLOD cells";
            case Stage::Writing:
                return std::format( "building and writing the HLOD cells: {} written", m_CellsDone.load() );
        }
        return {};
    }

    Common::BoolResultStr WorldPartitionHLODBuilder::Plan()
    {
        auto saved = ReadSavedWorld( m_ScenePath );
        if ( !saved )
            return Common::MakeError( "Build HLODs: " + saved.GetError() );
        SavedWorld                           world      = saved.ExtractValue();
        const std::optional<WC::StoredHLODs> previous   = PreviousBuild( m_ScenePath, m_Options );
        const auto                           registries = Registries();
        // Phase 1 runs the process's builders (Core::HLODBuilders) over the rows to build, with a landscape
        // builder that also keeps each tile's area and a mesh geometry that keeps each merged-material mesh's bake
        // source.
        ::Desert::Core::Rules::HLODBuilderRegistry  builders = ::Desert::Core::HLODBuilders();
        std::vector<LandscapeHLODBakeTile>          areas;
        ::Desert::Core::Rules::LandscapeHLODBuilder collect =
             [&]( const auto& request ) -> Common::ResultStr<::Desert::Assets::StaticMeshComponentSer>
        {
            auto area = ::Desert::Core::LandscapeHLODTileAreaOf( request );
            if ( !area )
                return Common::MakeError<::Desert::Assets::StaticMeshComponentSer>( area.GetError() );
            auto mesh = ::Desert::Core::BuildLandscapeHLODMesh( request );
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
        builders.SetLandscapeBuilder( std::make_shared<::Desert::Core::Rules::HLODBuilderLandscape>( collect ) );
        // Every mesh HLOD whose builder merges materials, built once with its bake source (BuildHLODMeshForBake);
        // Finish binds the material its bake writes (WP-HLOD-MATBAKE). Every MeshApproximate cell's shell
        // (UE UHLODBuilderMeshApproximate), wrapped once and kept by its key for Finish, its cell queued for the
        // texture capture (WP-HLOD-APPROX).
        std::vector<HLODMaterialBakeJob>     meshJobs;
        std::vector<HLODApproximateBakeCell> approximations;
        m_ApproxShells.clear();
        const ::Desert::Core::Rules::HLODMeshBuilder collectMesh =
             [&]( const ::Desert::Core::Rules::HLODMeshRequest& request )
             -> Common::ResultStr<::Desert::Assets::StaticMeshComponentSer>
        {
            using Block = ::Desert::Assets::StaticMeshComponentSer;
            if ( request.Approximation.has_value() )
            {
                const std::string key = ::Desert::Core::HLODApproximationKey( request );
                if ( const auto known = m_ApproxShells.find( key ); known != m_ApproxShells.end() )
                {
                    Block copy = known->second;
                    return Common::MakeSuccess( std::move( copy ) );
                }
                auto merged = ::Desert::Core::MergeHLODParts( request, registries );
                if ( !merged )
                    return Common::MakeError<Block>( merged.GetError() );
                auto shell =
                     ::Desert::Core::BuildApproximateShell( merged.GetValue().Mesh, *request.Approximation );
                if ( !shell )
                    return Common::MakeError<Block>( "the approximation was refused: " + shell.GetError() );
                const ::Desert::Core::ApproximateShell& built = shell.GetValue();
                LOG_INFO(
                     "[Build HLODs] approximation A{0}: {1} source triangles -> {2} wrapped ({3} voxels along "
                     "the longest side, {4} inner cavities removed) -> {5} triangles within {6:.1f} cm",
                     key, built.SourceTriangles, built.WrappedTriangles, built.VoxelCount, built.CavitiesRemoved,
                     built.Triangles, built.Deviation );
                Block block;
                block.EditMesh = ::Desert::Geometry::ToSerialized( built.Mesh );
                approximations.push_back(
                     HLODApproximateBakeCell{ key, *request.Approximation,
                                              std::vector<::Desert::Assets::InstancedStaticMeshComponentSer>(
                                                   request.Parts.begin(), request.Parts.end() ),
                                              *block.EditMesh } );
                m_ApproxShells.emplace( key, block );
                return Common::MakeSuccess( std::move( block ) );
            }
            if ( !request.MergeMaterials.has_value() )
                return ::Desert::Core::BuildHLODMesh( request, registries );
            auto built = ::Desert::Core::BuildHLODMeshForBake( request, registries );
            if ( !built )
                return Common::MakeError<Block>( built.GetError() );
            // ResultStr::GetValue is const-only: the build is taken out to move its bake source into the job.
            ::Desert::Core::HLODMeshBuild build = built.ExtractValue();
            if ( !build.Bake.has_value() )
                return Common::MakeError<Block>( "a merged-material HLOD mesh came back without its bake source" );
            const std::string& key = build.Bake->Key;
            const bool known = std::any_of( meshJobs.begin(), meshJobs.end(), [&]( const HLODMaterialBakeJob& job )
                                            { return job.Source.Key == key; } );
            if ( !known )
                meshJobs.push_back( HLODMaterialBakeJob{ std::move( *build.Bake ), build.Mesh } );
            return Common::MakeSuccess( std::move( build.Mesh ) );
        };
        builders.SetMeshGeometry( collectMesh );
        // The probe only gathers what the rows to build bake: its files are dropped, so the landscape splines'
        // meshes are not built for it (no spline builder: they stay named holes in the dropped files).
        ::Desert::Core::Rules::HLODBuilderRegistry probeBuilders = builders;
        probeBuilders.SetLandscapeSplineBuilder( nullptr );
        if ( auto probe = WC::BuildWorldHLODs(
                  world.Loadable.Scene, world.Descriptors, registries, WC::PrefabFileReader(), probeBuilders,
                  previous ? &*previous : nullptr,
                  [this]( WC::CookedFile ) -> Common::BoolResultStr
                  {
                      if ( m_Cancel )
                          return Common::MakeError( "the build was cancelled" );
                      ++m_CellsDone;
                      return BOOLSUCCESS;
                  },
                  WC::HLODFileOutput::Probe );
             !probe )
            return Common::MakeError( "Build HLODs: " + probe.GetError() );
        auto plan = std::make_unique<Planned>();
        if ( !approximations.empty() )
        {
            // Its own parse of the same saved text: the landscape bake takes the first.
            auto captured = ::Desert::Core::ParseLoadableScene( m_ScenePath.string(), world.Text );
            if ( !captured )
                return Common::MakeError( "Build HLODs: " + captured.GetError() );
            plan->Captured = captured.ExtractValue();
        }
        plan->Text           = std::move( world.Text );
        plan->Loadable       = std::move( world.Loadable );
        plan->Areas          = std::move( areas );
        plan->MeshJobs       = std::move( meshJobs );
        plan->Approximations = std::move( approximations );
        m_Plan               = std::move( plan );
        return BOOLSUCCESS;
    }

    void WorldPartitionHLODBuilder::StartBakes()
    {
        const std::unique_ptr<Planned> plan = std::move( m_Plan );
        if ( !plan->Approximations.empty() )
        {
            LOG_INFO( "[Build HLODs] '{0}': capturing the textures of {1} approximated cell(s)",
                      m_ScenePath.stem().string(), plan->Approximations.size() );
            m_ApproxBake = std::make_unique<HLODApproximateBake>( m_ScenePath, std::move( *plan->Captured ),
                                                                  std::move( plan->Approximations ), m_Assets );
        }
        if ( !plan->MeshJobs.empty() )
        {
            LOG_INFO( "[Build HLODs] '{0}': baking the merged materials of {1} mesh HLOD(s)",
                      m_ScenePath.stem().string(), plan->MeshJobs.size() );
            m_MeshBake = std::make_unique<HLODMaterialBake>( m_ScenePath, plan->Text, std::move( plan->MeshJobs ),
                                                             m_Assets );
        }
        if ( !plan->Areas.empty() )
        {
            LOG_INFO( "[Build HLODs] '{0}': baking the BaseColor of {1} landscape tile(s)",
                      m_ScenePath.stem().string(), plan->Areas.size() );
            m_Bake = std::make_unique<LandscapeHLODBake>( m_ScenePath, std::move( *plan->Loadable ),
                                                          std::move( plan->Areas ), m_Assets );
        }
    }

    bool WorldPartitionHLODBuilder::Refuse( std::string error )
    {
        m_Bake.reset();
        m_MeshBake.reset();
        m_ApproxBake.reset();
        m_LandscapeBaked.clear();
        m_MeshBaked.clear();
        m_Approximated.clear();
        m_ApproxShells.clear();
        m_Done  = true;
        m_Error = std::move( error );
        return true;
    }

    bool WorldPartitionHLODBuilder::Tick()
    {
        if ( m_Done )
            return true;
        // A worker phase in flight is polled, never waited for: the editor draws on while it runs.
        if ( m_Work.valid() )
        {
            if ( m_Work.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
                return false;
            const Common::BoolResultStr result = m_Work.get();
            if ( m_Stage == Stage::Writing )
            {
                m_Done = true;
                if ( !result )
                    m_Error = result.GetError();
                m_LandscapeBaked.clear();
                m_MeshBaked.clear();
                m_Approximated.clear();
                m_ApproxShells.clear();
                return true;
            }
            if ( !result )
                return Refuse( result.GetError() );
            StartBakes();
            m_Stage = Stage::Baking;
            return false; // the bakes run from the next frame on; with none to run, Finish starts then
        }
        // The landscape bake first, then the merged materials, then the approximated cells' capture; what a
        // finished stage wrote waits in m_LandscapeBaked / m_MeshBaked / m_Approximated for Finish.
        if ( m_Bake )
        {
            if ( !m_Bake->Tick() )
                return false;
            const std::unique_ptr<LandscapeHLODBake> bake = std::move( m_Bake );
            if ( bake->Error() )
                return Refuse( "Build HLODs: the landscape bake was refused: " + *bake->Error() );
            m_LandscapeBaked = bake->Baked();
            if ( m_MeshBake || m_ApproxBake )
                return false; // the next stage bakes from the next frame on
        }
        else if ( m_MeshBake )
        {
            if ( !m_MeshBake->Tick() )
                return false;
            const std::unique_ptr<HLODMaterialBake> bake = std::move( m_MeshBake );
            if ( bake->Error() )
                return Refuse( "Build HLODs: the merged material bake was refused: " + *bake->Error() );
            m_MeshBaked = bake->Baked();
            if ( m_ApproxBake )
                return false; // the approximated cells' capture runs from the next frame on
        }
        else if ( m_ApproxBake )
        {
            if ( !m_ApproxBake->Tick() )
                return false;
            const std::unique_ptr<HLODApproximateBake> bake = std::move( m_ApproxBake );
            if ( bake->Error() )
                return Refuse( "Build HLODs: the approximation capture was refused: " + *bake->Error() );
            m_Approximated = bake->Baked();
        }
        m_Stage = Stage::Writing;
        RunOnWorker( &WorldPartitionHLODBuilder::Finish );
        return false;
    }

    Common::BoolResultStr WorldPartitionHLODBuilder::Finish()
    {
        const std::vector<LandscapeHLODBaked>&   baked        = m_LandscapeBaked;
        const std::vector<HLODMaterialBaked>&    meshBaked    = m_MeshBaked;
        const std::vector<HLODApproximateBaked>& approximated = m_Approximated;
        auto saved = ReadSavedWorld( m_ScenePath );
        if ( !saved )
            return Common::MakeError( "Build HLODs: " + saved.GetError() );
        const SavedWorld&                          world      = saved.GetValue();
        const std::optional<WC::StoredHLODs>       previous   = PreviousBuild( m_ScenePath, m_Options );
        const auto                                 registries = Registries();
        ::Desert::Core::Rules::HLODBuilderRegistry builders   = ::Desert::Core::HLODBuilders();
        // Each landscape HLOD bound to the material its tile's bake wrote (UE ULandscapeHLODBuilder). The slot
        // does not enter HLODBuildChecksum (it hashes the member records), so binding changes no staleness.
        ::Desert::Core::Rules::LandscapeHLODBuilder landscape =
             [&]( const auto& request ) -> Common::ResultStr<::Desert::Assets::StaticMeshComponentSer>
        {
            auto mesh = ::Desert::Core::BuildLandscapeHLODMesh( request );
            if ( !mesh )
                return mesh;
            const auto& id = request.Records[request.Tile].id;
            for ( const LandscapeHLODBaked& tile : baked )
                if ( id && static_cast<uint64_t>( tile.Tile ) == static_cast<uint64_t>( *id ) )
                {
                    ::Desert::Assets::StaticMeshComponentSer block = mesh.GetValue();
                    block.MaterialSlots                            = std::vector<::Desert::Assets::AssetGuidRef>{
                         ::Desert::Assets::AssetGuidRef{ tile.MaterialGuid, tile.MaterialPath } };
                    return Common::MakeSuccess( std::move( block ) );
                }
            return mesh;
        };
        builders.SetLandscapeBuilder( std::make_shared<::Desert::Core::Rules::HLODBuilderLandscape>( landscape ) );
        // Each merged-material mesh HLOD is the very mesh its bake was made for, bound to the material the bake
        // wrote (UE FMeshMergingSettings::bMergeMaterials). Each approximated cell's shell - Plan's, by its key
        // (wrapped again only if Start did not see it) - bound to the one material its capture wrote. One that was
        // not baked is refused by name: drawn without its material it would stand in grey.
        const ::Desert::Core::Rules::HLODMeshBuilder mesh =
             [&]( const ::Desert::Core::Rules::HLODMeshRequest& request )
             -> Common::ResultStr<::Desert::Assets::StaticMeshComponentSer>
        {
            using Block = ::Desert::Assets::StaticMeshComponentSer;
            if ( request.Approximation.has_value() )
            {
                const std::string key = ::Desert::Core::HLODApproximationKey( request );
                const auto        found =
                     std::find_if( approximated.begin(), approximated.end(),
                                   [&]( const HLODApproximateBaked& cell ) { return cell.Key == key; } );
                if ( found == approximated.end() )
                    return Common::MakeError<Block>( std::format(
                         "approximation A{} has no captured material (the saved world changed during the build?); "
                         "run Build HLODs again",
                         key ) );
                Block block;
                if ( const auto shell = m_ApproxShells.find( key ); shell != m_ApproxShells.end() )
                    block = shell->second;
                else
                {
                    auto rebuilt = ::Desert::Core::BuildHLODMesh( request, registries );
                    if ( !rebuilt )
                        return rebuilt;
                    block = rebuilt.ExtractValue();
                }
                block.MaterialSlots = std::vector<::Desert::Assets::AssetGuidRef>{
                     ::Desert::Assets::AssetGuidRef{ found->MaterialGuid, found->MaterialPath } };
                return Common::MakeSuccess( std::move( block ) );
            }
            if ( !request.MergeMaterials.has_value() )
                return ::Desert::Core::BuildHLODMesh( request, registries );
            const std::string key = ::Desert::Core::HLODMeshKey( request );
            for ( const HLODMaterialBaked& entry : meshBaked )
                if ( entry.Key == key )
                    return Common::MakeSuccess( entry.Mesh );
            return Common::MakeError<::Desert::Assets::StaticMeshComponentSer>( std::format(
                 "the merged material of mesh HLOD {} was not baked (the saved world changed during the build?); "
                 "run Build HLODs again",
                 key ) );
        };
        builders.SetMeshGeometry( mesh );
        // Each cell's file is written the moment it is built and freed with it (UE builds and saves its HLOD
        // actors cell by cell), so the world's HLODs are never in memory at once. A kept row's file stays.
        const std::filesystem::path dir    = WC::BuiltHLODDirectory( m_ScenePath );
        const WC::BuiltHLODSink     writer = WC::BuiltHLODWriter( m_ScenePath );
        std::size_t                 bytes  = 0;
        std::size_t                 files  = 0;
        auto built = WC::BuildWorldHLODs( world.Loadable.Scene, world.Descriptors, registries,
                                          WC::PrefabFileReader(), builders, previous ? &*previous : nullptr,
                                          [&]( WC::CookedFile file ) -> Common::BoolResultStr
                                          {
                                              if ( m_Cancel )
                                                  return Common::MakeError( "the build was cancelled" );
                                              ++m_CellsDone;
                                              bytes += file.Bytes.size();
                                              ++files;
                                              LOG_INFO( "[Build HLODs] {0}: {1} bytes",
                                                        ( dir / file.Name ).string(), file.Bytes.size() );
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
        m_Summary = std::format( "Build HLODs '{}': built {} of {} HLOD cells ({}); {} file(s), {} bytes written; "
                                 "read back: 0 stale cells",
                                 m_ScenePath.stem().string(), result.Built.size(), result.Index.Cells.size(),
                                 m_Options.ForceAll ? "force all" : "others up to date", files, bytes );
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
            for ( const WC::BuiltHLODCell& row : index.Cells )
                if ( row.File )
                    status.BuiltUnits.push_back( row.Unit );
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
