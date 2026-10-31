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
        // x = tile extent (cm), y = the tile's continuous LOD (LandscapeLodFromScreenSize), z = the tile's
        // visibility layer + 1 (0 = no holes; LandscapeWeightDraw::VisibilityLayer, LandscapeVisibility),
        // w = the LOD whose grid the tile is drawn with (floor of y; the vertex count is that grid's).
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
        // The LOD a vertex on each border takes: max(tile, neighbour) for West, East, South, North.
        glm::vec4 LodEdges{ 0.0f };
        // ...and on each corner: the max of the four tiles sharing it, at (-x,-z), (+x,-z), (-x,+z), (+x,+z).
        glm::vec4 LodCorners{ 0.0f };
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
        // and lens scale them in the shader (TerrainUB::GrassLens.x), not here (GRASS-VIS-17).
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> GrassCover{};
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> GrassCoverLight{};
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> GrassCoverRange{};
        // LANDSCAPE DISPLACEMENT (UE 5.4+ landscape Nanite displacement: the material's Displacement output, the
        // layers' height x Magnitude around Center, faded out with distance). x = DisplacementMagnitude (cm; 0 = the
        // tile is drawn flat), y = DisplacementCenter, z = DisplacementFadeStart, w = DisplacementFadeEnd (cm).
        glm::vec4 Displacement{ 0.0f };
        // xyz = the MAIN camera (cm): the fade's origin for the camera AND every caster view, so a cascade sees
        // the surface the camera sees. w = 1: the tile is drawn diced in rings (DiceRings), 0: its plain LOD grid.
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
        // quality's DistanceScale on GrassCoverRange x / y) is the frame's TerrainUB::GrassLens: D = width x
        // PixelsPerUnit in the shader (LandscapeGrassCoverAt). Nothing in this row follows the lens or the render
        // width, so a zoom or a resolution change neither re-uploads it nor rebuilds the tile's ray tracing row
        // (LandscapeRayTracing compares the row) - UE's view uniforms apart from the primitive's scene data.
        std::array<glm::vec4, 2> GrassCoverFloorWidth{};
    };

    static_assert( World::Landscape::kLandscapeMaxWeightLayers ==
                        2u * World::Landscape::kLandscapeWeightmapChannels,
                   "TerrainInstance carries exactly two weightmap pages of layer data - LandscapeWeights.glslh "
                   "blends two RGBA8 pages" );
    static_assert( sizeof( TerrainInstance ) == 65 * sizeof( glm::vec4 ),
                   "TerrainInstance must stay sixty-five 16-byte slots - the GLSL mirror in TerrainInstance.glslh "
                   "reads these offsets" );
    static_assert(
         offsetof( TerrainInstance, Params2 ) == 16 && offsetof( TerrainInstance, GroundColor ) == 32 &&
              offsetof( TerrainInstance, LandscapeFrame ) == 48 &&
              offsetof( TerrainInstance, LandscapeTile ) == 64 && offsetof( TerrainInstance, LodEdges ) == 80 &&
              offsetof( TerrainInstance, LodCorners ) == 96 && offsetof( TerrainInstance, LayerColors ) == 112 &&
              offsetof( TerrainInstance, LayerAlphaBlend ) == 240 &&
              offsetof( TerrainInstance, LayerInput ) == 272 && offsetof( TerrainInstance, InputSlices ) == 304 &&
              offsetof( TerrainInstance, GroundInput ) == 432 && offsetof( TerrainInstance, GrassCover ) == 448 &&
              offsetof( TerrainInstance, GrassCoverLight ) == 576 &&
              offsetof( TerrainInstance, GrassCoverRange ) == 704 &&
              offsetof( TerrainInstance, Displacement ) == 832 &&
              offsetof( TerrainInstance, DisplacementView ) == 848 &&
              offsetof( TerrainInstance, DiceWindow ) == 864 &&
              offsetof( TerrainInstance, DisplacementTiling ) == 880 &&
              offsetof( TerrainInstance, InputTileSize ) == 896 &&
              offsetof( TerrainInstance, InputBlend ) == 928 && offsetof( TerrainInstance, DiceRings ) == 960 &&
              offsetof( TerrainInstance, DicePool ) == 976 && offsetof( TerrainInstance, DiceClip ) == 992 &&
              offsetof( TerrainInstance, GrassCoverFloorWidth ) == 1008,
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
    // instead of the heightmap's last mip. The whole tile takes ONE continuous LOD per view: the main camera's
    // for the camera's passes and the cascades (they draw the camera's vertices, so a sun shadow cannot swim
    // against its caster), and a point/spot light's own, from its position, for that light's views — their
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

    inline LandscapeLodSettings MakeLandscapeLodSettings( uint32_t quadsPerTile )
    {
        LandscapeLodSettings settings;
        const uint32_t       maxLod    = LandscapeMaxLod( quadsPerTile );
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
