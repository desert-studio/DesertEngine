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

#include <Engine/Core/Serialize/WorldPartitionHLODBuilders.hpp>

#include <Engine/Geometry/MeshNormalMapBake.hpp>

#include <Common/Utilities/AssetRegistry.hpp>

#include <optional>
#include <string>

#include <span>

namespace Desert::Core
{
    // UE UHLODBuilderMeshSimplifySettings (FMeshProxySettings' triangle target, as a fraction): what a
    // MeshSimplify layer's Settings state (HLODLayerSerialized::Settings), read by its builder alone.
    // TrianglePercent is the share of the merged mesh's triangles kept, in (0, 1]; REQUIRED - the builder refuses
    // Settings without it rather than pick a share nobody chose.
    struct HLODMeshSimplifySettings
    {
        std::optional<float> TrianglePercent;
    };

    // THE PROCESS'S HLOD BUILDERS (UE: every UHLODBuilder subclass of the loaded modules), registered on first
    // use: Instancing, MeshMerge and MeshSimplify under their type's name (Rules::kBuiltInHLODBuilders) and the
    // landscape builder (BuildLandscapeHLODMesh). A project or game module registers its own here, at startup, for
    // a Custom layer to name (Rules::HLODBuilderRegistry::Register). The editor's Build HLODs builds with a copy
    // of it.
    [[nodiscard]] Rules::HLODBuilderRegistry& HLODBuilders();

    // The geometry of a MeshMerge / MeshSimplify HLOD, for one request; the builders pass the cook's @p
    // registries. A request with MergeMaterials answers BuildHLODMeshForBake's mesh: one section over the atlas and
    // NO material slot - only the editor's Build HLODs, which bakes that material, may build such a layer
    // (WorldPartitionPanel states its own mesh geometry on its builders, binds M_HLOD_<key> and refuses a merged
    // mesh whose material was not baked).
    [[nodiscard]] Common::ResultStr<Assets::StaticMeshComponentSer>
    BuildHLODMesh( const Rules::HLODMeshRequest&                 request,
                   std::span<const Common::Utils::AssetRegistry> registries );

    // The side of the square a bake proxy is laid out on (HLODMaterialBakeSource::Proxy), in cm: atlas UV (u, v)
    // sits at world (u, 0, v) * this.
    inline constexpr float kHLODBakeProxySideCm = 1000.0f;

    // THE BAKE INPUT OF A MESH HLOD WITH MERGED MATERIALS (WP-HLOD-MATBAKE; UE IMeshMergeUtilities::
    // MergeComponentsToStaticMesh / CreateProxyMesh with bMergeMaterials, whose FMaterialBakingModule renders each
    // source material over the mesh's UV layout):
    struct HLODMaterialBakeSource
    {
        // Names the baked files (M_HLOD_<Key>.demat, T_HLOD_<Key>_*.detex): HLODMeshKey of the request, so the
        // Build HLODs probe and its final build - two passes over one saved world - name one mesh the same way.
        std::string                 Key;
        HLODMaterialMergeSerialized Settings;
        // THE SOURCE SURFACE LAID FLAT ON ITS ATLAS: every merged source triangle with its corners at
        // (u, 0, v) * kHLODBakeProxySideCm of their atlas UV (u, v), carrying their WORLD normal, tangent and
        // bitangent and their SOURCE UV, one section per source material (the slots in first-seen order), turned
        // to face +Y. Seen from straight above through the G-buffer it gives, per atlas texel, the source
        // material's BaseColor, Metallic, world Normal, Roughness, AO and Emissive at that surface point - any
        // material, as the renderer evaluates it.
        Assets::StaticMeshComponentSer Proxy;
        // MeshSimplify only: the merged source in world space (positions, vertex normals, triangles) with its
        // ATLAS UVs - the layout the proxy is laid out on - what the simplified mesh's texels are projected onto
        // along their normals (UE's proxy transfer; BakeMeshMaps' correspondence). Empty for MeshMerge, whose
        // built mesh is that atlas itself.
        Geometry::BakeSurface Source;
    };

    // BuildHLODMesh's mesh and, for a request with MergeMaterials, its bake input (BuildHLODMesh is this with the
    // input dropped, so the bake and the build cannot differ). MeshMerge: the merged triangles in one section,
    // UV layer 0 the source UV islands packed by UVPacker (texel density even in world space), world frames kept.
    // MeshSimplify: the merged mesh simplified, then AutoUV (patches, packed at TextureSize) and tangents rebuilt.
    // Refused by name as BuildHLODMesh is, and when the islands do not pack or the simplified mesh does not
    // unwrap.
    struct HLODMeshBuild
    {
        Assets::StaticMeshComponentSer        Mesh;
        std::optional<HLODMaterialBakeSource> Bake;
    };
    [[nodiscard]] Common::ResultStr<HLODMeshBuild>
    BuildHLODMeshForBake( const Rules::HLODMeshRequest&                 request,
                          std::span<const Common::Utils::AssetRegistry> registries );

    // 16 hex digits hashing everything the request builds from (type, triangle share, MergeMaterials, every part's
    // mesh, materials and instance matrices).
    [[nodiscard]] std::string HLODMeshKey( const Rules::HLODMeshRequest& request );

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

    // THE GROUND RECTANGLE A LANDSCAPE HLOD COVERS, in world centimetres on X/Z: sample (0, 0) of the tile at
    // (OriginX, OriginZ), the last sample at (OriginX + SizeX, OriginZ + SizeZ). BuildLandscapeHLODMesh gives
    // vertex (x, z) the TexCoord (x / (SamplesX - 1), z / (SamplesZ - 1)), so a texture baked top-down over
    // exactly this rectangle (texel row 0 at OriginZ, column 0 at OriginX) maps 1:1 onto the stand-in - the
    // editor's Build HLODs bakes the tile's landscape material into it (UE ULandscapeHLODBuilder). Read from
    // the same tile and frame the mesh is built from; refused for the same reasons.
    struct LandscapeHLODTileArea
    {
        Common::UUID Tile; // the tile's record id (LandscapeHLODRequest::Records[Tile].id)
        float        OriginX = 0.0f;
        float        OriginZ = 0.0f;
        float        SizeX   = 0.0f;
        float        SizeZ   = 0.0f;
    };
    [[nodiscard]] Common::ResultStr<LandscapeHLODTileArea>
    LandscapeHLODTileAreaOf( const Rules::LandscapeHLODRequest& request );
} // namespace Desert::Core
