#pragma once

#include <Common/Core/Math/Rounding.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <Common/Json/Template.hpp>

namespace Desert::Graphic
{
    class Image2D;
}

namespace Desert::Graphic::System
{
    // ── The terrain batching contract ───────────────────────────────────────────────────────────────
    //
    // The terrain pass records EVERY draw of a frame before the GPU executes any of them, and a
    // material's descriptor sets are written at most once per frame BEFORE its first bind
    // (VulkanMaterialBackend's per-frame stamp — updating a set bound in a recording command buffer is
    // illegal without update-after-bind). So nothing that varies per terrain may travel through the
    // shared material's uniform block or its descriptors:
    //
    //   - PER-DRAW DATA (sizes, LODs, frame, layer modes) rides as a row of the
    //     `TerrainInstances[]` storage buffer below, named per draw by the same push-constant index
    //     that names the material param row. This is the transport the param fix already used
    //     (Engine/Core/Formats/MaterialParamRow.hpp); TerrainUB simply had not moved with it, and on a
    //     scene with two terrains the GPU read the LAST one's Model/Params for both.
    //
    //   - TEXTURES cannot ride in a row — a sampler is a descriptor and a descriptor set belongs to the
    //     material — so terrains wanting different textures need different materials, keyed by
    //     TerrainTextureKey below. Same rule, same key shape as MeshRenderer's GenericTextureKey.

    // One terrain's per-draw engine data, as the Terrain shader reads it. This struct and the GLSL
    // `TerrainInstance` in Shaders/Common/TerrainInstance.glslh are two statements of one layout; every
    // field is a mat4 or vec4, so the std430 offsets are trivially 16-byte multiples (the rule
    // MaterialParamRow.hpp applies to parameter rows for the same reason: a layout too simple to get wrong
    // instead of one checked).
    struct TerrainInstance
    {
        // x = tile extent (cm), y = the tile's FINEST continuous LOD (the least of its sections', SectionLods),
        // z = the tile's visibility layer + 1 (0 = no holes; LandscapeWeightDraw::VisibilityLayer,
        // LandscapeVisibility), w = the LOD of the tile's grid (floor of y): the grid the dice rings, the virtual
        // shadow map's procedural draw and the ray tracing BLAS walk; a section's raster draw takes its own
        // coarser grid (LandscapeDrawSplit::SectionGridLod).
        glm::vec4 Params{ 0.0f };
        // x = 1 / LOD blend range (UE's InvLODBlendRange), y = the tile's weight layer count (0 = no weightmap
        // bound; the surface keeps its ground), z = the weightmap's page count (LandscapeWeightmapPageCount
        // of y: the pages are stacked along the image's height, see LandscapeWeightDraw), w = the tile's
        // LandscapeNeighbourMask (which sides have their ring row in the heightmap).
        glm::vec4 Params2{ 0.0f };
        // The ground under the painted layers: rgb = the root's FIRST layer colour, a = 1 when the root names
        // a layer (UE shows its first layer where nothing is painted); a = 0 draws white ground. See
        // LandscapeWeightDraw::Ground.
        glm::vec4 GroundColor{ 0.0f };
        // x/z = the ROOT's origin, y = its base height, w = spacing (cm between samples), see LandscapeTileDraw.
        glm::vec4 LandscapeFrame{ 0.0f };
        // x/y = this tile's first sample in the landscape's global sample grid, z = quads per tile side,
        // w = vertical scale (cm per local height unit).
        glm::vec4 LandscapeTile{ 0.0f };
        // THE SECTIONS (TERRAIN-LOD-1; UE: a landscape component's sections each take their own LOD from their own
        // bounds' screen size): x = a section's quads a side (LandscapeSectionQuads), y = the sections a tile side
        // (LandscapeSectionCount), z = a section's coarsest grid LOD (LandscapeSectionMaxLod), w unused.
        glm::vec4 Sections{ 0.0f };
        // The continuous LOD of every section and of the ring of the neighbours' sections around them:
        // kLandscapeSectionTable x kLandscapeSectionTable floats, section (sx, sz) for sx, sz in [-1, Sections.y]
        // at (sz + 1) * kLandscapeSectionTable + sx + 1, four to a vec4 (0 = no neighbour there: nothing to agree
        // with). A vertex's LOD is its section's, blended toward the max() with the section across its border
        // (LandscapeVertexEval.glslh LandscapeVertexLod) - the same max() from both sides, so a seam between
        // sections, and between tiles, is one value.
        std::array<glm::vec4, 9> SectionLods{};
        // The colour each tile layer paints (rgb) and whether that layer is drawn at all (a = 1; a = 0 ignores
        // it), in the tile's layer order - layer i is page i / 4, channel i % 4. See LandscapeWeightDraw.
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> LayerColors{};
        // Per layer, four to a vec4 like the pages: 1 = the layer is NoWeightBlend (UE LB_AlphaBlend), laid
        // over the weight blend.
        std::array<glm::vec4, 2> LayerAlphaBlend{};
        // Per layer, four to a vec4 like the pages: the landscape material's input that names this layer's
        // LayerName (TerrainLayerInputs; -1: no input does, the layer keeps its colour).
        std::array<glm::vec4, 2> LayerInput{ glm::vec4( -1.0f ), glm::vec4( -1.0f ) };
        // Per material input k: its slice in the albedo, normal, ARM and height arrays (x, y, z, w; -1 = the
        // input has no map of that kind - flat colour, flat normal, AO/roughness 1, height 0.5).
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> InputSlices{};
        // x = the input the GROUND draws with (the root's first layer, by name; -1 = none), yzw unused.
        glm::vec4 GroundInput{ -1.0f, 0.0f, 0.0f, 0.0f };
        // GRASS-FIELD: per tile layer, the grass field's far cover (LandscapeGrassCover, see LandscapeWeightDraw):
        // the blades the generator thins out with distance are carried on by the ground's own shading.
        // GrassCoverRange x / y = the blades' AUTHORED Start / EndCullDistance (cm): the view's Foliage quality
        // and lens scale them in the shader (CameraUB::GrassLens.x), not here (GRASS-VIS-17).
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> GrassCover{};
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> GrassCoverLight{};
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> GrassCoverRange{};
        // LANDSCAPE DISPLACEMENT (UE 5.4+ landscape Nanite displacement: the material's Displacement output, the
        // layers' height x Magnitude around Center, faded out with distance). x = DisplacementMagnitude (cm; 0 = the
        // tile is drawn flat), y = DisplacementCenter, z = DisplacementFadeStart, w = DisplacementFadeEnd (cm).
        glm::vec4 Displacement{ 0.0f };
        // xyz = the MAIN camera (cm): the fade's origin for the camera AND every cascade view, so a cascade sees
        // the surface the camera sees. w = 1: the tile is drawn diced in rings (DiceRings), 0: its plain LOD grid.
        // The virtual shadow map's row (LandscapeVirtualShadowRow) holds its anchor in xyz and 1 + the reach (cm)
        // in w: its fade is the lower envelope of the fades of every eye within the reach of the anchor
        // (LandscapeDisplacement.glslh LandscapeDicedPoint), so its cached pages do not follow the eye's step.
        glm::vec4 DisplacementView{ 0.0f };
        // THE DICE RINGS (LandscapeDisplacement.glslh), square rings of cells around the main camera on the tile's
        // LOD-0 grid, each diced finer than the ring outside it (UE Nanite: finer where a triangle is larger on
        // screen): x, y = the camera's cell (tile-local, may lie outside the tile), z = the half-extent (cells) of
        // the fade's square - every cell the fade reaches, dice 1 and displaced -, w = DicingRate pixels' width in
        // cm per cm of distance (the footprint the layer heights' mip is read at).
        glm::vec4 DiceWindow{ 0.0f };
        // x = FarTilingDistance (cm), y = FarTilingScale: the layer height's far fetch, as the surface blends it.
        glm::vec4 DisplacementTiling{ 0.0f };
        // Per material input k (four to a vec4): LayerKTileSize (cm) and LayerKBlend, the material values the
        // displaced vertex needs in every program (the casters have no landscape material).
        std::array<glm::vec4, 2> InputTileSize{};
        std::array<glm::vec4, 2> InputBlend{};
        // ...and the half-extents (cells) of the rings diced 2, 4, 8 and 16 sub-cells a side (x..w; -1 = no such
        // ring). Each lies inside the one before: a cell's dice is the finest ring holding it.
        glm::vec4 DiceRings{ -1.0f };
        // THE DICE POOL (LandscapeDicePool.shader): x = the tile's first vertex in the frame's pool of displaced
        // sub-vertices, evaluated once per frame in the camera's rings and read by every view's draw (-1 = the
        // tile has none: it is not drawn diced); y = the finest dice level THIS row's view draws - 4 for the
        // camera, coarser for a cascade (LandscapeCasterDicedRow), whose cells then read every k-th pool vertex.
        glm::vec4 DicePool{ -1.0f, 4.0f, 0.0f, 0.0f };
        // THE VIEW'S FOOTPRINT ON THE DICE (UE Nanite culls every cluster outside the view before it is diced):
        // [x, y) .. [z, w) - the tile-local LOD-0 cells (the camera's GLOBAL cells minus the tile's first sample,
        // not clamped to the tile, so two tiles of a border agree) that the main camera's frustum reaches between
        // the landscape's lowest and highest displaced height, plus a margin. The rings diced 2..16 are clipped to
        // it; a cell outside keeps the fade square's dice 1, displaced, so no crack opens at the clip and an
        // off-screen bump still casts into the view (LandscapeDiceFootprint, LandscapeDisplacement.glslh
        // LandscapeCellDiceOf). The casters draw the camera's rows, so they keep the same clip.
        glm::vec4 DiceClip{ 0.0f };
        // GRASS-VIS-17: per tile layer, four to a vec4 like the pages, its blades' mean width per MinScreenWidth
        // pixel (cm / px; 0 = no screen-width floor; LandscapeWeightDraw::GrassCoverFloorWidth) - the primitive's
        // half of the floor's distance D. The view's half (GrassFieldLens::PixelsPerUnit, and the Foliage
        // quality's DistanceScale on GrassCoverRange x / y) is the view block's CameraUB::GrassLens: D = width x
        // PixelsPerUnit in the shader (LandscapeGrassCoverAt). Nothing in this row follows the lens or the render
        // width, so a zoom or a resolution change neither re-uploads it nor rebuilds the tile's ray tracing row
        // (LandscapeRayTracing compares the row) - UE's view uniforms apart from the primitive's scene data.
        std::array<glm::vec4, 2> GrassCoverFloorWidth{};
        // GPU-COST-29: per tile layer, four to a vec4, the share of its grass leaf area that is blades (their
        // survivors widen 1 / sqrt(share)); the rest is mesh varieties, whose survivors keep their size and shrink
        // out. The far cover weighs the two coverages by it (GrassFieldShare.glslh GrassFieldCoverageOf).
        std::array<glm::vec4, 2> GrassCoverWiden{};
        // GRASS-SHADOW-1: per tile layer, four to a vec4, its mesh varieties' bounding radius about the root at
        // their mean scale (cm, weighted by leaf area; 0 = no mesh variety; LandscapeWeightDraw::
        // GrassCoverFarLodRadius) - the primitive's half of the meshes' far LOD distance. The view's half is
        // CameraUB::GrassLens.z (GrassFieldLens::FarLodScale): the cover takes over what the far LOD drops
        // (GrassFieldShare.glslh GrassMeshFarLod), so the ground past it is as covered as the field it expects.
        std::array<glm::vec4, 2> GrassCoverFarLodRadius{};
    };

