#include "WorldPartitionBuilder.hpp"

#include <Editor/Core/BuilderRunOptions.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/ShotOptions.hpp>
#include <Editor/LevelEditor/SceneFiles.hpp>
#include <Editor/LevelEditor/SceneWorkspace.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/EntityDescriptorIndex.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <system_error>

namespace Desert::Editor
{
    namespace
    {
        constexpr std::string_view LoadingModeName( BuilderLoadingMode mode )
        {
            switch ( mode )
            {
                case BuilderLoadingMode::Custom:
                    return "custom loading";
                case BuilderLoadingMode::EntireWorld:
                    return "the entire world";
                case BuilderLoadingMode::IterativeCells2D:
                    return "iterative 2D cells";
            }
            return "an unknown loading mode";
        }

        std::string BuilderList()
        {
            std::string list;
            for ( const WorldPartitionBuilderRow& row : WorldPartitionBuilderRows() )
                list += std::format( "\n  {} - {}", row.Name, row.Summary );
            return list;
        }

        // The editor's load (EditorRegions::LoadHeld) of what a builder step holds. An entity that differs from its
        // file and would leave is refused, not dropped: UE saves the dirty packages before a builder runs, so a
        // builder never unloads an unsaved edit - the user saves (or discards) and runs it again.
        Common::ResultStr<::Desert::Core::EditorRegions::RegionOutcome>
        HoldForBuilder( ::Desert::Core::Scene& scene, Assets::AssetManager* assets,
                        const ::Desert::Core::EditorRegions::HeldRegions& held )
        {
            auto loaded = ::Desert::Core::EditorRegions::LoadHeld(
                 scene, assets, held, ::Desert::Core::EditorRegions::UnsavedPolicy::Ask );
            if ( !loaded || loaded.GetValue().Unsaved.empty() )
                return loaded;
            std::string names;
            for ( const ::Desert::Core::EditorRegions::UnsavedEntity& entity : loaded.GetValue().Unsaved )
                names += std::format( "{}{}", names.empty() ? "" : ", ", entity.Name );
            return Common::MakeError<::Desert::Core::EditorRegions::RegionOutcome>( std::format(
                 "{} entit{} differ from their files and would be unloaded ({}): save the world first",
                 loaded.GetValue().Unsaved.size(), loaded.GetValue().Unsaved.size() == 1 ? "y" : "ies", names ) );
        }
    } // namespace

    Common::BoolResultStr IWorldPartitionBuilder::PreRun( BuilderContext& /*context*/ )
    {
        return BOOLSUCCESS;
    }

    Common::ResultStr<BuilderStep> IWorldPartitionBuilder::Continue( BuilderContext& /*context*/ )
    {
        return Common::MakeError<BuilderStep>(
             "the builder answered Pending and has no continuation: its step can never finish" );
    }

    Common::BoolResultStr IWorldPartitionBuilder::PostRun( BuilderContext& /*context*/, bool /*succeeded*/ )
    {
        return BOOLSUCCESS;
    }

    const WorldPartitionBuilderRow* FindWorldPartitionBuilder( std::string_view name )
    {
        for ( const WorldPartitionBuilderRow& row : WorldPartitionBuilderRows() )
            if ( row.Name == name )
                return &row;
        return nullptr;
    }

    Common::BoolResultStr ValidateBuilderRequest( const BuilderRunOptions& options )
    {
        const WorldPartitionBuilderRow* row = FindWorldPartitionBuilder( options.Builder );
        if ( row == nullptr )
            return Common::MakeError( std::format( "--builder '{}' is not a World Partition builder; the builders are:{}",
                                                   options.Builder, BuilderList() ) );
        if ( options.IterativeCellSize > 0.0f && !row->CellsAllowed )
            return Common::MakeError(
                 std::format( "--builder-cell-size: the {} builder runs over {} and cannot be run cell by cell",
                              row->Name, LoadingModeName( row->Mode ) ) );
        if ( options.OnlyDirty && !row->OnlyDirtyAllowed )
            return Common::MakeError(
                 std::format( "--only-dirty means nothing to the {} builder (it is ResaveEntities')", row->Name ) );
        return BOOLSUCCESS;
    }

    std::vector<::Desert::Core::Rules::CellBounds>
    IterativeCells2D( const ::Desert::Core::Rules::CellBounds& world, float cellSize )
    {
        std::vector<::Desert::Core::Rules::CellBounds> cells;
        if ( !( cellSize > 0.0f ) || world.MaxX < world.MinX || world.MaxZ < world.MinZ )
            return cells;
        const auto firstX = static_cast<long long>( std::floor( world.MinX / cellSize ) );
        const auto lastX  = static_cast<long long>( std::floor( world.MaxX / cellSize ) );
        const auto firstZ = static_cast<long long>( std::floor( world.MinZ / cellSize ) );
        const auto lastZ  = static_cast<long long>( std::floor( world.MaxZ / cellSize ) );
        for ( long long z = firstZ; z <= lastZ; ++z )
            for ( long long x = firstX; x <= lastX; ++x )
            {
                const float minX = static_cast<float>( x ) * cellSize;
                const float minZ = static_cast<float>( z ) * cellSize;
                cells.push_back(
                     ::Desert::Core::Rules::CellBounds{ minX, minZ, minX + cellSize, minZ + cellSize } );
            }
        return cells;
    }

