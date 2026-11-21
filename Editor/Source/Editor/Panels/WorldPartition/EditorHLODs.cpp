#include "EditorHLODs.hpp"

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/EntityPackages.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/ECS/EditorHLODComponent.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <Engine/ECS/RelationshipComponent.hpp>

namespace Desert::Editor
{
    namespace
    {
        namespace Rules = ::Desert::Core::Rules;
        namespace WC    = ::Desert::Core::WorldCells;

        // Tags @p root and everything below it (a record that instantiates a prefab brings children).
        void TagTree( entt::registry& registry, entt::entity root, std::size_t hlod )
        {
            std::vector<entt::entity> open{ root };
            while ( !open.empty() )
            {
                const entt::entity entity = open.back();
                open.pop_back();
                if ( !registry.has<ECS::EditorHLODComponent>( entity ) )
                    registry.emplace<ECS::EditorHLODComponent>( entity, ECS::EditorHLODComponent{ hlod } );
                if ( registry.has<ECS::RelationshipComponent>( entity ) )
                {
                    const std::vector<entt::entity>& children =
                         registry.get<ECS::RelationshipComponent>( entity ).Children;
                    open.insert( open.end(), children.begin(), children.end() );
                }
            }
        }
    } // namespace

    Common::BoolResultStr EditorHLODs::Make( ::Desert::Core::Scene& scene, ::Desert::Assets::AssetManager* assets,
                                             const std::vector<bool>& layerLoaded )
    {
        Release( scene );
        m_Status.clear();
        const auto& partition = scene.GetWorldPartition();
        const auto& baseline  = scene.Packages()->BaselinePath();
        if ( !partition.has_value() )
        {
            m_Status = "The scene is not a partitioned world.";
            return BOOLSUCCESS;
        }
        if ( !baseline )
        {
            m_Status = "The world is not saved: its HLODs are built from its files.";
            return BOOLSUCCESS;
        }
        if ( !Rules::AnyGridHLODLayer( *partition ) )
        {
            m_Status = "No runtime grid of this world names an HLOD layer.";
            return BOOLSUCCESS;
        }
        const std::filesystem::path path = *baseline;
        auto                        world = ::Desert::Core::EditorRegions::PlanWorldOnDisk( path, *partition );
        if ( !world )
            return Common::MakeError( std::format( "editor HLODs of '{}': {}", path.string(), world.GetError() ) );
        auto stored = WC::ReadBuiltHLODs( path );
        if ( !stored )
        {
            m_Status = std::format( "No saved HLODs ({}). Run Build > Build HLODs.", stored.GetError() );
            LOG_WARN( "[WorldPartition] Editor HLODs of '{0}': {1}", path.string(), m_Status );
            return BOOLSUCCESS;
        }
        m_World                               = world.ExtractValue();
        const Rules::WorldPartitionPlan& plan = m_World->Plan;
        std::unordered_map<std::string, const WC::BuiltHLODCell*> rows;
        for ( const WC::BuiltHLODCell& row : stored.GetValue().Index.Cells )
            rows.emplace( row.Unit, &row );

        // Every record of the world, held or not: an HLOD record with one's id would be made over it the moment a
        // region load brings it (Play refuses the same world by the same test). The index lists each HLOD's record
        // ids (StoredCellRecords checks its file against them), so the test needs no proxy file.
        const std::unordered_set<std::uint64_t> worldIds( m_World->RowIds.begin(), m_World->RowIds.end() );
        std::vector<std::size_t>                units;
        std::size_t                             missing = 0;
        std::size_t                             records = 0;
        const auto refuse = [&]( const std::string& why )
        {
            Release( scene );
            return Common::MakeError( std::format( "editor HLODs of '{}': {}", path.string(), why ) );
        };
        for ( std::size_t unit = plan.AlwaysLoaded.size(); unit < Rules::ResidencyUnitCount( plan ); ++unit )
        {
            const std::string name = Rules::DescribeResidencyUnit( plan, unit );
            const auto        row  = rows.find( name );
            if ( row == rows.end() )
            {
                ++missing;
                continue;
            }
            // A row without a file, or whose file lists no record, stands in for nothing (as in Play).
            if ( !row->second->File.has_value() || row->second->Ids.empty() )
                continue;
            MadeHLOD& made = m_HLODs.emplace_back();
            made.Unit      = unit;
            made.Row       = *row->second;
            for ( const std::uint64_t id : made.Row.Ids )
            {
                if ( worldIds.contains( id ) || scene.FindEntityByID( Common::UUID( id ) ).has_value() )
                    return refuse(
                         std::format( "an HLOD record of {} has the id of a record of the world or of an "
                                      "entity the scene holds; rename the world",
                                      name ) );
                made.Ids.emplace_back( id );
            }
            records += made.Ids.size();
            units.push_back( unit );
        }
        auto runtime = Rules::HLODRuntime::Make( plan, units );
        if ( !runtime )
            return refuse( runtime.GetError() );
        m_Runtime = runtime.ExtractValue();
        m_Reader  = std::move( stored.GetValue().Reader );
        m_Assets  = assets;
        m_Status  = std::format( "{} saved HLOD(s), {} record(s), each read when drawn; {} cell(s) have no saved HLOD.",
                                 m_HLODs.size(), records, missing );
        LOG_INFO( "[WorldPartition] Editor HLODs of '{0}': {1}", path.string(), m_Status );
        return SyncHeld( scene, layerLoaded );
    }

