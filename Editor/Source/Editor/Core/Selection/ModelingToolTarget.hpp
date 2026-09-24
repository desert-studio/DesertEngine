#pragma once

// Ported from UE 5.8 Engine/Plugins/Runtime/MeshModelingToolset/Source/ModelingComponents/Private/
// ModelingToolTargetUtil.cpp:302-335 (GetDynamicMeshCopy) and :453-500 (CommitDynamicMeshUpdate), adapted: the
// target is an entity's StaticMeshComponent instead of a UToolTarget; the persistent-dynamic-mesh branch is
// the component's EditableMesh, the MeshDescription branch is the .stmesh behind MeshHandle lifted through
// Geometry::DynamicMeshFromMeshAssetData (P7); meshes are immutable and shared, so the "copy" is a reference
// and the commit stays ECS::SetEditableMesh.

#include <Common/Core/ResultStr.hpp>

#include <Engine/Geometry/DynamicMeshAsset.hpp> // FDynamicMesh3

#include <memory>
#include <string_view>

namespace Desert::ECS
{
    struct StaticMeshComponent;
}

namespace Desert::Editor
{
    // WHAT A MODELING TOOL EDITS, for ANY static mesh (UE: every StaticMeshComponent is a tool target). Every
    // Modeling tool - Select Elements, the mesh operations, Plane Cut, Trim, the XForm tab - reads its input
    // through GetToolTargetMesh, and no tool reads StaticMeshComponent::EditableMesh itself.
    struct ToolTargetMesh
    {
        // The mesh the tool reads: the EditableMesh when the component has one, else the lifted asset.
        std::shared_ptr<const Geometry::FDynamicMesh3> Mesh;
        // What the component holds NOW (null for an asset-only entity): the "before" an undo step restores.
        // The commit (ECS::SetEditableMesh) and its undo record are ONE step, the lift included, so an undo
        // returns the entity to its asset with no EditableMesh (MeshHandle is never touched).
        std::shared_ptr<const Geometry::FDynamicMesh3> Committed;
    };

    // Refused, by name, when the component has neither an EditableMesh nor a readable .stmesh behind its
    // MeshHandle (a primitive, an unset handle, a skinned or non-welding file). A lift is cached per file and
    // write time, so repeated calls return the SAME mesh object: the element selection tracks by identity.
    [[nodiscard]] Common::ResultStr<ToolTargetMesh> GetToolTargetMesh( const ECS::StaticMeshComponent& component );

    // The lift itself, a pure function of the file's bytes (ReadMeshAssetData + DynamicMeshFromMeshAssetData).
    [[nodiscard]] Common::ResultStr<std::shared_ptr<const Geometry::FDynamicMesh3>>
    LiftStaticMeshBytes( std::string_view bytes, std::string_view whatFor );
} // namespace Desert::Editor
