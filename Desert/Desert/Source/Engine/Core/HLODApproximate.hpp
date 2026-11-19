#pragma once

// THE MESH APPROXIMATE HLOD (WP-HLOD-APPROX): UE 5.5 UHLODBuilderMeshApproximate, which hands the cell's actors to
// IGeometryProcessing_ApproximateActors (GeometryProcessing/ApproximateActors, FApproximateActorsImpl), the
// pattern in the project's own geometry: the cell's parts merged in world space (MergeHLODParts) are
//   1. voxel-wrapped - their fast winding number sampled on a grid of WorldSpaceApproximationAccuracy cells and
//      meshed by Marching Cubes (Geometry::VoxWrap, UE TImplicitSolidify), open shells thickened first, so the
//      result is closed and manifold whatever the source's holes, overlaps and single-sided walls;
//   2. closed morphologically by GapDistance (Geometry::VoxMorph Close, UE bFillGaps / TImplicitMorphology), so
//      cracks between neighbouring houses are filled and a quarter becomes one shell;
//   3. stripped of inner cavities - the closed components wound against the outer surface (a courtyard sealed
//      by the wrap, the inside of a hollow house), which nobody outside ever sees;
//   4. simplified until it would leave the unsimplified shell by more than GeometricDeviation (UE SimplifyMethod
//      GeometricTolerance), normals split at HardNormalAngle (UE bEstimateHardNormals) and auto-UV'd at the
//      texture size (Geometry::AutoUV).
// The textures come from a CAPTURE of the source meshes (UE FSceneCapturePhotoSet + FRenderCaptureMapBaker): the
// editor photographs the cell's sources from the HLODCaptureViews directions into the G-buffer (BaseColor +
// Metallic, world Normal + Roughness, AO) and BakeApproximationTextures projects every texel of the shell into the
// photos that see its surface. One mesh and one material per cell.

#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp>
#include <Engine/Geometry/RenderMeshData.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Desert::Core
{
    // The shell and what each step left of it (for the build log and the scenario's statistics).
    struct ApproximateShell
    {
        Geometry::DynamicMesh3 Mesh;                 // world space, UV layer 0, normals split per NormalMode
        int32_t                VoxelCount       = 0; // wrap cells along the merged mesh's longest side
        int32_t                SourceTriangles  = 0; // the merged parts
        int32_t                WrappedTriangles = 0; // after wrap, closure and cavity removal
        int32_t                CavitiesRemoved  = 0; // closed components dropped as inner cavities
        int32_t                Triangles        = 0; // the result
        float                  Deviation        = 0.0f; // cm, the simplified shell from the unsimplified one
    };

    // Refused by name: settings outside Rules::ValidateHLODApproximation, a mesh without triangles, a step that
    // refuses (the wrap or the closure finds no solid), a shell nothing of which stays outside its cavities.
    [[nodiscard]] Common::ResultStr<ApproximateShell> BuildApproximateShell( const Geometry::DynamicMesh3&    merged,
                                                                            const HLODApproximationSettings& settings );

    // The name a cell's approximation is stored under: a hash of what the shell is built from (each part's mesh,
    // materials and world instances, and the settings), so the bake's textures and material follow the cell's
    // content - a rebuild of an unchanged cell finds its own files again, a changed cell gets new ones.
    [[nodiscard]] std::string HLODApproximationKey( const Rules::HLODMeshRequest& request );

    // One capture photo's ORTHOGRAPHIC camera: square, HalfExtent cm from its centre to each side, looking along
    // Forward from Eye; a point P lands at x = dot(P - Eye, Right) / HalfExtent, y = dot(P - Eye, Up) / HalfExtent
    // (column (x + 1) / 2 of the width, row (1 - y) / 2 of the height: row 0 is the photo's +Up edge, as the
    // readback's row 0 is) at depth dot(P - Eye, Forward), seen between NearCm and FarCm.
    struct HLODCaptureView
    {
        glm::vec3 Eye{ 0.0f };
        glm::vec3 Right{ 1.0f, 0.0f, 0.0f };
        glm::vec3 Up{ 0.0f, 1.0f, 0.0f };
        glm::vec3 Forward{ 0.0f, 0.0f, -1.0f };
        float     HalfExtent = 1.0f;
        float     NearCm     = 1.0f;
        float     FarCm      = 2.0f;
    };

    // UE FSceneCapturePhotoSet's directions: the six axes and the eight cube diagonals, each framing the sphere
    // around [@p boundsMin, @p boundsMax] (world cm), so every face of a convex block is seen square-on by one
    // photo and every corner region by a diagonal. The editor places its cameras from these; the bake projects
    // through the same numbers.
    [[nodiscard]] std::vector<HLODCaptureView> HLODCaptureViews( const glm::vec3& boundsMin,
                                                                 const glm::vec3& boundsMax );

    // One photo, read back from the G-buffer (Mesh/Surface/GBufferOutputs.glslh), Width x Height, row 0 first.
    struct HLODCapturePhoto
    {
        HLODCaptureView        View;
        uint32_t               Width  = 0;
        uint32_t               Height = 0;
        std::vector<uint8_t>   BaseColorMetallic; // RGBA8: linear BaseColor.rgb, Metallic.a (GBufferA)
        std::vector<glm::vec4> NormalRoughness;   // world Normal.xyz (zero = nothing drew), Roughness.w (GBufferB)
        std::vector<float>     Occlusion;         // material AO (GBufferEmissive.a)
    };

    // The shell's three maps, TextureSize square, row 0 = V 0: BaseColor linear RGBA8 (the writer states it sRGB),
    // Normal in the tangent frame of @p shell's render vertices (RGBA8, z reconstructed), ORM = AO, Roughness,
    // Metallic (RGBA8, StandardSurface's OrmTexture packing).
    struct HLODBakedTextures
    {
        uint32_t             Size = 0;
        std::vector<uint8_t> BaseColor;
        std::vector<uint8_t> Normal;
        std::vector<uint8_t> ORM;
        uint32_t             CapturedTexels = 0; // texels a photo saw
        uint32_t             FilledTexels   = 0; // texels no photo saw, filled from their captured neighbours
    };

    // UE FRenderCaptureMapBaker: every texel of @p shell's render triangles (rasterised in UV space) takes the photo
    // that sees its surface most squarely - a photo sees it when the camera ray through the texel's point first
    // meets @p source (the merged source mesh, world space) within the shell's tolerance of that point
    // (accuracy + gap + deviation) and the photo drew something there. Texels no photo saw (the underside on the
    // ground, the UV gutters) are filled outward from captured ones. Refused: no photos, a photo whose arrays do
    // not match its size, a shell no photo sees at all.
    [[nodiscard]] Common::ResultStr<HLODBakedTextures>
    BakeApproximationTextures( const Geometry::RenderMeshData& shell, const Geometry::DynamicMesh3& source,
                               std::span<const HLODCapturePhoto> photos, const HLODApproximationSettings& settings );
} // namespace Desert::Core
