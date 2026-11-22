#include <Engine/Core/WorldStreamer.hpp>

#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/AssetRootPin.hpp>
#include <Engine/Core/Serialize/SceneDependencies.hpp>
#include <Engine/Graphic/TextureStreamingDemand.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Common/Core/Constants.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/PrefabExpansion.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/EntityReferenceEvents.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>
#include <Projects/WorldPartitionSettings.hpp>

#include <Common/Content/ContentChunks.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/ProcessMemory.hpp>

#include <Common/Json/Json.hpp>

#include <algorithm>
#include <format>
#include <set>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>
#include <Engine/ECS/CameraComponent.hpp>
#include <Engine/ECS/RelationshipComponent.hpp>
#include <Engine/World/Partition/StreamingSourceComponent.hpp>
#include <Engine/GameplayDebugger/GameplayDebugger.hpp>

namespace Desert::Core
{
    namespace
    {
        // THE MADE PART OF A UNIT STILL ACTIVATING IS NOT DRAWN (WP-ACTIVATE-SLICED; UE registers a streamed
        // level's components before ULevel::bIsVisible). Every entity a slice made — each record's entity and its
        // subtree, since a prefab instance's inner entities are no records — gets ECS::StreamingPendingComponent,
        // which ECS::IsHidden reads, and is listed in @p made so the unit's reveal clears exactly those.
        void HideMade( Scene& scene, std::span<const Common::UUID> recordIds, std::span<const std::size_t> records,
                       std::vector<std::uint32_t>& made )
        {
            entt::registry&           registry = scene.GetRegistry();
            std::vector<entt::entity> walk;
            for ( const std::size_t record : records )
            {
                // A record the loader refused by name (a prefab that did not load) has no entity to hide.
                const auto entity = scene.FindEntityByID( recordIds[record] );
                if ( !entity.has_value() )
                    continue;
                walk.push_back( entity->get().GetHandle() );
                while ( !walk.empty() )
                {
                    const entt::entity next = walk.back();
                    walk.pop_back();
                    if ( !registry.valid( next ) || registry.has<ECS::StreamingPendingComponent>( next ) )
                        continue; // a child record already reached through its parent
                    registry.emplace<ECS::StreamingPendingComponent>( next );
                    made.push_back( static_cast<std::uint32_t>( next ) );
                    if ( const auto* relationship = registry.try_get<ECS::RelationshipComponent>( next ) )
                        walk.insert( walk.end(), relationship->Children.begin(), relationship->Children.end() );
                }
            }
        }

        // The unit is whole: what HideMade hid is drawn from this frame on.
        void RevealMade( Scene& scene, std::vector<std::uint32_t>& made )
        {
            entt::registry& registry = scene.GetRegistry();
            for ( const std::uint32_t raw : made )
            {
                const auto entity = static_cast<entt::entity>( raw );
                if ( !registry.valid( entity ) || !registry.has<ECS::StreamingPendingComponent>( entity ) )
                    continue; // destroyed since it was made (a script, its parent's tree)
                registry.remove<ECS::StreamingPendingComponent>( entity );
                scene.MarkRenderStateDirty( entity, false );
            }
            made.clear();
        }

        // Where a record's "assets:" content keys resolve (GatherSceneDependencies), as the save resolves it.
        std::filesystem::path AssetsRoot()
        {
            return Common::Constants::Path::FullPath(
                 Common::Constants::Path::Dir( Common::Constants::Path::ContentDir::Assets ) );
        }

        // The ground rectangle of cell unit @p unit of @p plan (the world's, or the HLOD residency's).
        const Rules::CellBounds& UnitBounds( const Rules::WorldPartitionPlan& plan, const std::size_t unit )
        {
            return plan.Cells.at( unit - plan.AlwaysLoaded.size() ).Bounds;
        }
    } // namespace

    namespace
    {
        // THE RECORDS OF AN HLOD RESIDENCY UNIT (WP-FAR-8): its nodes' saved files, in composite order, read on
        // a worker. A node with no row stands in with no records (the residency plan gave it none).
        class StoredHLODCellSource final : public Rules::WorldCellSource
        {
        public:
            StoredHLODCellSource( std::shared_ptr<const WorldCells::StoredHLODs>             held,
                                  std::vector<std::vector<const WorldCells::BuiltHLODCell*>> unitRows )
                 : m_Held( std::move( held ) ), m_UnitRows( std::move( unitRows ) )
            {
            }

            [[nodiscard]] Common::ResultStr<std::vector<Assets::EntityData>>
            UnitRecords( std::size_t unit ) const override
            {
                using Result = std::vector<Assets::EntityData>;
                if ( unit >= m_UnitRows.size() )
                    return Common::MakeError<Result>( std::format(
                         "HLOD cell source: unit {} of {} does not exist", unit, m_UnitRows.size() ) );
                Result out;
                for ( const WorldCells::BuiltHLODCell* row : m_UnitRows[unit] )
                {
                    if ( row == nullptr )
                        continue;
                    auto records = WorldCells::StoredCellRecords( *m_Held, *row );
                    if ( !records )
                        return Common::MakeError<Result>( "the HLOD of " + row->Unit + ": " + records.GetError() );
                    Result read = records.ExtractValue();
                    out.insert( out.end(), std::make_move_iterator( read.begin() ),
                                std::make_move_iterator( read.end() ) );
                }
                return Common::MakeSuccess( std::move( out ) );
            }

        private:
            std::shared_ptr<const WorldCells::StoredHLODs>             m_Held;
            std::vector<std::vector<const WorldCells::BuiltHLODCell*>> m_UnitRows;
        };

        double MsSince( std::chrono::steady_clock::time_point started )
        {
            return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started ).count();
        }

        // A cooked world's cells - its world cells or its HLOD cells (WP-FAR-8) - holding each loaded cell's
        // region chunk while the cell is loaded (WP23): the chunk unmounts when the last loaded cell of its region
        // goes.
        class RegionChunkCellSource final : public Rules::WorldCellSource
        {
        public:
            RegionChunkCellSource( std::shared_ptr<const WorldCells::WorldIndex> index, std::string directory,
                                   RegionChunks regions, WorldCells::UnitSet set )
                 : m_Index( std::move( index ) ), m_Regions( regions ), m_Set( set ),
                   m_Cooked( *m_Index, CookedFileReader( std::move( directory ), std::move( regions ) ), set )
            {
            }

            [[nodiscard]] Common::ResultStr<std::vector<Assets::EntityData>>
            UnitRecords( std::size_t unit ) const override
            {
                return m_Cooked.UnitRecords( unit );
            }
            void HoldUnit( std::size_t unit ) const override
            {
                if ( const std::string* chunk = ChunkOf( unit ) )
                    if ( auto held = Common::Content::HoldChunkOnDemand( *chunk ); !held )
                        LOG_ERROR( "[WorldPartition] unit {}: {}", unit, held.GetError() );
            }
            void ReleaseUnit( std::size_t unit ) const override
            {
                if ( const std::string* chunk = ChunkOf( unit ) )
                    if ( auto released = Common::Content::ReleaseChunkOnDemand( *chunk ); !released )
                        LOG_ERROR( "[WorldPartition] unit {}: {}", unit, released.GetError() );
            }

        private:
            const std::string* ChunkOf( std::size_t unit ) const
            {
                const std::vector<WorldCells::IndexUnit>& units = WorldCells::UnitsOf( *m_Index, m_Set );
                if ( !m_Regions || unit >= units.size() )
                    return nullptr;
                const auto chunk = m_Regions->find( units[unit].File );
                return chunk != m_Regions->end() ? &chunk->second : nullptr;
            }

