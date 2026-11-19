#pragma once

// THE OCTAHEDRAL IMPOSTOR (IMPOSTORS-HLOD): UE ImpostorBaker (Ryan Brucks' "Octahedral Impostors" plugin, the
// ImposterBaker / ImposterCommon.ush pattern Fortnite's far trees and the HLOD "Instancing/Impostor" layers use).
// A source mesh is photographed from a GRID of directions laid on an octahedron (full sphere) or a hemi-octahedron
// (upper hemisphere only - what stands on the ground is never seen from below) and each photo becomes one FRAME of
// a square atlas, FramesPerSide x FramesPerSide frames of FrameResolution texels. At draw time one camera-facing
// quad per instance finds the three grid frames around the view direction, projects the view ray into each frame's
// photo plane, samples all three and blends them by their barycentric weights (frame blending: no pop when the
// view direction crosses a frame), then pushes its depth by the frame's stored depth (pixel depth offset) so the
// quad intersects the world and casts shadows like the solid it stands in for.
//
// This header is the ENGINE half, pure and testable: the settings and their ranges, the octahedral mapping, the
// frame grid and its blend, the capture cameras (the same HLODCaptureViewFacing the MeshApproximate capture uses,
// so one editor capture path photographs both), and the bake that packs the read-back G-buffer photos into the
// atlas maps with a depth channel. The shader half mirrors the mapping and the blend function for function
// (Editor/Resources/Shaders/Common/ImpostorOctahedral.glslh): a change here is a change there.
//
// Conventions shared with that shader:
//   - world +Y is up; the hemi-octahedron covers directions with y >= 0;
//   - a FRAME is a grid VERTEX, not a cell: frame (col, row) photographs from the direction decoded at
//     uv = (col, row) / (FramesPerSide - 1), so the outermost frames sit exactly on the octahedron's edges and
//     poles and every direction lies inside a triangle of three frames (UE: OctaSphereLerp grid);
//   - the camera DIRECTION of a frame points from the centre towards the camera; the capture looks along
//     -direction;
//   - atlas texel (x, y) of frame (col, row) is photo pixel (x - col * R, y - row * R): row 0 of a photo (its +Up
//     edge) is the frame's top row, as the readback's row 0 is.