    static_assert( World::Landscape::kLandscapeMaxWeightLayers ==
                        2u * World::Landscape::kLandscapeWeightmapChannels,
                   "TerrainInstance carries exactly two weightmap pages of layer data - LandscapeWeights.glslh "
                   "blends two RGBA8 pages" );
    static_assert(
         sizeof( TerrainInstance ) == 77 * sizeof( glm::vec4 ),
         "TerrainInstance must stay seventy-seven 16-byte slots - the GLSL mirror in TerrainInstance.glslh "
         "reads these offsets" );
    static_assert(
         offsetof( TerrainInstance, Params2 ) == 16 && offsetof( TerrainInstance, GroundColor ) == 32 &&
              offsetof( TerrainInstance, LandscapeFrame ) == 48 &&
              offsetof( TerrainInstance, LandscapeTile ) == 64 && offsetof( TerrainInstance, Sections ) == 80 &&
              offsetof( TerrainInstance, SectionLods ) == 96 && offsetof( TerrainInstance, LayerColors ) == 240 &&
              offsetof( TerrainInstance, LayerAlphaBlend ) == 368 &&
              offsetof( TerrainInstance, LayerInput ) == 400 && offsetof( TerrainInstance, InputSlices ) == 432 &&
              offsetof( TerrainInstance, GroundInput ) == 560 && offsetof( TerrainInstance, GrassCover ) == 576 &&
              offsetof( TerrainInstance, GrassCoverLight ) == 704 &&
              offsetof( TerrainInstance, GrassCoverRange ) == 832 &&
              offsetof( TerrainInstance, Displacement ) == 960 &&
              offsetof( TerrainInstance, DisplacementView ) == 976 &&
              offsetof( TerrainInstance, DiceWindow ) == 992 &&
              offsetof( TerrainInstance, DisplacementTiling ) == 1008 &&
              offsetof( TerrainInstance, InputTileSize ) == 1024 &&
              offsetof( TerrainInstance, InputBlend ) == 1056 && offsetof( TerrainInstance, DiceRings ) == 1088 &&
              offsetof( TerrainInstance, DicePool ) == 1104 && offsetof( TerrainInstance, DiceClip ) == 1120 &&
              offsetof( TerrainInstance, GrassCoverFloorWidth ) == 1136 &&
              offsetof( TerrainInstance, GrassCoverWiden ) == 1168 &&
              offsetof( TerrainInstance, GrassCoverFarLodRadius ) == 1200,
         "TerrainInstance fields moved - the GLSL mirror in TerrainInstance.glslh no longer agrees" );

