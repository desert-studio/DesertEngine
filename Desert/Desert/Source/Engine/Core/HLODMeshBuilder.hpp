#pragma once

// THE GEOMETRY SIDE OF A MESH HLOD LAYER (WP-FAR-7): UE's UHLODBuilderMeshMerge and UHLODBuilderMeshSimplify
// (Engine/Source/Editor/HLODBuilders/Private/HLODBuilderMeshMerge.cpp, HLODBuilderMeshSimplify.cpp), the pattern
// and not the letter. UE hands the cell's components to IMeshMergeUtilities::MergeComponentsToStaticMesh (merge)
// or CreateProxyMesh (simplify) and gets one UStaticMesh; here the parts the Instancing pass chose
// (Rules::HLODMeshRequest) are gathered into one render mesh in WORLD space, one section per distinct material,
// welded into a DynamicMesh3, and for MeshSimplify collapsed by the editor's own QEM simplifier
// (Geometry::SimplifyToTriangleCount in MeshSimplifyOperation, over the ported QemSimplification) - no second
// simplifier. Normals are recomputed from the result. The answer is a StaticMesh block whose EditMesh is the built
// mesh and whose material slots are the distinct materials, in first-seen order: what the HLOD record carries and
// ComponentRegistry loads like any editor-built mesh.
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
    // THE HLOD BUILDERS OF A COOK, in one place: every world cook - editor Play (WorldStreamer), the packager
    // (GamePackager) and the WorldCook tool - takes its builders from here, so the three cannot build different
    // stand-ins for one world. Mesh binds @p registries (the caller keeps them alive for the cook); Landscape
    // reads the tile's height file itself.
    struct HLODBuilders
    {
        Rules::HLODMeshBuilder      Mesh;
        Rules::LandscapeHLODBuilder Landscape;
    };

    [[nodiscard]] HLODBuilders MakeHLODBuilders( std::span<const Common::Utils::AssetRegistry> registries );

    // The build itself, for one request; MakeHLODBuilders binds @p registries into it.
    [[nodiscard]] Common::ResultStr<Assets::StaticMeshComponentSer>
    BuildHLODMesh( const Rules::HLODMeshRequest&                 request,
                   std::span<const Common::Utils::AssetRegistry> registries );

    // THE LANDSCAPE HLOD OF ONE TILE (WP-FAR-6, Rules::LandscapeHLODRequest): the tile's heights, read from its
    // HeightFile (DLHT, resolved by Constants::Path::FullPath and read by ReadLandscapeTileFile, exactly as the
    // LandscapeTile load does), laid out by its root's frame (LandscapeTileFrame over the root record's composed
    // world position) - the same numbers the terrain shader samples, so the stand-in meets the near tiles on their
    // shared edge rows. The interior keeps every 2^LOD-th sample, the LOD picked from the request's
    // MinVisibleDistance as UE's ComputeRequiredLandscapeLOD does; the tile's four border rows keep EVERY sample
    // (the cells along them are fanned from their centre), so the edge is exactly the near tile's edge. A quad
    // with a corner in the tile's visibility layer hole is left out, as the terrain discards it. Normals from
    // central differences of the full-resolution samples. Refused by name: the tile's block or root does not
    // read, the height file is missing or does not decode, the tile does not match its root's size.
    [[nodiscard]] Common::ResultStr<Assets::StaticMeshComponentSer>
    BuildLandscapeHLODMesh( const Rules::LandscapeHLODRequest& request );
} // namespace Desert::Core
