// Ported from UE 5.5 (pattern)
// Engine/Source/Editor/WorldPartitionEditor/Private/WorldPartition/WorldPartitionHLODsBuilder.cpp (SetupHLODActors
// / BuildHLODActors / DeleteHLODActors: an HLOD actor whose hash is unchanged is skipped unless -ForceBuild) and
// Engine/Source/Editor/WorldPartitionEditor/Private/WorldPartition/HLOD/Utilities/WorldPartitionHLODUtilities.cpp
// (ComputeHLODHash: the input actors, the assets they use, the HLOD layer's settings and the builder's base key),
// adapted: the HLOD actors are rows of BuiltHLODIndex saved beside the world (WorldCells.hpp), the hash is
// WorldCells::SavedHLODStatus, and the landscape BaseColor bake runs over editor frames (LandscapeHLODBake).

#include <Engine/Core/Serialize/PrefabExpansion.hpp>
#include "WorldPartitionHLODBuilder.hpp"

#include <Landscape/Public/LandscapeSplineHLODMesh.hpp>

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Core/HLODApproximate.hpp>
#include <Engine/Core/HLODMeshBuilder.hpp>
#include <Engine/Core/Serialize/EntityDescriptorIndex.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/StoredAssetForm.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <set>
#include <span>
#include <system_error>
#include <utility>
#include <Common/Json/ReadWrite.hpp>

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

        // THE HLOD MESH IS A STATIC MESH ASSET BESIDE THE CELLS (WP-PERF; UE: an HLOD actor's component draws the
        // UStaticMesh its builder saved, never geometry inlined in the actor). Until WP-PERF the built EditMesh
        // rode inside the cell as JSON numbers: a landscape tile's cell was 80-85 MB and took ~70 s to read and
        // rebuild at the start of Play (measured 11-18 on WP_Protocol). The record now names
        // `HLODMesh_<hash>.stmesh`, the binary source asset an import writes, whose render form the loader derives
        // through the DDC like any static mesh's. Named by the hash of what it stores, so a rebuilt cell whose
        // mesh did not change writes nothing and keeps the file's GUID. Its slots are unassigned: the record's
        // MaterialSlots bind the materials (UE OverrideMaterials), so the asset is pure geometry and Plan can
        // store it before the bakes. One render LOD - the HLOD is itself the far level of what it stands for. A
        // new file's render form is derived here, into the DDC, so a mesh the loader could not derive is refused
        // by name instead of becoming a file that draws nothing, and the first Play reads it instead of building
        // it.
        Common::ResultStr<::Desert::Assets::StaticMeshComponentSer>
        StoreHLODMesh( ::Desert::Assets::StaticMeshComponentSer block, const std::filesystem::path& dir )
        {
            using Block = ::Desert::Assets::StaticMeshComponentSer;
            if ( !block.EditMesh )
                return Common::MakeSuccess( std::move( block ) );
            ::Desert::Assets::MeshSourceAsset asset;
            asset.Kind                      = Common::Content::ContentKind::StaticMesh;
            asset.Import.Provenance         = ::Desert::Assets::MeshSourceProvenance::Recovered;
            asset.Import.Settings.LodPolicy = ::Desert::Assets::MeshLodPolicy::None;
            int slots                       = 1;
            for ( const int id : block.EditMesh->MaterialIds )
                slots = std::max( slots, id + 1 );
            for ( int k = 0; k < slots; ++k )
                asset.Source.MaterialSlots.push_back(
                     ::Desert::Assets::MeshMaterialSlot{ std::format( "Slot{}", k ), {} } );
            asset.Source.Models.push_back( ::Desert::Assets::MeshSourceModel{ std::move( *block.EditMesh ) } );
            block.EditMesh.reset();
            const std::filesystem::path file =
                 dir / std::format( "HLODMesh_{:016x}.stmesh", ::Desert::Assets::MeshSourceHash( asset.Source ) );
            asset.Name = file.stem().string();
            std::error_code ec;
            if ( std::filesystem::exists( file, ec ) )
            {
                auto header =
                     Common::Content::ReadAssetHeader( file, ::Desert::Assets::MeshAssetHeaderReadContext() );
                if ( !header )
                    return Common::MakeError<Block>(
                         std::format( "HLOD mesh '{}': {}", file.generic_string(), header.GetError() ) );
                asset.Guid = header.GetValue().Guid;
                ::Desert::Assets::ContentRegistry::NoteFile( file );
            }
            else
            {
                asset.Guid = Common::Content::AssetGuid::Generate();
                std::filesystem::create_directories( dir, ec );
                if ( auto written = ::Desert::Assets::WriteMeshSourceAssetFile( file, asset ); !written )
                    return Common::MakeError<Block>( std::format( "HLOD mesh '{}' could not be written: {}",
                                                                  file.generic_string(), written.GetError() ) );
                ::Desert::Assets::ContentRegistry::NoteFile( file );
                if ( auto render = ::Desert::Assets::LoadMeshPlatformData( file ); !render )
                    return Common::MakeError<Block>( std::format( "HLOD mesh '{}' has no render form: {}",
                                                                  file.generic_string(), render.GetError() ) );
            }
            const auto form = ::Desert::Core::Serialize::StoredFormFor( "StaticMeshAsset" );
            if ( !form )
                return Common::MakeError<Block>(
                     "the static mesh reference has no stored form (StoredAssetForm.cpp)" );
            block.MeshHandle =
                 ::Desert::Assets::AssetGuidRef{ Common::Content::AssetGuidToText( asset.Guid ),
                                                 ::Desert::Core::Serialize::RenderStoredForm(
                                                      *form, Common::AssetHandle::StableKeyForPath( file ) ) };
            return Common::MakeSuccess( std::move( block ) );
        }

        // The name the Impostor layer type's builder is registered under (Rules::kBuiltInHLODBuilders), which
        // Build HLODs overrides in its copy of the registry with the builder that bakes.
        [[nodiscard]] std::string ImpostorBuilderName()
        {
            ::Desert::Core::HLODLayerSerialized layer;
            layer.Type = ::Desert::Core::HLODLayerType::Impostor;
            return std::string( ::Desert::Core::Rules::HLODBuilderNameOf( layer ) );
        }

        // The ISM block that draws an impostor: its card mesh stored as `HLODMesh_<hash>.stmesh` beside the cells
        // (StoreHLODMesh, as every built HLOD mesh) and its one M_I<key> material.
        [[nodiscard]] Common::ResultStr<::Desert::Assets::InstancedStaticMeshComponentSer>
        StoreImpostorCard( const HLODApproximateBaked& baked, const std::filesystem::path& dir )
        {
            using Card = ::Desert::Assets::InstancedStaticMeshComponentSer;
            if ( !baked.Card )
                return Common::MakeError<Card>(
                     std::format( "impostor I{} was baked without its card", baked.Key ) );
            ::Desert::Assets::StaticMeshComponentSer block;
            block.EditMesh = *baked.Card;
            auto stored    = StoreHLODMesh( std::move( block ), dir );
            if ( !stored )
                return Common::MakeError<Card>( stored.GetError() );
            if ( !stored.GetValue().MeshHandle )
                return Common::MakeError<Card>(
                     std::format( "impostor I{}'s card was stored without a handle", baked.Key ) );
            Card card;
            card.MeshGuid      = stored.GetValue().MeshHandle->Guid;
            card.MeshPath      = stored.GetValue().MeshHandle->Path;
            card.MaterialGuids = std::vector<std::string>{ baked.MaterialGuid };
            card.MaterialPaths = std::vector<std::string>{ baked.MaterialPath };
            return Common::MakeSuccess( std::move( card ) );
        }

        // A landscape HLOD's geometry is a function of its tile and of the distance it is first drawn from
        // (BuildLandscapeHLODMesh sizes it by MinVisibleDistance): the key Plan stores it under and Finish asks
        // by.
        std::optional<std::pair<std::uint64_t, float>>
        LandscapeShellKey( const ::Desert::Core::Rules::LandscapeHLODRequest& request )
        {
            if ( request.Tile >= request.Records.size() || !request.Records[request.Tile].id )
                return std::nullopt;
            return std::pair{ static_cast<std::uint64_t>( *request.Records[request.Tile].id ),
                              request.MinVisibleDistance };
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

    Common::ResultStr<::Desert::Assets::InstancedStaticMeshComponentSer>
    StoreBakedImpostorCard( const HLODApproximateBaked& baked, const std::filesystem::path& dir )
    {
        return StoreImpostorCard( baked, dir );
    }

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
        const std::filesystem::path                 dir = WC::BuiltHLODDirectory( m_ScenePath );
        m_LandscapeShells.clear();
        // Each tile's mesh is stored the moment it is built (StoreHLODMesh) and kept by its key for Finish.
        ::Desert::Core::Rules::LandscapeHLODBuilder collect =
             [&]( const auto& request ) -> Common::ResultStr<::Desert::Assets::StaticMeshComponentSer>
        {
            const auto key = LandscapeShellKey( request );
            if ( const auto shell = key ? m_LandscapeShells.find( *key ) : m_LandscapeShells.end();
                 shell != m_LandscapeShells.end() )
                return Common::MakeSuccess( shell->second );
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
            if ( !known )
            {
                // The bake states the normal map in the tangent frame of this very mesh.
                if ( !mesh.GetValue().EditMesh )
                    return Common::MakeError<::Desert::Assets::StaticMeshComponentSer>(
                         std::format( "the landscape HLOD of tile {} carries no mesh",
                                      static_cast<uint64_t>( area.GetValue().Tile ) ) );
                areas.push_back( LandscapeHLODBakeTile{ area.GetValue(), *mesh.GetValue().EditMesh } );
            }
            auto stored = StoreHLODMesh( mesh.ExtractValue(), dir );
            if ( stored && key )
                m_LandscapeShells.emplace( *key, stored.GetValue() );
            return stored;
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
        // Every Impostor layer's mesh (IMPOSTORS-HLOD, UE ImpostorBaker), queued once by its key for the capture
        // as ONE mesh at identity: the atlas and the card are mesh-local, every instance draws the same bake. The
        // probe's files are dropped, so its batch is answered as it came; Finish binds the baked card.
        const ::Desert::Core::HLODImpostorBaker collectImpostor =
             [&]( const ::Desert::Core::HLODImpostorRequest& request )
             -> Common::ResultStr<::Desert::Assets::InstancedStaticMeshComponentSer>
        {
            const std::string key   = ::Desert::Core::HLODImpostorKey( request.Part, request.Settings );
            const bool        known = std::any_of( approximations.begin(), approximations.end(),
                                                   [&]( const HLODApproximateBakeCell& cell )
                                                   { return cell.Impostor.has_value() && cell.Key == key; } );
            if ( !known )
            {
                HLODApproximateBakeCell cell;
                cell.Key                                               = key;
                cell.Impostor                                          = request.Settings;
                ::Desert::Assets::InstancedStaticMeshComponentSer part = request.Part;
                std::array<float, 16>                             identity{};
                identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
                part.InstanceTransforms = std::vector<std::array<float, 16>>{ identity };
                cell.Parts.push_back( std::move( part ) );
                approximations.push_back( std::move( cell ) );
            }
            return Common::MakeSuccess( request.Part );
        };
        if ( auto overridden =
                  builders.Override( ImpostorBuilderName(),
                                     std::make_shared<::Desert::Core::HLODBuilderImpostor>( collectImpostor ) );
             !overridden )
            return Common::MakeError( "Build HLODs: " + overridden.GetError() );
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
        const std::filesystem::path                dir        = WC::BuiltHLODDirectory( m_ScenePath );
        // Each landscape HLOD bound to the material its tile's bake wrote (UE ULandscapeHLODBuilder). The mesh is
        // the one Plan built and stored (m_LandscapeShells), built here only for a tile Plan did not see.
        ::Desert::Core::Rules::LandscapeHLODBuilder landscape =
             [&]( const auto& request ) -> Common::ResultStr<::Desert::Assets::StaticMeshComponentSer>
        {
            ::Desert::Assets::StaticMeshComponentSer block;
            const auto                               key = LandscapeShellKey( request );
            if ( const auto shell = key ? m_LandscapeShells.find( *key ) : m_LandscapeShells.end();
                 shell != m_LandscapeShells.end() )
                block = shell->second;
            else
            {
                auto mesh = ::Desert::Core::BuildLandscapeHLODMesh( request );
                if ( !mesh )
                    return mesh;
                auto stored = StoreHLODMesh( mesh.ExtractValue(), dir );
                if ( !stored )
                    return stored;
                block = stored.ExtractValue();
            }
            const auto& id = request.Records[request.Tile].id;
            for ( const LandscapeHLODBaked& tile : baked )
                if ( id && static_cast<uint64_t>( tile.Tile ) == static_cast<uint64_t>( *id ) )
                {
                    block.MaterialSlots = std::vector<::Desert::Assets::AssetGuidRef>{
                         ::Desert::Assets::AssetGuidRef{ tile.MaterialGuid, tile.MaterialPath } };
                    break;
                }
            return Common::MakeSuccess( std::move( block ) );
        };
        builders.SetLandscapeBuilder( std::make_shared<::Desert::Core::Rules::HLODBuilderLandscape>( landscape ) );
        // Each merged-material mesh HLOD is the very mesh its bake was made for, bound to the material the bake
        // wrote (UE FMeshMergingSettings::bMergeMaterials). Each approximated cell's shell - Plan's, by its key
        // (wrapped again only if Start did not see it) - bound to the one material its capture wrote. One that was
        // not baked is refused by name: drawn without its material it would stand in grey.
        const auto geometry = [&]( const ::Desert::Core::Rules::HLODMeshRequest& request )
             -> Common::ResultStr<::Desert::Assets::StaticMeshComponentSer>
        {
            using Block = ::Desert::Assets::StaticMeshComponentSer;
            if ( request.Approximation.has_value() )
            {
                const std::string key = ::Desert::Core::HLODApproximationKey( request );
                const auto        found = std::find_if( approximated.begin(), approximated.end(),
                                                        [&]( const HLODApproximateBaked& cell )
                                                        { return !cell.Card.has_value() && cell.Key == key; } );
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
        // Every built mesh - merged, simplified, approximated, a landscape spline's - is stored as its asset.
        const ::Desert::Core::Rules::HLODMeshBuilder mesh =
             [&]( const ::Desert::Core::Rules::HLODMeshRequest& request )
             -> Common::ResultStr<::Desert::Assets::StaticMeshComponentSer>
        {
            auto built = geometry( request );
            if ( !built )
                return built;
            return StoreHLODMesh( built.ExtractValue(), dir );
        };
        builders.SetMeshGeometry( mesh );
        // Each impostor batch draws the card and material its capture wrote (by its key). One that was not baked
        // is refused by name: drawn as the source mesh it would silently cost what the impostor saves.
        const ::Desert::Core::HLODImpostorBaker impostor =
             [&]( const ::Desert::Core::HLODImpostorRequest& request )
             -> Common::ResultStr<::Desert::Assets::InstancedStaticMeshComponentSer>
        {
            const std::string key = ::Desert::Core::HLODImpostorKey( request.Part, request.Settings );
            const auto        found =
                 std::find_if( approximated.begin(), approximated.end(), [&]( const HLODApproximateBaked& cell )
                               { return cell.Card.has_value() && cell.Key == key; } );
            if ( found == approximated.end() )
                return Common::MakeError<::Desert::Assets::InstancedStaticMeshComponentSer>(
                     std::format( "impostor I{} was not baked (the saved world changed during the build?); run "
                                  "Build HLODs again",
                                  key ) );
            return StoreImpostorCard( *found, dir );
        };
        if ( auto overridden = builders.Override(
                  ImpostorBuilderName(), std::make_shared<::Desert::Core::HLODBuilderImpostor>( impostor ) );
             !overridden )
            return Common::MakeError( "Build HLODs: " + overridden.GetError() );
        const ::Desert::Core::Rules::LandscapeSplineHLODGeometry splines =
             [&]( const ::Desert::Core::Rules::LandscapeSplineHLODRequest& request,
                  std::span<const Common::Utils::AssetRegistry>            from )
             -> Common::ResultStr<std::optional<::Desert::Assets::StaticMeshComponentSer>>
        {
            using Built = std::optional<::Desert::Assets::StaticMeshComponentSer>;
            auto built  = ::Desert::Landscape::BuildLandscapeSplineHLODMesh( request, from );
            if ( !built || !built.GetValue().has_value() )
                return built;
            Built spline = built.ExtractValue();
            auto  stored = StoreHLODMesh( std::move( *spline ), dir );
            if ( !stored )
                return Common::MakeError<Built>( stored.GetError() );
            return Common::MakeSuccess( Built( stored.ExtractValue() ) );
        };
        builders.SetLandscapeSplineBuilder(
             std::make_shared<::Desert::Core::Rules::HLODBuilderLandscapeSplines>( splines ) );
        // Each cell's file is written the moment it is built and freed with it (UE builds and saves its HLOD
        // actors cell by cell), so the world's HLODs are never in memory at once. A kept row's file stays.
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
        std::size_t           records = 0;
        std::set<std::string> meshes; // every HLOD mesh a saved cell names (MeshHandle.Path)
        for ( const WC::BuiltHLODCell& cell : stored.GetValue().Index.Cells )
        {
            auto cellRecords = WC::StoredCellRecords( stored.GetValue(), cell );
            if ( !cellRecords )
                return Common::MakeError( "Build HLODs: read back: " + cellRecords.GetError() );
            records += cellRecords.GetValue().size();
            for ( const ::Desert::Assets::EntityData& record : cellRecords.GetValue() )
                for ( const auto& [component, value] : record.Components )
                {
                    if ( component != "StaticMesh" )
                        continue;
                    auto block = Common::Json::Read<::Desert::Assets::StaticMeshComponentSer>(
                         Common::Json::Write( value ) );
                    if ( !block )
                        return Common::MakeError(
                             std::format( "Build HLODs: read back {}: {}", cell.Unit, block.GetError() ) );
                    if ( block.GetValue().MeshHandle )
                        meshes.insert( block.GetValue().MeshHandle->Path );
                }
        }
        // A mesh no saved cell names any more is the old geometry of a rebuilt cell: deleted with its registry row
        // (UE deletes the HLOD packages a build no longer produces), so the folder holds what the index draws.
        std::size_t removed = 0;
        {
            const auto form = ::Desert::Core::Serialize::StoredFormFor( "StaticMeshAsset" );
            if ( !form )
                return Common::MakeError( "the static mesh reference has no stored form (StoredAssetForm.cpp)" );
            std::error_code ec;
            for ( const auto& entry : std::filesystem::directory_iterator( dir, ec ) )
            {
                const std::filesystem::path& file = entry.path();
                if ( file.extension() != ".stmesh" || !file.stem().string().starts_with( "HLODMesh_" ) ||
                     meshes.contains( ::Desert::Core::Serialize::RenderStoredForm(
                          *form, Common::AssetHandle::StableKeyForPath( file ) ) ) )
                    continue;
                std::error_code removeError;
                if ( !std::filesystem::remove( file, removeError ) )
                    return Common::MakeError(
                         std::format( "Build HLODs: the unused HLOD mesh '{}' was not removed: {}",
                                      file.generic_string(), removeError.message() ) );
                ::Desert::Assets::ContentRegistry::Update( file );
                ++removed;
            }
        }
        // Against the world the build cut: prefab instances expanded as the cook expands them (WP21).
        auto cut = WC::ExpandWorldForCook( world.Loadable.Scene, world.Descriptors, WC::PrefabFileReader() );
        if ( !cut )
            return Common::MakeError( "Build HLODs: read back: " + cut.GetError() );
        auto readBack = WC::SavedHLODStatus( cut.GetValue().World.Scene, cut.GetValue().Descriptors,
                                             &stored.GetValue().Index, registries, builders );
        if ( !readBack )
            return Common::MakeError( "Build HLODs: read back: " + readBack.GetError() );
        const std::vector<std::string> outdated = readBack.GetValue().OutdatedNames();
        for ( const std::string& line : outdated )
            LOG_ERROR( "[Build HLODs] the HLOD read back is outdated: {0}", line );
        for ( const std::string& unit : result.Built )
            LOG_INFO( "[Build HLODs] built {0}", unit );
        LOG_INFO( "[Build HLODs] '{0}': {1} HLOD mesh asset(s) named by the cells, {2} unused one(s) removed",
                  m_ScenePath.stem().string(), meshes.size(), removed );
        LOG_INFO(
             "[Build HLODs] '{0}': built {1} of {2} HLOD cells ({3}); {4} file(s), {5} bytes written, {6} HLOD "
             "record(s) in {7}; read back: {8} stale cell(s)",
             m_ScenePath.stem().string(), result.Built.size(), result.Index.Cells.size(),
             m_Options.ForceAll ? "force all" : "others up to date", files, bytes, records, dir.string(),
             outdated.size() );
        if ( !outdated.empty() )
            return Common::MakeError( std::format( "Build HLODs: {} cell(s) read back stale", outdated.size() ) );
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
        auto index = std::make_shared<WC::BuiltHLODIndex>(); // no saved index: every wanted node is missing
        if ( HasSavedIndex( scenePath ) )
        {
            auto stored = WC::ReadBuiltHLODs( scenePath );
            if ( !stored )
                return Common::MakeError<HLODStatus>( stored.GetError() );
            *index       = std::move( stored.GetValue().Index );
            status.Built = true;
        }
        // The world the build cuts and the cook checks: prefab instances expanded (WP21).
        auto cut = WC::ExpandWorldForCook( world.Loadable.Scene, world.Descriptors, WC::PrefabFileReader() );
        if ( !cut )
            return Common::MakeError<HLODStatus>( cut.GetError() );
        auto judged = WC::SavedHLODStatus( cut.GetValue().World.Scene, cut.GetValue().Descriptors,
                                           status.Built ? index.get() : nullptr, Registries(),
                                           ::Desert::Core::HLODBuilders() );
        if ( !judged )
            return Common::MakeError<HLODStatus>( judged.GetError() );
        status.World = judged.ExtractValue();
        status.Index = std::move( index );
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