    // One tile's painted weight layers, as the surface shader blends them (LandscapeWeights.glslh). Kept
    // apart from LandscapeTileDraw: that one feeds the LOD/seam keys, and painting must not touch them.
    // The weightmap is LandscapeECSSystem's RGBA8 copy of the tile's layer weights: every page of the tile
    // (UE's weightmap textures, four layers each) stacked along the image's height, page p in rows
    // [p * SamplesZ, (p + 1) * SamplesZ) - LandscapeWeightmapAtlasTexels. Updated in place per stroke so its
    // address - part of TerrainTextureKey - stays put. LayerCount 0 means no weightmap: the tile shows its
    // ground.
    struct LandscapeWeightDraw
    {
        std::shared_ptr<Image2D> Weightmap;
        // Bumped by LandscapeECSSystem whenever it rewrites the weightmap's pixels (a paint stroke): the
        // ray-tracing BLAS of the tile reclassifies its hole cells on a change, and a cached local shadow whose
        // caster discards by the visibility layer keys on it (TerrainRenderer::LocalShadowSignature).
        uint64_t                                                           Revision   = 0;
        uint32_t                                                           LayerCount = 0;
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> Colors{};
        std::array<glm::vec4, 2>                                           AlphaBlend{};
        // Per tile layer, its LayerName (empty for a layer the root does not name and for the visibility
        // layer): the key TerrainRenderer matches against the material's inputs (UE's LayerBlend LayerName).
        std::array<std::string, World::Landscape::kLandscapeMaxWeightLayers> LayerNames{};
        // The root's first layer colour (rgb) with a = 1, or a = 0 when the root names no layer - drawn
        // where the tile has no weightmap or its layers leave a pixel unclaimed.
        glm::vec4 Ground{ 0.0f };
        // That first layer's LayerName (empty when the root names none): the ground's material input.
        std::string GroundLayer;
        // The tile layer that is its visibility mask (LandscapeWeightChannels::Visibility), -1 for none: the
        // surface and the shadow caster discard where it reaches 0.5 (UE's Landscape Visibility Mask).
        int32_t VisibilityLayer = -1;
        // GRASS-FIELD: per tile layer, its grass field's far cover (TerrainInstance::GrassCover*), zero for a
        // layer that grows no grass. LandscapeECSSystem::GrassCoverOf derives it from the layer's grass types.
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> GrassCover{};
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> GrassCoverLight{};
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> GrassCoverRange{};
        // GRASS-VIS-16: per tile layer, its blades' mean width per MinScreenWidth pixel (cm / px; 0 = no
        // screen-width floor) - the view-independent half of the floor's distance
        // (TerrainInstance::GrassCoverFloorWidth).
        std::array<float, World::Landscape::kLandscapeMaxWeightLayers> GrassCoverFloorWidth{};
        // GPU-COST-29: per tile layer, the share of its grass leaf area that is blades - survivors that widen -
        // the rest meshes that shrink out (TerrainInstance::GrassCoverWiden).
        std::array<float, World::Landscape::kLandscapeMaxWeightLayers> GrassCoverWiden{};
        // GRASS-SHADOW-1: per tile layer, its mesh varieties' bounding radius at their mean scale (cm; 0 = no
        // mesh variety) - the view-independent half of the meshes' far LOD distance
        // (TerrainInstance::GrassCoverFarLodRadius).
        std::array<float, World::Landscape::kLandscapeMaxWeightLayers> GrassCoverFarLodRadius{};
    };

    // Where one landscape tile sits, in the form the seam needs. The ROOT's origin and the tile's first
    // GLOBAL sample, not the tile's own origin: a vertex on a shared edge is then the same integer sample
    // index seen from both tiles, turned into a position by the same operations on the same numbers, so
    // both tiles place it bit for bit identically. Per-tile origins would be two roundings of one point.
    struct LandscapeTileDraw
    {
        float    OriginX      = 0.0f;
        float    OriginZ      = 0.0f;
        float    BaseY        = 0.0f;
        float    SpacingCm    = 0.0f;
        float    ZScale       = 0.0f;
        int32_t  FirstSampleX = 0;
        int32_t  FirstSampleZ = 0;
        uint32_t QuadsPerTile = 0u;
        // The world heights (cm) of the tile's lowest and highest sample (LandscapeTileData::LowestSample /
        // HighestSample), undisplaced: the vertical slab the view's footprint on the dice is cut from.
        float MinY = 0.0f;
        float MaxY = 0.0f;
        // World::Landscape::LandscapeNeighbourMask of the neighbours whose ring row the heightmap carries.
        uint32_t NeighbourMask = 0u;
        // The tile's entity (its GPU-scene motion row and ray-tracing state are keyed by it), and the revision
        // of the heightmap's pixels, bumped whenever LandscapeECSSystem rewrites them (sculpt, neighbour ring).
        uint32_t Entity         = 0u;
        uint64_t HeightRevision = 0u;
    };

    // ── Landscape LOD ───────────────────────────────────────────────────────────────────────────────
    //
    // Ported from UE 5.8 Engine/Source/Runtime/Landscape/Private/LandscapeRender.cpp:575-589
    // (FLandscapeRenderSystem::ComputeLODFromScreenSize) and :1547-1593 (the per-LOD screen-size ratios of
    // FLandscapeComponentSceneProxy), and Engine/Source/Runtime/Engine/Public/SceneManagement.h
    // (ComputeBoundsScreenRadiusSquared), adapted: the UE proxy's LOD0ScreenSize / LOD0DistributionSetting /
    // LODDistributionSetting / LODBlendRange are UE's defaults as constants (no per-landscape override yet),
    // no r.StaticMeshLODDistanceScale, and MaxLOD is the last stride-2^L grid of the tile (LandscapeLod.glslh)
    // instead of the heightmap's last mip. Each SECTION of a tile (below) takes ONE continuous LOD per view: the
    // main camera's for the camera's passes and the cascades (they draw the camera's vertices, so a sun shadow cannot
    // swim against its caster), and a point/spot light's own, from its position, for that light's views — their
    // depth is cached, and a camera-chosen LOD would redraw it on every camera move (TerrainRenderer).
    inline constexpr float kLandscapeLod0ScreenSize   = 0.5f;
    inline constexpr float kLandscapeLod0Distribution = 1.25f;
    inline constexpr float kLandscapeLodDistribution  = 3.0f;
    inline constexpr float kLandscapeLodBlendRange    = 1.0f;

    // The coarsest grid LOD of a tile: its stride is the largest power of two below the tile's samples, so
    // the next grid (the morph target) is the tile's four corners.
    inline uint32_t LandscapeMaxLod( uint32_t quadsPerTile )
    {
        uint32_t lod = 0u;
        while ( ( 2u << lod ) < quadsPerTile + 1u )
            ++lod;
        return lod;
    }

    struct LandscapeLodSettings
    {
        float LOD0ScreenSizeSquared               = 0.0f;
        float LOD1ScreenSizeSquared               = 0.0f;
        float LODOnePlusDistributionScalarSquared = 0.0f;
        float LastLODScreenSizeSquared            = 0.0f;
        float LastLODIndex                        = 0.0f;
    };

    // UE's per-LOD screen-size ratios for a primitive whose coarsest grid LOD is @p maxLod.
    inline LandscapeLodSettings MakeLandscapeLodSettingsTo( uint32_t maxLod )
    {
        LandscapeLodSettings settings;
        float                divider   = std::max( kLandscapeLod0Distribution, 1.01f );
        float                ratio     = kLandscapeLod0ScreenSize;
        settings.LOD0ScreenSizeSquared = ratio * ratio;
        ratio /= divider;
        settings.LOD1ScreenSizeSquared               = ratio * ratio;
        divider                                      = std::max( kLandscapeLodDistribution, 1.01f );
        settings.LODOnePlusDistributionScalarSquared = divider * divider;
        float last                                   = settings.LOD0ScreenSizeSquared;
        for ( uint32_t lod = 1u; lod <= maxLod; ++lod )
        {
            last = ratio * ratio;
            ratio /= divider;
        }
        settings.LastLODScreenSizeSquared = last;
        settings.LastLODIndex             = static_cast<float>( maxLod );
        return settings;
    }

    // ...for a whole tile drawn as one section (its grid's coarsest LOD is LandscapeMaxLod).
    inline LandscapeLodSettings MakeLandscapeLodSettings( uint32_t quadsPerTile )
    {
        return MakeLandscapeLodSettingsTo( LandscapeMaxLod( quadsPerTile ) );
    }

