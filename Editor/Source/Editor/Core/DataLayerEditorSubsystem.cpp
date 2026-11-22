#include "DataLayerEditorSubsystem.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <entt/entt.hpp>

#include <algorithm>
#include <format>
#include <functional>
#include <utility>
#include <Engine/ECS/RelationshipComponent.hpp>
#include <Engine/ECS/UUIDComponent.hpp>
#include <Engine/World/Partition/DataLayersComponent.hpp>
#include <Engine/World/Partition/HiddenByDataLayersComponent.hpp>

namespace Desert::Editor::DataLayerEditor
{
    namespace Rules = ::Desert::Core::Rules;

    namespace
    {
        // One world's session state. `Visible` holds only the layers this session toggled, against the world's
        // InitiallyVisible, so a layer made later starts at its own default.
        struct WorldState
        {
            std::weak_ptr<::Desert::Core::Scene>      World;
            std::vector<std::pair<std::string, bool>> Visible;
            std::vector<std::string>                  Current;
        };

        std::vector<WorldState>& States()
        {
            static std::vector<WorldState> states;
            return states;
        }

        // The state of @p scene, or null when this session has not touched its layers. A gone world's entry is
        // dropped first, so a new scene at a freed address never inherits it.
        WorldState* Find( const ::Desert::Core::Scene& scene )
        {
            std::vector<WorldState>& states = States();
            std::erase_if( states, []( const WorldState& state ) { return state.World.expired(); } );
            for ( WorldState& state : states )
                if ( state.World.lock().get() == &scene )
                    return &state;
            return nullptr;
        }

        WorldState& Obtain( const std::shared_ptr<::Desert::Core::Scene>& scene )
        {
            if ( WorldState* found = Find( *scene ) )
                return *found;
            States().push_back( WorldState{ scene, {}, {} } );
            return States().back();
        }

        Common::BoolResultStr RefuseUnknown( const ::Desert::Core::Scene* scene, const std::string& layer )
        {
            if ( scene == nullptr || !scene->GetWorldPartition().has_value() )
                return Common::MakeError<bool>( std::string( "Data Layers: the world is not partitioned." ) );
            if ( !Rules::DataLayerIndexOf( *scene->GetWorldPartition(), layer ).has_value() )
                return Common::MakeError<bool>(
                     std::format( "Data Layers: the world states no layer '{}'.", layer ) );
            return Common::MakeSuccess( true );
        }

        // @p entity and every entity of the hierarchy under it, itself first.
        void ForEachInSubtree( entt::registry& registry, const entt::entity entity,
                               const std::function<void( entt::entity )>& visit )
        {
            visit( entity );
            if ( const auto* relationship = registry.try_get<ECS::RelationshipComponent>( entity ) )
            {
                const std::vector<entt::entity> children = relationship->Children; // visit may move the pool
                for ( const entt::entity child : children )
                    if ( registry.valid( child ) )
                        ForEachInSubtree( registry, child, visit );
            }
        }

        // The entity that hangs off nothing above @p entity: the root of its hierarchy, its composite's anchor.
        entt::entity RootOf( const entt::registry& registry, entt::entity entity )
        {
            while ( const auto* relationship = registry.try_get<ECS::RelationshipComponent>( entity ) )
            {
                if ( relationship->Parent == entt::null || !registry.valid( relationship->Parent ) )
                    break;
                entity = relationship->Parent;
            }
            return entity;
        }

        bool StatesLayer( const entt::registry& registry, const entt::entity entity, const std::string& layer )
        {
            const auto* member = registry.try_get<ECS::DataLayersComponent>( entity );
            return member != nullptr && std::find( member->Data.Layers.begin(), member->Data.Layers.end(),
                                                   layer ) != member->Data.Layers.end();
        }

        void JoinLayer( entt::registry& registry, const entt::entity entity, const std::string& layer )
        {
            if ( StatesLayer( registry, entity, layer ) )
                return;
            if ( auto* member = registry.try_get<ECS::DataLayersComponent>( entity ) )
                member->Data.Layers.push_back( layer );
            else
                registry.emplace<ECS::DataLayersComponent>( entity ).Data.Layers.push_back( layer );
        }

