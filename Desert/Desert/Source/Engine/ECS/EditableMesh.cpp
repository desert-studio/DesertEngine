#include "EditableMesh.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>

namespace Desert::ECS
{
    Common::BoolResultStr SetEditableMesh( StaticMeshComponent&                      component,
                                           std::shared_ptr<const Geometry::EditMesh> mesh )
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
        Geometry::RenderMeshData data = render.ExtractValue();

        auto runtime = std::make_shared<DynamicMesh>( data.Vertices, data.Indices, data.Submeshes );
        if ( auto uploaded = runtime->Invalidate(); !uploaded.IsSuccess() )
            return Common::MakeFormattedError<bool>(
                 "SetEditableMesh: {} render vertices could not be uploaded: {}", data.Vertices.size(),
                 uploaded.GetError() );

        component.EditableMesh = std::move( mesh );
        component.RuntimeMesh  = std::move( runtime );
        return Common::MakeSuccess( true );
    }

    void ClearEditableMesh( StaticMeshComponent& component )
    {
        component.EditableMesh.reset();
        component.RuntimeMesh.reset();
    }
} // namespace Desert::ECS
