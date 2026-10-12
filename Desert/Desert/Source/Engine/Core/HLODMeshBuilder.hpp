#pragma once

// THE GEOMETRY SIDE OF A MESH HLOD LAYER (WP-FAR-7): UE's UHLODBuilderMeshMerge and UHLODBuilderMeshSimplify
// (Engine/Source/Editor/HLODBuilders/Private/HLODBuilderMeshMerge.cpp, HLODBuilderMeshSimplify.cpp), the pattern
// and not the letter. UE hands the cell's components to IMeshMergeUtilities::MergeComponentsToStaticMesh (merge)
// or CreateProxyMesh (simplify) and gets one UStaticMesh; here the parts the Instancing pass chose
// (Rules::HLODMeshRequest) are gathered into one render mesh in WORLD space, one section per distinct material,
// welded into a DynamicMesh3, and for MeshSimplify collapsed by the editor's own QEM simplifier
// (Geometry::QemSimplification::SimplifyToTriangleCount) - no second simplifier. Normals are recomputed from the
// result. The answer is a StaticMesh block whose EditMesh is the built mesh and whose material slots are the
// distinct materials, in first-seen order: what the HLOD record carries and ComponentRegistry loads like any
// editor-built mesh.
//
// WHERE GEOMETRY COMES FROM: a Primitive from Geometry::MakePrimitive (the generators the scene's primitives are
// drawn from), a mesh asset from its render form (Assets::LoadMeshPlatformData, the bytes StaticMeshAsset draws)
// found through @p registries by {MeshGuid, MeshPath}. A part whose mesh is in no registry or does not read is
// an ERROR naming it: a merged HLOD with a silently missing piece is a hole nobody listed.

#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>

#include <Common/Utilities/AssetRegistry.hpp>

#include <span>

namespace Desert::Core
{
    [[nodiscard]] Rules::HLODMeshBuilder
    MakeHLODMeshBuilder( std::span<const Common::Utils::AssetRegistry> registries );

    // The build itself, for one request; MakeHLODMeshBuilder binds @p registries into it.
    [[nodiscard]] Common::ResultStr<Assets::StaticMeshComponentSer>
    BuildHLODMesh( const Rules::HLODMeshRequest&                 request,
                   std::span<const Common::Utils::AssetRegistry> registries );
} // namespace Desert::Core