        void LeaveLayer( entt::registry& registry, const entt::entity entity, const std::string& layer )
        {
            auto* member = registry.try_get<ECS::DataLayersComponent>( entity );
            if ( member == nullptr )
                return;
            std::erase( member->Data.Layers, layer );
            if ( member->Data.Layers.empty() )
                registry.remove<ECS::DataLayersComponent>( entity ); // absent = no layer, never an empty list
        }

        // The roots of @p entities' hierarchies, each once, by UUID.
        std::vector<Common::UUID> CompositeRoots( const ::Desert::Core::Scene&     scene,
                                                  const std::vector<Common::UUID>& entities )
        {
            const entt::registry&     registry = scene.GetRegistry();
            std::vector<Common::UUID> roots;
            for ( const Common::UUID& uuid : entities )
            {
                const auto found = scene.FindEntityByID( uuid );
                if ( !found )
                    continue;
                const entt::entity root = RootOf( registry, found->get().GetHandle() );
                if ( !registry.has<ECS::UUIDComponent>( root ) )
                    continue;
                const Common::UUID rootId = registry.get<ECS::UUIDComponent>( root ).UUID;
                if ( std::find( roots.begin(), roots.end(), rootId ) == roots.end() )
                    roots.push_back( rootId );
            }
            return roots;
        }

        Common::ResultStr<std::size_t> EditMembership( ::Desert::Core::Scene&           scene,
                                                       const std::vector<Common::UUID>& entities,
                                                       const std::string& layer, const bool join )
        {
            if ( const auto known = RefuseUnknown( &scene, layer ); !known )
                return Common::MakeError<std::size_t>( known.GetError() );
            if ( entities.empty() )
                return Common::MakeError<std::size_t>( std::format( "Data Layers: nothing is selected to {} '{}'.",
                                                                    join ? "add to" : "remove from", layer ) );

            entt::registry&                 registry = scene.GetRegistry();
            const std::vector<Common::UUID> roots    = CompositeRoots( scene, entities );
            std::vector<entt::entity>       changing;
            for ( const Common::UUID& root : roots )
                if ( const auto found = scene.FindEntityByID( root ) )
                    ForEachInSubtree( registry, found->get().GetHandle(),
                                      [&]( const entt::entity entity )
                                      {
                                          if ( StatesLayer( registry, entity, layer ) != join )
                                              changing.push_back( entity );
                                      } );
            if ( changing.empty() )
                return Common::MakeSuccess( std::size_t{ 0 } );

            Commands::MutateEntitiesUndoable(
                 roots, std::format( "{} data layer '{}'", join ? "Add to" : "Remove from", layer ),
                 [&]
                 {
                     for ( const entt::entity entity : changing )
                     {
                         if ( join )
                             JoinLayer( registry, entity, layer );
                         else
                             LeaveLayer( registry, entity, layer );
                     }
                 } );
            return Common::MakeSuccess( changing.size() );
        }
    } // namespace

    bool IsVisible( const ::Desert::Core::Scene& scene, const std::string& layer )
    {
        const auto& partition = scene.GetWorldPartition();
        const auto  index =
             partition.has_value() ? Rules::DataLayerIndexOf( *partition, layer ) : std::optional<std::size_t>();
        if ( !index.has_value() )
            return true;
        if ( const WorldState* state = Find( scene ) )
            for ( const auto& [name, visible] : state->Visible )
                if ( name == layer )
                    return visible;
        return ( *partition->DataLayers )[*index].InitiallyVisible;
    }

    std::vector<bool> EffectiveVisibility( const ::Desert::Core::Scene& scene )
    {
        const auto& partition = scene.GetWorldPartition();
        if ( !partition.has_value() || !partition->DataLayers.has_value() )
            return {};
        std::vector<bool> own;
        own.reserve( partition->DataLayers->size() );
        for ( const ::Desert::Core::DataLayerSerialized& layer : *partition->DataLayers )
            own.push_back( IsVisible( scene, layer.Name ) );
        return Rules::EffectiveDataLayerFlags( *partition, own );
    }

