// GetToolTargetMesh's registry half: the component's MeshHandle resolved to its .stmesh file. Kept apart from
// ModelingToolTarget.cpp so the rule itself compiles into a suite with no asset registry and no device.
#include "ModelingToolTarget.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Editor
{
    Common::ResultStr<ToolTargetMesh> GetToolTargetMesh( const ECS::StaticMeshComponent& component )
    {
        if ( component.EditableMesh || !component.MeshHandle )
            return GetToolTargetMeshAt( component.EditableMesh, {} );
        const auto* asset = Runtime::ResourceRegistry::GetMeshService()->GetAsset( component.MeshHandle );
        if ( !asset )
            return Common::MakeFormattedError<ToolTargetMesh>( "static mesh asset {} is not loaded",
                                                               static_cast<uint64_t>( component.MeshHandle ) );
        return GetToolTargetMeshAt( nullptr, asset->GetMetadata().Filepath );
    }
} // namespace Desert::Editor
