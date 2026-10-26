#include "Editor/LevelEditor/ScenePrimitiveBounds.hpp"

#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Serialization/VFXSystem.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>

namespace Desert::Editor
{
    namespace
    {
        void Grow( ScenePrimitiveBounds& bounds, const ::Common::Math::AABB& world )
        {
            bounds.Box.Min = glm::min( bounds.Box.Min, world.Min );
            bounds.Box.Max = glm::max( bounds.Box.Max, world.Max );
        }

        void Include( ScenePrimitiveBounds& bounds, const ECS::Entity& entity,
                      const std::vector<Submesh>* submeshes )
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
            Grow( bounds, world );
            ++bounds.Meshes;
        }

        const std::vector<Submesh>* AssetSubmeshes( const Assets::AssetHandle& handle )
        {
            const auto* asset = Runtime::ResourceRegistry::GetMeshService()->GetAsset( handle );
            return asset != nullptr ? &asset->GetSubmeshes() : nullptr;
        }

        // A foliage field: the one shared mesh through every instance's WORLD matrix (InstanceTransforms are
        // world matrices — MeshECSSystem draws them without the entity's transform).
        void IncludeInstances( ScenePrimitiveBounds& bounds, const ECS::InstancedStaticMeshComponent& ism,
                               const PrimitiveSubmeshes primitives )
        {
            const std::vector<Submesh>* submeshes = nullptr;
            if ( ism.RuntimeMesh )
                submeshes = &ism.RuntimeMesh->GetSubmeshes();
            else if ( ism.Primitive.has_value() )
                submeshes = primitives( *ism.Primitive );
            else if ( ism.MeshHandle != 0u )
                submeshes = AssetSubmeshes( ism.MeshHandle );
            if ( submeshes == nullptr )
            {
                ++bounds.Missing;
                return;
            }
            const auto local = Geometry::LocalBounds( *submeshes );
            if ( Geometry::IsEmpty( local ) )
                return;
            for ( const glm::mat4& instance : ism.InstanceTransforms )
            {
                Grow( bounds, Geometry::TransformBounds( instance, local ) );
                ++bounds.Instances;
            }
        }
    } // namespace

    const std::vector<Submesh>* SharedPrimitiveSubmeshes( const Geometry::PrimitiveType type )
    {
        const auto* shared = Geometry::PrimitiveMeshFactory::GetShared( type );
        return shared != nullptr ? &shared->GetSubmeshes() : nullptr;
    }

    ScenePrimitiveBounds MeasureScenePrimitives( entt::registry& registry, const PrimitiveSubmeshes primitives )
    {
        ScenePrimitiveBounds bounds;
        for ( const auto handle : registry.view<ECS::TransformComponent, ECS::StaticMeshComponent>() )
        {
            if ( ECS::IsHidden( registry, handle ) )
                continue;
            const ECS::Entity entity( handle, registry );
            const auto&       mesh = registry.get<ECS::StaticMeshComponent>( handle );
            if ( mesh.RuntimeMesh )
                Include( bounds, entity, &mesh.RuntimeMesh->GetSubmeshes() );
            else if ( mesh.Primitive.has_value() )
            {
                if ( const auto* submeshes = primitives( *mesh.Primitive ) )
                    Include( bounds, entity, submeshes );
            }
            else if ( mesh.MeshHandle != 0u )
                Include( bounds, entity, AssetSubmeshes( mesh.MeshHandle ) );
        }
        for ( const auto handle : registry.view<ECS::TransformComponent, ECS::SkinnedMeshComponent>() )
        {
            if ( ECS::IsHidden( registry, handle ) )
                continue;
            const ECS::Entity entity( handle, registry );
            const auto&       mesh = registry.get<ECS::SkinnedMeshComponent>( handle );
            if ( mesh.RuntimeMesh )
                Include( bounds, entity, &mesh.RuntimeMesh->GetSubmeshes() );
            else if ( mesh.MeshHandle != 0u )
                Include( bounds, entity, AssetSubmeshes( mesh.MeshHandle ) );
        }
        for ( const auto handle : registry.view<ECS::InstancedStaticMeshComponent>() )
        {
            const auto& ism = registry.get<ECS::InstancedStaticMeshComponent>( handle );
            if ( ECS::IsHidden( registry, handle ) || ism.InstanceTransforms.empty() )
                continue;
            IncludeInstances( bounds, ism, primitives );
        }

        // The tiles the renderer draws: the square from the root's frame, the height range from the samples
        // the tile actually holds (not the whole R16 range the shadow culler must assume).
        for ( const ECS::LandscapeTileRef& tile : ECS::DrawableLandscapeTiles( registry ).Tiles )
        {
            if ( ECS::IsHidden( registry, tile.Entity ) )
                continue;
            const auto& heights = *tile.Component->Heights;
            const auto  rect =
                 World::Landscape::LandscapeTileBounds( tile.Root, tile.Component->TileX, tile.Component->TileZ );
            const float low = tile.Frame.BaseY +
                              World::Landscape::LandscapeHeightCm( heights.LowestSample(), tile.Frame.ZScale );
            const float high = tile.Frame.BaseY +
                               World::Landscape::LandscapeHeightCm( heights.HighestSample(), tile.Frame.ZScale );
            Grow( bounds, { glm::vec3( std::min( rect.MinX, rect.MaxX ), std::min( low, high ),
                                       std::min( rect.MinZ, rect.MaxZ ) ),
                            glm::vec3( std::max( rect.MinX, rect.MaxX ), std::max( low, high ),
                                       std::max( rect.MinZ, rect.MaxZ ) ) } );
            ++bounds.Tiles;
        }

        // Effects: the enabled emitters' fixed bounds through the entity's world transform (UE: a Niagara
        // component's Bounds from UNiagaraSystem::FixedBounds). A system still being read is Missing; one that
        // failed to read draws nothing and is left out (its service has already said why).
        for ( const auto handle : registry.view<ECS::TransformComponent, ECS::VFXComponent>() )
        {
            const auto& vfx = registry.get<ECS::VFXComponent>( handle );
            if ( ECS::IsHidden( registry, handle ) || static_cast<uint64_t>( vfx.Data.System ) == 0u )
                continue;
            const auto resolved = Runtime::ResourceRegistry::GetVFXSystemService()->Get( vfx.Data.System );
            if ( !resolved.IsSuccess() )
                continue;
            if ( !resolved.GetValue() )
            {
                ++bounds.Missing;
                continue;
            }
            // The union of the enabled emitters' fixed bounds (VFXSystemExtent), placed by the same rule the
            // renderer culls with (VFXBoundsAt); a system with no enabled emitter draws nothing and is left out.
            const auto extent = Assets::Serialization::VFXSystemExtent( *resolved.GetValue() );
            if ( !extent )
                continue;
            Grow( bounds, Assets::Serialization::VFXBoundsAt(
                               *extent, ECS::Entity( handle, registry ).GetWorldTransform() ) );
            ++bounds.Effects;
        }
        return bounds;
    }
} // namespace Desert::Editor