    void EditorHLODs::Release( ::Desert::Core::Scene& scene )
    {
        for ( const MadeHLOD& made : m_HLODs )
        {
            if ( made.State != Proxy::Made )
                continue; // never made, or refused (MakeProxy destroyed what it made)
            for ( const Common::UUID& id : made.Ids )
                if ( const auto found = scene.FindEntityByID( id ) )
                {
                    const ECS::Entity entity = found->get();
                    scene.DestroyEntity( entity );
                }
        }
        Forget();
    }

    void EditorHLODs::Forget()
    {
        // A read in flight is dropped with its future (a promise's future does not wait): its worker finishes and
        // nobody takes what it read.
        m_HLODs.clear();
        m_Runtime.reset();
        m_World.reset();
        m_Reader  = {};
        m_Waiting = 0;
    }

    void EditorHLODs::ReleaseAll( ::Desert::Core::Scene& scene )
    {
        entt::registry&           registry = scene.GetRegistry();
        std::vector<entt::entity> tagged;
        for ( const entt::entity entity : registry.view<ECS::EditorHLODComponent>() )
            tagged.push_back( entity );
        // A child goes with its parent (DestroyEntity takes the subtree): it is no longer valid when its turn comes.
        for ( const entt::entity entity : tagged )
            if ( registry.valid( entity ) )
                scene.DestroyEntity( ECS::Entity( entity, registry ) );
    }

    bool EditorHLODs::IsInScene( const ::Desert::Core::Scene& scene ) const
    {
        for ( const MadeHLOD& made : m_HLODs )
            if ( made.State == Proxy::Made && !made.Ids.empty() )
                return scene.FindEntityByID( made.Ids.front() ).has_value();
        return true;
    }

    Common::BoolResultStr EditorHLODs::SyncHeld( ::Desert::Core::Scene&   scene,
                                                 const std::vector<bool>& layerLoaded )
    {
        if ( !m_Runtime.has_value() || !m_World.has_value() )
            return BOOLSUCCESS;
        const ::Desert::Core::EntityPackages& packages = *scene.Packages();
        const std::vector<std::uint64_t>&     rowIds   = m_World->RowIds;
        const Rules::ResidencyState           state    = Rules::EditorResidency(
             m_World->Plan, [&]( const std::size_t record )
             { return record < rowIds.size() && packages.IsLoaded( Common::UUID( rowIds[record] ) ); } );
        // The HLOD of a cell of runtime layers none of which is loaded in the editor stands in for nothing: the
        // editor holds a composite of layers only while any of them is loaded (UE ResolveIsLoadedInEditor), and
        // the HLOD carries the cell's layers. The switch's own layer gate (Sync) hides it.
        using LayerState                      = ::Desert::Core::DataLayerRuntimeState;
        const Rules::WorldPartitionPlan& plan = m_World->Plan;
        std::vector<LayerState>          layers( Rules::ResidencyUnitCount( plan ), LayerState::Activated );
        for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
        {
            const std::vector<std::size_t>& of     = plan.Cells[cell].DataLayers;
            bool                            loaded = of.empty();
            for ( const std::size_t layer : of )
                loaded = loaded || ( layer < layerLoaded.size() && layerLoaded[layer] );
            if ( !loaded )
                layers[plan.AlwaysLoaded.size() + cell] = LayerState::Unloaded;
        }
        if ( auto synced = m_Runtime->Sync( state, nullptr, *this, layers ); !synced )
            return Common::MakeError( "editor HLODs: " + synced.GetError() );
        return BOOLSUCCESS;
    }

