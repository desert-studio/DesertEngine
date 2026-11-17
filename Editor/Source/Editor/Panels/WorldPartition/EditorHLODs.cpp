#include "EditorHLODs.hpp"

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/EntityPackages.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditorHLODComponent.hpp>

#include <Common/Core/Logger.hpp>

#include <filesystem>
#include <format>
#include <unordered_map>
#include <utility>

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

    Common::BoolResultStr EditorHLODs::Make( ::Desert::Core::Scene& scene, ::Desert::Assets::AssetManager* assets )
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

        const ::Desert::Core::SceneSerializer serializer( &scene, assets );
        std::vector<std::size_t>              units;
        std::size_t                           missing = 0;
        std::size_t                           records = 0;
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
            // ONE CELL'S RECORDS ALIVE AT A TIME (WorldStreamer::CellHLOD): read, made, dropped before the next.
            auto read = WC::StoredCellRecords( stored.GetValue(), *row->second );
            if ( !read )
                return refuse( name + ": " + read.GetError() );
            const std::size_t hlod = m_HLODs.size();
            MadeHLOD&         made = m_HLODs.emplace_back();
            made.Unit              = unit;
            for ( const ::Desert::Assets::EntityData& record : read.GetValue() )
            {
                if ( !record.id.has_value() )
                    continue;
                if ( scene.FindEntityByID( *record.id ).has_value() )
                    return refuse( std::format( "an HLOD record of {} has the id of an entity the scene holds; "
                                                "rename the world",
                                                name ) );
                made.Ids.push_back( *record.id );
            }
            // Listed before it is made: a refusal half-way destroys what was made of this cell too.
            if ( auto instantiated = serializer.InstantiateRecords( read.GetValue(), scene.GetSceneName(), nullptr );
                 !instantiated )
                return refuse( name + ": " + instantiated.GetError() );
            for ( const Common::UUID& id : m_HLODs[hlod].Ids )
                if ( const auto entity = scene.FindEntityByID( id ) )
                    TagTree( scene.GetRegistry(), entity->get().GetHandle(), hlod );
            records += m_HLODs[hlod].Ids.size();
            units.push_back( unit );
        }
        auto runtime = Rules::HLODRuntime::Make( plan, units );
        if ( !runtime )
            return refuse( runtime.GetError() );
        m_Runtime = runtime.ExtractValue();
        // Hidden until the switch and the user's view say otherwise (Apply).
        for ( MadeHLOD& made : m_HLODs )
        {
            for ( const Common::UUID& id : made.Ids )
                if ( const auto entity = scene.FindEntityByID( id ) )
                    scene.SetVisibleRecursive( entity->get(), false );
            made.Drawn = false;
        }
        m_Status = std::format( "{} saved HLOD(s), {} record(s); {} cell(s) have no saved HLOD.", m_HLODs.size(),
                                records, missing );
        LOG_INFO( "[WorldPartition] Editor HLODs of '{0}': {1}", path.string(), m_Status );
        return SyncHeld( scene );
    }

    void EditorHLODs::Release( ::Desert::Core::Scene& scene )
    {
        for ( const MadeHLOD& made : m_HLODs )
            for ( const Common::UUID& id : made.Ids )
                if ( const auto found = scene.FindEntityByID( id ) )
                {
                    const ECS::Entity entity = found->get();
                    scene.DestroyEntity( entity );
                }
        Forget();
    }

    void EditorHLODs::Forget()
    {
        m_HLODs.clear();
        m_Runtime.reset();
        m_World.reset();
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
            if ( !made.Ids.empty() )
                return scene.FindEntityByID( made.Ids.front() ).has_value();
        return true;
    }

    Common::BoolResultStr EditorHLODs::SyncHeld( ::Desert::Core::Scene& scene )
    {
        if ( !m_Runtime.has_value() || !m_World.has_value() )
            return BOOLSUCCESS;
        const ::Desert::Core::EntityPackages& packages = *scene.Packages();
        const std::vector<std::uint64_t>&     rowIds   = m_World->RowIds;
        const Rules::ResidencyState           state    = Rules::EditorResidency(
             m_World->Plan, [&]( const std::size_t record )
             { return record < rowIds.size() && packages.IsLoaded( Common::UUID( rowIds[record] ) ); } );
        if ( auto synced = m_Runtime->Sync( state, *this ); !synced )
            return Common::MakeError( "editor HLODs: " + synced.GetError() );
        return BOOLSUCCESS;
    }

    void EditorHLODs::Apply( ::Desert::Core::Scene& scene, const EditorHLODView& view,
                             const std::optional<glm::vec3>& eye )
    {
        if ( !m_World.has_value() )
            return;
        const Rules::WorldPartitionPlan& plan = m_World->Plan;
        for ( MadeHLOD& made : m_HLODs )
        {
            bool drawn = made.StandsIn && view.Show;
            if ( drawn && eye.has_value() )
            {
                const Rules::PlannedCell& cell = plan.Cells.at( made.Unit - plan.AlwaysLoaded.size() );
                drawn = Rules::InEditorHLODRange( cell.Bounds, cell.Height, eye->x, eye->y, eye->z, view.MinDistance,
                                                  view.MaxDistance );
            }
            if ( made.Drawn == drawn )
                continue;
            for ( const Common::UUID& id : made.Ids )
                if ( const auto entity = scene.FindEntityByID( id ) )
                    scene.SetVisibleRecursive( entity->get(), drawn );
            made.Drawn = drawn;
        }
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