    inline float LandscapeLodFromScreenSize( const LandscapeLodSettings& settings, float screenSizeSquared )
    {
        if ( screenSizeSquared <= settings.LastLODScreenSizeSquared )
            return settings.LastLODIndex;
        if ( screenSizeSquared > settings.LOD1ScreenSizeSquared )
            return ( settings.LOD0ScreenSizeSquared -
                     std::min( screenSizeSquared, settings.LOD0ScreenSizeSquared ) ) /
                   ( settings.LOD0ScreenSizeSquared - settings.LOD1ScreenSizeSquared );
        // No longer a linear fraction (UE's words): log base (distribution^2) of the size ratio.
        return std::min( settings.LastLODIndex,
                         1.0f + std::log( settings.LOD1ScreenSizeSquared / screenSizeSquared ) /
                                     std::log( settings.LODOnePlusDistributionScalarSquared ) );
    }

    // A tile's LOD and what its borders and corners take. `lodAt(dx, dz)` is the continuous LOD of the tile
    // at that offset from this one, or nullopt where there is none (the landscape's own edge: nothing to
    // agree with there, so the tile's own LOD). max() on both sides of a border is what makes it one value.
    struct LandscapeTileLod
    {
        float     Center = 0.0f;
        glm::vec4 Edges{ 0.0f };
        glm::vec4 Corners{ 0.0f };
    };

    template <class LodAt>
    LandscapeTileLod LandscapeTileLods( float center, LodAt&& lodAt )
    {
        const auto at = [&]( int dx, int dz )
        {
            const std::optional<float> lod = lodAt( dx, dz );
            return std::max( center, lod.value_or( center ) );
        };
        const auto corner = [&]( int dx, int dz )
        { return std::max( { at( dx, 0 ), at( 0, dz ), at( dx, dz ) } ); };
        LandscapeTileLod result;
        result.Center  = center;
        result.Edges   = glm::vec4( at( -1, 0 ), at( 1, 0 ), at( 0, -1 ), at( 0, 1 ) );
        result.Corners = glm::vec4( corner( -1, -1 ), corner( 1, -1 ), corner( -1, 1 ), corner( 1, 1 ) );
        return result;
    }

    // ── Landscape sections (TERRAIN-LOD-1) ──────────────────────────────────────────────────────────
    //
    // UE: a landscape component is drawn as sections, and each section takes its OWN LOD from its own bounds'
    // screen size (FLandscapeRenderSystem::SectionLODValues; FLandscapeComponentSceneProxy's LOD0ScreenSize
    // applies to the section's bounds, whatever its quads), its seams stitched by the morph toward the max()
    // with the neighbour (LandscapeVertexFactory.ush CalcLOD). One LOD per 255-quad tile kept LOD 0 out to the
    // tile sphere's reach (~76 m on FIELD_Barn's 23.5 cm grid) - every triangle past ~15 m sub-pixel along the
    // view. Here a tile is split into sections of UE's 63-quad size (64 samples with the shared edge) on sample
    // lines that are multiples of every section grid's stride, so a section is a window of the tile's grid at
    // its own LOD (LandscapeSectionDrawsOf) and its vertices snap and morph in tile coordinates
    // (LandscapeLod.glslh) without leaving it: a section's coarsest grid is two cells a side, morphing to its
    // corners (LandscapeSectionMaxLod). A tile of at most one section's quads is one section - the tile, as
    // before.
    inline constexpr uint32_t kLandscapeMaxSections  = 4u;  // sections a tile side, at most
    inline constexpr uint32_t kLandscapeSectionQuads = 64u; // a section's quads a side (UE's 63 + the shared edge)
    // The side of TerrainInstance::SectionLods' table: the sections and a ring of the neighbours' around them.
    // TerrainInstance.glslh LANDSCAPE_SECTION_TABLE is the same number.
    inline constexpr uint32_t kLandscapeSectionTable = kLandscapeMaxSections + 2u;
    static_assert( kLandscapeSectionTable * kLandscapeSectionTable <= 9u * 4u,
                   "TerrainInstance::SectionLods holds the whole table" );

    // A section's quads a side: kLandscapeSectionQuads, or a power-of-two share of the tile when the tile would
    // need more than kLandscapeMaxSections of them (the power of two keeps every grid's stride dividing it).
    inline uint32_t LandscapeSectionQuads( uint32_t quadsPerTile )
    {
        uint32_t span = 1u;
        while ( span < quadsPerTile )
            span <<= 1u;
        return std::max( span / kLandscapeMaxSections, std::min( span, kLandscapeSectionQuads ) );
    }

    // Sections a tile side (the last one is narrower when the section does not divide the quads).
    inline uint32_t LandscapeSectionCount( uint32_t quadsPerTile )
    {
        const uint32_t quads = std::max( quadsPerTile, 1u );
        const uint32_t sq    = LandscapeSectionQuads( quads );
        return ( quads + sq - 1u ) / sq;
    }

    // A section's coarsest grid LOD: its stride is half the section, so the morph target is the section's corners
    // and no vertex snaps across a section line. A one-section tile keeps the tile's (LandscapeMaxLod).
    inline uint32_t LandscapeSectionMaxLod( uint32_t quadsPerTile )
    {
        if ( LandscapeSectionCount( quadsPerTile ) <= 1u )
            return LandscapeMaxLod( quadsPerTile );
        return LandscapeMaxLod( LandscapeSectionQuads( quadsPerTile ) - 1u );
    }

    // A tile's sections' continuous LODs, row-major (sz * kLandscapeMaxSections + sx).
    using LandscapeSectionLodArray = std::array<float, kLandscapeMaxSections * kLandscapeMaxSections>;

    // A tile's SectionLods table and its finest section's LOD. `sectionsAt(dx, dz)` is the section LODs of the
    // tile at that offset from this one (dx = dz = 0: this tile), or nullptr where there is none. Every tile of a
    // landscape has the same quads, so the ring holds the neighbours' border sections exactly as they hold them.
    struct LandscapeSectionLodTable
    {
        std::array<glm::vec4, 9> Lods{};
        float                    Finest = 0.0f;
    };

    template <class SectionsAt>
    LandscapeSectionLodTable LandscapeSectionLodsOf( uint32_t quadsPerTile, SectionsAt&& sectionsAt )
    {
        const int                sections = static_cast<int>( LandscapeSectionCount( quadsPerTile ) );
        LandscapeSectionLodTable table;
        table.Finest = std::numeric_limits<float>::max();
        for ( int sz = -1; sz <= sections; ++sz )
        {
            for ( int sx = -1; sx <= sections; ++sx )
            {
                const int                       dx   = sx < 0 ? -1 : ( sx >= sections ? 1 : 0 );
                const int                       dz   = sz < 0 ? -1 : ( sz >= sections ? 1 : 0 );
                const LandscapeSectionLodArray* tile = sectionsAt( dx, dz );
                const int                       local =
                     ( sz - dz * sections ) * static_cast<int>( kLandscapeMaxSections ) + ( sx - dx * sections );
                const float lod   = tile != nullptr ? ( *tile )[static_cast<size_t>( local )] : 0.0f;
                const int   index = ( sz + 1 ) * static_cast<int>( kLandscapeSectionTable ) + sx + 1;
                table.Lods[static_cast<size_t>( index / 4 )][index % 4] = lod;
                if ( dx == 0 && dz == 0 )
                    table.Finest = std::min( table.Finest, lod );
            }
        }
        return table;
    }