    Common::BoolResultStr SetVisible( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                      const std::string& layer, const bool visible )
    {
        if ( const auto known = RefuseUnknown( scene.get(), layer ); !known )
            return Common::MakeError<bool>( known.GetError() );
        const bool  before = IsVisible( *scene, layer );
        WorldState& state  = Obtain( scene );
        std::erase_if( state.Visible, [&]( const auto& entry ) { return entry.first == layer; } );
        const auto index = Rules::DataLayerIndexOf( *scene->GetWorldPartition(), layer );
        if ( visible != ( *scene->GetWorldPartition()->DataLayers )[*index].InitiallyVisible )
            state.Visible.emplace_back( layer, visible );
        return Common::MakeSuccess( before != visible );
    }

    std::size_t UpdateEntityVisibility( ::Desert::Core::Scene& scene )
    {
        entt::registry&         registry  = scene.GetRegistry();
        const auto&             partition = scene.GetWorldPartition();
        const bool              playing   = scene.IsPlaying();
        const std::vector<bool> visible   = playing ? std::vector<bool>() : EffectiveVisibility( scene );

        std::vector<entt::entity> hide;
        std::vector<entt::entity> show;
        for ( const entt::entity entity : registry.view<ECS::DataLayersComponent>() )
        {
            bool hidden = false;
            if ( !playing && partition.has_value() )
            {
                bool inLayer = false;
                bool drawn   = false;
                for ( const std::string& name : registry.get<ECS::DataLayersComponent>( entity ).Data.Layers )
                    if ( const auto index = Rules::DataLayerIndexOf( *partition, name );
                         index.has_value() && *index < visible.size() )
                    {
                        inLayer = true;
                        drawn   = drawn || visible[*index];
                    }
                hidden = inLayer && !drawn; // UE: visible while ANY of its layers is
            }
            if ( hidden != registry.has<ECS::HiddenByDataLayersComponent>( entity ) )
                ( hidden ? hide : show ).push_back( entity );
        }
        // A member that left every layer (its component removed) is no longer hidden by one.
        for ( const entt::entity entity :
              registry.view<ECS::HiddenByDataLayersComponent>( entt::exclude<ECS::DataLayersComponent> ) )
            show.push_back( entity );

        for ( const entt::entity entity : hide )
        {
            registry.emplace<ECS::HiddenByDataLayersComponent>( entity );
            if ( registry.has<ECS::UUIDComponent>( entity ) )
                Core::SelectionManager::RemoveFromSelection( registry.get<ECS::UUIDComponent>( entity ).UUID );
        }
        for ( const entt::entity entity : show )
            registry.remove<ECS::HiddenByDataLayersComponent>( entity );
        return hide.size() + show.size();
    }

    bool IsCurrent( const ::Desert::Core::Scene& scene, const std::string& layer )
    {
        const std::vector<std::string> current = CurrentLayers( scene );
        return std::find( current.begin(), current.end(), layer ) != current.end();
    }

    std::vector<std::string> CurrentLayers( const ::Desert::Core::Scene& scene )
    {
        std::vector<std::string> current;
        const WorldState*        state     = Find( scene );
        const auto&              partition = scene.GetWorldPartition();
        if ( state == nullptr || !partition.has_value() || !partition->DataLayers.has_value() )
            return current;
        for ( const ::Desert::Core::DataLayerSerialized& layer : *partition->DataLayers )
            if ( std::find( state->Current.begin(), state->Current.end(), layer.Name ) != state->Current.end() )
                current.push_back( layer.Name );
        return current;
    }