    void EditorHLODs::Apply( ::Desert::Core::Scene& scene, const EditorHLODView& view,
                             const std::optional<glm::vec3>& eye )
    {
        m_Waiting = 0;
        if ( !m_World.has_value() )
            return;
        const Rules::WorldPartitionPlan& plan    = m_World->Plan;
        bool                             madeOne = false;
        for ( std::size_t hlod = 0; hlod < m_HLODs.size(); ++hlod )
        {
            MadeHLOD& made   = m_HLODs[hlod];
            bool      wanted = made.StandsIn && view.Show;
            if ( wanted && eye.has_value() )
            {
                const Rules::PlannedCell& cell = plan.Cells.at( made.Unit - plan.AlwaysLoaded.size() );
                wanted = Rules::InEditorHLODRange( cell.Bounds, cell.Height, eye->x, eye->y, eye->z,
                                                   view.MinDistance, view.MaxDistance );
            }
            if ( wanted && made.State == Proxy::Unread )
                StartRead( made );
            // One landed proxy made per frame: making its entities (and its mesh) is the main thread's part.
            if ( !madeOne && made.State == Proxy::Reading &&
                 made.Read.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready )
            {
                madeOne = true;
                MakeProxy( scene, hlod );
            }
            if ( wanted && ( made.State == Proxy::Unread || made.State == Proxy::Reading ) )
                ++m_Waiting;
            const bool drawn = wanted && made.State == Proxy::Made;
            if ( made.Drawn == drawn )
                continue;
            for ( const Common::UUID& id : made.Ids )
                if ( const auto entity = scene.FindEntityByID( id ) )
                    scene.SetVisibleRecursive( entity->get(), drawn );
            made.Drawn = drawn;
        }
    }

    void EditorHLODs::StartRead( MadeHLOD& made )
    {
        made.State = Proxy::Reading;
        made.Read  = Common::JobSystem::Get().Async(
             [reader = m_Reader, row = made.Row]() noexcept -> ProxyRecords
             {
                 WC::StoredHLODs stored;
                 stored.Reader = reader;
                 return WC::StoredCellRecords( stored, row );
             },
             Common::JobPriority::Background );
    }

    void EditorHLODs::MakeProxy( ::Desert::Core::Scene& scene, const std::size_t hlod )
    {
        MadeHLOD&    made = m_HLODs[hlod];
        ProxyRecords read = made.Read.get();
        const auto   refuse = [&]( const std::string& why )
        {
            for ( const Common::UUID& id : made.Ids )
                if ( const auto entity = scene.FindEntityByID( id ) )
                    scene.DestroyEntity( entity->get() );
            made.State = Proxy::Refused;
            m_Status   = std::format( "The saved HLOD of {} was refused: {}", made.Row.Unit, why );
            LOG_ERROR( "[WorldPartition] Editor HLOD of {0}: {1}", made.Row.Unit, why );
        };
        if ( !read )
            return refuse( read.GetError() );
        // Make checked these ids against the world; an entity made since (a paste, a new entity) is checked here.
        for ( const Common::UUID& id : made.Ids )
            if ( scene.FindEntityByID( id ).has_value() )
                return refuse( "an HLOD record has the id of an entity the scene holds; rename the world" );
        const ::Desert::Core::SceneSerializer serializer( &scene, m_Assets );
        if ( auto instantiated = serializer.InstantiateRecords( read.GetValue(), scene.GetSceneName(), nullptr );
             !instantiated )
            return refuse( instantiated.GetError() );
        // Hidden until Apply draws it (this frame, when it is still wanted).
        for ( const Common::UUID& id : made.Ids )
            if ( const auto entity = scene.FindEntityByID( id ) )
            {
                TagTree( scene.GetRegistry(), entity->get().GetHandle(), hlod );
                scene.SetVisibleRecursive( entity->get(), false );
            }
        made.Drawn = false;
        made.State = Proxy::Made;
    }

    std::size_t EditorHLODs::DrawnCount() const noexcept
    {
        std::size_t drawn = 0;
        for ( const MadeHLOD& made : m_HLODs )
            if ( made.Drawn == true )
                ++drawn;
        return drawn;
    }

    void EditorHLODs::SetHLODVisible( const std::size_t hlod, const bool visible )
    {
        m_HLODs.at( hlod ).StandsIn = visible;
    }
} // namespace Desert::Editor