    // The continuous LOD of section (sx, sz), sx, sz in [-1, sections], of a SectionLods table.
    inline float LandscapeSectionLodAt( const std::array<glm::vec4, 9>& lods, int sx, int sz )
    {
        const int index = ( sz + 1 ) * static_cast<int>( kLandscapeSectionTable ) + sx + 1;
        return lods[static_cast<size_t>( index / 4 )][index % 4];
    }

    // Vertices of a tile's draw at grid LOD `gridLod`: six per cell (LandscapeLod.glslh decodes them).
    inline uint32_t LandscapeLodVertexCount( uint32_t quadsPerTile, uint32_t gridLod )
    {
        const uint32_t stride = 1u << gridLod;
        const uint32_t cells  = ( quadsPerTile + stride - 1u ) / stride;
        return cells * cells * 6u;
    }

    // The dice rings (LandscapeDisplacement.glslh): dice 1, 2, 4, 8, 16 sub-cells a side - the finest puts a
    // vertex every 6.25 cm on a 1 m landscape.
    constexpr int kLandscapeDiceLevels = 5;

    // THE VIRTUAL SHADOW MAP'S ANCHOR REACH (TURN-CACHE-2; UE: a landscape's cached VSM pages are invalidated by a
    // LOD change only past its height-error threshold, never by the eye's step): how far the eye walks from the
    // anchor before the displaced landscape's virtual shadow pages are redrawn. The caster takes, in the fade's
    // band, the lowest surface any eye within the reach would draw (bumps at the smallest fade, pits at the
    // largest), so no receiver lies under it; the price is the relief's self-shadow missing in that band widened
    // by the reach. 4 m: one redraw per 40 frames of FPS-FIELD-BARN-WALK's 10 cm step, and the relief nearer
    // than DisplacementFadeStart - 4 m (11 m by default) is exact.
    constexpr float kLandscapeVsmAnchorReachCm = 400.0f;

    // Dice ring @p level (dice 1 << level) of a row, clamped to the tile's @p cells x @p cells grid: [x0, x1) x
    // [z0, z1); empty when the ring is absent. LandscapeDisplacement.glslh LandscapeDiceRingRect is the same.
    // @p pool: the ring as the CAMERA dices it (the dice pool's layout), not as the row's view draws it (a caster
    // stops at DicePool.y).
    inline glm::ivec4 LandscapeDiceRingRect( const TerrainInstance& row, int level, int cells, bool pool = false )
    {
        const float half = level == 0 ? row.DiceWindow.z : row.DiceRings[static_cast<glm::length_t>( level - 1 )];
        if ( row.DisplacementView.w < 0.5f || half < 0.0f ||
             ( !pool && static_cast<float>( level ) > row.DicePool.y + 0.5f ) )
            return glm::ivec4( 0 );
        const int cx = static_cast<int>( std::floor( row.DiceWindow.x + 0.5f ) );
        const int cz = static_cast<int>( std::floor( row.DiceWindow.y + 0.5f ) );
        const int h  = static_cast<int>( Common::Math::RoundToNearest( half ) );
        // Past the fade's square the rings are clipped to the view's footprint (DiceClip).
        const glm::ivec4 clip = level > 0
                                     ? glm::ivec4( glm::floor( row.DiceClip + glm::vec4( 0.5f ) ) )
                                     : glm::ivec4( INT32_MIN / 2, INT32_MIN / 2, INT32_MAX / 2, INT32_MAX / 2 );
        return glm::ivec4( std::clamp( std::max( cx - h, clip.x ), 0, cells ),
                           std::clamp( std::max( cz - h, clip.y ), 0, cells ),
                           std::clamp( std::min( cx + h + 1, clip.z ), 0, cells ),
                           std::clamp( std::min( cz + h + 1, clip.w ), 0, cells ) );
    }

    inline uint32_t LandscapeDiceRectArea( const glm::ivec4& r )
    {
        return r.z > r.x && r.w > r.y ? static_cast<uint32_t>( ( r.z - r.x ) * ( r.w - r.y ) ) : 0u;
    }

    // The draw's vertex count with the row's dice rings (LandscapeDisplacement.glslh decodes the same order: the
    // ring diced 16, then 8, 4, 2, 1 - each the cells of its square outside the finer one, dice x dice sub-cells
    // a cell - then every cell outside the fade's square, undiced; six vertices a (sub-)cell).
    inline uint32_t LandscapeDicedVertexCount( uint32_t quadsPerTile, uint32_t gridLod,
                                               const TerrainInstance& row )
    {
        const uint32_t all = LandscapeLodVertexCount( quadsPerTile, gridLod );
        if ( gridLod != 0u || row.DisplacementView.w < 0.5f )
            return all;
        const int cells = static_cast<int>( quadsPerTile );
        uint32_t  total = 0u, inner = 0u;
        for ( int level = kLandscapeDiceLevels - 1; level >= 0; --level )
        {
            const uint32_t area = LandscapeDiceRectArea( LandscapeDiceRingRect( row, level, cells ) );
            total += ( area - inner ) * ( 1u << ( 2 * level ) ) * 6u;
            inner = area;
        }
        return total + all - inner * 6u;
    }

    // ── THE TILE'S GRID, INDEXED (TERRAIN-GPU-2) ────────────────────────────────────────────────────────
    //
    // UE FLandscapeSharedBuffers: one index buffer per grid size, shared by every tile drawn at it, over the
    // grid's (cells + 1)^2 nodes - each node one vertex, which the post-transform cache shades once for the
    // triangles around it (about twice along 255-cell rows), where the procedural decode (six vertices a cell)
    // evaluated every node six times, ten heightmap fetches each. The undiced cells of every tile draw through it
    // (TerrainRenderer::RecordDraws); the dice rings keep the procedural decode - their sub-vertices are the dice
    // pool's, read and not evaluated, and their layout moves with the camera every frame.
    //
    // The vertex stage tells the two apart by the draw's first instance (TerrainVertex.glslh): an indexed grid
    // draw starts at kLandscapeGridFirstInstance and every instance past it is one grid row further (a strip of
    // rows beside the rings is one instanced draw of one row's index range); the procedural draws, the virtual
    // shadow map's indirect ones included, are instance 0. LandscapeDisplacement.glslh
    // LANDSCAPE_GRID_FIRST_INSTANCE is the same number.
    inline constexpr uint32_t kLandscapeGridFirstInstance = 1u;
    // ...and which grid it draws (TERRAIN-LOD-1: a section draws the tile's grid at its own LOD): the first
    // instance is kLandscapeGridFirstInstance + gridLod * kLandscapeGridLodInstances + the first row, the grid's
    // rows never reaching the next LOD's block. LandscapeDisplacement.glslh LANDSCAPE_GRID_LOD_INSTANCES is the
    // same number.
    inline constexpr uint32_t kLandscapeGridLodInstances = 4096u;

    // Cells a side of a tile's grid at grid LOD @p gridLod (LandscapeLodVertexCount's: the last cell is narrower
    // when the stride does not divide the quads).
    inline uint32_t LandscapeLodCells( uint32_t quadsPerTile, uint32_t gridLod )
    {
        const uint32_t stride = 1u << gridLod;
        return ( quadsPerTile + stride - 1u ) / stride;
    }

