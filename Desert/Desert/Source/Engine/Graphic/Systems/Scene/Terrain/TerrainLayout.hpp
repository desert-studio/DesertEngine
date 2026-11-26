#pragma once

// THE LANDSCAPE'S GPU RECORDS, declared once: Tools/ShaderProtocolGen writes their GLSL twins into
// Generated/Terrain/Terrain.glslh (read by Common/TerrainInstance.glslh and Common/LandscapeSurface.glslh) and checks
// the layouts against these declarations at compile time.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>
#include <LandscapeCore/Public/LandscapeLayerLimits.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Desert::Graphic::System
{
    using Vec4Array2            = std::array<glm::vec4, 2>;
    using Vec4Array9            = std::array<glm::vec4, 9>;
    using Vec4ArrayWeightLayers = std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers>;

    static_assert( World::Landscape::kLandscapeMaxWeightLayers ==
                        2u * World::Landscape::kLandscapeWeightmapChannels,
                   "TerrainInstance carries exactly two weightmap pages of layer data - LandscapeWeights.glslh "
                   "blends two RGBA8 pages" );

    // One terrain's per-draw engine data, as the Terrain shader reads it.
#define DESERT_TERRAIN_INSTANCE_FIELDS( X ) \
    /* x = tile extent (cm), y = the tile's FINEST continuous LOD (the least of its sections', SectionLods), z \
       = the tile's visibility layer + 1 (0 = no holes; LandscapeWeightDraw::VisibilityLayer, \
       LandscapeVisibility), w = the LOD of the tile's grid (floor of y): the grid the dice rings, the virtual \
       shadow map's procedural draw and the ray tracing BLAS walk; a section's raster draw takes its own \
       coarser grid (LandscapeDrawSplit::SectionGridLod). */ \
    X( glm::vec4, Params, "Params", glm::vec4( 0.0f ) ) \
    /* x = 1 / LOD blend range (UE's InvLODBlendRange), y = the tile's weight layer count (0 = no weightmap \
       bound; the surface keeps its ground), z = the weightmap's page count (LandscapeWeightmapPageCount of y: \
       the pages are stacked along the image's height, see LandscapeWeightDraw), w = the tile's \
       LandscapeNeighbourMask (which sides have their ring row in the heightmap). */ \
    X( glm::vec4, Params2, "Params2", glm::vec4( 0.0f ) ) \
    /* The ground under the painted layers: rgb = the root's FIRST layer colour, a = 1 when the root names a \
       layer (UE shows its first layer where nothing is painted); a = 0 draws white ground. See \
       LandscapeWeightDraw::Ground. */ \
    X( glm::vec4, GroundColor, "GroundColor", glm::vec4( 0.0f ) ) \
    /* x/z = the ROOT's origin, y = its base height, w = spacing (cm between samples), see LandscapeTileDraw. */ \
    X( glm::vec4, LandscapeFrame, "LandscapeFrame", glm::vec4( 0.0f ) ) \
    /* x/y = this tile's first sample in the landscape's global sample grid, z = quads per tile side, w = \
       vertical scale (cm per local height unit). */ \
    X( glm::vec4, LandscapeTile, "LandscapeTile", glm::vec4( 0.0f ) ) \
    /* THE SECTIONS (TERRAIN-LOD-1; UE: a landscape component's sections each take their own LOD from their \
       own bounds' screen size): x = a section's quads a side (LandscapeSectionQuads), y = the sections a tile \
       side (LandscapeSectionCount), z = a section's coarsest grid LOD (LandscapeSectionMaxLod), w unused. */ \
    X( glm::vec4, Sections, "Sections", glm::vec4( 0.0f ) ) \
    /* The continuous LOD of every section and of the ring of the neighbours' sections around them: \
       kLandscapeSectionTable x kLandscapeSectionTable floats, section (sx, sz) for sx, sz in [-1, Sections.y] \
       at (sz + 1) * kLandscapeSectionTable + sx + 1, four to a vec4 (0 = no neighbour there: nothing to agree \
       with). A vertex's LOD is its section's, blended toward the max() with the section across its border \
       (LandscapeVertexEval.glslh LandscapeVertexLod) - the same max() from both sides, so a seam between \
       sections, and between tiles, is one value. */ \
    X( Vec4Array9, SectionLods, "SectionLods", {} ) \
    /* The colour each tile layer paints (rgb) and whether that layer is drawn at all (a = 1; a = 0 ignores \
       it), in the tile's layer order - layer i is page i / 4, channel i % 4. See LandscapeWeightDraw. */ \
    X( Vec4ArrayWeightLayers, LayerColors, "LayerColors", {} ) \
    /* Per layer, four to a vec4 like the pages: 1 = the layer is NoWeightBlend (UE LB_AlphaBlend), laid over \
       the weight blend. */ \
    X( Vec4Array2, LayerAlphaBlend, "LayerAlphaBlend", {} ) \
    /* Per layer, four to a vec4 like the pages: the landscape material's input that names this layer's \
       LayerName (TerrainLayerInputs; -1: no input does, the layer keeps its colour). */ \
    X( Vec4Array2, LayerInput, "LayerInput", ( Vec4Array2{ glm::vec4( -1.0f ), glm::vec4( -1.0f ) } ) ) \
    /* Per material input k: its slice in the albedo, normal, ARM and height arrays (x, y, z, w; -1 = the \
       input has no map of that kind - flat colour, flat normal, AO/roughness 1, height 0.5). */ \
    X( Vec4ArrayWeightLayers, InputSlices, "InputSlices", {} ) \
    /* x = the input the GROUND draws with (the root's first layer, by name; -1 = none), y = the tile's empty \
       weightmap channels (LandscapeWeightDraw::EmptyLayers, a bit mask as a float; 0 = every channel walked), \
       zw unused. */ \
    X( glm::vec4, GroundInput, "GroundInput", glm::vec4( -1.0f, 0.0f, 0.0f, 0.0f ) ) \
    /* GRASS-FIELD: per tile layer, the grass field's far cover (LandscapeGrassCover, see \
       LandscapeWeightDraw): the blades the generator thins out with distance are carried on by the ground's \
       own shading. GrassCoverRange x / y = the blades' AUTHORED Start / EndCullDistance (cm): the view's \
       Foliage quality and lens scale them in the shader (CameraUB::GrassLens.x), not here (GRASS-VIS-17). */ \
    X( Vec4ArrayWeightLayers, GrassCover, "GrassCover", {} ) \
    X( Vec4ArrayWeightLayers, GrassCoverLight, "GrassCoverLight", {} ) \
    X( Vec4ArrayWeightLayers, GrassCoverRange, "GrassCoverRange", {} ) \
    /* LANDSCAPE DISPLACEMENT (UE 5.4+ landscape Nanite displacement: the material's Displacement output, the \
       layers' height x Magnitude around Center, faded out with distance). x = DisplacementMagnitude (cm; 0 = \
       the tile is drawn flat), y = DisplacementCenter, z = DisplacementFadeStart, w = DisplacementFadeEnd \
       (cm). */ \
    X( glm::vec4, Displacement, "Displacement", glm::vec4( 0.0f ) ) \
    /* xyz = the MAIN camera (cm): the fade's origin for the camera AND every cascade view, so a cascade sees \
       the surface the camera sees. w = 1: the tile is drawn diced in rings (DiceRings), 0: its plain LOD \
       grid. The virtual shadow map's row (LandscapeVirtualShadowRow) holds its anchor in xyz and 1 + the \
       reach (cm) in w: its fade is the lower envelope of the fades of every eye within the reach of the \
       anchor (LandscapeDisplacement.glslh LandscapeDicedPoint), so its cached pages do not follow the eye's \
       step. */ \
    X( glm::vec4, DisplacementView, "DisplacementView", glm::vec4( 0.0f ) ) \
    /* THE DICE RINGS (LandscapeDisplacement.glslh), square rings of cells around the main camera on the \
       tile's LOD-0 grid, each diced finer than the ring outside it (UE Nanite: finer where a triangle is \
       larger on screen): x, y = the camera's cell (tile-local, may lie outside the tile), z = the half-extent \
       (cells) of the fade's square - every cell the fade reaches, dice 1 and displaced -, w = DicingRate \
       pixels' width in cm per cm of distance (the footprint the layer heights' mip is read at). */ \
    X( glm::vec4, DiceWindow, "DiceWindow", glm::vec4( 0.0f ) ) \
    /* x = FarTilingDistance (cm), y = FarTilingScale: the layer height's far fetch, as the surface blends it. */ \
    X( glm::vec4, DisplacementTiling, "DisplacementTiling", glm::vec4( 0.0f ) ) \
    /* Per material input k (four to a vec4): LayerKTileSize (cm) and LayerKBlend, the material values the \
       displaced vertex needs in every program (the casters have no landscape material). */ \
    X( Vec4Array2, InputTileSize, "InputTileSize", {} ) \
    X( Vec4Array2, InputBlend, "InputBlend", {} ) \
    /* ...and the half-extents (cells) of the rings diced 2, 4, 8 and 16 sub-cells a side (x..w; -1 = no such \
       ring). Each lies inside the one before: a cell's dice is the finest ring holding it. */ \
    X( glm::vec4, DiceRings, "DiceRings", glm::vec4( -1.0f ) ) \
    /* THE DICE POOL (LandscapeDicePool.shader): x = the tile's first vertex in the frame's pool of displaced \
       sub-vertices, evaluated once per frame in the camera's rings and read by every view's draw (-1 = the \
       tile has none: it is not drawn diced); y = the finest dice level THIS row's view draws - 4 for the \
       camera, coarser for a cascade (LandscapeCasterDicedRow), whose cells then read every k-th pool vertex. */ \
    X( glm::vec4, DicePool, "DicePool", glm::vec4( -1.0f, 4.0f, 0.0f, 0.0f ) ) \
    /* THE VIEW'S FOOTPRINT ON THE DICE (UE Nanite culls every cluster outside the view before it is diced): \
       [x, y) .. [z, w) - the tile-local LOD-0 cells (the camera's GLOBAL cells minus the tile's first sample, \
       not clamped to the tile, so two tiles of a border agree) that the main camera's frustum reaches between \
       the landscape's lowest and highest displaced height, plus a margin. The rings diced 2..16 are clipped \
       to it; a cell outside keeps the fade square's dice 1, displaced, so no crack opens at the clip and an \
       off-screen bump still casts into the view (LandscapeDiceFootprint, LandscapeDisplacement.glslh \
       LandscapeCellDiceOf). The casters draw the camera's rows, so they keep the same clip. */ \
    X( glm::vec4, DiceClip, "DiceClip", glm::vec4( 0.0f ) ) \
    /* GRASS-VIS-17: per tile layer, four to a vec4 like the pages, its blades' mean width per MinScreenWidth \
       pixel (cm / px; 0 = no screen-width floor; LandscapeWeightDraw::GrassCoverFloorWidth) - the primitive's \
       half of the floor's distance D. The view's half (GrassFieldLens::PixelsPerUnit, and the Foliage \
       quality's DistanceScale on GrassCoverRange x / y) is the view block's CameraUB::GrassLens: D = width x \
       PixelsPerUnit in the shader (LandscapeGrassCoverAt). Nothing in this row follows the lens or the render \
       width, so a zoom or a resolution change neither re-uploads it nor rebuilds the tile's ray tracing row \
       (LandscapeRayTracing compares the row) - UE's view uniforms apart from the primitive's scene data. */ \
    X( Vec4Array2, GrassCoverFloorWidth, "GrassCoverFloorWidth", {} ) \
    /* GPU-COST-29: per tile layer, four to a vec4, the share of its grass leaf area that is blades (their \
       survivors widen 1 / sqrt(share)); the rest is mesh varieties, whose survivors keep their size and \
       shrink out. The far cover weighs the two coverages by it (GrassFieldShare.glslh GrassFieldCoverageOf). */ \
    X( Vec4Array2, GrassCoverWiden, "GrassCoverWiden", {} ) \
    /* GRASS-SHADOW-1: per tile layer, four to a vec4, its mesh varieties' bounding radius about the root at \
       their mean scale (cm, weighted by leaf area; 0 = no mesh variety; LandscapeWeightDraw:: \
       GrassCoverFarLodRadius) - the primitive's half of the meshes' far LOD distance. The view's half is \
       CameraUB::GrassLens.z (GrassFieldLens::FarLodScale): the cover takes over what the far LOD drops \
       (GrassFieldShare.glslh GrassMeshFarLod), so the ground past it is as covered as the field it expects. */ \
    X( Vec4Array2, GrassCoverFarLodRadius, "GrassCoverFarLodRadius", {} )
    DESERT_SHADER_STRUCT( TerrainInstance, "TerrainInstance", Std430, DESERT_TERRAIN_INSTANCE_FIELDS );
#undef DESERT_TERRAIN_INSTANCE_FIELDS

    /**
     * @brief THE LANDSCAPE MATERIAL'S VALUES the ground reads (Terrain.shader / TerrainGBuffer.shader Properties).
     *
     * Its GLSL `LandscapeMaterial` (generated into Generated/Terrain/Terrain.glslh) is the struct the one statement
     * of the ground (EvaluateLandscapeSurface) takes - the raster builds it from its Material, the landscape's ray
     * tracing hit group reads it from its row. vec4 slots only, so one layout in std140, std430 and here.
     */
#define DESERT_LANDSCAPE_MATERIAL_VALUES_FIELDS( X ) \
    /* / FarTilingDistance (cm), FarTilingScale, MacroVariationSize (cm), MacroVariationStrength. */ \
    X( glm::vec4, Tiling, "Tiling", glm::vec4( 0.0f ) ) \
    /* / DetailNormalScale (times the input's tile size), DetailNormalStrength, MacroHueSize (cm), \
       MacroHueStrength / (radians). */ \
    X( glm::vec4, Detail, "Detail", glm::vec4( 0.0f ) ) \
    X( Vec4ArrayWeightLayers, LayerTint, "LayerTint", {} ) /* / Input k's LayerKTint. */ \
    /* / Input k's (LayerKTileSize cm, LayerKBlend, LayerKNormalStrength, 0). */ \
    X( Vec4ArrayWeightLayers, LayerParams, "LayerParams", {} ) \
    /* / The weather's: x = Porosity (how much of the rain's film the ground soaks in, \
       Common/WeatherRain.glslh), / y, z, w = 0. */ \
    X( glm::vec4, Weather, "Weather", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( LandscapeMaterialValues, "LandscapeMaterial", Both,
                          DESERT_LANDSCAPE_MATERIAL_VALUES_FIELDS );
#undef DESERT_LANDSCAPE_MATERIAL_VALUES_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kTerrainStructs[] = {
        &kTerrainInstanceStruct,
        &kLandscapeMaterialValuesStruct,
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kTerrainShaderInclude{
        .Path         = "Generated/Terrain/Terrain.glslh",
        .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Terrain/TerrainLayout.hpp",
        .Structs      = kTerrainStructs,
    };
} // namespace Desert::Graphic::System