            std::shared_ptr<const WorldCells::WorldIndex> m_Index;
            RegionChunks                                  m_Regions;
            WorldCells::UnitSet                           m_Set; // the world's cells, or the HLOD cells (WP-FAR-8)
            WorldCells::CookedCellSource                  m_Cooked;
        };
    } // namespace

    WorldStreamer::WorldStreamer( Scene& scene, Assets::AssetManager& assets, std::string sceneName )
         : m_Scene( &scene ), m_Assets( &assets ), m_SceneName( std::move( sceneName ) )
    {
#if DESERT_DEV_INSTRUMENTS
        // The world's debugger reads its streaming through this streamer while it lives (UE: the gameplay
        // debugger reaches the world's UWorldPartitionSubsystem); the destructor takes it back.
        scene.GetGameplayDebugger().SetWorldStreamer( this );
#endif
    }

    Common::ResultStr<std::unique_ptr<WorldStreamer>>
    WorldStreamer::Begin( Scene& scene, Assets::AssetManager& assets, const std::string& snapshotJson,
                          const std::filesystem::path&            scenePath,
                          std::span<const Rules::StreamingSource> instruments )
    {
        using Result       = std::unique_ptr<WorldStreamer>;
        const auto started = std::chrono::steady_clock::now();
        auto       parsed  = ParseLoadableScene( "<Play snapshot>", snapshotJson );
        if ( !parsed )
            return Common::MakeError<Result>( parsed.GetError() );

        SceneSerialized source = std::move( parsed.ExtractValue().Scene );
        // ONE PREFAB EXPANSION FOR THE COOK AND PLAY (WP21-PLAY; UE: an embedded Level Instance is the same
        // actors in the cooked World Partition and in PIE). A partitioned world streams the records the cook
        // writes: every prefab instance is expanded by ExpandPrefabInstances - the cook's own step - and the live
        // instance (root + the entities PrefabFactory made under it) is replaced by those records, so the plan,
        // the stale check against the saved HLODs and every cell Play activates are the cooked world's.
        if ( source.WorldPartition.has_value() )
        {
            auto expanded = WorldCells::ExpandPrefabInstances( source, WorldCells::PrefabFileReader() );
            if ( !expanded )
                return Common::MakeError<Result>( "world streaming of '" + source.SceneName +
                                                  "': " + expanded.GetError() );
            if ( expanded.GetValue().Instances > 0 )
            {
                WorldCells::ExpandedScene       world = expanded.ExtractValue();
                std::vector<Assets::EntityData> made;
                for ( std::size_t record = 0; record < world.Slots.size(); ++record )
                {
                    const WorldCells::ExpandedSlot& slot = world.Slots[record];
                    if ( !slot.Instance )
                        continue;
                    // The root keeps the instance's id; its inner entities go with it.
                    if ( auto live = scene.FindEntityByID( *source.Entities[record].id ); live.has_value() )
                        scene.DestroyEntity( live->get() );
                    made.insert(
                         made.end(), world.Scene.Entities.begin() + static_cast<std::ptrdiff_t>( slot.First ),
                         world.Scene.Entities.begin() + static_cast<std::ptrdiff_t>( slot.First + slot.Count ) );
                }
                if ( auto instantiated =
                          SceneSerializer( &scene, &assets ).InstantiateRecords( made, source.SceneName, nullptr );
                     !instantiated )
                    return Common::MakeError<Result>(
                         "world streaming of '" + source.SceneName +
                         "': making the expanded prefab instances failed: " + instantiated.GetError() );
                LOG_INFO( "[WorldPartition] Play of '{0}': {1} prefab instance(s) expanded into {2} record(s), as "
                          "the cook expands them ({3} Standalone: streamed whole, one composite each)",
                          source.SceneName, world.Instances, made.size(), world.Standalone );
                source = std::move( world.Scene );
            }
        }
        auto   snapshot = std::make_shared<const SceneSerialized>( std::move( source ) );
        Result streamer( new WorldStreamer( scene, assets, snapshot->SceneName ) );
        // THE PROJECT'S STREAMING SETTINGS (Engine.json + this platform's layer), read as Play begins: a change
        // in Project Settings applies to the next Play.
        static_cast<Project::WorldPartitionSettings&>( streamer->m_Settings ) =
             Project::CurrentWorldPartitionSettings();
        std::vector<Rules::StreamingSource> sources = GatherSources( scene, instruments );
        // BEFORE the executor begins: its Begin destroys every unwanted record, and Destroy finds each by id.
        // A record without one is refused by that Begin; the null id here is never looked up.
        streamer->m_RecordIds.reserve( snapshot->Entities.size() );
        for ( const Assets::EntityData& record : snapshot->Entities )
            streamer->m_RecordIds.push_back( record.id.value_or( Common::UUID( 0 ) ) );
        auto begun = Rules::BeginWorldStreaming( *snapshot, RegistryMeshBounds(), streamer->m_Settings, sources,
                                                 *streamer, WorldCells::ScriptReferenceKinds() );
        if ( !begun )
            return Common::MakeError<Result>( "world streaming: " + begun.GetError() );
        if ( !begun.GetValue().has_value() )
            return Common::MakeSuccess( Result() );

        streamer->m_Executor = begun.ExtractValue();
        // On the scene's context once the executor exists (WorldStreamerOfScene): a script that asks before the
        // first Tick reads a plan and a residency, never a streamer without either.
        scene.GetRegistry().set<WorldStreamerOfScene>( WorldStreamerOfScene{ streamer.get() } );
        // A streaming begin is what a WorldPartition block produces; a begin without one is a broken invariant.
        const std::optional<WorldPartitionSerialized>& partition = snapshot->WorldPartition;
        if ( !partition.has_value() )
            return Common::MakeError<Result>( "world streaming of '" + snapshot->SceneName +
                                              "': streaming began but the snapshot states no WorldPartition" );
        streamer->m_Partition = *partition;
        streamer->UseSources( std::move( sources ) );
        streamer->m_Snapshot   = snapshot;
        streamer->m_Loader     = std::make_unique<WorldCellLoader>(
             std::make_shared<Rules::MemoryCellSource>( streamer->Executor().Plan(), snapshot->Entities ) );
        std::vector<Rules::GenerationIssue> hlodIssues; // the saved HLODs' findings, for the generation log
        {
            // THE SAVED HLODs (WP-FAR-6B D1; UE: PIE shows what Build HLODs saved): Play reads them and never
            // builds them. Missing or stale is a logged error naming the scene and the step; Play goes on without
            // the HLODs concerned.
            const Rules::WorldPartitionPlan&              plan = streamer->Executor().Plan();
            std::unordered_map<Common::UUID, std::size_t> byId;
            for ( std::size_t record = 0; record < snapshot->Entities.size(); ++record )
                byId.emplace( streamer->m_RecordIds[record], record );
            std::vector<CellHLOD>             hlods;
            std::vector<Rules::HLODExclusion> holes;
            bool                              streamed = false; // BeginHLODCells took the HLODs
            if ( auto layers = Rules::ValidateHLODLayers( *partition ); !layers )
                return Common::MakeError<Result>( "world streaming of '" + snapshot->SceneName +
                                                  "': " + layers.GetError() );
            if ( Rules::AnyGridHLODLayer( *partition ) )
            {
                auto stored = scenePath.empty()
                                   ? Common::MakeError<WorldCells::StoredHLODs>(
                                          "'" + snapshot->SceneName + "' is not saved, so it has no built HLODs" )
                                   : WorldCells::ReadBuiltHLODs( scenePath );
                if ( !stored )
                {
                    LOG_ERROR( "[WorldPartition] Play of '{0}' runs WITHOUT HLODs: {1}. Run Build > Build HLODs.",
                               snapshot->SceneName, stored.GetError() );
                    hlodIssues.push_back( WorldCells::MissingHLODsIssue( stored.GetError() ) );
                }
                else
                {
                    // Shared with every HLOD read below: BeginHLODs runs them after this block has ended.
                    const auto held = std::make_shared<const WorldCells::StoredHLODs>( stored.ExtractValue() );
                    const auto registries = std::span( &Assets::ContentRegistry::Get(), 1 );
                    const std::vector<Rules::EntityDescriptor> descriptors =
                         Rules::DescribeEntities( snapshot->Entities );
                    auto stale = WorldCells::StaleHLODCells( held->Index, *snapshot, descriptors, registries );
                    if ( !stale )
                        return Common::MakeError<Result>( "world streaming of '" + snapshot->SceneName +
                                                          "': " + stale.GetError() );
                    const std::set<std::string> staleUnits( stale.GetValue().begin(), stale.GetValue().end() );
                    hlodIssues = WorldCells::StaleHLODIssues( stale.GetValue() );
                    std::unordered_map<std::string, const WorldCells::BuiltHLODCell*> rows;
                    for ( const WorldCells::BuiltHLODCell& row : held->Index.Cells )
                        rows.emplace( row.Unit, &row );
                    auto hierarchy = Rules::PlanHLODHierarchy( *partition, plan );
                    if ( !hierarchy )
                        return Common::MakeError<Result>( "world streaming of '" + snapshot->SceneName +
                                                          "': " + hierarchy.GetError() );
                    if ( hierarchy.GetValue().Streams() )
                    {
                        // THE HLOD CELLS (WP-FAR-8): every node's row, read when its HLOD cell is wanted.
                        std::vector<const WorldCells::BuiltHLODCell*> nodeRows;
                        // One line for every node without a current HLOD, not one per node: an edit that dirties
                        // a layer leaves hundreds of them, and the count with the first names says the same.
                        std::vector<std::string> withoutHLOD;
                        for ( const Rules::HLODNode& node : hierarchy.GetValue().Nodes )
                        {
                            const auto row = rows.find( node.Name );
                            if ( staleUnits.contains( node.Name ) || row == rows.end() )
                            {
                                withoutHLOD.push_back( node.Name );
                                nodeRows.push_back( nullptr );
                                continue;
                            }
                            if ( !node.ForHLODCell )
                                for ( const WorldCells::IndexNotInstanced& missing : row->second->NotInstanced )
                                    holes.push_back( missing.Reason );
                            for ( const std::uint64_t id : row->second->Ids )
                                if ( byId.contains( Common::UUID( id ) ) )
                                    return Common::MakeError<Result>(
                                         "world streaming of '" + snapshot->SceneName + "': an HLOD record of " +
                                         node.Name + " has a record's id; rename the world" );
                            nodeRows.push_back( row->second->File.has_value() && !row->second->Ids.empty()
                                                     ? row->second
                                                     : nullptr );
                        }
                        if ( !withoutHLOD.empty() )
                        {
                            std::string named;
                            for ( std::size_t i = 0; i < withoutHLOD.size() && i < 8; ++i )
                                named += ( i == 0 ? "" : ", " ) + withoutHLOD[i];
                            LOG_ERROR( "[WorldPartition] Play of '{0}': the saved HLOD of {1} node(s) is missing or "
                                       "stale, so they have none ({2}{3}). Run Build > Build HLODs.",
                                       snapshot->SceneName, withoutHLOD.size(), named,
                                       withoutHLOD.size() > 8 ? ", ..." : "" );
                        }
                        LOG_INFO(
                             "[WorldPartition] Play of '{0}': {1} saved HLOD(s) in {2} HLOD cell(s) from {3}, "
                             "{4} stale",
                             snapshot->SceneName, nodeRows.size(), hierarchy.GetValue().Cells.size(),
                             WorldCells::BuiltHLODDirectory( scenePath ).string(), staleUnits.size() );
                        // The HLOD residency over the saved rows' record counts, and per unit its nodes' rows in
                        // composite order (ResidencyUnitMembers' order) for the source.
                        std::vector<std::size_t>               counts;
                        std::vector<std::vector<Common::UUID>> nodeIds;
                        for ( const WorldCells::BuiltHLODCell* row : nodeRows )
                        {
                            counts.push_back( row == nullptr ? 0 : row->Ids.size() );
                            std::vector<Common::UUID>& ids = nodeIds.emplace_back();
                            if ( row != nullptr )
                            {
                                for ( const std::uint64_t id : row->Ids )
                                    ids.emplace_back( id );
                                streamer->NoteImpostorPair( row->ImpostorCards, row->ImpostorFallbacks );
                            }
                        }
                        auto residency = Rules::MakeHLODResidency( *partition, hierarchy.GetValue(), counts );
                        if ( !residency )
                            return Common::MakeError<Result>( "world streaming of '" + snapshot->SceneName +
                                                              "': " + residency.GetError() );
                        const Rules::HLODResidency&                                r = residency.GetValue();
                        std::vector<std::vector<const WorldCells::BuiltHLODCell*>> unitRows(
                             Rules::ResidencyUnitCount( r.Plan ) );
                        for ( std::size_t a = 0; a < r.Plan.AlwaysLoaded.size(); ++a )
                            unitRows[a].push_back( nodeRows[r.Plan.AlwaysLoaded[a]] );
                        for ( std::size_t c = 0; c < r.Plan.Cells.size(); ++c )
                            for ( const std::size_t n : r.Plan.Cells[c].Composites )
                                unitRows[r.Plan.AlwaysLoaded.size() + c].push_back( nodeRows[n] );
                        std::shared_ptr<const Rules::WorldCellSource> source =
                             std::make_shared<StoredHLODCellSource>( held, std::move( unitRows ) );
                        Rules::HLODHierarchy streamedHierarchy = hierarchy.ExtractValue();
                        Rules::HLODResidency streamedResidency = residency.ExtractValue();
                        if ( auto begun = streamer->BeginHLODCells(
                                  std::move( streamedHierarchy ), std::move( streamedResidency ),
                                  std::move( nodeIds ), std::move( source ), holes );
                             !begun )
                            return Common::MakeError<Result>( begun.GetError() );
                        streamed = true;
                    }
                    for ( std::size_t unit = plan.AlwaysLoaded.size();
                          !streamed && unit < Rules::ResidencyUnitCount( plan ); ++unit )
                    {
                        const std::string name = Rules::HLODRowName( *partition, plan, unit );
                        const auto        row  = rows.find( name );
                        if ( staleUnits.contains( name ) || row == rows.end() )
                        {
                            LOG_ERROR(
                                 "[WorldPartition] Play of '{0}': the saved HLOD of {1} is missing or stale, "
                                 "so that cell has none. Run Build > Build HLODs.",
                                 snapshot->SceneName, name );
                            continue;
                        }
                        for ( const WorldCells::IndexNotInstanced& missing : row->second->NotInstanced )
                            holes.push_back( missing.Reason );
                        // A row without a file, or whose file lists no record, stands in for nothing.
                        if ( !row->second->File.has_value() || row->second->Ids.empty() )
                            continue;
                        // Read when BeginHLODs makes it, not now (CellHLOD): the read shares `held` (this block
                        // ends before that call); `byId` and `snapshot` outlive it.
                        const WorldCells::BuiltHLODCell* cell = row->second;
                        streamer->NoteImpostorPair( cell->ImpostorCards, cell->ImpostorFallbacks );
                        hlods.push_back( CellHLOD{
                             unit,
                             [held, &byId, &snapshot, cell,
                              name]() -> Common::ResultStr<std::vector<Assets::EntityData>>
                             {
                                 using Records = std::vector<Assets::EntityData>;
                                 auto records  = WorldCells::StoredCellRecords( *held, *cell );
                                 if ( !records )
                                     return Common::MakeError<Records>( "world streaming of '" +
                                                                        snapshot->SceneName + "', " + name + ": " +
                                                                        records.GetError() );
                                 for ( const Assets::EntityData& record : records.GetValue() )
                                     if ( byId.contains( *record.id ) )
                                         return Common::MakeError<Records>(
                                              "world streaming of '" + snapshot->SceneName +
                                              "': an HLOD record of " + name +
                                              " has a record's id; rename the world" );
                                 return records;
                             } } );
                    }
                    if ( !streamed )
                        LOG_INFO( "[WorldPartition] Play of '{0}': {1} saved HLOD(s) from {2}, {3} stale cell(s)",
                                  snapshot->SceneName, hlods.size(),
                                  WorldCells::BuiltHLODDirectory( scenePath ).string(), staleUnits.size() );
                }
            }
            if ( !streamed )
                if ( auto begun = streamer->BeginHLODs( std::move( hlods ), holes ); !begun )
                    return Common::MakeError<Result>( begun.GetError() );
        }
        // THE STREAMING GENERATION LOG OF THIS PLAY (WP-MAP-CHECK; UE writes StreamingGeneration-*.log for PIE): the
        // plan Play streams, its findings (logged above by ReportPlanIssues) and the saved HLODs' state.
        if ( Common::Constants::Path::HasProject() )
        {
            const std::vector<Rules::EntityDescriptor> described = Rules::DescribeEntities( snapshot->Entities );
            std::vector<Rules::GenerationIssue> issues = Rules::PlanIssues( streamer->Executor().Plan(), described );
            issues.insert( issues.end(), hlodIssues.begin(), hlodIssues.end() );
            auto log = WorldCells::WriteStreamingGenerationLog( Common::Constants::Path::ProjectDir(), snapshot->SceneName,
                                                                *partition, streamer->Executor().Plan(), described, issues );
            if ( !log )
                LOG_ERROR( "[WorldPartition] Play of '{0}': {1}", snapshot->SceneName, log.GetError() );
            if ( log )
                LOG_INFO( "[WorldPartition] Play of '{0}': streaming generation log {1}", snapshot->SceneName,
                          log.GetValue().string() );
        }
        streamer->m_MostResident = streamer->Executor().LiveRecords();
        // WHERE the sources stand, not only how many: a source nobody placed (the player's view point when no pawn
        // spawns, GatherSources) keeps its cells resident, and a count alone cannot say which one did.
        std::string where;
        for ( const Rules::StreamingSource& source : streamer->m_LastSources )
            where += std::format( "{}({:.0f}, {:.0f}, {:.0f})", where.empty() ? "" : ", ", source.Position.x,
                                  source.Position.y, source.Position.z );
        LOG_INFO( "[WorldPartition] '{0}': Play streams {1} record(s); {2} resident at the start around {3} "
                  "streaming source(s) at {5}, the rest destroyed until their cell is wanted ({4:.1f} ms to parse, "
                  "plan and destroy).",
                  streamer->m_SceneName, snapshot->Entities.size(), streamer->m_MostResident,
                  streamer->m_LastSources.size(), MsSince( started ), where );
        if ( const std::size_t unresolved = streamer->Executor().UnresolvedReferences(); unresolved > 0 )
            LOG_INFO( "[WorldPartition] '{0}': {1} reference(s) from records resident at the start name an entity "
                      "that is not resident (they resolve when it is).",
                      streamer->m_SceneName, unresolved );
        // THE PLAY START IS THE LARGEST DEPARTURE OF ALL. The editor had the whole world open; the executor has
        // just destroyed every record outside the resident cells, and what those records drew - built meshes,
        // materials, textures - stayed resident until the first cell happened to leave. Measured (WP14 flight):
        // six corpus textures, 21.3 MB, resident from frame 0 in a world whose first resident cells name none.
        if ( streamer->m_MostResident < snapshot->Entities.size() )
        {
            Assets::AssetEvictionSchedule::Request(
                 "world '" + streamer->m_SceneName + "': Play began, " +
                 std::to_string( snapshot->Entities.size() - streamer->m_MostResident ) +
                 " record(s) outside the resident cells destroyed" );
        }
        return Common::MakeSuccess( std::move( streamer ) );
    }

    Common::ResultStr<std::unique_ptr<WorldStreamer>>
    WorldStreamer::BeginCooked( Scene& scene, Assets::AssetManager& assets, CookedWorldStart world )
    {
        using Result = std::unique_ptr<WorldStreamer>;
        Result streamer( new WorldStreamer( scene, assets, world.Index->SceneName ) );
        // THE PROJECT'S STREAMING SETTINGS (the packaged Config/Engine.json + this platform's layer), read as the
        // game's world begins.
        static_cast<Project::WorldPartitionSettings&>( streamer->m_Settings ) =
             Project::CurrentWorldPartitionSettings();
        // Every read of `world` is resolved before the plan is moved out of it: an argument list is not
        // sequenced (clang evaluates it left to right, MSVC right to left).
        const auto  recordCount  = world.Indexed.RecordIds.size();
        const auto& partition    = world.Index->WorldPartition;
        auto&       observations = world.Indexed.Observations;
        auto        plan         = std::move( world.Indexed.Plan );
        auto        begun        = Rules::ResidencyExecutor::BeginFromAlwaysLoaded(
             std::move( plan ), partition, streamer->m_Settings, recordCount, observations );
        if ( !begun )
            return Common::MakeError<Result>( "world streaming of '" + streamer->m_SceneName +
                                              "': " + begun.GetError() );
        streamer->m_Executor = begun.ExtractValue();
        // On the scene's context once the executor exists (WorldStreamerOfScene): a script that asks before the
        // first Tick reads a plan and a residency, never a streamer without either.
        scene.GetRegistry().set<WorldStreamerOfScene>( WorldStreamerOfScene{ streamer.get() } );
        streamer->m_RecordIds.reserve( world.Indexed.RecordIds.size() );
        for ( const std::uint64_t id : world.Indexed.RecordIds )
            streamer->m_RecordIds.emplace_back( id );
        streamer->m_Partition = partition;
        streamer->m_Index     = world.Index;
        streamer->m_Loader    = std::make_unique<WorldCellLoader>( std::make_shared<RegionChunkCellSource>(
             world.Index, world.Directory, world.RegionChunkOfFile, WorldCells::UnitSet::World ) );
        auto streamedHLODs    = WorldCells::PlanHLODsFromIndex( *world.Index, streamer->Executor().Plan() );
        if ( !streamedHLODs )
            return Common::MakeError<Result>( "world streaming of '" + streamer->m_SceneName +
                                              "': " + streamedHLODs.GetError() );
        std::optional<WorldCells::IndexedHLODs> plannedHLODs = streamedHLODs.ExtractValue();
        if ( plannedHLODs.has_value() )
        {
            // THE HLOD CELLS (WP-FAR-8): the cooked HLOD units stream as the world's cells do - read on workers
            // through the same region chunks, under the second residency and its gate (TickWithHLODs); only the
            // tops of the chains are made now.
            std::vector<std::vector<Common::UUID>> nodeIds;
            std::vector<Rules::HLODExclusion>      holes;
            for ( std::size_t n = 0; n < plannedHLODs->Hierarchy.Nodes.size(); ++n )
            {
                const WorldCells::IndexHLODNode& node = world.Index->HLODNodes.at( n );
                if ( !plannedHLODs->Hierarchy.Nodes[n].ForHLODCell )
                    for ( const WorldCells::IndexNotInstanced& missing : node.NotInstanced )
                        holes.push_back( missing.Reason );
                std::vector<Common::UUID>& ids = nodeIds.emplace_back();
                for ( const std::uint64_t id : node.Ids )
                    ids.emplace_back( id );
                streamer->NoteImpostorPair( node.ImpostorCards, node.ImpostorFallbacks );
            }
            LOG_INFO(
                 "[WorldPartition] '{0}': {1} cooked HLOD(s) in {2} HLOD unit(s) ({3} HLOD cell(s)) from '{4}'",
                 streamer->m_SceneName, nodeIds.size(), world.Index->HLODUnits.size(),
                 plannedHLODs->Residency.Plan.Cells.size(), world.Directory );
            std::shared_ptr<const Rules::WorldCellSource> source = std::make_shared<RegionChunkCellSource>(
                 world.Index, world.Directory, world.RegionChunkOfFile, WorldCells::UnitSet::HLOD );
            if ( auto begun = streamer->BeginHLODCells( std::move( plannedHLODs->Hierarchy ),
                                                        std::move( plannedHLODs->Residency ), std::move( nodeIds ),
                                                        std::move( source ), holes );
                 !begun )
                return Common::MakeError<Result>( begun.GetError() );
        }
        else
        {
            // Every HLOD file is read now, on this thread: no layer is spatially loaded, so every HLOD stands in
            // for its cell from the first frame to the last.
            const WorldCells::FileReader      reader = CookedFileReader( world.Directory, world.RegionChunkOfFile );
            std::vector<CellHLOD>             hlods;
            std::vector<Rules::HLODExclusion> holes;
            for ( std::size_t hlod = 0; hlod < world.Index->HLODs.size(); ++hlod )
            {
                const WorldCells::IndexHLOD& row = world.Index->HLODs[hlod];
                for ( const WorldCells::IndexNotInstanced& missing : row.NotInstanced )
                    holes.push_back( missing.Reason );
                if ( !row.File.has_value() )
                    continue;
                streamer->NoteImpostorPair( row.ImpostorCards, row.ImpostorFallbacks );
                // Read when BeginHLODs makes it (CellHLOD); `world` and `reader` outlive that call, made below.
                hlods.push_back( CellHLOD{
                     row.Unit,
                     [&world, &reader, hlod, &streamer]() -> Common::ResultStr<std::vector<Assets::EntityData>>
                     {
                         auto records = WorldCells::HLODRecords( *world.Index, reader, hlod );
                         if ( !records )
                             return Common::MakeError<std::vector<Assets::EntityData>>(
                                  "world streaming of '" + streamer->m_SceneName + "': " + records.GetError() );
                         return records;
                     } } );
            }
            if ( auto begun = streamer->BeginHLODs( std::move( hlods ), holes ); !begun )
                return Common::MakeError<Result>( begun.GetError() );
        }
        streamer->m_MostResident = streamer->Executor().LiveRecords();
        LOG_INFO( "[WorldPartition] '{0}': a cooked world of {1} record(s) in {2} unit(s) from '{3}'; {4} "
                  "always-loaded record(s) are entities, every cell is read on a worker when first wanted.",
                  streamer->m_SceneName, world.Indexed.RecordIds.size(), world.Index->Units.size(),
                  world.Directory, streamer->m_MostResident );
        return Common::MakeSuccess( std::move( streamer ) );
    }

    std::vector<Rules::StreamingSource>
    WorldStreamer::GatherSources( Scene& scene, std::span<const Rules::StreamingSource> instruments,
                                  std::vector<std::uint32_t>* entities, std::vector<std::uint32_t>* withoutCamera )
    {
        std::vector<Rules::StreamingSource> sources;
        entt::registry&                     registry = scene.GetRegistry();
        for ( const auto entity : registry.view<ECS::StreamingSourceComponent>() )
        {
            const ECS::StreamingSourceData& data = registry.get<ECS::StreamingSourceComponent>( entity ).Data;
            if ( !data.Enabled )
                continue;
            const glm::mat4        world = ECS::Entity( entity, registry ).GetWorldTransform();
            Rules::StreamingSource source;
            source.Position = glm::vec3( world[3] );
            // The entity's forward is its local −Z (the camera's convention), in world axes.
            source.Facing                = -glm::vec3( world[2] );
            source.Priority              = data.Priority;
            source.UseVelocityForSorting = data.UseVelocityForSorting;
            source.TargetGrids           = data.TargetGrids;
            source.TargetHLODLayers      = data.TargetHLODLayers;
            source.TargetBehavior        = data.TargetBehavior;
            source.LookAheadTime         = data.LookAheadTime;
            source.MaxLookAhead          = data.MaxLookAhead;
            // One shape, always (UE's FStreamingSourceShape on the component): with the defaults it is exactly
            // the circle of the range a source with no shapes loads in.
            Rules::StreamingSourceShape shape;
            shape.UseGridLoadingRange = !data.OverrideLoadingRange;
            shape.Radius              = data.LoadingRange;
            shape.IsSector            = data.IsSector;
            shape.SectorAngle         = data.SectorAngle;
            if ( data.IsSector && data.SectorAngleFromCamera )
            {
                const auto* camera = registry.try_get<ECS::CameraComponent>( entity );
                if ( camera == nullptr )
                {
                    if ( withoutCamera != nullptr )
                        withoutCamera->push_back( static_cast<std::uint32_t>( entity ) );
                }
                else
                {
                    // The component's FOV is what gameplay zooms; the width comes from the viewport it was last
                    // drawn into. Before its first frame nothing has drawn through it, it has no width yet, and
                    // its vertical angle stands for one frame.
                    const float aspect =
                         camera->Camera ? camera->Camera->GetViewportAspect().value_or( 1.0f ) : 1.0f;
                    shape.SectorAngle = CameraViewAngle( camera->Data.FOV, aspect );
                }
            }
            shape.Location            = data.ShapeOffset;
            shape.Yaw                 = data.ShapeYaw;
            source.Shapes.push_back( shape );
            sources.push_back( std::move( source ) );
            if ( entities != nullptr )
                entities->push_back( static_cast<std::uint32_t>( entity ) );
        }
        // THE PROVIDERS: sources a script states with no entity (WorldPartition.provideSource).
        if ( const auto* provided = registry.try_ctx<ProvidedStreamingSources>(); provided != nullptr )
            for ( const auto& [name, source] : provided->Sources )
                sources.push_back( source );
        // THE PLAYER (UE's APlayerController, a streaming source provider at GetPlayerViewPoint): with a pawn
        // the pawn's own component above stands for it; with none - a level without a Default Pawn, played from
        // here or through an Auto Activate camera - the player is a source where it looks from.
        if ( scene.GetPlayerPawn() == entt::null )
            if ( const std::optional<glm::mat4> view = scene.PlayerViewPoint() )
            {
                Rules::StreamingSource player;
                player.Position = glm::vec3( ( *view )[3] );
                player.Facing   = -glm::vec3( ( *view )[2] );
                sources.push_back( std::move( player ) );
            }
        sources.insert( sources.end(), instruments.begin(), instruments.end() );
        return sources;
    }

    // THE HLOD CELLS' WORLD (WP-FAR-8): what the HLOD residency's actions do - the same reads on workers and the
    // same InstantiateRecords / DestroyEntity as the world's cells, over the HLOD record space.
    class WorldStreamer::HLODCellWorld final : public Rules::ResidencyWorld
    {
    public:
        HLODCellWorld( WorldStreamer& owner, std::shared_ptr<const Rules::WorldCellSource> source,
                       std::vector<Common::UUID> recordIds )
             : m_Owner( owner ), m_Source( source ), m_Loader( std::move( source ) ),
               m_RecordIds( std::move( recordIds ) )
        {
        }

        [[nodiscard]] const Rules::WorldCellSource& Source() const
        {
            return *m_Source;
        }

        // What HLOD cell @p unit's read records name (WorldCellLoader::Dependencies); null when it is not read.
        [[nodiscard]] const std::vector<std::string>* Dependencies( std::size_t unit ) const
        {
            return m_Loader.Dependencies( unit );
        }

        // A slice of an HLOD cell (THE SLICES): made hidden, revealed whole by FinishActivation — the switch
        // then shows or hides each HLOD by its own Visibility, in the same tick.
        [[nodiscard]] Common::BoolResultStr Activate( std::size_t unit, std::size_t firstMember,
                                                      std::span<const std::size_t> records ) override
        {
            const std::vector<Assets::EntityData>* read = m_Loader.Records( unit );
            if ( read == nullptr || firstMember + records.size() > read->size() )
                return Common::MakeError( std::format(
                     "world streaming of '{}': {} is activated at records {}..{} with {} record(s) read",
                     m_Owner.m_SceneName, Name( unit ), firstMember, firstMember + records.size(),
                     read == nullptr ? 0 : read->size() ) );
            auto made = SceneSerializer( m_Owner.m_Scene, m_Owner.m_Assets )
                             .InstantiateRecords( std::span<const Assets::EntityData>( *read ).subspan(
                                                       firstMember, records.size() ),
                                                  m_Owner.m_SceneName, nullptr );
            if ( made )
                HideMade( *m_Owner.m_Scene, m_RecordIds, records, m_Made[unit] );
            return made;
        }

        // An HLOD cell has no bodies: whole is drawn.
        [[nodiscard]] Common::ResultStr<bool> FinishActivation( std::size_t                  unit,
                                                                std::span<const std::size_t> records ) override
        {
            RevealMade( *m_Owner.m_Scene, m_Made[unit] );
            m_Made.erase( unit );
            LOG_INFO( "[WorldPartition] '{0}': {1} loaded, {2} HLOD record(s); process {3}", m_Owner.m_SceneName,
                      Name( unit ), records.size(), Common::Utils::ReadProcessFootprint().Describe() );
            return Common::MakeSuccess( true );
        }

        [[nodiscard]] Rules::ActivationRecordKind ActivationKindOf( std::size_t unit,
                                                                    std::size_t member ) const override
        {
            const std::vector<Assets::EntityData>* read = m_Loader.Records( unit );
            // A count that differs from the plan is refused by the first slice's Activate, by name.
            return read != nullptr && member < read->size() ? Rules::ActivationKindOf( ( *read )[member] )
                                                            : Rules::ActivationRecordKind::Plain;
        }

        void Destroy( std::span<const std::size_t> records ) override
        {
            for ( const std::size_t record : records )
                if ( const auto entity = m_Owner.m_Scene->FindEntityByID( m_RecordIds.at( record ) );
                     entity.has_value() )
                    m_Owner.m_Scene->DestroyEntityAtFence( entity->get() );
        }

        void StartLoad( std::size_t unit, std::uint64_t ticket ) override
        {
            m_Loader.Start( unit, ticket );
        }

        void CancelLoad( std::size_t unit, std::uint64_t ticket ) override
        {
            m_Loader.Cancel( unit, ticket );
        }

        void Unload( std::size_t unit ) override
        {
            m_Loader.Unload( unit );
            m_Made.erase( unit ); // destroyed by the Deactivate before this
            LOG_INFO( "[WorldPartition] '{0}': {1} unloaded.", m_Owner.m_SceneName, Name( unit ) );
        }

        [[nodiscard]] std::vector<Rules::LoadOutcome> TakeLoadOutcomes() override
        {
            std::vector<Rules::LoadOutcome> finished = m_Loader.TakeFinished();
            for ( const Rules::LoadOutcome& outcome : finished )
                if ( !outcome.Ok )
                    LOG_ERROR( "[WorldPartition] '{0}': reading {1} failed, it is retried: {2}",
                               m_Owner.m_SceneName, Name( outcome.Unit ), outcome.Reason );
            return finished;
        }

    private:
        [[nodiscard]] std::string Name( std::size_t unit ) const
        {
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set before this world is made
            return Rules::DescribeHLODUnit( *m_Owner.m_HLODHierarchy, *m_Owner.m_HLODResidency, unit );
        }

        WorldStreamer&                                m_Owner;
        std::shared_ptr<const Rules::WorldCellSource> m_Source;
        WorldCellLoader                               m_Loader;
        std::vector<Common::UUID>                     m_RecordIds;
        // Per Activating HLOD cell: the entities its slices made, hidden until it is whole.
        std::unordered_map<std::size_t, std::vector<std::uint32_t>> m_Made;
    };

    float WorldStreamer::CameraViewAngle( const float verticalFovDegrees, const float aspect )
    {
        const double halfVertical =
             static_cast<double>( verticalFovDegrees ) * 0.5 * 3.14159265358979323846 / 180.0;
        const double horizontal = 2.0 * std::atan( std::tan( halfVertical ) * static_cast<double>( aspect ) );
        return static_cast<float>( std::min( 360.0, horizontal * 180.0 / 3.14159265358979323846 ) );
    }

    void WorldStreamer::StampVelocities( std::vector<Rules::StreamingSource>& sources,
                                         std::span<const std::uint32_t> entities, double nowSeconds )
    {
        // UE's UWorldPartitionSubsystem::UpdateStreamingSourcesVelocity: the distance an entity's source moved
        // since the last Tick, over the time between them. A source new this Tick has none yet.
        const double                                 elapsed = nowSeconds - m_SourcesAt;
        std::unordered_map<std::uint32_t, glm::vec3> now;
        for ( std::size_t index = 0; index < entities.size(); ++index )
        {
            now[entities[index]] = sources[index].Position;
            const auto before    = m_SourcePositions.find( entities[index] );
            if ( before != m_SourcePositions.end() && elapsed > 0.0 )
                sources[index].Velocity =
                     ( sources[index].Position - before->second ) / static_cast<float>( elapsed );
        }
        m_SourcePositions = std::move( now );
        m_SourcesAt       = nowSeconds;
    }

    void WorldStreamer::UseSources( std::vector<Rules::StreamingSource> sources )
    {
        if ( sources.empty() && !m_SaidNoSource )
        {
            m_SaidNoSource = true;
            LOG_ERROR( "[WorldPartition] '{0}': Play has no streaming source — no entity has an enabled Streaming "
                       "Source, the level spawned no pawn and the player has no view — so only the always-loaded "
                       "part of the world is resident. Give the level a Default Pawn or put a Streaming Source on "
                       "an entity.",
                       m_SceneName );
        }
        m_LastSources = std::move( sources );
    }

    Common::BoolResultStr WorldStreamer::Tick( double                                  nowSeconds,
                                               std::span<const Rules::StreamingSource> instruments )
    {
        std::vector<std::uint32_t>          entities;
        std::vector<std::uint32_t>          withoutCamera;
        std::vector<Rules::StreamingSource> gathered =
             GatherSources( *m_Scene, instruments, &entities, &withoutCamera );
        for ( const std::uint32_t entity : withoutCamera )
            if ( m_SaidWithoutCamera.insert( entity ).second )
                LOG_ERROR( "[WorldPartition] '{0}': entity {1}'s Streaming Source takes its sector's angle from "
                           "its camera (Angle From Camera) but the entity has no Camera; it streams its authored "
                           "Sector Angle of {2} degrees until it has one.",
                           m_SceneName, entity,
                           m_Scene->GetRegistry()
                                .get<ECS::StreamingSourceComponent>( static_cast<entt::entity>( entity ) )
                                .Data.SectorAngle );
        StampVelocities( gathered, entities, nowSeconds );
        UseSources( std::move( gathered ) );
        const std::span<const Rules::StreamingSource> sources = m_LastSources;

        // UE's priority level streaming: while play waits behind the overlay, activation has more of the frame.
        Executor().SetPriorityActivation( m_LastTick.Streaming.Blocks() );
        m_LastTick = TickReport{};
        // The machine's impostor side first: a quality change re-shows the HLODs already shown (SyncHLODImpostors),
        // and the switch below shows any newly shown HLOD with the same side.
        SyncHLODImpostors();
        // The HLODs switch on the state this very tick leaves, before the frame is drawn.
        // NOLINTBEGIN(bugprone-unchecked-optional-access): a streamer that began holds its HLOD switch
        auto tick = m_HLODExecutor.has_value()
                         ? Rules::TickWithHLODs( Executor(), *m_HLODExecutor, *m_HLODResidency, *m_HLODs, sources,
                                                 nowSeconds, *this, *m_HLODCells, *this )
                         : Rules::TickWithHLODs( Executor(), *m_HLODs, sources, nowSeconds, *this, *this );
        // NOLINTEND(bugprone-unchecked-optional-access)
        if ( !tick )
            return Common::MakeError( "world streaming of '" + m_SceneName + "': " + tick.GetError() );

        const Rules::ResidencyTick& done = tick.GetValue();
        m_LastTick.Tick                  = done;
        for ( const Rules::ActivationDone& unitDone : done.Completed )
        {
            const std::string name = Rules::DescribeResidencyUnit( Executor().Plan(), unitDone.Unit );
            m_LastTick.ActivatedUnits += ( m_LastTick.ActivatedUnits.empty() ? "" : " " ) + name;
            ++m_Activations;
            m_WorstUnitMs = std::max( m_WorstUnitMs, unitDone.Ms );
            // A unit made over several frames is SAID: it is what the budget did, and where a slow cell shows.
            if ( unitDone.Frames > 1 )
                LOG_INFO( "[WorldPartition] '{0}': {1} activated in {2} frames: {3} record(s) in {4} slice(s), "
                          "{5:.2f} ms on the main thread, collision ready",
                          m_SceneName, name, unitDone.Frames, unitDone.Records, unitDone.Slices, unitDone.Ms );
        }
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): a streamer that began holds its HLOD switch
        for ( const Rules::WarmupOverrun& overrun : m_HLODs->TakeWarmupOverruns() )
            LOG_INFO(
                 "[WorldPartition] '{0}': the HLOD warmup of {1} ran out of {2} frames before its assets were "
                 "resident; {3} shown with what had arrived.",
                 m_SceneName,
                 overrun.HLODUnit // NOLINT(bugprone-unchecked-optional-access): only a streamed hierarchy's
                      ? Rules::DescribeHLODUnit( *m_HLODHierarchy, *m_HLODResidency, overrun.Unit )
                      : Rules::DescribeResidencyUnit( Executor().Plan(), overrun.Unit ),
                 Rules::HLODRuntime::kWarmupMaxFrames,
                 overrun.Showing ? "the cell is" : "what stands in for it is" );
        m_LastTick.LiveRecords           = Executor().LiveRecords();
        m_LastTick.LoadsInFlight         = m_Loader->InFlight();
        m_MostResident                   = std::max( m_MostResident, m_LastTick.LiveRecords );
        m_LastTick.Streaming = Rules::AssessStreaming( Executor().Plan(), m_Partition, m_Settings,
                                                       Executor().ActivationCost(), Executor().State(), sources,
                                                       Executor().UnitDataLayerStates() );
        const bool wasWaiting = m_FramesWaiting > 0;
        m_FramesWaiting       = m_LastTick.Streaming.Blocks() ? m_FramesWaiting + 1 : 0;
        if ( m_FramesWaiting == 1 )
        {
            LOG_INFO( "[WorldPartition] '{0}': the cell under a streaming source ({1}) is not resident; play "
                      "waits for it",
                      m_SceneName,
                      Rules::DescribeResidencyUnit( Executor().Plan(), m_LastTick.Streaming.UnderSource ) );
        }
        else if ( wasWaiting && m_FramesWaiting == 0 )
        {
            LOG_INFO( "[WorldPartition] '{0}': the cell under the streaming source is resident; play resumes",
                      m_SceneName );
        }
        // Blocking disabled (Engine.json WorldPartition.BlockOnSlowStreaming off): the same Critical state is said
        // once per stretch, and play goes on over the HLOD or the empty ground.
        const bool criticalUnblocked = m_LastTick.Streaming.Performance == Rules::StreamingPerformance::Critical &&
                                       !m_LastTick.Streaming.BlockOnSlowStreaming;
        if ( criticalUnblocked && !m_SaidCriticalUnblocked )
        {
            LOG_WARN(
                 "[WorldPartition] '{0}': slow streaming (blocking disabled): the cell under a streaming source "
                 "({1}) is not resident; play goes on",
                 m_SceneName,
                 Rules::DescribeResidencyUnit( Executor().Plan(), m_LastTick.Streaming.UnderSource ) );
        }
        m_SaidCriticalUnblocked = criticalUnblocked;
        if ( Rules::ReleasesCellAssets( done ) )
        {
            Assets::AssetEvictionSchedule::Request(
                 "world '" + m_SceneName + "': " + std::to_string( done.UnitsDeactivated ) +
                 " cell(s) deactivated, " + std::to_string( done.UnitsUnloaded ) + " unloaded" );
        }
        WorldStreamingWait wait{ m_LastTick.Streaming, m_FramesWaiting, {} };
        // The entity sources lead the gathered list, in the order of entities.
        for ( std::size_t index = 0; index < entities.size(); ++index )
        {
            auto completion = Rules::AssessSourceCompletion( Executor().Plan(), m_Partition, Executor().State(),
                                                             m_LastSources[index], Executor().UnitDataLayerStates() );
            if ( !completion )
                return Common::MakeError( "world streaming of '" + m_SceneName + "': " + completion.GetError() );
            wait.Sources.emplace( entities[index], completion.GetValue() );
        }
        m_Scene->GetRegistry().set<WorldStreamingWait>( std::move( wait ) );
        PublishStreamingEvents();
        // A reference across a cell boundary is legal and its reader handles the absence — but it is SAID.
        if ( done.DeferredReferences > 0 || done.UnboundReferences > 0 )
        {
            LOG_INFO( "[WorldPartition] '{0}': {1} reference(s) from records just activated name an entity that "
                      "is not resident (they resolve when it is), and {2} reference(s) from resident records "
                      "named an entity that just left (they read null until it returns).",
                      m_SceneName, done.DeferredReferences, done.UnboundReferences );
        }
        // WP-ENTITY-REFS: a reference whose target arrived or left is carried to the running scripts that hold the
        // entity it named (ECS::EntityReferenceBindings) and said per cell.
        if ( !done.Rebound.empty() || !done.Unbound.empty() )
        {
            std::vector<ECS::EntityReferenceBinding> bindings;
            bindings.reserve( done.Rebound.size() + done.Unbound.size() );
            std::map<std::size_t, std::size_t> reboundInto;
            std::map<std::size_t, std::size_t> unboundFrom;
            for ( const auto& [holder, target] : done.Rebound )
            {
                bindings.push_back( ECS::EntityReferenceBinding{ m_RecordIds.at( holder ), m_RecordIds.at( target ) } );
                ++reboundInto[Executor().UnitOf( target )];
            }
            for ( const auto& [holder, target] : done.Unbound )
            {
                bindings.push_back( ECS::EntityReferenceBinding{ m_RecordIds.at( holder ), m_RecordIds.at( target ) } );
                ++unboundFrom[Executor().UnitOf( target )];
            }
            ECS::PublishEntityReferenceBindings( m_Scene->GetRegistry(), bindings );
            for ( const auto& [unit, count] : reboundInto )
                LOG_INFO( "[WorldPartition] '{0}': rebound {1} reference{2} into {3}", m_SceneName, count,
                          count == 1 ? "" : "s", Rules::DescribeResidencyUnit( Executor().Plan(), unit ) );
            for ( const auto& [unit, count] : unboundFrom )
                LOG_INFO( "[WorldPartition] '{0}': unbound {1} reference{2} from {3}; they read nil until it returns",
                          m_SceneName, count, count == 1 ? "" : "s",
                          Rules::DescribeResidencyUnit( Executor().Plan(), unit ) );
        }
        SayDataLayerUnits();
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WorldStreamer::SetDataLayerRuntimeState( const std::string_view layer,
                                                                   const DataLayerRuntimeState state )
    {
        const std::optional<std::size_t> index = Rules::DataLayerIndexOf( m_Partition, layer );
        if ( !index.has_value() )
            return Common::MakeError( std::format( "world streaming of '{}': the world states no data layer '{}'",
                                                   m_SceneName, layer ) );
        const DataLayerRuntimeState before = Executor().DataLayerStates().at( *index );
        if ( auto set = Executor().SetDataLayerState( *index, state ); !set.IsSuccess() )
            return Common::MakeError( std::format( "world streaming of '{}': {}", m_SceneName, set.GetError() ) );
        LOG_INFO( "[WorldPartition] '{0}': data layer '{1}' {2} -> {3}.", m_SceneName, layer,
                  Rules::DataLayerRuntimeStateName( before ), Rules::DataLayerRuntimeStateName( state ) );
        return BOOLSUCCESS;
    }

    std::optional<DataLayerRuntimeState> WorldStreamer::GetDataLayerRuntimeState( const std::string_view layer ) const
    {
        const std::optional<std::size_t> index = Rules::DataLayerIndexOf( m_Partition, layer );
        if ( !index.has_value() || ( *m_Partition.DataLayers )[*index].Type != DataLayerType::Runtime )
            return std::nullopt;
        return m_Executor->DataLayerStates().at( *index ); // NOLINT(bugprone-unchecked-optional-access): began
    }

    DataLayerRuntimeState WorldStreamer::GetUnitEffectiveRuntimeState( const std::size_t unit ) const
    {
        return m_Executor->UnitDataLayerStates().at( unit ); // NOLINT(bugprone-unchecked-optional-access): began
    }

    // ONE LINE PER CHANGE OF A UNIT OF RUNTIME DATA LAYERS: not loaded (its layers are Unloaded), loaded and not
    // activated (Loaded), activated - named as its cell file is (Rules::CellStem), so the log and the cooked
    // directory say the same name.
    void WorldStreamer::SayDataLayerUnits()
    {
        const Rules::WorldPartitionPlan& plan  = Executor().Plan();
        const std::size_t                units = Rules::ResidencyUnitCount( plan );
        m_SaidLayerUnit.resize( units, 0 );
        for ( std::size_t unit = 0; unit < units; ++unit )
        {
            const std::vector<std::size_t> layers = Rules::UnitRuntimeDataLayers( plan, m_Partition, unit );
            if ( layers.empty() )
                continue;
            const DataLayerRuntimeState wanted = Executor().UnitDataLayerStates().at( unit );
            const Rules::Residency      now    = Executor().State().Units.at( unit ).State;
            std::uint8_t                say    = 0;
            std::string_view            what;
            if ( wanted == DataLayerRuntimeState::Unloaded )
            {
                say  = 1;
                what = "not loaded: its data layers are Unloaded";
            }
            else if ( wanted == DataLayerRuntimeState::Loaded && now == Rules::Residency::Loaded )
            {
                say  = 2;
                what = "loaded, not activated: its data layers are Loaded";
            }
            else if ( now == Rules::Residency::Activated )
            {
                say  = 3;
                what = "activated";
            }
            if ( say == 0 || m_SaidLayerUnit[unit] == say )
                continue;
            m_SaidLayerUnit[unit] = say;
            std::string names;
            for ( const std::size_t layer : layers )
                names += ( names.empty() ? "" : ", " ) + ( *m_Partition.DataLayers )[layer].Name;
            const std::string cell =
                 unit < plan.AlwaysLoaded.size()
                      ? "AlwaysLoaded" + Rules::DataLayersCellSuffix( m_Partition, layers ) + " (" +
                             Rules::DescribeResidencyUnit( plan, unit ) + ")"
                      : Rules::CellStem( m_Partition, plan.Cells.at( unit - plan.AlwaysLoaded.size() ) );
            LOG_INFO( "[WorldPartition] '{0}': cell {1} (data layers {2}) {3}.", m_SceneName, cell, names, what );
        }
    }

    void WorldStreamer::PublishStreamingEvents()
    {
        const Rules::WorldPartitionPlan&         plan  = Executor().Plan();
        const std::vector<Rules::UnitResidency>& units = Executor().State().Units;
        WorldPartitionStreamingEvents            events;
        events.Publication = ++m_EventPublication;
        if ( m_StatesSeen.size() != units.size() )
            m_StatesSeen.assign( units.size(), Rules::Residency::Unloaded );
        for ( std::size_t unit = 0; unit < units.size(); ++unit )
        {
            const Rules::Residency now  = units[unit].State;
            const Rules::Residency seen = m_StatesSeen[unit];
            if ( now == seen )
                continue;
            m_StatesSeen[unit] = now;
            events.Updated     = true;
            if ( unit < plan.AlwaysLoaded.size() )
                continue; // the always-loaded part is resident for the whole session; it is no cell's news
            if ( now == Rules::Residency::Activated )
                events.Activated.push_back( Rules::DescribeResidencyUnit( plan, unit ) );
            else if ( now == Rules::Residency::Unloaded &&
                      ( seen == Rules::Residency::Loaded || seen == Rules::Residency::Activated ) )
                events.Unloaded.push_back( Rules::DescribeResidencyUnit( plan, unit ) );
        }
        m_Scene->GetRegistry().set<WorldPartitionStreamingEvents>( std::move( events ) );
    }

