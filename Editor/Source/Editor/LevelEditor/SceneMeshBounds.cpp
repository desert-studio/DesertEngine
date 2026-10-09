#include "Editor/LevelEditor/SceneMeshBounds.hpp"

#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Editor
{
    namespace
    {
        void Include( SceneMeshBounds& bounds, const ECS::Entity& entity, const std::vector<Submesh>* submeshes )
        {
            if ( submeshes == nullptr )
            {
                ++bounds.Missing;
                return;
            }
            const auto world =
                 Geometry::TransformBounds( entity.GetWorldTransform(), Geometry::LocalBounds( *submeshes ) );
            if ( Geometry::IsEmpty( world ) )
                return;
            bounds.Box.Min = glm::min( bounds.Box.Min, world.Min );
            bounds.Box.Max = glm::max( bounds.Box.Max, world.Max );
            ++bounds.Meshes;
        }

        const std::vector<Submesh>* AssetSubmeshes( const Assets::AssetHandle& handle )
        {
            const auto* asset = Runtime::ResourceRegistry::GetMeshService()->GetAsset( handle );
            return asset != nullptr ? &asset->GetSubmeshes() : nullptr;
        }
    } // namespace

    const std::vector<Submesh>* SharedPrimitiveSubmeshes( const Geometry::PrimitiveType type )
    {
        const auto* shared = Geometry::PrimitiveMeshFactory::GetShared( type );
        return shared != nullptr ? &shared->GetSubmeshes() : nullptr;
    }

    SceneMeshBounds MeasureSceneMeshes( entt::registry& registry, const PrimitiveSubmeshes primitives )
    {
        SceneMeshBounds bounds;
        for ( const auto handle : registry.view<ECS::TransformComponent, ECS::StaticMeshComponent>() )
        {
            const ECS::Entity entity( handle, registry );
            const auto&       mesh = registry.get<ECS::StaticMeshComponent>( handle );
            if ( mesh.RuntimeMesh )
                Include( bounds, entity, &mesh.RuntimeMesh->GetSubmeshes() );
            else if ( mesh.Primitive.has_value() )
            {
                if ( const auto* submeshes = primitives( *mesh.Primitive ) )
                    Include( bounds, entity, submeshes );
            }
            else if ( mesh.MeshHandle )
                Include( bounds, entity, AssetSubmeshes( mesh.MeshHandle ) );
        }
        for ( const auto handle : registry.view<ECS::TransformComponent, ECS::SkinnedMeshComponent>() )
        {
            const ECS::Entity entity( handle, registry );
            const auto&       mesh = registry.get<ECS::SkinnedMeshComponent>( handle );
            if ( mesh.RuntimeMesh )
                Include( bounds, entity, &mesh.RuntimeMesh->GetSubmeshes() );
            else if ( mesh.MeshHandle )
                Include( bounds, entity, AssetSubmeshes( mesh.MeshHandle ) );
        }
        return bounds;
    }
} // namespace Desert::Editor