    // The grid's index buffer: cells row-major (z outer), six indices a cell in LandscapeLod.glslh's corner order
    // (LandscapeLodCornerX / Z: (0,0) (1,1) (1,0), (0,0) (0,1) (1,1) - the procedural decode's two triangles, same
    // diagonal, same winding), node (x, z) = z * (cells + 1) + x.
    inline std::vector<uint32_t> LandscapeGridIndices( uint32_t cells )
    {
        constexpr std::array<uint32_t, 6> cornerX{ 0u, 1u, 1u, 0u, 0u, 1u };
        constexpr std::array<uint32_t, 6> cornerZ{ 0u, 1u, 0u, 0u, 1u, 1u };
        const uint32_t                    pitch = cells + 1u;
        std::vector<uint32_t>             indices;
        indices.reserve( static_cast<size_t>( cells ) * cells * 6u );
        for ( uint32_t z = 0; z < cells; ++z )
            for ( uint32_t x = 0; x < cells; ++x )
                for ( size_t c = 0; c < cornerX.size(); ++c )
                    indices.push_back( ( z + cornerZ[c] ) * pitch + x + cornerX[c] );
        return indices;
    }

    // A tile's draw, split: the head of the procedural decode - the dice rings, finest first
    // (LandscapeDisplacement.glslh LandscapeDecodeDrawVertex) - and the grid's cells outside the outermost ring's
    // square (Hole), which the decode would draw after them and the grid index buffer draws instead.
    struct LandscapeDrawSplit
    {
        uint32_t   Cells        = 0; // the grid's cells a side: its index buffer
        uint32_t   RingVertices = 0; // the procedural draw: vertices [0, RingVertices) of the decode
        glm::ivec4 Hole{ 0 };        // [x0, x1) x [z0, z1): the cells the rings draw; empty = no ring
        // [x0, x1) x [z0, z1), clamped to the grid: the grid cells drawn (LandscapeGridRangesOf) - every one by
        // default; a scrolled cascade's strip narrows a draw to the patches it reaches
        // (TerrainRenderer::RecordShadowView), whose rings then ride with one of them.
        glm::ivec4 Window{ 0, 0, std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max() };
        // THE SECTIONS (TERRAIN-LOD-1): the tile's quads and grid LOD (Cells' grid: Hole and Window are in its
        // cells), its sections a side and their quads, and the grid LOD each section draws with - the floor of its
        // own LOD (no vertex of it takes a finer one: LandscapeVertexEval.glslh LandscapeVertexLod), never finer
        // than the tile's grid, and the tile's grid where the dice rings reach it (their hole is that grid's cells).
        uint32_t                                                             Quads        = 0;
        uint32_t                                                             GridLod      = 0;
        uint32_t                                                             Sections     = 1;
        uint32_t                                                             SectionQuads = 0;
        std::array<uint8_t, kLandscapeMaxSections * kLandscapeMaxSections> SectionGridLod{};
        // THE SECTIONS THE VIEW SEES (TERRAIN-COST-2; UE culls every landscape component against the view before
        // it draws it): bit sz * kLandscapeMaxSections + sx set = section (sx, sz)'s grid cells are drawn. Every
        // section by default (the casters' draws); the camera's rasters clear the sections outside its frustum
        // (TerrainRenderer LandscapeSectionViewMask). The dice rings are not in it: their draw is the procedural
        // one.
        uint32_t Visible = ~0u;
    };

    inline bool LandscapeSectionVisible( const LandscapeDrawSplit& split, uint32_t sx, uint32_t sz )
    {
        return ( ( split.Visible >> ( sz * kLandscapeMaxSections + sx ) ) & 1u ) != 0u;
    }

    // The split of the row's draw at grid LOD @p gridLod: LandscapeDicedVertexCount's walk, kept as the decode
    // keeps it (a ring with no cells leaves the finer square the inner one). RingVertices + (Cells^2 - Hole's
    // cells) x 6 is LandscapeDicedVertexCount.
    inline LandscapeDrawSplit LandscapeDrawSplitOf( uint32_t quadsPerTile, uint32_t gridLod,
                                                    const TerrainInstance& row )
    {
        LandscapeDrawSplit split;
        split.Cells        = LandscapeLodCells( quadsPerTile, gridLod );
        split.Quads        = quadsPerTile;
        split.GridLod      = gridLod;
        split.Sections     = LandscapeSectionCount( quadsPerTile );
        split.SectionQuads = LandscapeSectionQuads( quadsPerTile );
        const uint32_t coarsest = std::max( LandscapeSectionMaxLod( quadsPerTile ), gridLod );
        for ( uint32_t sz = 0; sz < split.Sections; ++sz )
        {
            for ( uint32_t sx = 0; sx < split.Sections; ++sx )
            {
                const float lod = std::max(
                     LandscapeSectionLodAt( row.SectionLods, static_cast<int>( sx ), static_cast<int>( sz ) ), 0.0f );
                const uint32_t own = static_cast<uint32_t>( std::floor( std::min( lod, 31.0f ) ) );
                split.SectionGridLod[sz * kLandscapeMaxSections + sx] =
                     static_cast<uint8_t>( std::clamp( own, gridLod, coarsest ) );
            }
        }
        if ( gridLod != 0u || row.DisplacementView.w < 0.5f )
            return split;
        const int  cells = static_cast<int>( split.Cells );
        glm::ivec4 inner( 0 );
        for ( int level = kLandscapeDiceLevels - 1; level >= 0; --level )
        {
            const glm::ivec4 outer = LandscapeDiceRingRect( row, level, cells );
            const uint32_t   area  = LandscapeDiceRectArea( outer );
            if ( area == 0u )
                continue;
            split.RingVertices +=
                 ( area - std::min( LandscapeDiceRectArea( inner ), area ) ) * ( 1u << ( 2 * level ) ) * 6u;
            inner = outer;
        }
        split.Hole = inner;
        // The rings' hole is cut from the tile's grid (LOD 0 here): a section it reaches draws that grid.
        if ( inner.z > inner.x && inner.w > inner.y )
        {
            for ( uint32_t sz = 0; sz < split.Sections; ++sz )
            {
                for ( uint32_t sx = 0; sx < split.Sections; ++sx )
                {
                    const int32_t x0 = static_cast<int32_t>( sx * split.SectionQuads );
                    const int32_t z0 = static_cast<int32_t>( sz * split.SectionQuads );
                    const int32_t x1 = std::min( x0 + static_cast<int32_t>( split.SectionQuads ), cells );
                    const int32_t z1 = std::min( z0 + static_cast<int32_t>( split.SectionQuads ), cells );
                    if ( inner.x < x1 && x0 < inner.z && inner.y < z1 && z0 < inner.w )
                        split.SectionGridLod[sz * kLandscapeMaxSections + sx] = 0u;
                }
            }
        }
        return split;
    }

    // One indexed draw of a grid's index buffer: IndexCount indices from FirstIndex, the nodes offset by FirstRow
    // whole rows (the draw's vertex offset), Rows instances, each one row further - of the tile's grid at GridLod,
    // whose index buffer is the one of Cells cells a side.
    struct LandscapeGridRange
    {
        uint32_t FirstIndex = 0;
        uint32_t IndexCount = 0;
        uint32_t FirstRow   = 0;
        uint32_t Rows       = 1;
        uint32_t GridLod    = 0;
        uint32_t Cells      = 0;
    };

    // A run of one section row's sections drawing one grid, less the hole, is at most four rectangles.
    struct LandscapeGridRanges
    {
        std::array<LandscapeGridRange, kLandscapeMaxSections * kLandscapeMaxSections * 4> Ranges{};
        uint32_t                                                                      Count = 0;
    };