#include <Engine/Core/HLODApproximate.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Core
{
    // An impostor builder's settings (UE ImpostorBaker: FramesXY, bHemiOctahedron / ImpostorType, Resolution).
    // Ranges are refused by ValidateHLODImpostor.
    struct HLODImpostorSettings
    {
        // Frames along each side of the grid, 8..16 (UE default 16 full / 12 hemi). The atlas holds
        // FramesPerSide^2 photos; fewer frames = more parallax error between them, which frame blending hides only
        // up to about one frame's angular step (90 / (FramesPerSide - 1) degrees hemi, 180 / ... full).
        int32_t FramesPerSide = 12;
        // True: hemi-octahedron, the upper hemisphere only (anything standing on the ground: trees, houses,
        // rocks). False: the full octahedron (an object seen from below as well: a bridge, a floating rock).
        bool Hemisphere = true;
        // One frame's side in texels, a power of two 32..1024; the atlas side FramesPerSide x FrameResolution
        // must stay <= 8192 (the largest texture the cook writes).
        int32_t FrameResolution = 256;

        bool operator==( const HLODImpostorSettings& ) const = default;
    };

    // Refused by name: FramesPerSide outside 8..16, FrameResolution not a power of two in 32..1024, an atlas side
    // above 8192.
    [[nodiscard]] Common::BoolResultStr ValidateHLODImpostor( const HLODImpostorSettings& settings );

    // The atlas side in texels, FramesPerSide x FrameResolution (valid settings).
    [[nodiscard]] uint32_t HLODImpostorAtlasSide( const HLODImpostorSettings& settings );

    // UNIT direction -> grid uv in [0, 1]^2. Hemisphere: a direction below the horizon is first mirrored onto it
    // (y clamped to 0), so a camera slightly under a ground object still finds the horizon frames.
    [[nodiscard]] glm::vec2 ImpostorOctahedralEncode( const glm::vec3& direction, bool hemisphere );
    // Grid uv in [0, 1]^2 -> unit direction; the inverse of ImpostorOctahedralEncode on its image.
    [[nodiscard]] glm::vec3 ImpostorOctahedralDecode( const glm::vec2& uv, bool hemisphere );

    // The three frames whose photos are blended for a view direction, and their weights (sum 1, each >= 0):
    // the grid triangle the encoded direction falls in (UE ImposterCommon: the lower triangle (0,0) (1,0) (0,1) of
    // a grid cell, or the upper (1,1) (1,0) (0,1)).
    struct ImpostorFrameBlend
    {
        std::array<glm::ivec2, 3> Frames{};  // (col, row), each in [0, FramesPerSide - 1]
        glm::vec3                 Weights{}; // per frame
    };
    [[nodiscard]] ImpostorFrameBlend ImpostorBlendFrames( const glm::vec3& direction, int32_t framesPerSide,
                                                          bool hemisphere );

    // The sphere every frame frames: the centre of [boundsMin, boundsMax] and the half diagonal plus one
    // centimetre (HLODCaptureViews' sphere, so the same source captured both ways is framed alike).
    struct ImpostorSphere
    {
        glm::vec3 Centre{ 0.0f };
        float     Radius = 1.0f;
    };
    [[nodiscard]] ImpostorSphere HLODImpostorSphere( const glm::vec3& boundsMin, const glm::vec3& boundsMax );

    // The capture camera of every frame, row-major (index = row * FramesPerSide + col): HLODCaptureViewFacing
    // looking along -ImpostorOctahedralDecode(frame uv) at HLODImpostorSphere. The editor renders each one at
    // FrameResolution square and reads the G-buffer back as an HLODCapturePhoto, in this order.
    [[nodiscard]] std::vector<HLODCaptureView> HLODImpostorViews( const glm::vec3&            boundsMin,
                                                                  const glm::vec3&            boundsMax,
                                                                  const HLODImpostorSettings& settings );

    // The baked atlas: Side x Side texels, row 0 first, three RGBA8 maps.
    //   BaseColor   - linear BaseColor.rgb (the writer states it sRGB), A = coverage (255 drawn, 0 empty: the
    //                 shader's alpha test, and what frame blending renormalises by).
    //   NormalDepth - world Normal.xyz * 0.5 + 0.5, A = DEPTH: the surface's distance along the frame's view
    //                 direction from the sphere's centre plane, (t - (Radius + 1)) / Radius * 0.5 + 0.5 - 0.5 is
    //                 the plane through the centre, 0 the near edge of the sphere, 1 the far edge. The shader's
    //                 pixel depth offset reads it back in centimetres as (A - 0.5) * 2 * Radius.
    //   ORM         - AO, Roughness, Metallic (StandardSurface's OrmTexture packing), A = coverage.
    // Empty texels carry the colour of their nearest covered texel of the SAME frame (dilated a few texels), so a
    // bilinear tap at a silhouette does not darken towards the clear colour, and never another frame's colour.
    struct HLODImpostorAtlas
    {
        HLODImpostorSettings Settings;
        ImpostorSphere       Sphere;
        uint32_t             Side = 0;
        std::vector<uint8_t> BaseColor;
        std::vector<uint8_t> NormalDepth;
        std::vector<uint8_t> ORM;
        uint32_t             CoveredTexels = 0; // texels some photo drew
        // Covered texels whose ray through the source mesh met nothing (a sub-texel sliver the raster drew and the
        // ray slipped past): their depth is the centre plane. The build log states the count; a large share means
        // the photos and the source mesh disagree (wrong mesh, wrong transform).
        uint32_t DepthMisses = 0;
    };

    // Packs FramesPerSide^2 photos (HLODImpostorViews' order, each FrameResolution square) into the atlas, the
    // depth of every covered texel found by casting the photo's own orthographic ray into @p source (the mesh the
    // photos show, in the same space as the bounds). Refused, by name and numbers: invalid settings, a photo count
    // or size that does not match, a photo whose view is not the frame's (direction off by more than 1e-3), a
    // source without triangles, and photos in which nothing drew.
    [[nodiscard]] Common::ResultStr<HLODImpostorAtlas> BakeImpostorAtlas( const Geometry::DynamicMesh3& source,
                                                                          const glm::vec3&              boundsMin,
                                                                          const glm::vec3&              boundsMax,
                                                                          std::span<const HLODCapturePhoto> photos,
                                                                          const HLODImpostorSettings& settings );
} // namespace Desert::Core