    WorldPartitionBuilderRun::WorldPartitionBuilderRun( SceneWorkspace& workspace, SceneFiles& files,
                                                        std::shared_ptr<Assets::AssetManager>& assets )
         : m_Workspace( workspace ), m_Files( files ), m_Assets( assets )
    {
    }

    WorldPartitionBuilderRun::~WorldPartitionBuilderRun() = default;

    std::optional<int> WorldPartitionBuilderRun::Tick( bool sceneSettled )
    {
        if ( !BuilderRunOptions::Get().Active() || m_Finished )
            return std::nullopt;
        if ( !m_Started )
        {
            if ( !sceneSettled )
                return std::nullopt;
            m_Started   = true;
            m_StartedAt = std::chrono::steady_clock::now();
            if ( auto begun = Begin(); !begun )
                return Finish( std::move( begun ) );
            return std::nullopt;
        }
        auto step = Step();
        if ( !step )
            return Finish( Common::MakeError( step.GetError() ) );
        if ( step.GetValue() )
            return Finish( Common::MakeSuccess( true ) );
        return std::nullopt;
    }

    Common::BoolResultStr WorldPartitionBuilderRun::Begin()
    {
        const BuilderRunOptions& options = BuilderRunOptions::Get();
        m_Row                            = FindWorldPartitionBuilder( options.Builder );
        if ( m_Row == nullptr )
            return Common::MakeError( std::format( "--builder '{}' is not a World Partition builder", options.Builder ) );
        const std::shared_ptr<::Desert::Core::Scene> scene = m_Workspace.ActiveScene();
        if ( !scene )
            return Common::MakeError( "no scene is open" );
        // The scene the command line named, and no other: a refused load leaves the project's default (or the
        // Basic template) open, and a builder run on it would build and save a world nobody asked for.
        // Resolved as ShotDirector::QueueScene resolves it (a relative --scene is project content), and compared as
        // FILES: a name alone would take the project's default scene for a missing `--scene Sub/<same name>`.
        const std::filesystem::path open = m_Files.OpenScenePath();
        const std::filesystem::path requested =
             Common::Constants::Path::ProjectDir() / std::filesystem::path( ShotOptions::Get().Scene );
        std::error_code sameFileError;
        if ( open.empty() || !std::filesystem::equivalent( open, requested, sameFileError ) )
            return Common::MakeError( std::format( "--scene '{}' did not open (the editor holds '{}'); the log above "
                                                   "says why, and the builder runs on nothing else",
                                                   requested.string(), open.string() ) );

        m_Builder = m_Row->Make();
        m_Context = BuilderContext{ scene, m_Assets.get(), open, &options,
                                    [this]() -> Common::BoolResultStr
                                    {
                                        if ( !m_Files.SaveOpenScene() )
                                            return Common::MakeError(
                                                 "the scene was not saved; the log line above says why" );
                                        return BOOLSUCCESS;
                                    } };

        // Every data layer of the world is loaded for a builder (it builds the world, not one person's view of it).
        const auto& partition = scene->GetWorldPartition();
        if ( partition && partition->DataLayers )
            for ( const ::Desert::Core::DataLayerSerialized& layer : *partition->DataLayers )
                m_Layers =
                     ::Desert::Core::EditorRegions::WithDataLayerLoaded( *partition, m_Layers, layer.Name, true );
        const bool fromFiles = partition.has_value() && scene->Packages()->BaselinePath().has_value();

        const BuilderLoadingMode mode =
             options.IterativeCellSize > 0.0f ? BuilderLoadingMode::IterativeCells2D : m_Row->Mode;
        m_Cells.clear();
        if ( mode == BuilderLoadingMode::IterativeCells2D )
        {
            if ( !fromFiles )
                return Common::MakeError( std::format( "'{}' is not a partitioned world opened from its files; only "
                                                       "such a world is loaded cell by cell",
                                                       open.string() ) );
            auto index = ::Desert::Core::DescriptorIndex::Refresh( open );
            if ( !index )
                return Common::MakeError( "the world's descriptor index: " + index.GetError() );
            const std::vector<::Desert::Core::Rules::EntityDescriptor> descriptors =
                 ::Desert::Core::DescriptorIndex::Descriptors( index.GetValue().Index );
            const ::Desert::Core::Rules::WorldPartitionPlan plan = ::Desert::Core::Rules::PlanWorldPartition(
                 std::span<const ::Desert::Core::Rules::EntityDescriptor>( descriptors ), *partition,
                 ::Desert::Core::RegistryMeshBounds() );
            std::optional<::Desert::Core::Rules::CellBounds> world;
            for ( const ::Desert::Core::Rules::PlannedComposite& composite : plan.Composites )
            {
                if ( !composite.Footprint )
                    continue;
                const ::Desert::Core::Rules::CellBounds& foot = *composite.Footprint;
                if ( !world )
                    world = foot;
                world->MinX = std::min( world->MinX, foot.MinX );
                world->MinZ = std::min( world->MinZ, foot.MinZ );
                world->MaxX = std::max( world->MaxX, foot.MaxX );
                world->MaxZ = std::max( world->MaxZ, foot.MaxZ );
            }
            if ( !world )
                return Common::MakeError( "the world places nothing on the ground: there is no cell to load (run the "
                                          "builder without --builder-cell-size)" );
            const std::vector<::Desert::Core::Rules::CellBounds> squares =
                 IterativeCells2D( *world, options.IterativeCellSize );
            for ( std::size_t i = 0; i < squares.size(); ++i )
                m_Cells.push_back( BuilderCell{ squares[i], i, squares.size(), false } );
        }
        else
        {
            if ( mode == BuilderLoadingMode::EntireWorld && fromFiles )
            {
                auto loaded = HoldForBuilder(
                     *scene, m_Assets.get(),
                     ::Desert::Core::EditorRegions::HeldRegions{ .Whole = true, .DataLayers = m_Layers } );
                if ( !loaded )
                    return Common::MakeError( "loading the entire world: " + loaded.GetError() );
                if ( loaded.GetValue().Loaded > 0 || loaded.GetValue().Unloaded > 0 )
                    CommandHistory::Get().Clear();
                LOG_INFO( "[WorldPartitionBuilder] the entire world is loaded: {} record(s) read, {} left on disk",
                          loaded.GetValue().Loaded, loaded.GetValue().NotLoaded );
            }
            m_Cells.push_back( BuilderCell{} );
        }

        LOG_INFO( "[WorldPartitionBuilder] {} on '{}': {}, {} cell(s)", m_Row->Name, open.string(),
                  LoadingModeName( mode ), m_Cells.size() );
        return m_Builder->PreRun( m_Context );
    }