    // THE SECTIONS' DRAWS (TERRAIN-LOD-1): every section's cells on its own grid (LandscapeDrawSplit::
    // SectionGridLod), inside @p split.Window and outside @p split.Hole (both in the tile grid's cells, converted to
    // samples and to each section grid's cells; a coarser cell half inside the window is drawn whole, the window
    // being already a superset of what its strip reaches), as index ranges. A section row's run of sections on one
    // grid is one rectangle, less the hole's part of it (the rows below it, its rows left and right of it, the rows
    // above it); a rectangle as wide as its grid is one contiguous range of its rows, a narrower one a range of row
    // 0's indices (its columns) instanced once per row, grown down when the rectangle under it has its columns and
    // grid. Every cell of the window outside the hole once, none inside it.
    inline LandscapeGridRanges LandscapeGridRangesOf( const LandscapeDrawSplit& split )
    {
        LandscapeGridRanges ranges;
        const int32_t       quads = static_cast<int32_t>( split.Quads );
        const int32_t       tile  = static_cast<int32_t>( split.Cells );
        const int32_t       base  = static_cast<int32_t>( split.GridLod );
        // A tile-grid cell line in samples (the last line is the tile's last sample).
        const auto samples = [&]( const int32_t c ) { return std::min( std::clamp( c, 0, tile ) << base, quads ); };
        const int32_t wx0      = samples( split.Window.x );
        const int32_t wz0      = samples( split.Window.y );
        const int32_t wx1      = samples( split.Window.z );
        const int32_t wz1      = samples( split.Window.w );
        const int32_t hx0      = samples( split.Hole.x );
        const int32_t hz0      = samples( split.Hole.y );
        const int32_t hx1      = samples( split.Hole.z );
        const int32_t hz1      = samples( split.Hole.w );
        bool          lastWide = false;
        const auto    rect     = [&]( const uint32_t gridLod, const int32_t cells, const int32_t x0, const int32_t x1,
                               const int32_t z0, const int32_t z1 )
        {
            if ( x1 <= x0 || z1 <= z0 )
                return;
            const uint32_t     width = static_cast<uint32_t>( x1 - x0 );
            const uint32_t     rows  = static_cast<uint32_t>( z1 - z0 );
            const uint32_t     side  = static_cast<uint32_t>( cells );
            const bool         wide  = x0 == 0 && x1 == cells;
            LandscapeGridRange range;
            range.GridLod = gridLod;
            range.Cells   = side;
            if ( wide )
            {
                range.FirstIndex = static_cast<uint32_t>( z0 ) * side * 6u;
                range.IndexCount = rows * side * 6u;
            }
            else
            {
                range.FirstIndex = static_cast<uint32_t>( x0 ) * 6u;
                range.IndexCount = width * 6u;
                range.FirstRow   = static_cast<uint32_t>( z0 );
                range.Rows       = rows;
            }
            if ( ranges.Count > 0u )
            {
                LandscapeGridRange& last = ranges.Ranges[ranges.Count - 1u];
                if ( last.GridLod == gridLod && lastWide == wide )
                {
                    if ( wide && last.FirstIndex + last.IndexCount == range.FirstIndex )
                    {
                        last.IndexCount += range.IndexCount;
                        return;
                    }
                    if ( !wide && last.FirstIndex == range.FirstIndex && last.IndexCount == range.IndexCount &&
                         last.FirstRow + last.Rows == range.FirstRow )
                    {
                        last.Rows += range.Rows;
                        return;
                    }
                }
            }
            ranges.Ranges[ranges.Count++] = range;
            lastWide                      = wide;
        };
        const int32_t sq = static_cast<int32_t>( std::max( split.SectionQuads, 1u ) );
        for ( uint32_t sz = 0; sz < split.Sections; ++sz )
        {
            uint32_t runStart = 0;
            for ( uint32_t sx = 1; sx <= split.Sections; ++sx )
            {
                const uint32_t gridLod = split.SectionGridLod[sz * kLandscapeMaxSections + runStart];
                const bool     shown   = LandscapeSectionVisible( split, runStart, sz );
                if ( sx < split.Sections && split.SectionGridLod[sz * kLandscapeMaxSections + sx] == gridLod &&
                     LandscapeSectionVisible( split, sx, sz ) == shown )
                    continue;
                // A run of sections the view does not see draws nothing (LandscapeDrawSplit::Visible).
                if ( !shown )
                {
                    runStart = sx;
                    continue;
                }
                // The run [runStart, sx) of sections on grid gridLod, in samples, then in that grid's cells (a
                // section line is on every section grid; the tile's last sample closes the last cell).
                const int32_t stride = 1 << gridLod;
                const int32_t cells  = static_cast<int32_t>( LandscapeLodCells( split.Quads, gridLod ) );
                const auto    lo     = [&]( const int32_t s ) { return s / stride; };
                const auto    hi     = [&]( const int32_t s ) { return ( s + stride - 1 ) / stride; };
                const int32_t ax     = static_cast<int32_t>( runStart ) * sq;
                const int32_t az     = static_cast<int32_t>( sz ) * sq;
                const int32_t bx     = std::min( static_cast<int32_t>( sx ) * sq, quads );
                const int32_t bz     = std::min( az + sq, quads );
                const int32_t x0     = lo( std::max( ax, wx0 ) );
                const int32_t z0     = lo( std::max( az, wz0 ) );
                const int32_t x1     = hi( std::min( bx, wx1 ) );
                const int32_t z1     = hi( std::min( bz, wz1 ) );
                runStart             = sx;
                if ( x1 <= x0 || z1 <= z0 )
                    continue;
                // The hole's part of the rectangle (only a section on the tile's grid meets it: LandscapeDrawSplitOf).
                const int32_t ix0 = std::clamp( lo( hx0 ), x0, x1 );
                const int32_t iz0 = std::clamp( lo( hz0 ), z0, z1 );
                const int32_t ix1 = std::clamp( hi( hx1 ), x0, x1 );
                const int32_t iz1 = std::clamp( hi( hz1 ), z0, z1 );
                if ( ix1 <= ix0 || iz1 <= iz0 )
                {
                    rect( gridLod, cells, x0, x1, z0, z1 );
                    continue;
                }
                rect( gridLod, cells, x0, x1, z0, iz0 );   // the rows below the hole
                rect( gridLod, cells, x0, ix0, iz0, iz1 ); // its rows, left of it
                rect( gridLod, cells, ix1, x1, iz0, iz1 ); // its rows, right of it
                rect( gridLod, cells, x0, x1, iz1, z1 );   // the rows above it
            }
        }
        return ranges;
    }

    // The triangles a draw rasters: its rings' and its grid ranges' (TerrainRenderer's census of the camera's).
    inline uint64_t LandscapeDrawTriangles( const LandscapeDrawSplit& split )
    {
        uint64_t                  triangles = split.RingVertices / 3u;
        const LandscapeGridRanges ranges    = LandscapeGridRangesOf( split );
        for ( uint32_t r = 0; r < ranges.Count; ++r )
            triangles += uint64_t{ ranges.Ranges[r].IndexCount / 3u } * ranges.Ranges[r].Rows;
        return triangles;
    }

    // The dice pool's vertices for the row (LandscapeDisplacement.glslh LandscapeDecodePoolVertex walks the same
    // order): each ring's cells outside the finer ring, (dice + 1)^2 sub-vertices a cell; 0 when it is not diced.
    inline uint32_t LandscapeDicePoolVertexCount( uint32_t quadsPerTile, uint32_t gridLod,
                                                  const TerrainInstance& row )
    {
        if ( gridLod != 0u || row.DisplacementView.w < 0.5f )
            return 0u;
        const int cells = static_cast<int>( quadsPerTile );
        uint32_t  total = 0u, inner = 0u;
        for ( int level = kLandscapeDiceLevels - 1; level >= 0; --level )
        {
            const uint32_t area = LandscapeDiceRectArea( LandscapeDiceRingRect( row, level, cells, true ) );
            const uint32_t side = ( 1u << level ) + 1u;
            total += ( area - std::min( inner, area ) ) * side * side;
            if ( area > 0u )
                inner = area;
        }
        return total;
    }

