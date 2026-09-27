// GetToolTargetMesh's registry half: the component's MeshHandle resolved to its .stmesh file. Kept apart from
// ModelingToolTarget.cpp so the rule itself compiles into a suite with no asset registry and no device.
#include "ModelingToolTarget.hpp"

#include <Editor/Import/EditedMeshAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
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

    Common::ResultStr<std::filesystem::path> CommitEditableMeshToAsset( ECS::StaticMeshComponent& component )
    {
        if ( !component.EditableMesh )
            return Common::MakeError<std::filesystem::path>(
                 "the entity holds no edit: it already draws its static mesh asset as it is" );
        if ( !component.MeshHandle )
            return Common::MakeError<std::filesystem::path>(
                 "the edited mesh has no static mesh asset to write into (a created shape): write it as a new "
                 "asset with Output type Static Mesh" );
        auto* service = Runtime::ResourceRegistry::GetMeshService();
        auto* asset   = service->GetAsset( component.MeshHandle );
        if ( !asset )
            return Common::MakeFormattedError<std::filesystem::path>(
                 "static mesh asset {} is not loaded", static_cast<uint64_t>( component.MeshHandle ) );
        const std::filesystem::path path = asset->GetMetadata().Filepath;
        if ( auto written = WriteEditedMeshAsset( path, *component.EditableMesh ); !written.IsSuccess() )
            return Common::MakeError<std::filesystem::path>( written.GetError() );
        Assets::ContentRegistry::NoteFile( path );
        // Every entity that draws the asset draws the edit from the next frame: the parsed payload and the
        // built GPU mesh are dropped, and the next Get re-reads the file through the loader (the edited file
        // is preferred over the import's DDC envelope, Assets::IsEditedImportedMesh).
        if ( auto unloaded = asset->Unload(); !unloaded.IsSuccess() )
            return Common::MakeFormattedError<std::filesystem::path>( "'{}' was written but not reloaded: {}",
                                                                      path.generic_string(), unloaded.GetError() );
        service->EvictBuilt( component.MeshHandle );
        // The entity's own copy is now the asset's: it draws the asset again, like every other user of it.
        ECS::ClearEditableMesh( component );
        return Common::MakeSuccess( path );
    }
} // namespace Desert::Editor