#if DESERT_DEV_INSTRUMENTS
    void WorldStreamer::SetDebugLoadDelayTicks( std::uint32_t ticks )
    {
        m_Loader->SetDebugDelayTicks( ticks );
    }
#endif

    void WorldStreamer::StartLoad( std::size_t unit, std::uint64_t ticket )
    {
#if DESERT_DEV_INSTRUMENTS
        if ( unit >= m_LoadStarted.size() )
        {
            m_LoadStarted.resize( unit + 1 );
            m_UnitTimings.resize( unit + 1 );
        }
        m_LoadStarted[unit] = std::chrono::steady_clock::now();
#endif
        m_Loader->Start( unit, ticket );
    }

    void WorldStreamer::CancelLoad( std::size_t unit, std::uint64_t ticket )
    {
        m_Loader->Cancel( unit, ticket );
    }

    void WorldStreamer::Unload( std::size_t unit )
    {
        m_Loader->Unload( unit );
        m_Made.erase( unit ); // what it made was destroyed by the Deactivate before this
    }

    std::vector<Rules::LoadOutcome> WorldStreamer::TakeLoadOutcomes()
    {
        std::vector<Rules::LoadOutcome> finished = m_Loader->TakeFinished();
#if DESERT_DEV_INSTRUMENTS
        for ( const Rules::LoadOutcome& outcome : finished )
            if ( outcome.Ok && outcome.Unit < m_LoadStarted.size() )
                m_UnitTimings[outcome.Unit].LoadMs = MsSince( m_LoadStarted[outcome.Unit] );
#endif
        for ( const Rules::LoadOutcome& outcome : finished )
            if ( !outcome.Ok )
                LOG_ERROR( "[WorldPartition] '{0}': reading {1} failed, it is retried: {2}", m_SceneName,
                           Rules::DescribeResidencyUnit( Executor().Plan(), outcome.Unit ), outcome.Reason );
        return finished;
    }

    Common::BoolResultStr WorldStreamer::Activate( std::size_t unit, std::size_t firstMember,
                                                   std::span<const std::size_t> records )
    {
        const auto start = std::chrono::steady_clock::now();

        // StepResidency lets through only a Loaded unit, and a unit is Loaded only once its read was reported Ok.
        const std::vector<Assets::EntityData>* unitRecords = m_Loader->Records( unit );
        if ( unitRecords == nullptr )
            return Common::MakeError( "world streaming of '" + m_SceneName + "': unit " + std::to_string( unit ) +
                                      " is activated with no finished read of its records" );
        const std::size_t planned = Rules::ResidencyUnitRecords( Executor().Plan(), unit );
        if ( unitRecords->size() != planned || firstMember + records.size() > planned )
        {
            return Common::MakeError( std::format(
                 "world streaming of '{}': unit {} came back with {} record(s), the "
                 "plan holds {} (slice {}..{})",
                 m_SceneName, unit, unitRecords->size(), planned, firstMember, firstMember + records.size() ) );
        }
        auto made =
             SceneSerializer( m_Scene, m_Assets )
                  .InstantiateRecords(
                       std::span<const Assets::EntityData>( *unitRecords ).subspan( firstMember, records.size() ),
                       m_SceneName, nullptr );
        if ( made )
        {
            HideMade( *m_Scene, m_RecordIds, records, m_Made[unit] );
            // UE creates a primitive's physics state as it registers: the slice's bodies are made in the slice,
            // and its time is theirs too. Before the Play's physics world exists this makes nothing, and
            // FinishActivation waits for it.
            if ( auto* physics = m_Scene->FindSystem<ECS::PhysicsECSSystem>() )
                physics->CreatePendingBodies( m_Scene->GetRegistry() );
        }

        const double ms = MsSince( start );
        ++m_Slices;
        m_RecordsActivated += records.size();
        m_ActivationMs += ms;
        m_WorstSliceMs = std::max( m_WorstSliceMs, ms );
#if DESERT_DEV_INSTRUMENTS
        if ( unit < m_UnitTimings.size() )
            m_UnitTimings[unit].ActivationMs += ms; // every slice of the unit
#endif
        m_LastTick.ActivationMs += ms;
        return made;
    }

    Common::ResultStr<bool> WorldStreamer::FinishActivation( std::size_t unit,
                                                             std::span<const std::size_t> /*records*/ )
    {
        std::vector<std::uint32_t>& made = m_Made[unit];
        // ACTIVATED = COLLIDABLE. A scene with no physics system, or one whose Play refused to simulate (no
        // collision profiles, said once by the system), has no body to wait for.
        auto* physics = m_Scene->FindSystem<ECS::PhysicsECSSystem>();
        if ( physics != nullptr && !physics->RefusesSimulation() )
        {
            entt::registry& registry = m_Scene->GetRegistry();
            // The Play's physics world is made by the system's first Update: until then nothing can collide.
            if ( !physics->CreatePhysicsState( registry ) )
                return Common::MakeSuccess( false );
            for ( const std::uint32_t entity : made )
                if ( physics->AwaitsPhysics( static_cast<entt::entity>( entity ) ) )
                    return Common::MakeSuccess( false ); // its mesh is still being read
        }
        RevealMade( *m_Scene, made );
        m_Made.erase( unit );
        return Common::MakeSuccess( true );
    }

    Rules::ActivationRecordKind WorldStreamer::ActivationKindOf( std::size_t unit, std::size_t member ) const
    {
        const std::vector<Assets::EntityData>* unitRecords = m_Loader->Records( unit );
        // A count that differs from the plan is refused by the first slice's Activate, by name.
        return unitRecords != nullptr && member < unitRecords->size()
                    ? Rules::ActivationKindOf( ( *unitRecords )[member] )
                    : Rules::ActivationRecordKind::Plain;
    }

    void WorldStreamer::Destroy( std::span<const std::size_t> records )
    {
        for ( const std::size_t record : records )
        {
            // A child already went with its parent's tree; a prefab instance's inner entities go with its root.
            const auto entity = m_Scene->FindEntityByID( m_RecordIds.at( record ) );
            // Streamed out without a mid-frame wait for the render task (WP-OPT-UNLOAD): hidden now, destroyed at
            // the next view fence.
            if ( entity.has_value() )
                m_Scene->DestroyEntityAtFence( entity->get() );
        }
    }

    Common::BoolResultStr WorldStreamer::BeginHLODs( std::vector<CellHLOD>                 hlods,
                                                     std::span<const Rules::HLODExclusion> holes )
    {
        std::vector<std::size_t> units;
        units.reserve( hlods.size() );
        for ( const CellHLOD& hlod : hlods )
            units.push_back( hlod.Unit );
        auto runtime = Rules::HLODRuntime::Make( Executor().Plan(), units );
        if ( !runtime )
            return Common::MakeError( "world streaming of '" + m_SceneName + "': " + runtime.GetError() );
        m_HLODs = runtime.ExtractValue();
        // ONE HLOD'S RECORDS ALIVE AT A TIME (CellHLOD): read, made entities, dropped before the next read.
        const Common::Utils::ProcessFootprint before = Common::Utils::ReadProcessFootprint();
        m_HLODIds.clear();
        m_HLODDependencies.clear();
        const std::filesystem::path assetsRoot  = AssetsRoot();
        std::size_t recordCount = 0;
        for ( CellHLOD& hlod : hlods )
        {
            std::vector<Common::UUID>& ids     = m_HLODIds.emplace_back();
            auto                       records = hlod.Read();
            if ( !records )
                return Common::MakeError( records.GetError() );
            for ( const Assets::EntityData& record : records.GetValue() )
            {
                if ( !record.id.has_value() || record.id->IsNull() )
                    return Common::MakeError( "world streaming of '" + m_SceneName +
                                              "': a record of the HLOD of " +
                                              Rules::DescribeResidencyUnit( Executor().Plan(), hlod.Unit ) +
                                              " has no id, so it could not be shown or hidden" );
                ids.push_back( *record.id );
            }
            recordCount += records.GetValue().size();
            // What the HLOD draws, for its warmup (THE WARMUP): gathered while its records are in hand.
            m_HLODDependencies.push_back(
                 GatherSceneDependencies( records.GetValue(), nullptr, assetsRoot ).Guids );
            if ( auto made = SceneSerializer( m_Scene, m_Assets )
                                  .InstantiateRecords( records.GetValue(), m_SceneName, nullptr );
                 !made )
                return Common::MakeError( "world streaming of '" + m_SceneName + "': making the HLOD of " +
                                          Rules::DescribeResidencyUnit( Executor().Plan(), hlod.Unit ) +
                                          " failed: " + made.GetError() );
            hlod.Read = nullptr; // what the read holds goes with the records
        }
        // The census the Play start is judged by: the process before and after every HLOD became entities.
        LOG_INFO( "[WorldPartition] '{0}': {1} HLOD(s), {2} record(s) made one HLOD at a time; process before: "
                  "{3}; after: {4}",
                  m_SceneName, hlods.size(), recordCount, before.Describe(),
                  Common::Utils::ReadProcessFootprint().Describe() );
        // The layers' states from the first Sync on (WP-HLOD-DATA-LAYERS): an HLOD of a cell whose runtime data
        // layers start Unloaded or Loaded is never shown, not even for the frame before the first Tick.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): assigned just above
        if ( auto synced = m_HLODs->Sync( Executor().State(), nullptr, *this, Executor().UnitDataLayerStates() );
             !synced )
            return Common::MakeError( "world streaming of '" + m_SceneName + "': " + synced.GetError() );
        // From here on every switch is an event a flight reads (WP22c): the first Sync above set them all.
        m_HLODSwitchesSaid = true;
        // THE HOLES: a drawing record the builder could not batch has no stand-in (WorldPartitionHLODSwitch.hpp).
        const std::string holesText =
             holes.empty() ? std::string( "every drawing record has its stand-in" )
                           : std::to_string( holes.size() ) +
                                  " drawing record(s) have none and leave a hole while their cell is not drawn: " +
                                  Rules::DescribeHLODHoles( holes );
        // A hole is something the player sees vanish (WP-FAR-1: a whole landscape beyond the loading range), so
        // it is a warning, not a line among the info.
        if ( holes.empty() )
        {
            LOG_INFO( "[WorldPartition] '{0}': {1} HLOD(s), {2} record(s), stand in for the cells not drawn; {3}.",
                      m_SceneName, units.size(), recordCount, holesText );
        }
        else
        {
            LOG_WARN( "[WorldPartition] '{0}': {1} HLOD(s), {2} record(s), stand in for the cells not drawn; {3}.",
                      m_SceneName, units.size(), recordCount, holesText );
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WorldStreamer::BeginHLODCells( Rules::HLODHierarchy                          hierarchy,
                                                         Rules::HLODResidency                          residency,
                                                         std::vector<std::vector<Common::UUID>>        nodeIds,
                                                         std::shared_ptr<const Rules::WorldCellSource> source,
                                                         std::span<const Rules::HLODExclusion>         holes )
    {
        const auto failed = [this]( const std::string& why )
        { return Common::MakeError( "world streaming of '" + m_SceneName + "': " + why ); };
        if ( nodeIds.size() != hierarchy.Nodes.size() || residency.FirstRecord.size() != hierarchy.Nodes.size() )
            return failed( std::format( "{} HLOD id list(s) and {} residency node(s) for {} HLOD(s)",
                                        nodeIds.size(), residency.FirstRecord.size(), hierarchy.Nodes.size() ) );
        // The HLOD record space in node order: node n's records are [FirstRecord[n], FirstRecord[n] + its ids).
        std::vector<Common::UUID> recordIds( residency.Records );
        for ( std::size_t n = 0; n < nodeIds.size(); ++n )
        {
            const std::size_t first = residency.FirstRecord[n];
            const std::size_t end   = n + 1 < nodeIds.size() ? residency.FirstRecord[n + 1] : residency.Records;
            if ( end - first != nodeIds[n].size() )
                return failed( std::format( "the HLOD {} has {} record id(s) and its residency {} record(s)",
                                            hierarchy.Nodes[n].Name, nodeIds[n].size(), end - first ) );
            std::copy( nodeIds[n].begin(), nodeIds[n].end(),
                       recordIds.begin() + static_cast<std::ptrdiff_t>( first ) );
        }
        m_HLODHierarchy               = std::move( hierarchy );
        m_HLODResidency               = std::move( residency );
        m_HLODIds                     = std::move( nodeIds ); // per HLOD: its entities' ids
        const Rules::HLODHierarchy& h = *m_HLODHierarchy;
        const Rules::HLODResidency& r = *m_HLODResidency;
        m_HLODCells = std::make_unique<HLODCellWorld>( *this, std::move( source ), std::move( recordIds ) );

        auto runtime = Rules::HLODRuntime::Make( Executor().Plan(), h, r );
        if ( !runtime )
            return failed( runtime.GetError() );
        m_HLODs = runtime.ExtractValue();

        // THE TOP OF EVERY CHAIN, before the first frame and ONE HLOD AT A TIME (CellHLOD's rule): read, made
        // entities, dropped before the next read. Then, the same way, the HLOD cells above the world cells
        // Edit -> Play kept (HLODRuntime::CellsWantedAtBegin): without them the parent standing in for such an
        // HLOD cell would be drawn over those cells until it streamed in (THE SWAP, BY RESIDENCY). The rest
        // stream from the first Tick on, and until theirs activates the parent above stands in.
        const Common::Utils::ProcessFootprint before   = Common::Utils::ReadProcessFootprint();
        const std::vector<std::size_t>        atBegin  = m_HLODs->CellsWantedAtBegin( Executor().State() );
        std::size_t                           made     = 0;
        const auto                            makeUnit = [&]( std::size_t unit ) -> Common::BoolResultStr
        {
            auto records = m_HLODCells->Source().UnitRecords( unit );
            if ( !records )
                return failed( records.GetError() );
            made += records.GetValue().size();
            // What this HLOD cell's HLODs draw, for their warmup (THE WARMUP); a streamed cell's come with its
            // read.
            m_HLODUnitDependencies[unit] =
                 GatherSceneDependencies( records.GetValue(), nullptr, AssetsRoot() ).Guids;
            if ( auto instantiated = SceneSerializer( m_Scene, m_Assets )
                                          .InstantiateRecords( records.GetValue(), m_SceneName, nullptr );
                 !instantiated )
                return failed( "making " + Rules::DescribeHLODUnit( h, r, unit ) +
                               " failed: " + instantiated.GetError() );
            return BOOLSUCCESS;
        };
        // Only the tops of no runtime data layer (WP-HLOD-DATA-LAYERS): one of runtime layers streams by their
        // state from the first Tick, as the world's always-loaded composites of runtime layers do
        // (ResidencyExecutor::BeginFromAlwaysLoaded marks activated exactly the units made here).
        for ( std::size_t a = 0; a < r.Plan.AlwaysLoaded.size(); ++a )
        {
            if ( !Rules::UnitRuntimeDataLayers( r.Plan, r.Partition, a ).empty() )
                continue;
            if ( auto ok = makeUnit( a ); !ok )
                return ok;
        }
        for ( const std::size_t unit : atBegin )
        {
            if ( auto ok = makeUnit( unit ); !ok )
                return ok;
            LOG_INFO( "[WorldPartition] '{0}': {1} loaded at the start of Play, above the cells Edit kept.",
                      m_SceneName, Rules::DescribeHLODUnit( h, r, unit ) );
        }
        auto executor = Rules::ResidencyExecutor::BeginFromAlwaysLoaded( r.Plan, r.Partition, m_Settings,
                                                                         r.Records, {}, atBegin );
        if ( !executor )
            return failed( "the HLOD cells: " + executor.GetError() );
        m_HLODExecutor = executor.ExtractValue();
        // With both residencies' data layer states, as every TickWithHLODs Sync after it (WP-HLOD-DATA-LAYERS).
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): assigned just above
        const Rules::ResidencyExecutor& hlodCells = *m_HLODExecutor;
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): assigned just above
        Rules::HLODRuntime& hlodSwitch = *m_HLODs;
        if ( auto synced = hlodSwitch.Sync( Executor().State(), &hlodCells.State(), *this,
                                            Executor().UnitDataLayerStates(), hlodCells.UnitDataLayerStates() );
             !synced )
            return failed( synced.GetError() );
        m_HLODSwitchesSaid = true;
        // The census the Play start is judged by: only the tops are entities now.
        const std::string holesText =
             holes.empty() ? std::string( "every drawing record has its stand-in" )
                           : std::to_string( holes.size() ) +
                                  " drawing record(s) have none and leave a hole while their cell is not drawn: " +
                                  Rules::DescribeHLODHoles( holes );
        LOG_INFO(
             "[WorldPartition] '{0}': {1} HLOD(s) in {2} streamed HLOD cell(s) and {3} resident for the session "
             "({4} record(s) made one HLOD at a time); process before: {5}; after: {6}; {7}.",
             m_SceneName, h.Nodes.size(), r.Plan.Cells.size(), r.Plan.AlwaysLoaded.size(), made, before.Describe(),
             Common::Utils::ReadProcessFootprint().Describe(), holesText );
        return BOOLSUCCESS;
    }

    void WorldStreamer::NoteImpostorPair( std::span<const std::uint64_t> cards,
                                          std::span<const std::uint64_t> fallbacks )
    {
        m_ImpostorCards.insert( cards.begin(), cards.end() );
        m_ImpostorFallbacks.insert( fallbacks.begin(), fallbacks.end() );
    }

    void WorldStreamer::SyncHLODImpostors()
    {
        // Before Initialize (a tool, a headless test) the machine has no selection: the parameter's own default.
        const bool wanted =
             ( Common::Scalability::QualityState::Initialized()
                    ? Common::Scalability::QualityState::Resolved().As<int>(
                           Common::Scalability::Parameter::ViewDistanceHLODImpostors )
                    : Common::Scalability::SpecOf( Common::Scalability::Parameter::ViewDistanceHLODImpostors )
                           .Default ) != 0;
        if ( wanted == m_HLODImpostors )
            return;
        m_HLODImpostors = wanted;
        if ( m_ImpostorCards.empty() && m_ImpostorFallbacks.empty() )
            return;
        LOG_INFO( "[WorldPartition] '{0}': HLOD impostors {1} (ViewDistance.HLODImpostors); {2} card record(s), {3} "
                  "source-mesh record(s).",
                  m_SceneName, wanted ? "on" : "off", m_ImpostorCards.size(), m_ImpostorFallbacks.size() );
        if ( !m_HLODs.has_value() )
            return;
        for ( std::size_t hlod = 0; hlod < m_HLODs->Count(); ++hlod )
            if ( m_HLODs->IsVisible( hlod ) )
                SetHLODVisible( hlod, true );
    }

    void WorldStreamer::SetHLODVisible( std::size_t hlod, bool visible )
    {
        for ( const Common::UUID& id : m_HLODIds.at( hlod ) )
        {
            // An HLOD entity gameplay destroyed has nothing left to show or hide.
            const auto entity = m_Scene->FindEntityByID( id );
            if ( !entity.has_value() )
                continue;
            // One side of an Impostor layer's pair (m_ImpostorCards / m_ImpostorFallbacks), by the machine.
            const auto key   = static_cast<std::uint64_t>( id );
            const bool other = m_HLODImpostors ? m_ImpostorFallbacks.contains( key ) : m_ImpostorCards.contains( key );
            m_Scene->SetVisibleRecursive( entity->get(), visible && !other );
        }
        if ( !m_HLODSwitchesSaid || !m_HLODs.has_value() )
            return;
        // A grid's stand-in replaces only that grid's cells (UE UWorldPartitionRuntimeHashSet: each runtime
        // partition owns its HLOD setup), so the switch is said with the grid it belongs to.
        if ( m_HLODs->StandsForHLODUnit( hlod ) && m_HLODHierarchy.has_value() && m_HLODResidency.has_value() )
        {
            LOG_INFO( "[WorldPartition] '{0}': HLOD {1} for {2} {3}.", m_SceneName,
                      m_HLODHierarchy->Nodes.at( hlod ).Name,
                      Rules::DescribeHLODUnit( *m_HLODHierarchy, *m_HLODResidency, m_HLODs->UnitOf( hlod ) ),
                      visible ? "shown" : "hidden" );
            return;
        }
        const Rules::WorldPartitionPlan& plan = Executor().Plan();
        const std::size_t                unit = m_HLODs->UnitOf( hlod );
        const std::size_t                grid =
             unit < plan.AlwaysLoaded.size() ? 0 : plan.Cells.at( unit - plan.AlwaysLoaded.size() ).Grid;
        const std::string gridName = grid < m_Partition.RuntimePartitions.size()
                                          ? m_Partition.RuntimePartitions[grid].Name
                                          : std::string( "?" );
        LOG_INFO( "[WorldPartition] '{0}': grid '{1}' HLOD of {2} {3}.", m_SceneName, gridName,
                  Rules::DescribeResidencyUnit( plan, unit ), visible ? "shown" : "hidden" );
    }

    // ONE WARMUP IN FLIGHT: the rows its records name (their closure), the pins that keep them from the eviction
    // sweep while no entity names them yet, and the standing ask every view measures (TextureStreamingDemand).
    struct WorldStreamer::Warming
    {
        std::vector<Assets::ContentRegistry::ClosureRow>            Rows;
        std::vector<std::unique_ptr<Assets::AssetRootPin>>          Pins;
        std::shared_ptr<Graphic::TextureStreamingDemand::WarmupAsk> Ask;
    };

    bool WorldStreamer::WarmUp( std::map<std::size_t, std::unique_ptr<Warming>>& warming, const std::size_t key,
                                const Rules::CellBounds& bounds, const std::string& what,
                                const std::vector<std::string>* guids )
    {
        auto at = warming.find( key );
        if ( at == warming.end() )
        {
            // THE REQUEST, ONCE (UE PrepareToWarmup): the closure read on workers and pinned, and the ask over the
            // cell's bounds - a column (height does not partition), measured at the height of the source nearest
            // to it, the nearest its content can be to that eye.
            auto              made = std::make_unique<Warming>();
            const std::string why  = "HLOD warmup of " + what;
            if ( guids != nullptr )
                made->Rows = Assets::ContentRegistry::ClosureOfGuids( *guids );
            for ( const auto& row : made->Rows )
                made->Pins.push_back( std::make_unique<Assets::AssetRootPin>( row.Handle, why ) );
            Runtime::StartClosureReads( made->Rows );
            const glm::vec2 centre( 0.5f * ( bounds.MinX + bounds.MaxX ), 0.5f * ( bounds.MinZ + bounds.MaxZ ) );
            float           eyeHeight = 0.0f;
            float           nearest   = std::numeric_limits<float>::max();
            for ( const Rules::StreamingSource& source : m_LastSources )
            {
                const float distance = glm::length( glm::vec2( source.Position.x, source.Position.z ) - centre );
                if ( distance < nearest )
                {
                    nearest   = distance;
                    eyeHeight = source.Position.y;
                }
            }
            made->Ask         = std::make_shared<Graphic::TextureStreamingDemand::WarmupAsk>();
            made->Ask->Center = glm::vec3( centre.x, eyeHeight, centre.y );
            made->Ask->Radius =
                 0.5f * glm::length( glm::vec2( bounds.MaxX - bounds.MinX, bounds.MaxZ - bounds.MinZ ) );
            made->Ask->Textures = Runtime::ClosureTextures( made->Rows );
            at                  = warming.emplace( key, std::move( made ) ).first;
            PublishWarmups();
        }
        // THE FACT (UE counts WarmupNumFrames instead): every row drawn as it will be asked for.
        const Warming& warm = *at->second;
        return Runtime::ClosureArriving( warm.Rows, warm.Ask->Pixels.load( std::memory_order_relaxed ) ) == 0;
    }

    void WorldStreamer::PublishWarmups()
    {
        std::vector<std::shared_ptr<Graphic::TextureStreamingDemand::WarmupAsk>> asks;
        for ( const auto& [unit, warm] : m_WarmCells )
            asks.push_back( warm->Ask );
        for ( const auto& [hlod, warm] : m_WarmHLODs )
            asks.push_back( warm->Ask );
        for ( const auto& [unit, warm] : m_WarmHLODCells )
            asks.push_back( warm->Ask );
        Graphic::TextureStreamingDemand::Get().SetWarmups( this, m_Scene, std::move( asks ) );
    }

    bool WorldStreamer::WarmCell( const std::size_t unit )
    {
        // UE ShouldPerformWarmupForCell: a blocking load does not warm. While Play waits for the cell under a
        // source (BlocksPlay) the loading overlay is up, no swap is seen, and holding cells would only lengthen
        // it. The cell under a source is what Play waits for (CRITICAL): holding it would stop the game for a
        // frame before the blocking load above lets it go, so it is shown the step it is read.
        if ( m_FramesWaiting > 0 || UnderSource( unit ) )
            return true;
        const Rules::WorldPartitionPlan& plan = Executor().Plan();
        return WarmUp( m_WarmCells, unit, UnitBounds( plan, unit ), Rules::DescribeResidencyUnit( plan, unit ),
                       m_Loader->Dependencies( unit ) );
    }

    bool WorldStreamer::UnderSource( const std::size_t unit )
    {
        const Rules::WorldPartitionPlan& plan = Executor().Plan();
        if ( unit < plan.AlwaysLoaded.size() )
            return false;
        const Rules::PlannedCell& cell = plan.Cells.at( unit - plan.AlwaysLoaded.size() );
        return std::any_of( m_LastSources.begin(), m_LastSources.end(), [&]( const Rules::StreamingSource& source )
                            { return Rules::CellHolds( cell, source, m_Partition ); } );
    }

    bool WorldStreamer::WarmHLODCell( const std::size_t unit )
    {
        if ( m_FramesWaiting > 0 )
            return true; // a blocking load does not warm (WarmCell)
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asked only through a streamed hierarchy's gate
        const Rules::HLODResidency& residency = *m_HLODResidency;
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the hierarchy is held with its residency
        return WarmUp( m_WarmHLODCells, unit, UnitBounds( residency.Plan, unit ),
                       Rules::DescribeHLODUnit( *m_HLODHierarchy, residency, unit ),
                       m_HLODCells->Dependencies( unit ) );
    }

    void WorldStreamer::EndHLODCellWarmup( const std::size_t unit )
    {
        if ( m_WarmHLODCells.erase( unit ) > 0 )
            PublishWarmups();
    }

    bool WorldStreamer::WarmHLOD( const std::size_t hlod )
    {
        if ( m_FramesWaiting > 0 )
            return true; // a blocking load does not warm (WarmCell)
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asked only by the switch this streamer holds
        const Rules::HLODRuntime&       hlods = *m_HLODs;
        const std::vector<std::string>* guids = nullptr;
        if ( !hlods.Streamed() )
            guids = hlod < m_HLODDependencies.size() ? &m_HLODDependencies[hlod] : nullptr;
        else
        {
            // A streamed HLOD's entities are its HLOD cell's (HLODGate keeps that cell while the world cell
            // beneath is drawn): what that cell's records name, gathered at Begin or with its read.
            const std::size_t residesIn = hlods.ResidesIn( hlod );
            const auto        begun     = m_HLODUnitDependencies.find( residesIn );
            guids =
                 begun != m_HLODUnitDependencies.end() ? &begun->second : m_HLODCells->Dependencies( residesIn );
        }
        // The rectangle the HLOD stands in for: a world cell's, or (a parent) its HLOD cell's.
        const std::size_t unit = hlods.UnitOf( hlod );
        if ( hlods.StandsForHLODUnit( hlod ) )
        {
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): a parent exists only in a streamed hierarchy
            const Rules::HLODResidency& residency = *m_HLODResidency;
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the hierarchy is held with its residency
            return WarmUp( m_WarmHLODs, hlod, UnitBounds( residency.Plan, unit ),
                           Rules::DescribeHLODUnit( *m_HLODHierarchy, residency, unit ), guids );
        }
        const Rules::WorldPartitionPlan& plan = Executor().Plan();
        return WarmUp( m_WarmHLODs, hlod, UnitBounds( plan, unit ), Rules::DescribeResidencyUnit( plan, unit ),
                       guids );
    }

    void WorldStreamer::EndCellWarmup( const std::size_t unit )
    {
        if ( m_WarmCells.erase( unit ) > 0 )
            PublishWarmups();
    }

    void WorldStreamer::EndHLODWarmup( const std::size_t hlod )
    {
        if ( m_WarmHLODs.erase( hlod ) > 0 )
            PublishWarmups();
    }

    WorldStreamer::~WorldStreamer()
    {
        if ( m_Scene != nullptr )
        {
            entt::registry& registry = m_Scene->GetRegistry();
            if ( const auto* ref = registry.try_ctx<WorldStreamerOfScene>(); ref != nullptr && ref->Streamer == this )
            {
                registry.unset<WorldStreamerOfScene>();
                registry.unset<WorldPartitionStreamingEvents>();
            }
        }
        // The standing asks name this streamer's warmups; they go with it.
        Graphic::TextureStreamingDemand::Get().SetWarmups( this, nullptr, {} );
        // The scene outlives its streamer (hosts reset the streamer before clearing the scene); what the
        // streamer said about it must not outlive the streamer.
        if ( m_Scene != nullptr )
            m_Scene->GetRegistry().unset<WorldStreamingWait>();
#if DESERT_DEV_INSTRUMENTS
        if ( m_Scene != nullptr && m_Scene->GetGameplayDebugger().GetWorldStreamer() == this )
            m_Scene->GetGameplayDebugger().SetWorldStreamer( nullptr );
#endif
        if ( !m_Executor.has_value() )
            return;
        LOG_INFO(
             "[WorldPartition] '{0}': streamed {1} activation(s), {2} record(s) in {3} slice(s), {4:.2f} ms "
             "on the main thread ({5:.4f} ms per record, worst slice {6:.2f} ms, slowest unit {7:.2f} ms over "
             "its frames; per kind: {8}), their reads {9:.2f} ms on workers; at most {10} of {11} record(s) "
             "resident at once.",
             m_SceneName, m_Activations, m_RecordsActivated, m_Slices, m_ActivationMs,
             m_RecordsActivated > 0 ? m_ActivationMs / static_cast<double>( m_RecordsActivated ) : 0.0,
             m_WorstSliceMs, m_WorstUnitMs, Executor().ActivationCost().Describe(), m_Loader->WorkerMs(),
             m_MostResident, m_RecordIds.size() );
    }
} // namespace Desert::Core