    Common::ResultStr<bool> WorldPartitionBuilderRun::Step()
    {
        if ( m_Next >= m_Cells.size() )
            return Common::MakeSuccess( true );
        const BuilderCell& cell = m_Cells[m_Next];
        Common::ResultStr<BuilderStep> step = Common::MakeSuccess( BuilderStep::Done );
        if ( m_Pending )
            step = m_Builder->Continue( m_Context );
        else
        {
            if ( !cell.Whole )
            {
                auto loaded = HoldForBuilder(
                     *m_Context.Scene, m_Assets.get(),
                     ::Desert::Core::EditorRegions::HeldRegions{
                          .Regions    = { ::Desert::Core::EditorRegions::RegionLoader{ cell.Bounds } },
                          .DataLayers = m_Layers } );
                if ( !loaded )
                    return Common::MakeError<bool>(
                         std::format( "cell {}/{}: loading it: {}", cell.Index + 1, cell.Count, loaded.GetError() ) );
                if ( loaded.GetValue().Loaded > 0 || loaded.GetValue().Unloaded > 0 )
                    CommandHistory::Get().Clear();
                LOG_INFO( "[WorldPartitionBuilder] cell {}/{} ({:.0f},{:.0f})-({:.0f},{:.0f}) cm: {} record(s) "
                          "loaded, {} unloaded",
                          cell.Index + 1, cell.Count, cell.Bounds.MinX, cell.Bounds.MinZ, cell.Bounds.MaxX,
                          cell.Bounds.MaxZ, loaded.GetValue().Loaded, loaded.GetValue().Unloaded );
            }
            step = m_Builder->Run( m_Context, cell );
        }
        if ( !step )
            return Common::MakeError<bool>(
                 std::format( "cell {}/{}: {}", cell.Index + 1, cell.Count, step.GetError() ) );
        m_Pending = step.GetValue() == BuilderStep::Pending;
        if ( m_Pending )
            return Common::MakeSuccess( false );
        ++m_Next;
        return Common::MakeSuccess( m_Next >= m_Cells.size() );
    }

    int WorldPartitionBuilderRun::Finish( Common::BoolResultStr outcome )
    {
        m_Finished = true;
        if ( m_Builder )
        {
            auto post = m_Builder->PostRun( m_Context, static_cast<bool>( outcome ) );
            if ( !post && outcome )
                outcome = Common::MakeError( "PostRun: " + post.GetError() );
            // The builder's resources (a landscape bake's renderer) go now, while the device they were made on
            // is certainly alive, not with the editor layer at exit.
            m_Builder.reset();
        }
        const double seconds =
             std::chrono::duration<double>( std::chrono::steady_clock::now() - m_StartedAt ).count();
        const std::string_view name = m_Row ? m_Row->Name : std::string_view( BuilderRunOptions::Get().Builder );
        if ( !outcome )
        {
            LOG_ERROR( "[WorldPartitionBuilder] {}: FAILED after {:.1f} s: {}", name, seconds, outcome.GetError() );
            return 1;
        }
        LOG_INFO( "[WorldPartitionBuilder] {}: succeeded in {:.1f} s ({} cell(s))", name, seconds, m_Cells.size() );
        return 0;
    }
} // namespace Desert::Editor
