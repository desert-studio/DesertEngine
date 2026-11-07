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

            ECS::Entity    fieldEntity( field, registry );
            const uint64_t revision =
                 registry.get<ECS::InstancedStaticMeshComponent>( field ).GetInstanceRevision();
            // The field's children by reference, read fresh at each use: a spawn may add to the component pools
            // and move the vector. Never copied - a copy per Place/DestroyLast made a fresh spawn of N instances
            // O(N^2).
            static const std::vector<entt::entity> kNoChildren;
            const auto children = [&]() -> const std::vector<entt::entity>&
            {
                if ( !registry.has<ECS::RelationshipComponent>( field ) )
                    return kNoChildren;
                return registry.get<ECS::RelationshipComponent>( field ).Children;
            };

            // Unchanged since the last realization: nothing to do (an edit moves the revision, the child count,
            // the field's transform, its type or the prefab asset).
            const glm::mat4 fieldWorld = fieldEntity.GetWorldTransform();
            {
                const auto& foliage = registry.get<ECS::FoliageComponent>( field );
                const auto& last    = foliage.RuntimePrefabRealized;
                if ( last.Prefab == prefab.get() && last.InstanceRevision == revision &&
                     last.ChildCount == children().size() && last.Type == foliage.FoliageType &&
                     last.FieldWorld == fieldWorld )
                    continue;
            }

            World::Foliage::PrefabFoliageHost host;
            host.Realized = [&]
            {
                std::vector<glm::mat4> out;
                out.reserve( children().size() );
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
                const auto& list = children();
                if ( !list.empty() )
                    scene.DestroyEntity( ECS::Entity( list.back(), registry ) );
            };
            host.Place = [&]( std::size_t index, const glm::mat4& local )
            {
                if ( index >= children().size() )
                    return;
                const entt::entity                    child = children()[index];
                const ECS::Rules::DecomposedTransform parts = ECS::Rules::DecomposeTransform( local );
                registry.get_or_emplace<ECS::TransformComponent>( child );
                ECS::SetLocalTransform( registry, child,
                                        ECS::LocalTransform{ parts.Translation, parts.Rotation, parts.Scale } );
            };
            // A copy: a spawned prefab may add to the component pools and move the field's vector.
            const glm::mat4        toField = glm::inverse( fieldWorld );
            std::vector<glm::mat4> wanted =
                 registry.get<ECS::InstancedStaticMeshComponent>( field ).GetInstanceTransforms();
            for ( glm::mat4& instance : wanted )
                instance = toField * instance;
            if ( auto realized = ApplyPrefabFoliage( wanted, host ); !realized )
            {
                // Not recorded: the refusal is said once by the service, and the field is tried again next
                // frame, as before.
                types->RefusePrefab( type->Prefab.Guid, realized.GetError() );
                continue;
            }
            // Re-read: the spawns may have moved the component pools.
            auto& foliage                                  = registry.get<ECS::FoliageComponent>( field );
            foliage.RuntimePrefabRealized.InstanceRevision = revision;
            foliage.RuntimePrefabRealized.ChildCount       = children().size();
            foliage.RuntimePrefabRealized.FieldWorld       = fieldWorld;
            foliage.RuntimePrefabRealized.Type             = foliage.FoliageType;
            foliage.RuntimePrefabRealized.Prefab           = prefab.get();
        }
    }
} // namespace Desert::World::Foliage