    Common::BoolResultStr SetCurrent( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                      const std::string& layer, const bool current )
    {
        if ( const auto known = RefuseUnknown( scene.get(), layer ); !known )
            return Common::MakeError<bool>( known.GetError() );
        WorldState& state  = Obtain( scene );
        const bool  before = std::find( state.Current.begin(), state.Current.end(), layer ) != state.Current.end();
        if ( before == current )
            return Common::MakeSuccess( false );
        if ( current )
            state.Current.push_back( layer );
        else
            std::erase( state.Current, layer );
        return Common::MakeSuccess( true );
    }

    void ApplyCurrentLayers( ::Desert::Core::Scene& scene, const std::vector<Common::UUID>& roots )
    {
        const std::vector<std::string> current  = CurrentLayers( scene );
        entt::registry&                registry = scene.GetRegistry();
        for ( const Common::UUID& uuid : roots )
        {
            const auto found = scene.FindEntityByID( uuid );
            if ( !found )
                continue;
            const entt::entity entity = found->get().GetHandle();
            const entt::entity anchor = RootOf( registry, entity );
            // Under a parent: the composite's layers, whatever is current (a member states its anchor's layers).
            std::vector<std::string> layers = current;
            if ( anchor != entity )
            {
                const auto* member = registry.try_get<ECS::DataLayersComponent>( anchor );
                layers             = member != nullptr ? member->Data.Layers : std::vector<std::string>();
            }
            if ( layers.empty() )
                continue;
            ForEachInSubtree( registry, entity,
                              [&]( const entt::entity created )
                              {
                                  for ( const std::string& layer : layers )
                                      JoinLayer( registry, created, layer );
                              } );
        }
    }

    Common::ResultStr<std::size_t> AddEntities( ::Desert::Core::Scene&           scene,
                                                const std::vector<Common::UUID>& entities,
                                                const std::string&               layer )
    {
        return EditMembership( scene, entities, layer, true );
    }

    Common::ResultStr<std::size_t> RemoveEntities( ::Desert::Core::Scene&           scene,
                                                   const std::vector<Common::UUID>& entities,
                                                   const std::string&               layer )
    {
        return EditMembership( scene, entities, layer, false );
    }

    std::vector<Common::UUID> DirectMembers( const ::Desert::Core::Scene& scene, const std::string& layer )
    {
        std::vector<Common::UUID> members;
        const entt::registry&     registry = scene.GetRegistry();
        for ( const entt::entity entity :
              registry.view<const ECS::DataLayersComponent, const ECS::UUIDComponent>() )
            if ( StatesLayer( registry, entity, layer ) )
                members.push_back( registry.get<ECS::UUIDComponent>( entity ).UUID );
        return members;
    }

    Common::ResultStr<std::size_t> SelectMembers( ::Desert::Core::Scene& scene, const std::string& layer )
    {
        if ( const auto known = RefuseUnknown( &scene, layer ); !known )
            return Common::MakeError<std::size_t>( known.GetError() );
        const ::Desert::Core::WorldPartitionSerialized& partition = *scene.GetWorldPartition();
        const std::size_t                               within    = *Rules::DataLayerIndexOf( partition, layer );
        const entt::registry&                           registry  = scene.GetRegistry();
        std::vector<Common::UUID>                       selected;
        for ( const entt::entity entity :
              registry.view<const ECS::DataLayersComponent, const ECS::UUIDComponent>() )
        {
            const bool member = std::any_of( registry.get<ECS::DataLayersComponent>( entity ).Data.Layers.begin(),
                                             registry.get<ECS::DataLayersComponent>( entity ).Data.Layers.end(),
                                             [&]( const std::string& name )
                                             {
                                                 const auto index = Rules::DataLayerIndexOf( partition, name );
                                                 return index.has_value() &&
                                                        Rules::DataLayerIsWithin( partition, *index, within );
                                             } );
            if ( member )
                selected.push_back( registry.get<ECS::UUIDComponent>( entity ).UUID );
        }
        const std::size_t count = selected.size();
        Core::SelectionManager::SetSelection( std::move( selected ) );
        return Common::MakeSuccess( count );
    }
} // namespace Desert::Editor::DataLayerEditor
