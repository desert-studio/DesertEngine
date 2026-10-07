#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp> // the SkinnedMesh -> Mesh conversion below
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Editor
{
    // The mesh an entity is ACTUALLY drawn with, in the renderer's own precedence:
    //   1. an edited RuntimeMesh (a CubeGrid blockout, a mesh being modelled),
    //   2. the built asset mesh behind its handle,
    //   3. for a PRIMITIVE, the process-wide shared mesh every cube/sphere instances from.
    //
    // That third case is easy to forget and its symptom is silent — the caller concludes "no mesh" and
    // reports zeros or "not built yet" for every primitive in the scene, which is exactly the bug this
    // helper exists to stop repeating. Returns null only when there is genuinely nothing to draw yet.
    inline const ::Desert::Mesh* ResolveDrawnMesh( const ECS::Entity& entity )
    {
        if ( entity.HasComponent<ECS::StaticMeshComponent>() )
        {
            const auto& smc = entity.GetComponent<ECS::StaticMeshComponent>();
            if ( smc.RuntimeMesh && !smc.RuntimeMesh->GetSubmeshes().empty() )
                return smc.RuntimeMesh.get();
            if ( smc.MeshHandle )
            {
                if ( const auto* mesh = Runtime::ResourceRegistry::GetMeshService()->Get( smc.MeshHandle ) )
                    return mesh;
            }
            if ( smc.Primitive.has_value() )
                return Geometry::PrimitiveMeshFactory::GetShared( *smc.Primitive );
        }

        if ( entity.HasComponent<ECS::SkinnedMeshComponent>() )
        {
            const auto& skc = entity.GetComponent<ECS::SkinnedMeshComponent>();
            if ( skc.RuntimeMesh )
                return skc.RuntimeMesh.get();
            if ( skc.MeshHandle )
                return Runtime::ResourceRegistry::GetMeshService()->Get( skc.MeshHandle );
        }
        return nullptr;
    }
} // namespace Desert::Editor
