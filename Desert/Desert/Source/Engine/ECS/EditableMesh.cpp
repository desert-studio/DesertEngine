#include "EditableMesh.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>

namespace Desert::ECS
{
    Common::BoolResultStr SetEditableMesh( StaticMeshDesc& desc, std::shared_ptr<const Geometry::DynamicMesh3> mesh )
    {
        if ( !mesh )
            return Common::MakeError<bool>( "SetEditableMesh: no mesh - use ClearEditableMesh to drop one" );
        if ( mesh->TriangleCount() == 0 )
            return Common::MakeFormattedError<bool>(
                 "SetEditableMesh: the mesh has {} vertices and no triangle, so there is nothing to draw",
                 mesh->VertexCount() );

        auto render = Geometry::ToRenderMesh( *mesh );
        if ( !render.IsSuccess() )
            return Common::MakeError<bool>( "SetEditableMesh: " + render.GetError() );
        Geometry::RenderMeshData const data = render.ExtractValue();

        // CPU data only: the component lives on the game side, which may have no renderer (headless, server,
        // tests). The renderer uploads it on first submit (Mesh::EnsureGpuResources, SceneRenderer).
        auto runtime = std::make_shared<DynamicMesh>( data.Vertices, data.Indices, data.Submeshes );

        // Every material id the mesh uses gets a slot (CoverMaterialIds): here and not per tool, because every
        // writer of an editable mesh ends in this function. The runtime instances follow the slots: SetStaticMesh
        // drops them when the slots it is handed differ, and the mesh system rebuilds them.
        static_cast<void>( CoverMaterialIds( desc.MaterialSlots, data.SubmeshMaterialIds ) );

        desc.EditableMesh = std::move( mesh );
        desc.RuntimeMesh  = std::move( runtime );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SetEditableMesh( entt::registry& registry, entt::entity entity,
                                           std::shared_ptr<const Geometry::DynamicMesh3> mesh )
    {
        const auto* component = registry.try_get<StaticMeshComponent>( entity );
        if ( component == nullptr )
            return Common::MakeError<bool>( "SetEditableMesh: the entity has no StaticMeshComponent" );
        StaticMeshDesc desc = component->GetDesc();
        if ( auto set = SetEditableMesh( desc, std::move( mesh ) ); !set )
            return set;
        SetStaticMesh( registry, entity, desc );
        return Common::MakeSuccess( true );
    }

    void ClearEditableMesh( StaticMeshDesc& desc )
    {
        desc.EditableMesh.reset();
        desc.RuntimeMesh.reset();
    }

    void ClearEditableMesh( entt::registry& registry, entt::entity entity )
    {
        const auto* component = registry.try_get<StaticMeshComponent>( entity );
        if ( component == nullptr || ( !component->GetEditableMesh() && !component->GetRuntimeMesh() ) )
            return;
        StaticMeshDesc desc = component->GetDesc();
        ClearEditableMesh( desc );
        SetStaticMesh( registry, entity, desc );
    }

    bool ApplyStaticMeshEdit( entt::registry& registry, entt::entity entity, StaticMeshDesc edited )
    {
        const StaticMeshComponent& now              = registry.get<StaticMeshComponent>( entity );
        const bool                 meshChanged      = edited.MeshHandle != now.GetMeshHandle();
        const bool                 materialsChanged = edited.MaterialSlots != now.GetMaterialSlots();
        if ( !meshChanged && !materialsChanged )
            return false;
        if ( meshChanged && edited.MeshHandle )
        {
            edited.Primitive.reset();
            ClearEditableMesh( edited );
        }
        // A changed MaterialSlots drops the runtime instances inside SetStaticMesh; MeshECSSystem rebuilds them
        // from the slots on the next frame (AdoptMeshMaterialSlots fills a new mesh's extra sections from its
        // own materials) together with the binding that carries them.
        return SetStaticMesh( registry, entity, edited );
    }

    bool PostEditStaticMesh( entt::registry& registry, entt::entity entity, const StaticMeshDesc& before )
    {
        StaticMeshDesc edited = registry.get<StaticMeshComponent>( entity ).GetDesc();
        if ( edited.MeshHandle == before.MeshHandle && edited.MaterialSlots == before.MaterialSlots )
            return false;
        SetStaticMesh( registry, entity, before );
        return ApplyStaticMeshEdit( registry, entity, std::move( edited ) );
    }
} // namespace Desert::ECS
