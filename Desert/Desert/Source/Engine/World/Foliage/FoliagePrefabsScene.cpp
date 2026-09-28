// The scene half of FoliagePrefabs.hpp: realizes each Prefab-type field's instances as its children.
#include <Engine/World/Foliage/FoliagePrefabs.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/System/SystemRules.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/matrix.hpp>

#include <vector>

namespace Desert::World::Foliage
{
    void RealizePrefabFoliage( Core::Scene& scene )
    {
        auto* types = Runtime::ResourceRegistry::GetFoliageTypeService();
        if ( !types )
            return;
        entt::registry& registry = scene.GetRegistry();

        // Collected first: spawning adds entities to the registry the view walks.
        std::vector<entt::entity> fields;
        for ( const entt::entity field :
              registry.view<ECS::FoliageComponent, ECS::InstancedStaticMeshComponent>() )
            fields.push_back( field );

        for ( const entt::entity field : fields )
        {
            const auto* type = types->Get( registry.get<ECS::FoliageComponent>( field ).FoliageType );
            if ( !type || !type->IsPrefab() )
                continue;
            const auto prefab  = types->GetPrefab( *type );
            const auto manager = types->Manager();
            if ( !prefab || !manager )
                continue;

            ECS::Entity fieldEntity( field, registry );
            const auto  children = [&]() -> std::vector<entt::entity>
            {
                if ( !registry.has<ECS::RelationshipComponent>( field ) )
                    return {};
                return registry.get<ECS::RelationshipComponent>( field ).Children;
            };
            World::Foliage::PrefabFoliageHost host;
            host.Realized = [&]
            {
                std::vector<glm::mat4> out;
                for ( const entt::entity child : children() )
                    out.push_back( registry.has<ECS::TransformComponent>( child )
                                        ? registry.get<ECS::TransformComponent>( child ).GetTransform()
                                        : glm::mat4( 1.0f ) );
                return out;
            };
            host.Spawn = [&]() -> Common::BoolResultStr
            {
                const auto spawned = prefab->Instantiate( &scene, *manager, fieldEntity );
                if ( !spawned )
                    return Common::MakeFormattedError<bool>( "{}", spawned.GetError() );
                return BOOLSUCCESS;
            };
            host.DestroyLast = [&]
            {
                const auto list = children();
                if ( !list.empty() )
                    scene.DestroyEntity( ECS::Entity( list.back(), registry ) );
            };
            host.Place = [&]( std::size_t index, const glm::mat4& local )
            {
                const auto list = children();
                if ( index >= list.size() )
                    return;
                const ECS::Rules::DecomposedTransform parts = ECS::Rules::DecomposeTransform( local );
                auto& tc       = registry.get_or_emplace<ECS::TransformComponent>( list[index] );
                tc.Translation = parts.Translation;
                tc.Rotation    = parts.Rotation;
                tc.Scale       = parts.Scale;
            };
            // The field's transforms are world matrices; a child's transform is local to the field. A copy
            // either way: a spawned prefab may add to the component pools and move the field's vector.
            const glm::mat4        toField = glm::inverse( fieldEntity.GetWorldTransform() );
            std::vector<glm::mat4> wanted =
                 registry.get<ECS::InstancedStaticMeshComponent>( field ).InstanceTransforms;
            for ( glm::mat4& instance : wanted )
                instance = toField * instance;
            if ( auto realized = ApplyPrefabFoliage( wanted, host ); !realized )
                types->RefusePrefab( type->Prefab.Guid, realized.GetError() );
        }
    }
} // namespace Desert::World::Foliage