    // THE DICE POOL'S CORNERS (TERRAIN-COST-2; UE Nanite displaces each vertex once and its triangles index it):
    // the grid nodes of the camera's fade square (ring 0, which holds every pooled cell) - (x1 - x0 + 1) x (z1 -
    // z0 + 1), row-major from (x0, z0) - each evaluated once a frame (LandscapeEvaluateVertex: its LOD, morph,
    // height and gradient) by the pool's first dispatch, and read by every sub-vertex of the up to four diced
    // cells around it (LandscapeDicePool.shader); 0 when the row pools nothing.
    inline uint32_t LandscapeDicePoolCornerCount( uint32_t quadsPerTile, uint32_t gridLod,
                                                  const TerrainInstance& row )
    {
        if ( gridLod != 0u || row.DisplacementView.w < 0.5f )
            return 0u;
        const glm::ivec4 r = LandscapeDiceRingRect( row, 0, static_cast<int>( quadsPerTile ), true );
        if ( LandscapeDiceRectArea( r ) == 0u )
            return 0u;
        return static_cast<uint32_t>( ( r.z - r.x + 1 ) * ( r.w - r.y + 1 ) );
    }

    // THE VIEW'S FOOTPRINT ON THE GROUND (TerrainInstance.DiceClip): the world XZ rectangle (x0, z0, x1, z1) that
    // the perspective view @p viewProjection, whose eye is @p eye, reaches between the heights @p minY and @p maxY
    // - the bounding rectangle of the view's pyramid (apex at the eye, the near plane inside it) cut by that
    // slab: the pyramid's vertices inside the slab plus where its edges cross the slab's two planes (the vertices
    // of a convex body cut by a slab). @p reach caps the pyramid (cm along each corner ray): nothing past it is
    // diced. Empty (x0 > x1) when the view does not reach the slab.
    inline glm::vec4 LandscapeViewFootprint( const glm::mat4& viewProjection, const glm::vec3& eye, float minY,
                                             float maxY, float reach )
    {
        const glm::dmat4          inverse = glm::inverse( glm::dmat4( viewProjection ) );
        const glm::dvec3          apex( eye );
        std::array<glm::dvec3, 4> farCorners{};
        for ( int i = 0; i < 4; ++i )
        {
            // A point on the corner's ray at NDC depth 0.5 (finite for a standard, reversed or infinite far
            // plane): only the ray's direction is used.
            const glm::dvec4 p = inverse * glm::dvec4( ( i & 1 ) ? 1.0 : -1.0, ( i & 2 ) ? 1.0 : -1.0, 0.5, 1.0 );
            const glm::dvec3 d = glm::dvec3( p ) / p.w - apex;
            farCorners[static_cast<size_t>( i )] = apex + glm::normalize( d ) * static_cast<double>( reach );
        }
        glm::dvec4 box( std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                        std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest() );
        const auto add = [&]( const glm::dvec3& q )
        {
            box = glm::dvec4( std::min( box.x, q.x ), std::min( box.y, q.z ), std::max( box.z, q.x ),
                              std::max( box.w, q.z ) );
        };
        const double lo = minY, hi = maxY;
        const auto   point = [&]( const glm::dvec3& q )
        {
            if ( q.y >= lo && q.y <= hi )
                add( q );
        };
        const auto edge = [&]( const glm::dvec3& a, const glm::dvec3& b )
        {
            for ( const double y : { lo, hi } )
                if ( ( a.y - y ) * ( b.y - y ) < 0.0 )
                    add( a + ( b - a ) * ( ( y - a.y ) / ( b.y - a.y ) ) );
        };
        point( apex );
        for ( int i = 0; i < 4; ++i )
        {
            point( farCorners[static_cast<size_t>( i )] );
            edge( apex, farCorners[static_cast<size_t>( i )] );
        }
        // The far face's four edges, corners in ring order (-,-) (+,-) (+,+) (-,+).
        constexpr std::array<int, 4> ring{ 0, 1, 3, 2 };
        for ( int i = 0; i < 4; ++i )
            edge( farCorners[static_cast<size_t>( ring[static_cast<size_t>( i )] )],
                  farCorners[static_cast<size_t>( ring[static_cast<size_t>( ( i + 1 ) % 4 )] )] );
        return box.x > box.z ? glm::vec4( 1.0f, 1.0f, 0.0f, 0.0f ) : glm::vec4( box );
    }

    // THE CAMERA'S ROW DICED FOR AN ORTHOGRAPHIC SHADOW VIEW whose texel is @p worldPerTexel cm (UE: Nanite dices
    // every view by r.Nanite.DicingRate pixels of THAT view - a shadow view by its own texels). The camera's
    // rings stop at dice 2^L, the finest whose sub-cell edge still spans @p dicingRate texels; the finer rings
    // are dropped (-1, as ApplyLandscapeDisplacement leaves a ring no camera distance reaches), so every vertex
    // the cascade keeps is one of the camera's own, displaced by the same evaluate: the caster is the receivers'
    // surface sampled at the cascade's resolution. A row the camera does not dice is returned as it is.
    inline TerrainInstance LandscapeCasterDicedRow( const TerrainInstance& camera, float spacingCm,
                                                    float worldPerTexel, int dicingRate )
    {
        TerrainInstance row = camera;
        if ( camera.DisplacementView.w < 0.5f || worldPerTexel <= 0.0f || dicingRate <= 0 )
            return row;
        const float subCells = spacingCm / ( worldPerTexel * static_cast<float>( dicingRate ) );
        const int   finest   = subCells < 2.0f ? 0
                                               : std::min( kLandscapeDiceLevels - 1,
                                                           static_cast<int>( std::floor( std::log2( subCells ) ) ) );
        // The rings stay the camera's (the pool's layout); the row's view stops at `finest`.
        row.DicePool.y = static_cast<float>( finest );
        return row;
    }

    // The texture half of a terrain material's identity. Two terrains may share one material only if
    // they bind the same textures: the sorted texture-override handles plus the address of the landscape
    // tile's heightmap (runtime-owned, no asset handle — same spelling MeshRenderer uses for the font
    // atlas), and of its weightmap when it has one. Every landscape tile is therefore its own material.
    // Sorted, because two terrains naming the same textures in a different order are the same texture
    // set and must share one material rather than allocate two.
    inline std::string TerrainTextureKey( const MaterialOverrides& overrides, const void* heightmap,
                                          const void* weightmap )
    {
        std::vector<std::string> parts;
        parts.reserve( overrides.Textures.size() + 2 );
        for ( const auto& [name, handle] : overrides.Textures )
            if ( handle != 0 )
                parts.push_back( std::format( "{}={}", name, handle ) );
        if ( heightmap != nullptr )
            parts.push_back( std::format( "HeightmapTexture=@{}", std::bit_cast<uintptr_t>( heightmap ) ) );
        if ( weightmap != nullptr )
            parts.push_back( std::format( "WeightmapTexture=@{}", std::bit_cast<uintptr_t>( weightmap ) ) );
        std::sort( parts.begin(), parts.end() );

        // Every part is led by '|', so a key with no parts is empty.
        return parts.empty() ? std::string() : std::format( "|{}", Common::Text::Join( parts, "|" ) );
    }
} // namespace Desert::Graphic::System
