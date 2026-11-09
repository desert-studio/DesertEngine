#include "TerrainRenderer.hpp"
#include <Engine/Graphic/MeshDrawArguments.hpp>

#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/Frustum.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/RenderPass.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/Materials/MaterialBinder.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/Materials/DrawPushFields.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Core/Units.hpp>

#include <algorithm>
#include <tuple>
#include <array>
#include <cstddef>
#include <cstring>
#include <bit>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct TerrainShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, TerrainInstances );
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeVertexPool );
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeDiceCells );
    };

    // "Landscape: Dice Pool" (LandscapeDicePool.shader).
    struct DicePoolShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeDicePoolRows );
        DESERT_SHADER_PARAMETER( Texture, HeightmapTexture );
        DESERT_SHADER_PARAMETER( Texture, WeightmapTexture );
        DESERT_SHADER_PARAMETER( Texture, LayerHeightTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeDicePoolVertices );
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeDicePoolCorners );
    };
    constexpr std::string_view kDicePoolPass       = "Landscape: Dice Pool";
    constexpr std::string_view kDicePoolCornerPass = "Landscape: Dice Pool Corners";
    constexpr std::string_view kDicePoolShader     = "LandscapeDicePool";
    constexpr uint32_t         kDicePoolLocalSize  = 64; // LocalSize of the shader

    // "Landscape: Dice Cull" (LandscapeDiceCull.shader, TERRAIN-DICE-CULL).
    struct DiceCullShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeDiceCullRows );
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeDicePoolCorners );
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeDiceCells );
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeDiceCullArgs );
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeDiceCullView );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, LandscapeDiceHzbTexture );
    };
    struct DiceCullPush
    {
        glm::uvec4 Hzb{ 0u };      // xy = the HZB's mip-0 size, z = its mip count (0: the frustum alone)
        glm::uvec4 Tile{ 0u };     // the row, the fade square's cells, the first corner node, the first record
        glm::vec4  Bounds{ 0.0f }; // x = the displacement's reach (cm)
    };
    constexpr std::string_view kDiceCullPass      = "Landscape: Dice Cull";
    constexpr std::string_view kDiceCullShader    = "LandscapeDiceCull";
    constexpr uint32_t         kDiceCullLocalSize = 64; // LocalSize of the shader
    constexpr uint32_t         kDiceLevels =
         static_cast<uint32_t>( Desert::Graphic::System::kLandscapeDiceLevels );
    // A record is a VkDrawIndirectCommand - vertexCount (the cull adds to it), instanceCount, firstVertex,
    // firstInstance - and ring level l's draw starts at kLandscapeDiceCulledVertex x (l + 1)
    // (LandscapeDisplacement.glslh LANDSCAPE_DICE_CULLED_VERTEX, the same number).
    constexpr uint32_t kDiceCullRecordWords       = 4;
    constexpr uint32_t kLandscapeDiceCulledVertex = 1u << 27;
    // The eye's largest move (cm) since the HZB's frame at which last frame's HZB still rejects: a pure turn keeps
    // every occlusion (the same rays from the same eye); a moved eye uncovers what the HZB hid, and the terrain has
    // no second (post-occlusion) phase yet, so then the frustum alone culls.
    constexpr float kDiceHzbStillCm = 0.01f;
} // namespace

namespace Desert::Graphic::System
{
    namespace
    {
        // THE BOX A TILE CASTS FROM, as a cascade's caster box is tested against it (the content key's reach,
        // CascadeShadowContent, and a scrolled cascade's strips, RecordShadowView): the tile's own square - the
        // root's origin plus its first sample (SceneRenderer::SubmitLandscapeTile places it the same way) - over
        // its undisplaced slab, grown by the tile's side every way: the displacement moves the surface off the
        // slab by less than a tile.
        [[nodiscard]] Common::Math::AABB TileCasterBox( const LandscapeTileDraw& l )
        {
            const float side = l.SpacingCm * static_cast<float>( l.QuadsPerTile );
            const float x0   = l.OriginX + static_cast<float>( l.FirstSampleX ) * l.SpacingCm;
            const float z0   = l.OriginZ + static_cast<float>( l.FirstSampleZ ) * l.SpacingCm;
            return { glm::vec3( x0 - side, l.MinY - side, z0 - side ),
                     glm::vec3( x0 + 2.0f * side, l.MaxY + side, z0 + 2.0f * side ) };
        }

        // A SCROLLED CASCADE'S STRIP ON A TILE (RecordShadowView; UE: a scrolled cached map renders the region
        // its shift exposed alone, culling every primitive outside it): the patches of kStripPatchCells cells a
        // side of the tile's grid that a strip reaches, as windows of LandscapeDrawSplit::Window's form - a run of
        // reached patches of one patch row, grown down by the run of the next row with the same columns - and
        // whether the rings reach one. A patch's box is its samples' square over the tile's undisplaced slab,
        // grown by what moves a vertex off it: the LOD snap and morph across (a vertex snaps down to its LOD's
        // grid and morphs down to the next one, LandscapeLod.glslh: less than two cells of the coarsest grid any
        // vertex of the tile takes - its sections' LODs and their neighbours' across its borders), and the
        // displacement's reach along the normal every way ((height - Center) x Magnitude never leaves +-Magnitude,
        // TileViewBox's rule).
        constexpr int32_t kStripPatchCells = 16;

        struct StripPatches
        {
            std::vector<glm::ivec4> Windows; // [x0, x1) x [z0, z1) cells of the draw's grid
            bool                    Rings = false;
        };

        [[nodiscard]] StripPatches StripPatchesOf( const std::span<const Core::Frustum> strips,
                                                   const LandscapeTileDraw& l, const TerrainInstance& row,
                                                   const LandscapeDrawSplit& split )
        {
            StripPatches  patches;
            const int32_t cells = static_cast<int32_t>( split.Cells );
            if ( cells <= 0 || strips.empty() )
                return patches;
            const int32_t gridLod = std::clamp( static_cast<int32_t>( row.Params.w ), 0, 16 );
            const int32_t stride  = 1 << gridLod;
            float         lodMax  = std::max( row.Params.y, 0.0f );
            for ( const glm::vec4& lods : row.SectionLods ) // its sections' and the neighbours' across its borders
                lodMax = std::max( { lodMax, lods.x, lods.y, lods.z, lods.w } );
            const int32_t morphLod     = static_cast<int32_t>( std::floor( std::min( lodMax, 16.0f ) ) ) + 1;
            const float   morphSamples = std::ldexp( 1.0f, morphLod );
            const float   reach        = std::max( row.Displacement.x, 0.0f );
            const float   acrossCm     = morphSamples * std::abs( l.SpacingCm ) + reach;
            const float   tileX        = l.OriginX + static_cast<float>( l.FirstSampleX ) * l.SpacingCm;
            const float   tileZ        = l.OriginZ + static_cast<float>( l.FirstSampleZ ) * l.SpacingCm;
            const int32_t quads        = static_cast<int32_t>( l.QuadsPerTile );
            const float   lowY         = std::min( l.MinY, l.MaxY ) - reach;
            const float   highY        = std::max( l.MinY, l.MaxY ) + reach;
            // The box of the grid's cells [cx0, cx1) x [cz0, cz1).
            const auto cellsBox = [&]( const int32_t cx0, const int32_t cx1, const int32_t cz0, const int32_t cz1 )
            {
                const float ax = tileX + static_cast<float>( std::min( cx0 * stride, quads ) ) * l.SpacingCm;
                const float bx = tileX + static_cast<float>( std::min( cx1 * stride, quads ) ) * l.SpacingCm;
                const float az = tileZ + static_cast<float>( std::min( cz0 * stride, quads ) ) * l.SpacingCm;
                const float bz = tileZ + static_cast<float>( std::min( cz1 * stride, quads ) ) * l.SpacingCm;
                return Common::Math::AABB{
                     glm::vec3( std::min( ax, bx ) - acrossCm, lowY, std::min( az, bz ) - acrossCm ),
                     glm::vec3( std::max( ax, bx ) + acrossCm, highY, std::max( az, bz ) + acrossCm ) };
            };
            // Whether a strip reaches the grid's cells [cx0, cx1) x [cz0, cz1).
            const auto reaches = [&]( const int32_t cx0, const int32_t cx1, const int32_t cz0, const int32_t cz1 )
            {
                const Common::Math::AABB box = cellsBox( cx0, cx1, cz0, cz1 );
                for ( const Core::Frustum& strip : strips )
                    if ( strip.Intersects( box ) )
                        return true;
                return false;
            };
            // The patches per side of the grid. `reaches` for the four patches px0 .. px0 + 3 of the row
            // [cz0, cz1) (bit i: patch px0 + i; bits at or past `side` clear): the same boxes, each strip
            // testing the four at once (Intersects4, Intersects bit for bit), a lane kept when any strip keeps it
            // - the per-patch loop's "any strip" answer.
            const int32_t side     = ( cells + kStripPatchCells - 1 ) / kStripPatchCells;
            const auto    reaches4 = [&]( const int32_t px0, const int32_t cz0, const int32_t cz1 )
            {
                Common::Math::AABBQuad quad;
                uint32_t               live = 0u;
                for ( int32_t lane = 0; lane < 4 && px0 + lane < side; ++lane )
                {
                    const int32_t px = px0 + lane;
                    quad.Set( static_cast<std::size_t>( lane ),
                              cellsBox( px * kStripPatchCells, std::min( ( px + 1 ) * kStripPatchCells, cells ),
                                        cz0, cz1 ) );
                    live |= 1u << lane;
                }
                uint32_t kept = 0u;
                for ( const Core::Frustum& strip : strips )
                {
                    kept |= strip.Intersects4( quad ) & live;
                    if ( kept == live )
                        break;
                }
                return kept;
            };
            if ( !reaches( 0, cells, 0, cells ) )
                return patches;
            // The rings exist on the LOD-0 grid only: the hole is in the draw's cells.
            if ( split.RingVertices > 0u && split.Hole.z > split.Hole.x && split.Hole.w > split.Hole.y )
                patches.Rings = reaches( split.Hole.x, split.Hole.z, split.Hole.y, split.Hole.w );
            for ( int32_t pz = 0; pz < side; ++pz )
            {
                const int32_t cz0      = pz * kStripPatchCells;
                const int32_t cz1      = std::min( cz0 + kStripPatchCells, cells );
                int32_t       runStart = -1;
                uint32_t      kept4    = 0u;
                for ( int32_t px = 0; px <= side; ++px )
                {
                    bool kept = false;
                    if ( px < side )
                    {
                        if ( ( px & 3 ) == 0 )
                            kept4 = reaches4( px, cz0, cz1 );
                        kept = ( ( kept4 >> ( px & 3 ) ) & 1u ) != 0u;
                    }
                    if ( kept && runStart < 0 )
                        runStart = px;
                    if ( kept || runStart < 0 )
                        continue;
                    // The run [runStart, px) ends: the window above with the same columns grows down to it.
                    const int32_t cx0    = runStart * kStripPatchCells;
                    const int32_t cx1    = std::min( px * kStripPatchCells, cells );
                    bool          merged = false;
                    for ( glm::ivec4& window : patches.Windows )
                    {
                        if ( window.x == cx0 && window.z == cx1 && window.w == cz0 )
                        {
                            window.w = cz1;
                            merged   = true;
                            break;
                        }
                    }
                    if ( !merged )
                        patches.Windows.emplace_back( cx0, cz0, cx1, cz1 );
                    runStart = -1;
                }
            }
            return patches;
        }

        // THE VIEW IS THE VIEW BLOCK'S (GPU-COST-26; UE: the landscape's base pass reads
        // FViewUniformShaderParameters like every pass of the view): the surface programs read their matrices, the
        // eye, PreExposure and the grass field lens from CameraUB (Common/CameraUB.glslh,
        // ShaderProtocols::MakeCameraUB of this frame), written by SceneFrameBinding::ApplyTo in ApplySceneFrame.
        // The pass keeps no copy of the view: everything per-terrain rides as a TerrainInstance row
        // (TerrainBatch.hpp).

        // A landscape tile is placed by its LandscapeTileDraw: the shader builds world positions from the
        // root's origin and GLOBAL sample indices (see LandscapeTileDraw for why). The LOD
        // half is the tile's sections' table (LandscapeSectionLodsOf); the tile's grid is the floor of its finest
        // section's LOD, each section's raster draw the floor of its own (LandscapeDrawSplitOf), and every vertex
        // morphs from there by its blended one (LandscapeVertexEval.glslh LandscapeVertexLod).
        // THE LOD HALF OF A TILE'S ROW (LandscapeInstance): the finest section's LOD, the grid it draws (its
        // floor) and every section's. A light's row is the tile's row with no LOD (TerrainRenderer::LightQueueKey)
        // and the light's LODs put on it here, so the two halves cannot drift apart.
        void SetInstanceLods( TerrainInstance& instance, const LandscapeSectionLodTable& lod )
        {
            instance.Params.y    = lod.Finest;
            instance.Params.w    = std::floor( lod.Finest );
            instance.SectionLods = lod.Lods;
        }

        TerrainInstance LandscapeInstance( const TerrainDrawData& t, const LandscapeSectionLodTable& lod,
                                           const TerrainLayerInputs& layers )
        {
            const LandscapeTileDraw& l = t.Landscape;

            TerrainInstance instance;
            instance.Params =
                 glm::vec4( static_cast<float>( l.QuadsPerTile ) * l.SpacingCm, lod.Finest,
                            static_cast<float>( t.Weights.VisibilityLayer + 1 ), std::floor( lod.Finest ) );
            instance.Params2        = glm::vec4(
                 1.0f / std::max( 0.01f, kLandscapeLodBlendRange ), static_cast<float>( t.Weights.LayerCount ),
                 static_cast<float>( World::Landscape::LandscapeWeightmapPageCount( t.Weights.LayerCount ) ),
                 static_cast<float>( l.NeighbourMask ) );
            instance.GroundColor    = t.Weights.Ground;
            instance.LandscapeFrame = glm::vec4( l.OriginX, l.BaseY, l.OriginZ, l.SpacingCm );
            instance.LandscapeTile =
                 glm::vec4( static_cast<float>( l.FirstSampleX ), static_cast<float>( l.FirstSampleZ ),
                            static_cast<float>( l.QuadsPerTile ), l.ZScale );
            instance.Sections        = glm::vec4( static_cast<float>( LandscapeSectionQuads( l.QuadsPerTile ) ),
                                                  static_cast<float>( LandscapeSectionCount( l.QuadsPerTile ) ),
                                                  static_cast<float>( LandscapeSectionMaxLod( l.QuadsPerTile ) ), 0.0f );
            SetInstanceLods( instance, lod );
            instance.LayerColors     = t.Weights.Colors;
            instance.LayerAlphaBlend = t.Weights.AlphaBlend;
            instance.GrassCover      = t.Weights.GrassCover;
            instance.GrassCoverLight = t.Weights.GrassCoverLight;
            // The far cover's distances as the tile states them (GRASS-VIS-17): the authored Start /
            // EndCullDistance and the floor's width per pixel. The view's half - the Foliage quality and the lens
            // - is the view block's CameraUB::GrassLens, applied in the shader, so this row does not follow the
            // camera's lens.
            instance.GrassCoverRange = t.Weights.GrassCoverRange;
            for ( size_t c = 0; c < t.Weights.GrassCoverFloorWidth.size(); ++c )
                instance.GrassCoverFloorWidth[c / World::Landscape::kLandscapeWeightmapChannels]
                                             [static_cast<glm::length_t>(
                                                  c % World::Landscape::kLandscapeWeightmapChannels )] =
                     t.Weights.GrassCoverFloorWidth[c];
            for ( size_t c = 0; c < t.Weights.GrassCoverWiden.size(); ++c )
                instance.GrassCoverWiden[c /
                                         World::Landscape::kLandscapeWeightmapChannels][static_cast<glm::length_t>(
                     c % World::Landscape::kLandscapeWeightmapChannels )] = t.Weights.GrassCoverWiden[c];
            for ( size_t c = 0; c < t.Weights.GrassCoverFarLodRadius.size(); ++c )
                instance.GrassCoverFarLodRadius[c / World::Landscape::kLandscapeWeightmapChannels]
                                               [static_cast<glm::length_t>(
                                                    c % World::Landscape::kLandscapeWeightmapChannels )] =
                     t.Weights.GrassCoverFarLodRadius[c];
            // Each channel's material input, matched by its layer's NAME (UE's LayerBlend LayerName), and the
            // ground's by the root's first layer's.
            for ( size_t c = 0; c < t.Weights.LayerNames.size(); ++c )
                instance.LayerInput[c / World::Landscape::kLandscapeWeightmapChannels][static_cast<glm::length_t>(
                     c % World::Landscape::kLandscapeWeightmapChannels )] =
                     static_cast<float>( layers.InputOf( t.Weights.LayerNames[c] ) );
            instance.InputSlices   = layers.Slices;
            instance.GroundInput.x = static_cast<float>( layers.InputOf( t.Weights.GroundLayer ) );
            // The tile's layer allocation (LandscapeWeightDraw::EmptyLayers; 8 bits, exact as a float).
            instance.GroundInput.y = static_cast<float>( t.Weights.EmptyLayers );
            return instance;
        }

        // THE LANDSCAPE DISPLACEMENT of a tile's camera row (UE 5.4+ landscape displacement): the material's
        // values (Terrain.shader Displacement*), the main camera the fade is measured from, and the dice rings
        // around it - every cell of the LOD-0 grid the fade's square reaches, diced finer the larger it is on
        // screen. A grid past LOD 0 is never diced: its vertices are past LOD 1, where the relief is zero
        // (LandscapeDisplacementLodScale).
        void ApplyLandscapeDisplacement( TerrainInstance& instance, const LandscapeTileDraw& l,
                                         const TerrainRowValues& values, const glm::vec3& camera,
                                         const glm::mat4& projection, const glm::mat4& viewProjection,
                                         const std::optional<glm::vec2>& slab, uint32_t viewHeight,
                                         int dicingRate )
        {
            // The surface row's values, read once per row revision (TerrainRowValuesOf, CPU-FRAME-4):
            // LayerKTileSize and LayerKBlend (an Int: its bits, MaterialRowSlotValue) are the layer params' x and
            // y.
            for ( uint32_t k = 0; k < World::Landscape::kLandscapeMaxWeightLayers; ++k )
            {
                instance.InputTileSize[k / 4][static_cast<glm::length_t>( k % 4 )] =
                     values.Values.LayerParams[k].x;
                instance.InputBlend[k / 4][static_cast<glm::length_t>( k % 4 )] = values.Values.LayerParams[k].y;
            }
            instance.DisplacementTiling = glm::vec4( values.Values.Tiling.x, values.Values.Tiling.y, 0.0f, 0.0f );
            const float magnitude       = dicingRate > 0 ? values.Displacement.x : 0.0f;
            const float fadeStart       = std::max( values.Displacement.z, 1.0f );
            const float fadeEnd         = std::max( values.Displacement.w, fadeStart + 1.0f );
            instance.Displacement       = glm::vec4( magnitude, values.Displacement.y, fadeStart, fadeEnd );
            instance.DisplacementView = glm::vec4( camera, 0.0f );
            instance.DiceWindow       = glm::vec4( 0.0f );
            instance.DiceRings        = glm::vec4( -1.0f );
            instance.DiceClip         = glm::vec4( 0.0f );
            if ( magnitude <= 0.0f || instance.Params.w > 0.5f || l.SpacingCm <= 0.0f )
                return;

            // THE RINGS (UE r.Nanite.DicingRate: a diced triangle spans DicingRate pixels). A cell at distance d
            // spans S * K / d pixels across the view but only S * K / d * sin(a) along it, a the angle the ground
            // is seen at (sin a = h / sqrt(h^2 + d^2), h the eye's height over the ground): the cell's pixel
            // AREA is (S K / d)^2 sin(a), and a uniform dice x dice split keeps each sub-cell's area at rate^2
            // when dice = S K sqrt(sin a) / (d rate). Diced by the across edge alone (FPS-MAC p7) a sub-cell at
            // the eye's feet covered 2 x 0.7 pixels - 27 M vertices for 1.2 M pixels, and the 2x2 quads shaded
            // several times over. Dice 2^j holds out to the d_j solving S K sqrt(sin a(d)) / d = rate 2^j:
            // square rings of cells around the camera's cell, each half-extent a whole number of cells, the same
            // integers for every tile (global cell of the camera minus the tile's first sample), so a cell's dice
            // - and the edge factor min(dice, neighbour's dice) the decode stitches with - agrees across tiles.
            // The outermost square (dice 1, displaced) covers the fade: every vertex that moves is in a diced
            // cell.
            const float pixelsAtCm = projection[1][1] * 0.5f * static_cast<float>( viewHeight ) * l.SpacingCm;
            const float fadeCells  = std::ceil( fadeEnd / l.SpacingCm );
            const auto  globalX    = std::floor( ( camera.x - l.OriginX ) / l.SpacingCm );
            const auto  globalZ    = std::floor( ( camera.z - l.OriginZ ) / l.SpacingCm );
            const float cx         = globalX - static_cast<float>( l.FirstSampleX );
            const float cz         = globalZ - static_cast<float>( l.FirstSampleZ );
            const float cells      = static_cast<float>( l.QuadsPerTile );
            if ( cx + fadeCells < 0.0f || cz + fadeCells < 0.0f || cx - fadeCells >= cells ||
                 cz - fadeCells >= cells )
                return;
            instance.DiceWindow =
                 glm::vec4( cx, cz, fadeCells, static_cast<float>( dicingRate ) * l.SpacingCm / pixelsAtCm );
            instance.DisplacementView.w = 1.0f;
            // Rings finer than the fade's square only exist on a landscape with tiles near the eye (@p slab); h
            // is taken to the farther of its lowest and highest displaced height - the steeper view, the finer
            // dice.
            if ( !slab )
                return;
            const float eyeHeight     = std::max( { std::abs( camera.y - ( slab->x - magnitude ) ),
                                                    std::abs( camera.y - ( slab->y + magnitude ) ), l.SpacingCm } );
            const auto  subCellPixels = [&]( float d )
            {
                const float sine = eyeHeight / std::sqrt( eyeHeight * eyeHeight + d * d );
                return pixelsAtCm * std::sqrt( sine ) / std::max( d, 1.0f );
            };
            // THE RELIEF'S 1 PX STOP (TERRAIN-DICE-CULL): a sub-vertex is its cell's flat triangle moved by the
            // relief alone, so a dice past 1 adds nothing where the whole relief (DisplacementMagnitude times the
            // fade's weight there) spans under a pixel: magnitude x fade(d) x pixelsAtCm / (S d) < 1, falling with
            // d. The horizontal d is below the eye's, so the stop is never nearer than the pixel; the fade's square
            // (dice 1) stays, it holds every vertex that moves.
            const auto reliefPixels = [&]( float d )
            {
                const float t    = std::clamp( ( d - fadeStart ) / ( fadeEnd - fadeStart ), 0.0f, 1.0f );
                const float fade = 1.0f - t * t * ( 3.0f - 2.0f * t );
                return magnitude * fade * pixelsAtCm / ( l.SpacingCm * std::max( d, 1.0f ) );
            };
            float reliefCm = 0.0f, reliefFarCm = fadeEnd;
            for ( int step = 0; step < 32; ++step )
            {
                const float mid                                          = 0.5f * ( reliefCm + reliefFarCm );
                ( reliefPixels( mid ) >= 1.0f ? reliefCm : reliefFarCm ) = mid;
            }
            float outer = fadeCells;
            for ( int level = 1; level < kLandscapeDiceLevels; ++level )
            {
                const float target = static_cast<float>( dicingRate ) * static_cast<float>( 1 << level );
                // subCellPixels falls with d; the across-edge reach (sin a = 1) bounds the root from above.
                float nearCm = 0.0f, farCm = pixelsAtCm / target;
                for ( int step = 0; step < 32; ++step )
                {
                    const float mid                                     = 0.5f * ( nearCm + farCm );
                    ( subCellPixels( mid ) >= target ? nearCm : farCm ) = mid;
                }
                const float reach = std::min( nearCm, reliefCm );
                const float half  = std::min( std::floor( reach / l.SpacingCm ), outer );
                if ( half < 0.0f || reach < 0.5f * l.SpacingCm )
                    break;
                instance.DiceRings[static_cast<glm::length_t>( level - 1 )] = half;
                outer                                                       = half;
            }

            // THE VIEW'S FOOTPRINT (UE Nanite culls every cluster outside the view before dicing it): the rings
            // diced 2..16 are a square around the camera, and most of it is behind the camera, beside the view
            // or under its bottom edge - FPS-MAC p8: 27 M vertices a frame at FIELD_Barn's eye for 1.2 M pixels.
            // The rings are clipped to the cells the camera's frustum reaches between the landscape's lowest and
            // highest displaced height near the camera (@p slab, the same for every tile of the landscape, so a
            // border's two tiles agree on every cell's dice), widened by the displacement's sideways reach and
            // two cells, so no visible cell borders a stitched one.
            const float margin = std::ceil( magnitude / l.SpacingCm ) + 2.0f;
            const float lo     = slab->x - magnitude;
            const float hi     = slab->y + magnitude;
            const float square = ( fadeCells + margin + 2.0f ) * l.SpacingCm;
            const float reach =
                 4.0f * square + 4.0f * std::max( std::abs( camera.y - lo ), std::abs( camera.y - hi ) );
            const glm::vec4 foot = LandscapeViewFootprint( viewProjection, camera, lo, hi, reach );
            if ( foot.x > foot.z )
                return;
            const auto cellOf = [&]( float world, float origin, float centre, int32_t first )
            {
                const float clamped = std::clamp( world, centre - square, centre + square );
                return std::floor( ( clamped - origin ) / l.SpacingCm ) - static_cast<float>( first );
            };
            instance.DiceClip = glm::vec4( cellOf( foot.x, l.OriginX, camera.x, l.FirstSampleX ) - margin,
                                           cellOf( foot.y, l.OriginZ, camera.z, l.FirstSampleZ ) - margin,
                                           cellOf( foot.z, l.OriginX, camera.x, l.FirstSampleX ) + 1.0f + margin,
                                           cellOf( foot.w, l.OriginZ, camera.z, l.FirstSampleZ ) + 1.0f + margin );
        }

        // THE VIRTUAL SHADOW MAP'S ROW of a tile (TURN-CACHE-2): the camera's row @p camera - its LOD, its dice
        // rings and their clip, so the caster's triangles are the receivers' - with the displacement faded from @p
        // anchor as the lower envelope over every eye within @p reachCm of it (DisplacementView.w = 1 + reach,
        // LandscapeDisplacement.glslh). The dice-1 square grows to every cell that envelope displaces: the fade's
        // end plus the reach from the anchor, which is itself within the reach of the eye @p eye whose cell the
        // rings are centred on - so a tile the camera does not dice yet may be diced here. @p diceWidthPerCm is
        // DiceWindow.w (DicingRate pixels' width in cm per cm of distance, as ApplyLandscapeDisplacement puts it).
        // A tile with no displacement, or drawn past its LOD-0 grid (where the relief is zero), is the camera's.
        TerrainInstance LandscapeVirtualShadowRow( const TerrainInstance& camera, const LandscapeTileDraw& l,
                                                   const glm::vec3& eye, const glm::vec3& anchor,
                                                   const float reachCm, const float diceWidthPerCm )
        {
            TerrainInstance row = camera;
            if ( camera.Displacement.x <= 0.0f || camera.Params.w > 0.5f || l.SpacingCm <= 0.0f )
                return row;
            const float squareCells = std::ceil( ( camera.Displacement.w + 2.0f * reachCm ) / l.SpacingCm ) + 1.0f;
            const float cx =
                 std::floor( ( eye.x - l.OriginX ) / l.SpacingCm ) - static_cast<float>( l.FirstSampleX );
            const float cz =
                 std::floor( ( eye.z - l.OriginZ ) / l.SpacingCm ) - static_cast<float>( l.FirstSampleZ );
            const float cells = static_cast<float>( l.QuadsPerTile );
            if ( cx + squareCells < 0.0f || cz + squareCells < 0.0f || cx - squareCells >= cells ||
                 cz - squareCells >= cells )
                return row;
            if ( camera.DisplacementView.w < 0.5f )
            {
                // Not diced by the camera: the square alone, dice 1 (DiceRings and DiceClip stay none).
                row.DiceWindow = glm::vec4( cx, cz, squareCells, diceWidthPerCm );
                row.DiceRings  = glm::vec4( -1.0f );
            }
            else
                row.DiceWindow.z = std::max( camera.DiceWindow.z, squareCells );
            row.DisplacementView = glm::vec4( anchor, 1.0f + reachCm );
            return row;
        }

        // Whether tile @p l has a cell the virtual shadow map's envelope displaces (LandscapeVirtualShadowRow):
        // one within the fade's end plus the reach of the anchor. Only these tiles' caster changes with the
        // anchor.
        bool TileMeetsVsmEnvelope( const LandscapeTileDraw& l, const glm::vec3& anchor, const float fadeEndCm,
                                   const float reachCm )
        {
            const float x0     = l.OriginX + ( static_cast<float>( l.FirstSampleX ) * l.SpacingCm );
            const float z0     = l.OriginZ + ( static_cast<float>( l.FirstSampleZ ) * l.SpacingCm );
            const float extent = static_cast<float>( l.QuadsPerTile ) * l.SpacingCm;
            const float dx     = std::max(
                 { std::min( x0, x0 + extent ) - anchor.x, 0.0f, anchor.x - std::max( x0, x0 + extent ) } );
            const float dz = std::max(
                 { std::min( z0, z0 + extent ) - anchor.z, 0.0f, anchor.z - std::max( z0, z0 + extent ) } );
            return ( dx * dx ) + ( dz * dz ) <= ( fadeEndCm + reachCm ) * ( fadeEndCm + reachCm );
        }

        // The landscape material a tile drew with, for the landscape's ray tracing hit group: the values the
        // ground reads (Common/LandscapeSurface.glslh LandscapeMaterial) out of the drawn @p surface's row, and
        // each input's maps as the bindless slots of the images @p layers' arrays copied. Terrain.shader and
        // TerrainGBuffer.shader declare every one of these parameters (the ShaderCacheKey suite holds the two
        // Properties blocks equal); a cell without one is a broken shader, said, and its hits read zero for it.
        // THE SURFACE ROW'S NAMES, in TerrainRowValues' read order: the material's nine, the displacement's four
        // (kTerrainRowDisplacement..), then per weight layer k LayerKTint, LayerKTileSize, LayerKBlend,
        // LayerKNormalStrength (kTerrainRowLayers + 4k ..). Resolved to row slots once per surface layout
        // (ResolveTerrainRowSlots), never spelled per tile.
        enum TerrainRowSlot : uint32_t
        {
            kTerrainRowTint = 0,
            kTerrainRowFarTilingDistance,
            kTerrainRowFarTilingScale,
            kTerrainRowMacroVariationSize,
            kTerrainRowMacroVariationStrength,
            kTerrainRowDetailNormalScale,
            kTerrainRowDetailNormalStrength,
            kTerrainRowMacroHueSize,
            kTerrainRowMacroHueStrength,
            kTerrainRowDisplacementMagnitude,
            kTerrainRowDisplacementFadeStart,
            kTerrainRowDisplacementFadeEnd,
            kTerrainRowDisplacementCenter,
            kTerrainRowLayers,
        };
        constexpr std::array<std::string_view, kTerrainRowSlotCount> kTerrainRowSlotNames{
             "Tint",
             "FarTilingDistance",
             "FarTilingScale",
             "MacroVariationSize",
             "MacroVariationStrength",
             "DetailNormalScale",
             "DetailNormalStrength",
             "MacroHueSize",
             "MacroHueStrength",
             "DisplacementMagnitude",
             "DisplacementFadeStart",
             "DisplacementFadeEnd",
             "DisplacementCenter",
             "Layer0Tint",
             "Layer0TileSize",
             "Layer0Blend",
             "Layer0NormalStrength",
             "Layer1Tint",
             "Layer1TileSize",
             "Layer1Blend",
             "Layer1NormalStrength",
             "Layer2Tint",
             "Layer2TileSize",
             "Layer2Blend",
             "Layer2NormalStrength",
             "Layer3Tint",
             "Layer3TileSize",
             "Layer3Blend",
             "Layer3NormalStrength",
             "Layer4Tint",
             "Layer4TileSize",
             "Layer4Blend",
             "Layer4NormalStrength",
             "Layer5Tint",
             "Layer5TileSize",
             "Layer5Blend",
             "Layer5NormalStrength",
             "Layer6Tint",
             "Layer6TileSize",
             "Layer6Blend",
             "Layer6NormalStrength",
             "Layer7Tint",
             "Layer7TileSize",
             "Layer7Blend",
             "Layer7NormalStrength",
        };
        static_assert( kTerrainRowLayers == 13u && kTerrainRowSlotCount == kTerrainRowLayers + 4u * 8u,
                       "kTerrainRowSlotNames spells eight weight layers after the thirteen material values" );
        // Whether the frame material reads slot @p index (the displacement's four are the displacement's only).
        constexpr bool FrameMaterialReads( const size_t index )
        {
            return index < kTerrainRowDisplacementMagnitude || index >= kTerrainRowLayers;
        }

        // Each name's row slot in @p surface's layout (MaterialBinder::ReadRowParam's rule: a numeric parameter
        // whose slot lies in the row; else kNoTerrainRowSlot, read as zero). A name the frame material reads that
        // is missing is a broken shader - Terrain.shader and TerrainGBuffer.shader declare every one (the
        // ShaderCacheKey suite holds the two Properties blocks equal) - said here, once per resolve, since the
        // landscape's ray tracing hits read zero for it.
        void ResolveTerrainRowSlots( const DataDrivenMaterial& surface, const size_t rowSize,
                                     std::array<uint32_t, kTerrainRowSlotCount>& slots )
        {
            const auto& layout = surface.GetMaterialLayout();
            std::string missing;
            for ( size_t index = 0; index < kTerrainRowSlotCount; ++index )
            {
                const auto*    p    = layout.FindParam( kTerrainRowSlotNames[index] );
                const uint32_t slot = p != nullptr ? p->Offset / Core::Formats::kMaterialParamSlotSize : 0u;
                if ( p == nullptr || p->IsTexture || slot >= rowSize )
                {
                    slots[index] = kNoTerrainRowSlot;
                    if ( FrameMaterialReads( index ) )
                    {
                        if ( !missing.empty() )
                            missing += ", ";
                        missing += kTerrainRowSlotNames[index];
                    }
                    continue;
                }
                slots[index] = slot;
            }
            if ( !missing.empty() )
            {
                LOG_ERROR(
                     "[TerrainRenderer] '{}' has no {} parameter(s): the landscape's ray tracing hits read zero "
                     "for them",
                     surface.GetShaderName(), missing );
            }
        }

        // The values of @p row the frame material and the displacement read, through its resolved @p slots.
        TerrainRowValues TerrainRowValuesOf( const std::array<uint32_t, kTerrainRowSlotCount>& slots,
                                             const Core::Formats::MaterialParamRow&            row )
        {
            const auto read = [&]( const size_t index ) -> glm::vec4
            { return slots[index] == kNoTerrainRowSlot ? glm::vec4( 0.0f ) : row[slots[index]]; };

            TerrainRowValues values;
            values.Tint = read( kTerrainRowTint );
            values.Values.Tiling =
                 glm::vec4( read( kTerrainRowFarTilingDistance ).x, read( kTerrainRowFarTilingScale ).x,
                            read( kTerrainRowMacroVariationSize ).x, read( kTerrainRowMacroVariationStrength ).x );
            values.Values.Detail =
                 glm::vec4( read( kTerrainRowDetailNormalScale ).x, read( kTerrainRowDetailNormalStrength ).x,
                            read( kTerrainRowMacroHueSize ).x, read( kTerrainRowMacroHueStrength ).x );
            for ( uint32_t k = 0; k < World::Landscape::kLandscapeMaxWeightLayers; ++k )
            {
                const size_t layer         = kTerrainRowLayers + ( 4u * k );
                values.Values.LayerTint[k] = read( layer );
                // LayerKBlend is an Int: the row holds its bits (MaterialRowSlotValue).
                values.Values.LayerParams[k] = glm::vec4(
                     read( layer + 1u ).x, static_cast<float>( std::bit_cast<int32_t>( read( layer + 2u ).x ) ),
                     read( layer + 3u ).x, 0.0f );
            }
            values.Displacement =
                 glm::vec4( read( kTerrainRowDisplacementMagnitude ).x, read( kTerrainRowDisplacementCenter ).x,
                            read( kTerrainRowDisplacementFadeStart ).x, read( kTerrainRowDisplacementFadeEnd ).x );
            return values;
        }

        TerrainFrameMaterial FrameMaterialOf( const TerrainRowValues& values, const TerrainLayerInputs& layers )
        {
            TerrainFrameMaterial material;
            material.Tint   = values.Tint;
            material.Values = values.Values;
            for ( uint32_t k = 0; k < World::Landscape::kLandscapeMaxWeightLayers; ++k )
            {
                for ( uint32_t kind = 0; kind < kTerrainLayerMapKinds; ++kind )
                {
                    const float slice = layers.Slices[k][static_cast<glm::length_t>( kind )];
                    if ( slice < 0.0f )
                        continue;
                    const Image2D* source = layers.Sources[kind][static_cast<size_t>( slice )];
                    material.LayerMaps[k][static_cast<glm::length_t>( kind )] = source->GetBindlessSlot().Index;
                }
            }
            return material;
        }

        // Which landscape and which tile of it: tiles of one root share its origin, and a tile's index is its
        // first sample over the tile size. Two roots at one origin would be one landscape drawn twice.
        // A tile's world box: its quads and the whole height range its R16 samples can encode
        // (Common/LandscapeHeight.glslh: (s - 32768) / 128 * ZScale, s in 0..65535).
        struct VsmTileBox
        {
            glm::vec3 Min{ 0.0f };
            glm::vec3 Max{ 0.0f };
        };
        VsmTileBox TileBox( const LandscapeTileDraw& at )
        {
            constexpr float kLowestLocal  = ( 0.0f - 32768.0f ) / 128.0f;
            constexpr float kHighestLocal = ( 65535.0f - 32768.0f ) / 128.0f;
            const float     x0            = at.OriginX + ( static_cast<float>( at.FirstSampleX ) * at.SpacingCm );
            const float     z0            = at.OriginZ + ( static_cast<float>( at.FirstSampleZ ) * at.SpacingCm );
            const float     extent        = static_cast<float>( at.QuadsPerTile ) * at.SpacingCm;
            const float     y0            = at.BaseY + ( kLowestLocal * at.ZScale );
            const float     y1            = at.BaseY + ( kHighestLocal * at.ZScale );
            return {
                 .Min = glm::vec3( std::min( x0, x0 + extent ), std::min( y0, y1 ), std::min( z0, z0 + extent ) ),
                 .Max =
                      glm::vec3( std::max( x0, x0 + extent ), std::max( y0, y1 ), std::max( z0, z0 + extent ) ) };
        }

        // A tile's world box as the camera's rasters draw it (m_ViewDraws, UE's per-component frustum cull): its
        // quads, the heights its samples reach (the LOD morph and the border stitch only interpolate between
        // them), widened on every axis by the displacement's reach along the normal - @p displacementCm, the
        // row's magnitude (ApplyLandscapeDisplacement: 0 when the tile is not displaced); (height - Center) x
        // Magnitude never leaves +-Magnitude.
        Common::Math::AABB TileViewBox( const LandscapeTileDraw& at, const float displacementCm )
        {
            const float        x0     = at.OriginX + ( static_cast<float>( at.FirstSampleX ) * at.SpacingCm );
            const float        z0     = at.OriginZ + ( static_cast<float>( at.FirstSampleZ ) * at.SpacingCm );
            const float        extent = static_cast<float>( at.QuadsPerTile ) * at.SpacingCm;
            const float        reach  = std::max( displacementCm, 0.0f );
            Common::Math::AABB box;
            box.Min = glm::vec3( std::min( x0, x0 + extent ), std::min( at.MinY, at.MaxY ),
                                 std::min( z0, z0 + extent ) ) -
                      glm::vec3( reach );
            box.Max = glm::vec3( std::max( x0, x0 + extent ), std::max( at.MinY, at.MaxY ),
                                 std::max( z0, z0 + extent ) ) +
                      glm::vec3( reach );
            return box;
        }

        // THE SECTIONS THE CAMERA SEES (TERRAIN-COST-2; UE frustum-culls every landscape component before drawing
        // it): LandscapeDrawSplit::Visible for @p frustum - a section's box is its quads with the tile's height
        // range (TileViewBox's), widened by the displacement's reach. No vertex leaves its section (the LOD morph
        // snaps inside it, the border stitch moves along the shared edge), so a section outside the box draws no
        // pixel.
        uint32_t LandscapeSectionViewMask( const LandscapeTileDraw& at, const float displacementCm,
                                           const Core::Frustum& frustum )
        {
            const uint32_t quads    = std::max( at.QuadsPerTile, 1u );
            const uint32_t sq       = LandscapeSectionQuads( quads );
            const uint32_t sections = LandscapeSectionCount( quads );
            const float    reach    = std::max( displacementCm, 0.0f );
            const float    tileX    = at.OriginX + ( static_cast<float>( at.FirstSampleX ) * at.SpacingCm );
            const float    tileZ    = at.OriginZ + ( static_cast<float>( at.FirstSampleZ ) * at.SpacingCm );
            uint32_t       mask     = 0u;
            // FOUR SECTIONS PER TEST (Core::Frustum::Intersects4): the boxes are built exactly as before, in
            // row order, into one AABBQuad; every fourth box (and the last) tests the batch and lands each lane's
            // verdict on its own section's bit. Intersects4's lane is Intersects bit for bit, so the mask is the
            // one the per-section test wrote.
            Common::Math::AABBQuad quad;
            uint32_t               quadBits[4] = {};
            uint32_t               queued      = 0u;
            const auto             flush       = [&]()
            {
                const uint32_t kept = frustum.Intersects4( quad );
                for ( uint32_t lane = 0; lane < queued; ++lane )
                    if ( ( kept >> lane ) & 1u )
                        mask |= quadBits[lane];
                queued = 0u;
            };
            for ( uint32_t sz = 0; sz < sections; ++sz )
            {
                for ( uint32_t sx = 0; sx < sections; ++sx )
                {
                    const float x0 = tileX + ( static_cast<float>( sx * sq ) * at.SpacingCm );
                    const float z0 = tileZ + ( static_cast<float>( sz * sq ) * at.SpacingCm );
                    const float x1 =
                         tileX + ( static_cast<float>( std::min( ( sx + 1u ) * sq, quads ) ) * at.SpacingCm );
                    const float z1 =
                         tileZ + ( static_cast<float>( std::min( ( sz + 1u ) * sq, quads ) ) * at.SpacingCm );
                    Common::Math::AABB box;
                    box.Min = glm::vec3( std::min( x0, x1 ), std::min( at.MinY, at.MaxY ), std::min( z0, z1 ) ) -
                              glm::vec3( reach );
                    box.Max = glm::vec3( std::max( x0, x1 ), std::max( at.MinY, at.MaxY ), std::max( z0, z1 ) ) +
                              glm::vec3( reach );
                    quad.Set( queued, box );
                    quadBits[queued] = 1u << ( sz * kLandscapeMaxSections + sx );
                    if ( ++queued == 4u )
                        flush();
                }
            }
            if ( queued > 0u )
                flush();
            return mask;
        }

        struct TileKey
        {
            float   OriginX                            = 0.0f;
            float   OriginZ                            = 0.0f;
            int32_t X                                  = 0;
            int32_t Z                                  = 0;
            bool    operator==( const TileKey& ) const = default;
        };

        TileKey KeyOf( const LandscapeTileDraw& l, int32_t dx, int32_t dz )
        {
            const auto quads = static_cast<int32_t>( std::max( l.QuadsPerTile, 1u ) );
            return { l.OriginX, l.OriginZ, l.FirstSampleX / quads + dx, l.FirstSampleZ / quads + dz };
        }

        // The continuous LOD of each of the tile's sections for @p view (the main camera, or a point/spot light:
        // LocalShadowCasterLight): its own bounding sphere's screen size, UE's way (a section's LOD0ScreenSize is
        // its bounds', TERRAIN-LOD-1). The sphere holds the full height range, not the actual heights — a taller
        // sphere only means finer detail sooner. A one-section tile is the tile's sphere, as before sections.
        LandscapeSectionLodArray TileSectionLods( const LandscapeTileDraw& l, const Geometry::LODView& view )
        {
            const uint32_t             quads    = std::max( l.QuadsPerTile, 1u );
            const uint32_t             sq       = LandscapeSectionQuads( quads );
            const uint32_t             sections = LandscapeSectionCount( quads );
            const LandscapeLodSettings settings = MakeLandscapeLodSettingsTo( LandscapeSectionMaxLod( quads ) );
            const float                tileX    = l.OriginX + static_cast<float>( l.FirstSampleX ) * l.SpacingCm;
            const float                tileZ    = l.OriginZ + static_cast<float>( l.FirstSampleZ ) * l.SpacingCm;
            LandscapeSectionLodArray   lods{};
            for ( uint32_t sz = 0; sz < sections; ++sz )
            {
                for ( uint32_t sx = 0; sx < sections; ++sx )
                {
                    const uint32_t  x0      = sx * sq;
                    const uint32_t  z0      = sz * sq;
                    const float     extentX = static_cast<float>( std::min( x0 + sq, quads ) - x0 ) * l.SpacingCm;
                    const float     extentZ = static_cast<float>( std::min( z0 + sq, quads ) - z0 ) * l.SpacingCm;
                    const glm::vec3 center( tileX + static_cast<float>( x0 ) * l.SpacingCm + 0.5f * extentX,
                                            l.BaseY,
                                            tileZ + static_cast<float>( z0 ) * l.SpacingCm + 0.5f * extentZ );
                    const float     radius =
                         glm::length( glm::vec3( 0.5f * extentX, 256.0f * l.ZScale, 0.5f * extentZ ) );
                    lods[sz * kLandscapeMaxSections + sx] = LandscapeLodFromScreenSize(
                         settings, Geometry::BoundsScreenRadiusSquared( center, radius, view ) );
                }
            }
            return lods;
        }

        // Does the tile's ground (XZ) footprint meet the disc of @p radius around @p centre? Its heights are not
        // bounded here, so a tile above or below a light's reach is kept: a wasted draw, never a missing caster.
        bool TileMeetsDisc( const LandscapeTileDraw& l, const glm::vec3& centre, const float radius )
        {
            const float     side = static_cast<float>( l.QuadsPerTile ) * l.SpacingCm;
            const glm::vec2 lo( l.OriginX + static_cast<float>( l.FirstSampleX ) * l.SpacingCm,
                                l.OriginZ + static_cast<float>( l.FirstSampleZ ) * l.SpacingCm );
            const glm::vec2 closest = glm::clamp( glm::vec2( centre.x, centre.z ), lo, lo + side );
            const glm::vec2 d       = glm::vec2( centre.x, centre.z ) - closest;
            return glm::dot( d, d ) <= radius * radius;
        }

        // ONE VIEW'S TILE LODS, FLAT (CPU-AUDIT2 TERRAIN): a tile's section LODs by its key, in a vector the
        // renderer keeps (TerrainRenderer::m_TileLods, m_LightTileLods) so a frame fills it with no allocation.
        // Filled with SetTileLods, then SealTileLods sorts it by key once; a key set twice keeps the last LODs, as
        // a map's assignment did. FindTileLods is a binary search.
        bool TileKeyBefore( const TileKey& a, const TileKey& b )
        {
            return std::tie( a.OriginX, a.OriginZ, a.X, a.Z ) < std::tie( b.OriginX, b.OriginZ, b.X, b.Z );
        }
        TileKey KeyOfEntry( const TerrainTileLodEntry& e )
        {
            return { e.OriginX, e.OriginZ, e.X, e.Z };
        }
        void SetTileLods( std::vector<TerrainTileLodEntry>& lods, const TileKey& key,
                          const LandscapeSectionLodArray& value )
        {
            lods.push_back( { .OriginX = key.OriginX, .OriginZ = key.OriginZ, .X = key.X, .Z = key.Z, .Lods = value } );
        }
        void SealTileLods( std::vector<TerrainTileLodEntry>& lods )
        {
            std::stable_sort( lods.begin(), lods.end(), []( const TerrainTileLodEntry& a, const TerrainTileLodEntry& b )
                              { return TileKeyBefore( KeyOfEntry( a ), KeyOfEntry( b ) ); } );
            // Of each run of one key (in fill order, the sort is stable) the last entry stays.
            size_t kept = 0;
            for ( size_t i = 0; i < lods.size(); ++i )
            {
                if ( i + 1 < lods.size() && KeyOfEntry( lods[i] ) == KeyOfEntry( lods[i + 1] ) )
                    continue;
                lods[kept++] = lods[i];
            }
            lods.resize( kept );
        }
        const LandscapeSectionLodArray* FindTileLods( const std::vector<TerrainTileLodEntry>& lods, const TileKey& key )
        {
            const auto n = std::lower_bound( lods.begin(), lods.end(), key,
                                             []( const TerrainTileLodEntry& e, const TileKey& k )
                                             { return TileKeyBefore( KeyOfEntry( e ), k ); } );
            return n != lods.end() && KeyOfEntry( *n ) == key ? &n->Lods : nullptr;
        }

        // A tile's section table for one view, from that view's section LODs of every tile it draws (a neighbour
        // the view does not draw has nothing to agree with: no ring entry). @p lods is sealed (SealTileLods).
        LandscapeSectionLodTable TileLodsOf( const LandscapeTileDraw& l, const std::vector<TerrainTileLodEntry>& lods )
        {
            return LandscapeSectionLodsOf( l.QuadsPerTile, [&]( int dx, int dz ) -> const LandscapeSectionLodArray*
                                           { return FindTileLods( lods, KeyOf( l, dx, dz ) ); } );
        }

        // The tile proxy's map is still the one it was built from: the same owner (a freed image whose address came
        // back is another owner), or both absent.
        bool SameOwner( const std::weak_ptr<Image2D>& held, const std::shared_ptr<Image2D>& now )
        {
            return !held.owner_before( now ) && !now.owner_before( held );
        }

        // The code generation of the program @p material draws with (Shader::GetCodeGeneration): a recompile moves
        // it, and with it the material's row layout, so the params are written again.
        uint64_t ProgramGeneration( const DataDrivenMaterial& material )
        {
            const MaterialExecutor* executor = material.GetMaterialExecutor();
            if ( executor == nullptr )
                return 0u;
            const std::shared_ptr<Shader>& shader = executor->GetShader();
            return shader != nullptr ? shader->GetCodeGeneration() : 0u;
        }

        // The serial of the program @p material draws with (Shader::GetSerial): another program object (a
        // permutation, a reload) moves it, and with it the material's properties.
        uint64_t ProgramSerial( const DataDrivenMaterial& material )
        {
            const MaterialExecutor* executor = material.GetMaterialExecutor();
            if ( executor == nullptr )
                return 0u;
            const std::shared_ptr<Shader>& shader = executor->GetShader();
            return shader != nullptr ? shader->GetSerial() : 0u;
        }
    } // namespace

    namespace
    {
        // Whether a pipeline draws the BASE PASS over the view's depth prepass (the forward "TerrainPass", the
        // deferred "TerrainGBuffer" under an early-Z pass): then it tests CloserOrEqual (its own prepassed depth
        // passes) and writes no depth (UE: the base pass after a full depth prepass, DepthRead).
        // DepthEqual: the deferred "TerrainGBuffer" over "Deferred: Terrain Depth Prepass" (UE: the base pass
        // after a full depth pass, DepthEqual): its own prepassed depth passes, a hole the prepass discarded
        // fails.
        enum class TerrainDepthState : uint8_t
        {
            Shader,     // the shader's own `State` block
            BasePass,   // CloserOrEqual, no depth write
            DepthEqual, // Equal, no depth write
        };

        std::shared_ptr<GraphicsPipeline> CreateTerrainPipeline( SceneRenderer*                 sceneRenderer,
                                                                 const std::shared_ptr<Shader>& shader,
                                                                 const char*                    debugName,
                                                                 const RenderTargetLayout&      layout,
                                                                 std::string&                   error,
                                                                 TerrainDepthState depthState );

        std::shared_ptr<GraphicsPipeline> CreateTerrainPipeline( SceneRenderer* sceneRenderer,
                                                                 const char* shaderName, const char* debugName,
                                                                 const RenderTargetLayout& layout,
                                                                 std::string&              error,
                                                                 const TerrainDepthState   depthState )
        {
            const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( shaderName );
            if ( !shader )
            {
                error = std::string( "missing shader '" ) + shaderName + "'";
                return nullptr;
            }
            return CreateTerrainPipeline( sceneRenderer, shader, debugName, layout, error, depthState );
        }

        std::shared_ptr<GraphicsPipeline> CreateTerrainPipeline( SceneRenderer*                 sceneRenderer,
                                                                 const std::shared_ptr<Shader>& shader,
                                                                 const char*                    debugName,
                                                                 const RenderTargetLayout&      layout,
                                                                 std::string&                   error,
                                                                 const TerrainDepthState        depthState )
        {
            GraphicsPipelineSpecification spec;
            spec.DebugName   = debugName;
            spec.Shader      = shader;
            spec.TargetLayout = layout;
            spec.BlendEnable  = false;

            // Render-state (topology, cull, depth) is declared by the shader's
            // `#pragma state` — no longer hardcoded here. The pipeline comes from the shared cache.
            ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
            if ( depthState == TerrainDepthState::BasePass )
            {
                spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
                spec.DepthWriteEnabled = false;
            }
            else if ( depthState == TerrainDepthState::DepthEqual )
            {
                spec.DepthCompareOp    = DepthCompare::Equal;
                spec.DepthWriteEnabled = false;
            }

            const auto pipeline = sceneRenderer->GetPipelineCache().GetOrCreate( spec );
            if ( !pipeline )
            {
                error = pipeline.GetError();
                return nullptr;
            }
            return pipeline.GetValue();
        }
    } // namespace

    // The Forward program is the only forward terrain program: it draws in "TerrainPass" of AddFrameBasePass, on
    // the Forward render path only, and the ray-traced screen masks (sun, local lights) are made only on the
    // Deferred path, from the G-buffer (SceneRenderer::OnUpdate's composite block). No frame offers this pass a
    // mask, so it has no DESERT_*_SHADOW_SCREEN_MASK permutation: under HardwareTraced it reads the cascades and
    // the atlas, and answering so here keeps them drawn (SunCascadesRead / LocalShadowMapsRead).
    bool TerrainRenderer::ForwardReadsTexture( const std::string_view textureName ) const
    {
        if ( m_SceneRenderer == nullptr || m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred ||
             m_Queue.empty() || m_ForwardRefused || !ForwardPipeline() )
            return false;
        const auto& shader = ForwardPipeline()->GetSpecification().Shader;
        return shader && shader->SamplesTexture( textureName );
    }

    Common::BoolResultStr TerrainRenderer::Initialize()
    {
        std::string error;
        // The VSM permutation's pipeline follows the target layout: built again on the next frame that asks.
        m_PipelineVsm.reset();
        m_PipelineVsmRefused = false;
        // The no-holes depth twins follow the depth layouts too (EnsureDepthNoHolesPipelines).
        m_DepthNoHolesPipeline.reset();
        m_GBufferDepthNoHolesPipeline.reset();
        m_DepthNoHolesRefused = false;
#if DESERT_DEV_INSTRUMENTS
        // The twins follow their solid pipelines' layout: built again on the next wireframe frame.
        m_WireframePipeline.reset();
        m_WireframeVsmPipeline.reset();
        m_WireframeRefused = false;
#endif
        m_Pipeline = CreateTerrainPipeline( m_SceneRenderer, "Terrain", "TerrainPipeline", SceneTargetLayout(), error,
                                            TerrainDepthState::BasePass );
        if ( !m_Pipeline )
            return Common::MakeError( "TerrainRenderer: " + error );
        // The forward path's depth prepass program: the base pass above writes no depth, so the ground is in the
        // scene depth only through it.
        m_DepthPipeline = CreateTerrainPipeline( m_SceneRenderer, "TerrainDepth", "TerrainDepthPipeline",
                                                 SceneDepthLayout(), error, TerrainDepthState::Shader );
        if ( !m_DepthPipeline )
            return Common::MakeError( "TerrainRenderer: the depth prepass pipeline: " + error );

        // The deferred path's twin. Its absence leaves the terrain undrawn in Deferred, and says so; the
        // Forward path does not need it.
        m_GBufferPipeline = CreateTerrainPipeline( m_SceneRenderer, "TerrainGBuffer", "TerrainGBufferPipeline",
                                                   GBufferLayout(), error, TerrainDepthState::Shader );
        if ( !m_GBufferPipeline )
            return Common::MakeError( "TerrainRenderer: the G-buffer pipeline: " + error );
        // The deferred path's early-Z pair (UE: the landscape in the depth pass, its base pass testing that depth
        // without writes): TerrainDepth.shader into the G-buffer depth ("Deferred: Terrain Depth Prepass"), then
        // the G-buffer program EQUAL with no depth write (PrepassesGBuffer).
        m_GBufferDepthPipeline =
             CreateTerrainPipeline( m_SceneRenderer, "TerrainDepth", "TerrainGBufferDepthPipeline",
                                    GBufferDepthLayout(), error, TerrainDepthState::Shader );
        if ( !m_GBufferDepthPipeline )
            return Common::MakeError( "TerrainRenderer: the G-buffer depth prepass pipeline: " + error );
        // The base-pass half (UE: DepthEqual, no clip): TerrainGBuffer's DESERT_TERRAIN_DEPTH_EQUAL permutation
        // (GBufferDepthEqualVariant) on EQUAL / no write - the hole test and its discard are the prepass's alone,
        // and the fragment stage runs after the depth test, so a texel the grass or a mesh covers fetches no
        // layer.
        auto equalShader = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant(
             "TerrainGBuffer", GBufferDepthEqualVariant() );
        if ( !equalShader )
            return Common::MakeError( "TerrainRenderer: TerrainGBuffer|DESERT_TERRAIN_DEPTH_EQUAL was refused: " +
                                      std::string( equalShader.GetError() ) );
        m_GBufferPrepassedPipeline =
             CreateTerrainPipeline( m_SceneRenderer, equalShader.ExtractValue(),
                                    "TerrainGBufferPrepassedPipeline|DESERT_TERRAIN_DEPTH_EQUAL", GBufferLayout(),
                                    error, TerrainDepthState::DepthEqual );
        if ( !m_GBufferPrepassedPipeline )
            return Common::MakeError( "TerrainRenderer: the prepassed G-buffer pipeline: " + error );

        // Terrain materials are created lazily, one set per texture key, in PrepareFrame — a material made
        // now would only ever serve one key.
        return BOOLSUCCESS;
    }

    Common::BoolResultStr
    TerrainRenderer::CreateShadowPipeline( const std::shared_ptr<Framebuffer>& cascadeFramebuffer )
    {
        // A renderer BUILT WITHOUT CASCADES (Graphic::kNoShadowQuality — every asset thumbnail and preview)
        // asks nothing of its casters: the mesh renderer registers no cascade pass, so there is nothing to
        // cast into and no pipeline to build. The budget the SceneRenderer was built with is the one source
        // (MeshRenderer::SetupShadowPass reads the same count); only a missing target UNDER a non-zero
        // budget is a failure.
        if ( m_SceneRenderer->GetShadowQuality().CascadeCount == 0 )
            return BOOLSUCCESS;
        if ( !cascadeFramebuffer )
            return Common::MakeError( "TerrainRenderer: no cascade target to build the shadow caster against" );

        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "TerrainShadow" );
        if ( !shader )
            return Common::MakeError( "TerrainRenderer: missing shader 'TerrainShadow'" );

        GraphicsPipelineSpecification spec;
        spec.DebugName   = "TerrainShadowPipeline";
        spec.Shader      = shader;
        spec.Framebuffer = cascadeFramebuffer;
        spec.BlendEnable = false;
        ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
        // The cascades are STANDARD-Z (MeshRenderer::SetupShadowPass says why), unlike the camera: the
        // compare is the mesh casters' own, set here because a `ZTest` in the shader would be mirrored.
        spec.DepthTestEnabled = true;
        spec.DepthCompareOp   = CompareOp::LessOrEqual;

        const auto pipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
            return Common::MakeError( "TerrainRenderer: the shadow caster pipeline: " + pipeline.GetError() );
        m_ShadowPipeline = pipeline.GetValue();
        // A sun cascade's terrain draws pancaked, as its meshes do (MeshRenderer::SetupShadowPass: depth clamp):
        // ground nearer the sun than a level's depth range writes the range's sun-side end, not clipped. A local
        // light's view keeps the clipped state, its near plane leaving the light's fixture out (LocalShadowNear).
        spec.DebugName        = "TerrainShadowPipelinePancaked";
        spec.DepthClampEnable = true;
        const auto pancaked   = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pancaked )
            return Common::MakeError( "TerrainRenderer: the cascade caster pipeline: " + pancaked.GetError() );
        m_CascadeShadowPipeline = pancaked.GetValue();
        return BOOLSUCCESS;
    }

    void TerrainRenderer::EnsureGridIndices( const uint32_t cells )
    {
        if ( m_GridIndices.contains( cells ) )
            return;
        const std::vector<uint32_t> indices = LandscapeGridIndices( cells );
        std::shared_ptr<IndexBuffer> buffer = IndexBuffer::Create(
             indices.data(), static_cast<uint32_t>( indices.size() * sizeof( uint32_t ) ), BufferUsage::Static );
        if ( !buffer )
        {
            LOG_ERROR( "[TerrainRenderer] the {}-cell grid's index buffer was not created: its tiles are not drawn",
                       cells );
            m_GridIndices.emplace( cells, nullptr );
            return;
        }
        buffer->ClaimOwnership( ResourceOwner::SceneRenderer );
        if ( const auto uploaded = buffer->RT_Invalidate(); !uploaded.IsSuccess() )
        {
            LOG_ERROR( "[TerrainRenderer] the {}-cell grid's index buffer was not uploaded ({}): its tiles are not "
                       "drawn",
                       cells, uploaded.GetError() );
            buffer.reset();
        }
        m_GridIndices.emplace( cells, std::move( buffer ) );
    }

    void TerrainRenderer::SplitDraw( FrameDraw& draw, const uint32_t quadsPerTile, const uint32_t gridLod,
                                     const TerrainInstance& row )
    {
        draw.Split = LandscapeDrawSplitOf( quadsPerTile, gridLod, row );
        EnsureGridIndices( draw.Split.Cells );
        // TERRAIN-LOD-1: each section draws the tile's grid at its own LOD (LandscapeGridRangesOf).
        for ( uint32_t s = 0; s < draw.Split.Sections * draw.Split.Sections; ++s )
        {
            const uint32_t sx = s % draw.Split.Sections;
            const uint32_t sz = s / draw.Split.Sections;
            EnsureGridIndices(
                 LandscapeLodCells( quadsPerTile, draw.Split.SectionGridLod[sz * kLandscapeMaxSections + sx] ) );
        }
    }

    void TerrainRenderer::PushDicePoolJob( TerrainInstance& row, const TerrainDrawData& t,
                                           const TerrainLayerInputs& layers, const uint32_t pooled )
    {
        row.DicePool.x = static_cast<float>( m_DicePoolVertices );
        const uint32_t corners =
             LandscapeDicePoolCornerCount( t.Landscape.QuadsPerTile, static_cast<uint32_t>( row.Params.w ), row );
        m_DicePoolJobs.push_back( { .Row            = row,
                                    .Heightmap      = t.Heightmap,
                                    .Weightmap      = t.Weights.Weightmap != nullptr
                                                           ? std::shared_ptr<Image>( t.Weights.Weightmap )
                                                           : FallbackTextures::Get().GetFallbackTexture2D(
                                                             Core::Formats::ImageFormat::RGBA8F ),
                                    .HasWeightmap   = t.Weights.Weightmap != nullptr,
                                    .LayerHeights   = layers.Arrays[3] ? layers.Arrays[3] : m_EmptyLayerArray,
                                    .Vertices       = pooled,
                                    .Corners        = corners,
                                    .FirstCorner    = m_DicePoolCorners,
                                    .HeightRevision = t.Landscape.HeightRevision,
                                    .WeightRevision = t.Weights.Revision } );
        m_DicePoolVertices += pooled;
        m_DicePoolCorners += corners;
    }

    void TerrainRenderer::PrepareVirtualShadowRows( const glm::vec3& eye, const float diceWidthPerCm )
    {
        // The anchor stays bit for bit while the eye is within the reach (an ulp would be a new signature).
        if ( !m_VsmAnchored || glm::distance( eye, m_VsmAnchor ) > kLandscapeVsmAnchorReachCm )
        {
            m_VsmAnchor   = eye;
            m_VsmAnchored = true;
        }
        DESERT_VERIFY( m_FrameDraws.empty() ||
                            ( m_FrameDraws.size() == m_Queue.size() && m_FrameInstances.size() == m_Queue.size() ),
                       "the terrain's camera draws and rows are one per queued tile" );
        if ( m_FrameDraws.size() != m_Queue.size() || m_FrameInstances.size() != m_Queue.size() )
            return;
        m_VsmDraws.reserve( m_Queue.size() );
        m_VsmInstances.reserve( m_Queue.size() );
        for ( std::size_t i = 0; i < m_Queue.size(); ++i )
        {
            const TerrainDrawData&   t      = m_Queue[i];
            const LandscapeTileDraw& l      = t.Landscape;
            const FrameDraw&         camera = m_FrameDraws[i];
            TerrainInstance          row    = LandscapeVirtualShadowRow( m_FrameInstances[i], l, eye, m_VsmAnchor,
                                                                         kLandscapeVsmAnchorReachCm, diceWidthPerCm );
            if ( row.DisplacementView == m_FrameInstances[i].DisplacementView )
            {
                // Not displaced: the camera's own row and draw (its fade has no eye to follow).
                m_VsmDraws.push_back( camera );
                m_VsmInstances.push_back( row );
                continue;
            }
            FrameGroup&    group   = m_FrameGroups[camera.Group];
            const uint32_t gridLod = static_cast<uint32_t>( row.Params.w );
            if ( const uint32_t pooled = LandscapeDicePoolVertexCount( l.QuadsPerTile, gridLod, row );
                 pooled > 0u )
                PushDicePoolJob( row, t, *group.Layers, pooled );
            FrameDraw draw   = camera;
            draw.Row         = static_cast<uint32_t>( group.Instances.size() );
            draw.VertexCount = LandscapeDicedVertexCount( l.QuadsPerTile, gridLod, row );
            SplitDraw( draw, l.QuadsPerTile, gridLod, row );
            group.Instances.push_back( row );
            m_VsmDraws.push_back( draw );
            m_VsmInstances.push_back( row );
        }
    }

    void TerrainRenderer::PrepareFrame( const ViewFrame& frame )
    {
        // Last frame's group slots (ClearQueue pooled them): reused below with their vectors' capacity.
        if ( m_FrameGroups.empty() )
            std::swap( m_FrameGroups, m_FrameGroupPool );
        m_FrameDraws.clear();
        m_ViewDraws.clear();
        // The lights' draws are PrepareLocalShadowViews' (after UpdateLocalShadows gave the lights their views).
        ReleaseShadowDrawLists();
        m_FrameInstances.clear();
        m_FrameMaterials.clear();
        m_DicePoolJobs.clear();
        // The view depth prepass discards only in a hole: a frame whose tiles have no visibility layer takes the
        // early-tested TerrainDepth (ViewDepthPipeline). Conservative: any queued tile with one keeps the discard.
        m_FrameHasHoles = false;
        for ( const TerrainDrawData& t : m_Queue )
            m_FrameHasHoles = m_FrameHasHoles || t.Weights.VisibilityLayer >= 0;
        if ( !m_FrameHasHoles && !m_Queue.empty() )
            EnsureDepthNoHolesPipelines();
        m_DicePoolVertices = 0;
        m_DicePoolCorners  = 0;
        m_DiceCullJobs.clear();
        m_DiceCullCells = 0;
        m_DiceCulled    = false;
        m_VsmDraws.clear();
        m_VsmInstances.clear();
        if ( m_Queue.empty() )
        {
            m_FrameGroups.clear();
            return;
        }

        // Only the program this frame's render path shades with gets its rows; the shadow caster always
        // does (it has no param rows, only the instances).
        const bool deferred = m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred;
        // Shadows.Method VirtualMaps on the Forward path: the forward terrain reads the view's virtual shadow map
        // (Terrain.shader's DESERT_LOCAL_SHADOW_VSM permutation), not the atlas, which is not drawn then. A
        // refused permutation leaves the terrain undrawn on the forward path (said once), never on the atlas.
        m_ForwardVirtual = !deferred && m_SceneRenderer->LocalShadowsAreVirtual();
        m_ForwardRefused = m_ForwardVirtual && !EnsureForwardVsmPipeline();
        if ( m_ForwardRefused )
            m_ForwardVirtual = false; // the rows still go up (the casters read them); "TerrainPass" draws nothing

        // ── Resolve every terrain's material and pack its rows BEFORE any draw ──────────────
        //
        // The whole queue is recorded before the GPU executes anything, and a material's
        // descriptors and uniform block are written at most once per frame — so NOTHING that
        // varies per terrain may pass through a shared material's block or descriptors. Two
        // transports, each immune to the next draw's setup:
        //
        //   - per-draw DATA (params + the TerrainInstance) is rows in the material's storage
        //     buffers, named per draw by one push-constant index — a push is snapshotted at
        //     record time (Engine/Core/Formats/MaterialParamRow.hpp);
        //   - TEXTURES are the material's identity: one material per texture set, keyed by
        //     TerrainTextureKey, exactly as MeshRenderer keys its generic materials. Before
        //     the key existed the first terrain's textures were the frame's textures — the
        //     backend's per-frame stamp swallowed every later SetTexture silently.
        //
        // Rows upload up front and at final size for the same reason the mesh path does it:
        // growing a storage buffer reallocates the VkBuffer under a draw already recorded
        // against the old one.
        //
        // GPU-TERRAIN-HZB: the per-tile stages below (Tile LODs, Group textures, Tile params, Displacement, Frame
        // material) are CPU work only - no dispatch, no copy, no draw is recorded by them - so they are CPU
        // scopes (DESERT_PROFILE_SCOPE, aggregated by name: one row each per frame), not GPU passes: a
        // DESERT_PROFILE_PASS wrote a timestamp pair per tile into the frame's buffer (~80 empty GPU rows for
        // 20 tiles, each spending the frame's query budget). The stages that do record GPU work keep theirs
        // (Layer inputs: the layer arrays' copies; Tile proxy; Upload rows).
        //
        // What does not change from frame to frame is the tiles' proxies (TileProxy, ProxyOf): a tile's set, its
        // layer inputs and its samplers are looked up by entity and rebuilt only when a revision moved.
        ++m_PrepareSerial;
        size_t                           groupCount = 0;
        std::vector<TerrainLayerInputs*> layersThisFrame;
        // A tile whose GPU copy died (unloaded, its landscape removed) leaves its proxy: owner liveness, no compare.
        std::erase_if( m_TileProxies, []( const auto& entry ) { return entry.second.Heightmap.expired(); } );
        m_FrameDraws.reserve( m_Queue.size() );
        m_ViewDraws.reserve( m_Queue.size() );
        m_FrameInstances.reserve( m_Queue.size() );
        // The camera's frustum from the matrix its rasters draw with (RenderGBufferManual / GeometryPass /
        // RenderDepthManual: the view's jittered view-projection), so the cull and the clip agree to the pixel.
        const Core::Frustum cameraFrustum( frame.JitteredViewProjection, glm::mat4( 1.0f ) );
        m_FrameMaterials.reserve( m_Queue.size() );

        // Every tile's LOD first: a tile's borders take the max with its neighbours', so all must be known.
        m_TileLods.clear();
        {
            DESERT_PROFILE_SCOPE( "Terrain: Tile LODs" );
            for ( const auto& t : m_Queue )
                SetTileLods( m_TileLods, KeyOf( t.Landscape, 0, 0 ),
                             TileSectionLods( t.Landscape, Geometry::LODView{ frame.CameraPosition, frame.Projection } ) );
            SealTileLods( m_TileLods );
        }

        // Each landscape's height slab for the view's footprint on its dice (ApplyLandscapeDisplacement): the
        // lowest and highest sample of its tiles within the across-edge reach of the ring diced 2 - no finer ring
        // reaches a tile past it.
        const uint32_t                            viewHeight = m_SceneRenderer->GetViewExtent().Height;
        const int                                 dicingRate = m_SceneRenderer->GetDisplacementDicingRate();
        m_DiceSlabs.clear();
        const auto                                slabKey = []( const LandscapeTileDraw& l )
        { return std::array<float, 4>{ l.OriginX, l.OriginZ, l.BaseY, l.SpacingCm }; };
        for ( const auto& t : m_Queue )
        {
            const LandscapeTileDraw& l = t.Landscape;
            if ( dicingRate <= 0 || l.SpacingCm <= 0.0f )
                continue;
            const float ring2 = frame.Projection[1][1] * 0.5f * static_cast<float>( viewHeight ) * l.SpacingCm /
                                ( static_cast<float>( dicingRate ) * 2.0f );
            const float x0   = l.OriginX + static_cast<float>( l.FirstSampleX ) * l.SpacingCm;
            const float z0   = l.OriginZ + static_cast<float>( l.FirstSampleZ ) * l.SpacingCm;
            const float size = static_cast<float>( l.QuadsPerTile ) * l.SpacingCm;
            const float dx = std::max( { 0.0f, x0 - frame.CameraPosition.x, frame.CameraPosition.x - x0 - size } );
            const float dz = std::max( { 0.0f, z0 - frame.CameraPosition.z, frame.CameraPosition.z - z0 - size } );
            if ( std::max( dx, dz ) > ring2 + l.SpacingCm )
                continue;
            const auto key  = slabKey( l );
            const auto slab = std::find_if( m_DiceSlabs.begin(), m_DiceSlabs.end(),
                                            [&]( const auto& entry ) { return entry.first == key; } );
            if ( slab == m_DiceSlabs.end() )
                m_DiceSlabs.emplace_back( key, glm::vec2( l.MinY, l.MaxY ) );
            else
                slab->second = glm::vec2( std::min( slab->second.x, l.MinY ), std::max( slab->second.y, l.MaxY ) );
        }

        for ( const auto& t : m_Queue )
        {
            // The tile's proxy: its texture set's materials, layer inputs and samplers, rebuilt only when one of
            // its revisions moved (no TerrainTextureKey on a frame where nothing changed).
            const TileProxy&  proxy     = ProxyOf( t, deferred );
            ProgramMaterials& materials = *proxy.Materials;

            // The material's layer inputs (TerrainLayerInputs): keyed by the material's own textures, not the
            // tile's, so every tile of a landscape shares one set of arrays; brought up to date once per frame per
            // set (by its sources' generations: a streamed mip, a layer texture that landed).
            TerrainLayerInputs& layers = *proxy.Layers;
            if ( std::find( layersThisFrame.begin(), layersThisFrame.end(), &layers ) == layersThisFrame.end() )
            {
                layersThisFrame.push_back( &layers );
                if ( !m_EmptyLayerArray )
                    m_EmptyLayerArray = CreateTerrainEmptyLayerArray();
                DESERT_PROFILE_PASS( "Terrain: Layer inputs" );
                UpdateTerrainLayerInputs( materials.Forward->GetSchema(), *t.Overrides, proxy.LayerTag,
                                          m_EmptyLayerArray, layers );
            }

            // The first tile of a set this frame opens its group, in the next of last frame's slots.
            if ( materials.GroupFrame != m_PrepareSerial )
            {
                DESERT_PROFILE_SCOPE( "Terrain: Group textures" );
                materials.GroupFrame = m_PrepareSerial;
                materials.GroupIndex = groupCount;
                if ( groupCount == m_FrameGroups.size() )
                    m_FrameGroups.emplace_back();
                FrameGroup& opened = m_FrameGroups[groupCount++];
                opened.Key         = proxy.Key;
                opened.Materials   = &materials;
                opened.Layers      = &layers;
                opened.ParamRows.clear();
                opened.Instances.clear();

                // Textures are identical for every terrain of this group BY CONSTRUCTION (the key), so they
                // are bound once, from the terrain that opened the group. Unset samplers keep the backend
                // white fallback, so this is purely additive. Which overrides are samplers is the proxy's (once
                // per revision); the image each names is resolved here, since a texture that lands swaps it.
                DataDrivenMaterial* surface = deferred ? materials.GBuffer.get() : ( materials.*ForwardProgram() ).get();
                // Each sampler's image as it resolves NOW (a texture that lands or streams swaps it).
                m_GroupSamplerScratch.clear();
                for ( const uint32_t index : proxy.Samplers )
                {
                    auto* tex = Runtime::ResourceRegistry::GetTextureService()->Get(
                         Common::UUID( t.Overrides->Textures[index].second ) );
                    // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): the handle names this exact type
                    const auto* img = tex == nullptr ? nullptr
                                                     : static_cast<const Image2D*>(
                                                            Runtime::ResourceRegistry::GetImageService()->Resolve(
                                                                 tex->GetImageHandle() ) );
                    // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
                    m_GroupSamplerScratch.push_back( img );
                }
                // THE GROUP'S BINDINGS PERSIST (UE FLandscapeComponentSceneProxy): every input below equal to what
                // the set last bound binds nothing - each SetTexture of it would be an equal image, no write
                // (Texture2DProperty::SetImage) - so the block runs only when one of them moved.
                ProgramMaterials::BoundTextures& bound = materials.Bound;
                bool unchanged = bound.Valid && bound.Surface == surface && bound.Overrides == t.Overrides &&
                                 bound.Samplers == m_GroupSamplerScratch && bound.Heightmap == t.Heightmap.get() &&
                                 bound.Weightmap == t.Weights.Weightmap.get();
                for ( uint32_t kind = 0; unchanged && kind < kTerrainLayerMapKinds; ++kind )
                    unchanged = bound.Arrays[kind] == layers.Arrays[kind].get() &&
                                bound.ArrayNames[kind] == layers.ArrayNames[kind];
                if ( !unchanged )
                {
                    for ( size_t s = 0; s < proxy.Samplers.size(); ++s )
                    {
                        const std::string& name = t.Overrides->Textures[proxy.Samplers[s]].first;
                        const Image2D*     img  = m_GroupSamplerScratch[s];
                        if ( img != nullptr && !surface->SetTexture( name, img ) )
                            LogRefusedTextureOverride( surface->GetShaderName(), name );
                    }
                    for ( uint32_t kind = 0; kind < kTerrainLayerMapKinds; ++kind )
                        if ( !layers.ArrayNames[kind].empty() && layers.Arrays[kind] &&
                             !surface->SetTexture( layers.ArrayNames[kind], layers.Arrays[kind].get() ) )
                            LogRefusedTextureOverride( surface->GetShaderName(), layers.ArrayNames[kind] );
                    surface->SetTexture( "HeightmapTexture", t.Heightmap.get() );
                    materials.Shadow->SetTexture( "HeightmapTexture", t.Heightmap.get() );
                    materials.VirtualShadow->SetTexture( "HeightmapTexture", t.Heightmap.get() );
                    materials.Depth->SetTexture( "HeightmapTexture", t.Heightmap.get() );
                    if ( t.Weights.Weightmap != nullptr )
                    {
                        surface->SetTexture( "WeightmapTexture", t.Weights.Weightmap.get() );
                        // The casters read the visibility channel too: a hole must not cast a shadow.
                        materials.Shadow->SetTexture( "WeightmapTexture", t.Weights.Weightmap.get() );
                        materials.VirtualShadow->SetTexture( "WeightmapTexture", t.Weights.Weightmap.get() );
                        materials.Depth->SetTexture( "WeightmapTexture", t.Weights.Weightmap.get() );
                    }
                    bound.Valid     = true;
                    bound.Surface   = surface;
                    bound.Overrides = t.Overrides;
                    bound.Samplers.assign( m_GroupSamplerScratch.begin(), m_GroupSamplerScratch.end() );
                    bound.Heightmap = t.Heightmap.get();
                    bound.Weightmap = t.Weights.Weightmap.get();
                    for ( uint32_t kind = 0; kind < kTerrainLayerMapKinds; ++kind )
                    {
                        bound.Arrays[kind]     = layers.Arrays[kind].get();
                        bound.ArrayNames[kind] = layers.ArrayNames[kind];
                    }
                }
            }
            FrameGroup&         group   = m_FrameGroups[materials.GroupIndex];
            DataDrivenMaterial* surface = deferred ? materials.GBuffer.get() : ( materials.*ForwardProgram() ).get();

            {
                // The params are the overrides on the schema defaults, written again only when their revision
                // moved (ProgramMaterials::AppliedOverrides); GetParamRow below still runs every frame (it writes
                // the texture slots of the images the properties draw now).
                DESERT_PROFILE_SCOPE( "Terrain: Tile params" );
                const uint64_t generation = ProgramGeneration( *surface );
                if ( materials.AppliedOverrides != t.Overrides || materials.AppliedSurface != surface ||
                     materials.AppliedGeneration != generation )
                {
                    surface->ApplyDefaults();
                    for ( const auto& [name, value] : t.Overrides->Params )
                        surface->SetParamRaw( name, value );
                    materials.AppliedOverrides  = t.Overrides;
                    materials.AppliedSurface    = surface;
                    materials.AppliedGeneration = generation;
                }
            }

            // One GetParamRow per tile (it rewrites the row's texture slots on every call), and the row's values
            // read through slots resolved once per surface layout and cached by the row's revision (UE: the
            // uniform expression cache, re-evaluated on invalidation).
            const DataDrivenMaterial::ParamRowView rowView   = surface->GetParamRowView();
            const TerrainRowValues&                rowValues = RowValuesOf( materials, *surface, rowView );

            TerrainInstance instance      = LandscapeInstance( t, TileLodsOf( t.Landscape, m_TileLods ), layers );
            const auto      slabKeyOfTile = slabKey( t.Landscape );
            const auto      slab          = std::find_if( m_DiceSlabs.begin(), m_DiceSlabs.end(),
                                                          [&]( const auto& entry ) { return entry.first == slabKeyOfTile; } );
            {
                DESERT_PROFILE_SCOPE( "Terrain: Displacement" );
                std::optional<glm::vec2> tileSlab;
                if ( slab != m_DiceSlabs.end() )
                    tileSlab = slab->second;
                ApplyLandscapeDisplacement( instance, t.Landscape, rowValues, frame.CameraPosition, frame.Projection,
                                            frame.ViewProjection, tileSlab, viewHeight, dicingRate );
            }
            // The tile's displaced sub-vertices, evaluated once this frame (AddDicePoolPass) for every view's
            // draw.
            std::optional<uint32_t> poolCorners;
            if ( const uint32_t pooled = LandscapeDicePoolVertexCount(
                      t.Landscape.QuadsPerTile, static_cast<uint32_t>( instance.Params.w ), instance );
                 pooled > 0u )
            {
                PushDicePoolJob( instance, t, layers, pooled );
                poolCorners = m_DicePoolJobs.back().FirstCorner;
            }

            FrameDraw draw;
            draw.Group       = materials.GroupIndex;
            draw.Row         = static_cast<uint32_t>( group.Instances.size() );
            draw.VertexCount = LandscapeDicedVertexCount( t.Landscape.QuadsPerTile,
                                                          static_cast<uint32_t>( instance.Params.w ), instance );
            SplitDraw( draw, t.Landscape.QuadsPerTile, static_cast<uint32_t>( instance.Params.w ), instance );
            m_FrameDraws.push_back( draw );
            // FPS-FIELD-BARN (TERRAIN-GPU-1): a tile wholly outside the camera's frustum costs the camera's
            // rasters its whole grid of vertex work for no pixel - 6 of FIELD_Barn's 20 tiles (1.76 M of 5.47 M
            // vertices) at the measured eye. Its row, its dice-pool job and its caster draws stay (m_FrameDraws).
            // TERRAIN-COST-2: and of a tile it sees, a section outside it (the camera's tile: about half of its
            // 16, the densest - those around the eye) draws no grid cell for the camera
            // (LandscapeSectionViewMask).
            if ( cameraFrustum.Intersects( TileViewBox( t.Landscape, instance.Displacement.x ) ) )
            {
                FrameDraw view = draw;
                view.Split.Visible =
                     LandscapeSectionViewMask( t.Landscape, instance.Displacement.x, cameraFrustum );
                // TERRAIN-DICE-CULL: the camera's rasters draw the ring cells the frustum and the HZB keep.
                if ( view.Split.RingVertices > 0u && poolCorners )
                    QueueDiceCull( view, instance, *poolCorners );
                m_ViewDraws.push_back( view );
            }

            const auto& paramRow = *rowView.Row;
            group.ParamRows.insert( group.ParamRows.end(), paramRow.begin(), paramRow.end() );
            group.Instances.push_back( instance );
            m_FrameInstances.push_back( instance );
            {
                DESERT_PROFILE_SCOPE( "Terrain: Frame material" );
                m_FrameMaterials.push_back( FrameMaterialOf( rowValues, layers ) );
            }
        }

        // The slots past this frame's groups go (the set count shrank).
        m_FrameGroups.resize( groupCount );

        // THE GROUND'S TRIANGLE CENSUS (TERRAIN-LOD-1): what the camera's rasters (TerrainGBuffer, the depth
        // prepass, the forward pass) draw this frame - the rings' and every section's grid cells - logged when it
        // moves by a tenth, so a still view logs it once, a walk on every LOD step and a turn whenever the
        // sections in its frustum (LandscapeSectionViewMask) move it by a tenth.
        // TERRAIN-COST-2: the rings' (procedural) and the grid's share, the sections drawn of the seen tiles' (the
        // rest outside the frustum), and the camera rows' dice pool work (sub-vertices, and the corner nodes they
        // read; the virtual shadow map's rows add theirs below).
        {
            uint64_t triangles = 0u;
            uint64_t rings     = 0u;
            uint32_t drawn     = 0u;
            uint32_t sections  = 0u;
            for ( const FrameDraw& draw : m_ViewDraws )
            {
                triangles += LandscapeDrawTriangles( draw.Split );
                rings += draw.Split.RingVertices / 3u;
                for ( uint32_t k = 0; k < draw.Split.Sections * draw.Split.Sections; ++k )
                    drawn += static_cast<uint32_t>(
                         LandscapeSectionVisible( draw.Split, k % draw.Split.Sections, k / draw.Split.Sections ) );
                sections += draw.Split.Sections * draw.Split.Sections;
            }
            m_DiceCullGridTriangles = triangles - rings; // the dice cull's census adds the kept rings
            const uint64_t logged   = m_LoggedViewTriangles;
            if ( triangles * 10u > logged * 11u || triangles * 11u < logged * 10u )
            {
                LOG_INFO(
                     "[Terrain] camera triangles: {} (rings {}, grid {}) in {} tile draws, {} of {} sections in "
                     "the frustum ({} tiles queued); the camera rows' dice pool {} vertices from {} corner nodes",
                     triangles, rings, triangles - rings, m_ViewDraws.size(), drawn, sections, m_Queue.size(),
                     m_DicePoolVertices, m_DicePoolCorners );
                m_LoggedViewTriangles = triangles;
            }
        }
        // GPU-TERRAIN-GB: the camera's ground every 60th frame - its tile draws, their triangles, and the weight
        // layers each tile's surface walks: its layer allocation, the channels the root names and paints
        // somewhere on the tile (LandscapeWeightDraw::EmptyLayers; UE's per-component layer list).
        if ( m_PrepareSerial % 60u == 0u && !m_ViewDraws.empty() )
        {
            uint64_t triangles = 0u;
            uint64_t walked    = 0u;
            for ( const FrameDraw& draw : m_ViewDraws )
            {
                triangles += LandscapeDrawTriangles( draw.Split );
                const TerrainInstance& row   = m_FrameGroups[draw.Group].Instances[draw.Row];
                const auto             empty = static_cast<uint32_t>( row.GroundInput.y );
                const auto             count = static_cast<uint32_t>( row.Params2.y );
                for ( uint32_t c = 0; c < count && c < World::Landscape::kLandscapeMaxWeightLayers; ++c )
                    walked +=
                         static_cast<uint64_t>( row.LayerColors[c].a > 0.5f && ( empty & ( 1u << c ) ) == 0u );
            }
            LOG_INFO( "[Terrain] tiles {}, triangles {}, layers per tile avg {:.2f}", m_ViewDraws.size(),
                      triangles, static_cast<double>( walked ) / static_cast<double>( m_ViewDraws.size() ) );
        }

        // The virtual shadow map's own rows and pool jobs, past every camera row of their groups (a draw's Row
        // names its param row too, and these have none), before AddDicePoolPass reads the jobs.
        // Every method whose frame may raster virtual shadow pages: VirtualMaps (the sun and the local lights),
        // Stochastic (its lights resolved to vsm+screen, LocalLightsVirtualPages - known only after this).
        const Common::Scalability::ShadowMethod shadows = m_SceneRenderer->GetRenderMethods().Shadows.Effective;
        if ( LocalShadowsVirtual( m_SceneRenderer->GetRenderMethods() ) ||
             shadows == Common::Scalability::ShadowMethod::Stochastic )
        {
            const float pixelsPerCm = frame.Projection[1][1] * 0.5f * static_cast<float>( viewHeight );
            PrepareVirtualShadowRows( frame.CameraPosition, static_cast<float>( dicingRate ) / pixelsPerCm );
        }

        // ── Upload every group's material rows, still before any draw ───────────────────────────────────
        // (the instance rows go up in PrepareLocalShadowViews, once the lights' rows follow the camera's; the view
        // block goes on in ApplySceneFrame)
        DESERT_PROFILE_PASS( "Terrain: Upload rows" );
        for ( const auto& group : m_FrameGroups )
        {
            if ( group.ParamRows.empty() )
                continue;
            ProgramMaterials&   materials = *group.Materials;
            DataDrivenMaterial* surface =
                 deferred ? materials.GBuffer.get() : ( materials.*ForwardProgram() ).get();
            // ONE upload per group (UploadGroupRows): the programs that declare the block read the first one's
            // buffer; a frame-in-flight copy that already holds these rows is not written again.
            UploadGroupRows( materials.ParamUpload, RowBlocksOf( materials, surface ).Params,
                             group.ParamRows.data(),
                             static_cast<uint32_t>( group.ParamRows.size() * sizeof( glm::vec4 ) ) );
        }
    }

    void TerrainRenderer::ReleaseShadowDrawLists()
    {
        for ( LocalLightDraws& local : m_LocalLightDraws )
        {
            local.Draws.clear();
            m_DrawListPool.push_back( std::move( local.Draws ) );
        }
        for ( std::vector<FrameDraw>& draws : m_CascadeDraws )
        {
            draws.clear();
            m_DrawListPool.push_back( std::move( draws ) );
        }
        m_LocalLightDraws.clear();
        m_CascadeDraws.clear();
    }

    const TerrainRenderer::ProgramMaterials::RowBlocks& TerrainRenderer::RowBlocksOf( ProgramMaterials& materials,
                                                                                      DataDrivenMaterial* surface )
    {
        ProgramMaterials::RowBlocks&             blocks = materials.Blocks;
        const std::array<DataDrivenMaterial*, 4> programs{ surface, materials.Shadow.get(),
                                                           materials.VirtualShadow.get(), materials.Depth.get() };
        bool                                     same = blocks.Valid;
        for ( size_t k = 0; k < programs.size() && same; ++k )
        {
            const DataDrivenMaterial* program = programs[k];
            same                              = blocks.Programs[k] == program &&
                   blocks.Serials[k] == ( program != nullptr ? ProgramSerial( *program ) : 0u ) &&
                   blocks.Generations[k] == ( program != nullptr ? ProgramGeneration( *program ) : 0u );
        }
        if ( same )
            return blocks;
        for ( size_t k = 0; k < programs.size(); ++k )
        {
            DataDrivenMaterial* program = programs[k];
            blocks.Programs[k]          = program;
            blocks.Serials[k]           = 0u;
            blocks.Generations[k]       = 0u;
            blocks.Params[k]            = nullptr;
            blocks.Instances[k]         = nullptr;
            if ( program == nullptr )
                continue;
            blocks.Serials[k]     = ProgramSerial( *program );
            blocks.Generations[k] = ProgramGeneration( *program );
            // The depth-only programs (the casters, the view's depth prepass) have no Materials[] row block (no
            // Properties block): for them the slot is optional.
            if ( k == 0 )
                blocks.Params[k] = program->Get<StorageBufferProperty>( kMaterialRowBlock );
            else
                blocks.Params[k] = program->Find<StorageBufferProperty>( kMaterialRowBlock );
            blocks.Instances[k] = program->Get<StorageBufferProperty>( TerrainShaderParameters::TerrainInstances );
        }
        blocks.Valid = true;
        return blocks;
    }

    // ONE UPLOAD PER GROUP (SCENE-CACHE-2; UE Landscape: one uniform buffer per component, read by every pass):
    // the first program of a group that declares the block takes the group's rows into its own buffer; every later
    // program reads that same buffer (SetBuffer of the same buffer again is no write), so the surface, the two
    // casters and the depth prepass cost one upload, not four. FPS-CPU2-TAILS: and none when the frame-in-flight
    // copy already holds these bytes (ProgramMaterials::RowUpload).
    void TerrainRenderer::UploadGroupRows( ProgramMaterials::RowUpload&                 upload,
                                           const std::array<StorageBufferProperty*, 4>& blocks, const void* data,
                                           const uint32_t bytes )
    {
        StorageBufferProperty* uploaded = nullptr;
        for ( StorageBufferProperty* block : blocks )
        {
            if ( block == nullptr )
                continue;
            if ( uploaded != nullptr && uploaded->GetOwnedStorageBuffer() != nullptr )
            {
                block->SetBuffer( uploaded->GetOwnedStorageBuffer() );
                continue;
            }
            uploaded                                                      = block;
            const std::shared_ptr<ShaderResources::StorageBuffer>& target = block->GetOwnedStorageBuffer();
            const auto* const                                      from   = static_cast<const std::byte*>( data );
            const bool same = upload.Target == target.get() && upload.Bytes.size() == bytes &&
                              ( bytes == 0u || std::memcmp( upload.Bytes.data(), data, bytes ) == 0 );
            if ( !same )
            {
                upload.Bytes.assign( from, from + bytes );
                upload.Target = target.get();
                ++upload.Version; // from 1: a copy made since holds version 0, behind every write
            }
            if ( same && target != nullptr && block->IsWritten() &&
                 target->ActiveAppliedVersion() >= upload.Version )
                continue;
            if ( block->WriteRawData( data, bytes ) && target != nullptr )
                target->NoteActiveApplied( upload.Version );
        }
    }

    uint64_t TerrainRenderer::LightQueueKey()
    {
        m_LightBaseRows.resize( m_Queue.size() );
        ShadowSignatureHash hash;
        hash.Add( static_cast<uint64_t>( m_Queue.size() ) );
        for ( size_t i = 0; i < m_Queue.size(); ++i )
        {
            const TerrainDrawData& t     = m_Queue[i];
            const size_t           group = m_FrameDraws[i].Group;
            m_LightBaseRows[i] = LandscapeInstance( t, LandscapeSectionLodTable{}, *m_FrameGroups[group].Layers );
            // The base row holds every tile field the lights' LOD, reach test and key read (origin, base, spacing,
            // first sample, quads, height scale, neighbours); the rest is what their signature names.
            hash.Add( m_LightBaseRows[i] );
            hash.Add( static_cast<uint64_t>( group ) );
            hash.Add( t.Heightmap );
            hash.Add( t.Landscape.HeightRevision );
            hash.Add( t.Weights.Weightmap );
            hash.Add( t.Weights.Revision );
        }
        return hash.Value;
    }

    std::vector<TerrainRenderer::FrameDraw> TerrainRenderer::TakeDrawList()
    {
        if ( m_DrawListPool.empty() )
            return {};
        std::vector<FrameDraw> draws = std::move( m_DrawListPool.back() );
        m_DrawListPool.pop_back();
        draws.clear();
        return draws;
    }

    const TerrainRowValues& TerrainRenderer::RowValuesOf( ProgramMaterials& materials, const DataDrivenMaterial& surface,
                                                          const DataDrivenMaterial::ParamRowView& row )
    {
        // The slot table: once per surface program layout (its identity, code generation, layout and row size).
        ProgramMaterials::RowSlots& slots      = materials.Slots;
        const auto&                 layout     = surface.GetMaterialLayout();
        const MaterialExecutor*     executor   = surface.GetMaterialExecutor();
        const Shader*               shader     = executor != nullptr ? executor->GetShader().get() : nullptr;
        const uint64_t              serial     = shader != nullptr ? shader->GetSerial() : 0u;
        const uint64_t              generation = shader != nullptr ? shader->GetCodeGeneration() : 0u;
        if ( !slots.Resolved || slots.Surface != &surface || slots.Serial != serial || slots.Generation != generation ||
             slots.Layout != &layout || slots.Params != layout.Params.data() ||
             slots.ParamCount != layout.Params.size() || slots.RowSize != row.Row->size() )
        {
            ResolveTerrainRowSlots( surface, row.Row->size(), slots.Slots );
            slots.Resolved        = true;
            slots.Surface         = &surface;
            slots.Serial          = serial;
            slots.Generation      = generation;
            slots.Layout          = &layout;
            slots.Params          = layout.Params.data();
            slots.ParamCount      = layout.Params.size();
            slots.RowSize         = row.Row->size();
            materials.ValuesValid = false;
        }
        // The values: once per row revision (process-unique, NextMaterialRevision) of this surface.
        if ( !materials.ValuesValid || materials.ValuesSurface != &surface || materials.ValuesRevision != row.Revision )
        {
            materials.Values         = TerrainRowValuesOf( slots.Slots, *row.Row );
            materials.ValuesValid    = true;
            materials.ValuesSurface  = &surface;
            materials.ValuesRevision = row.Revision;
        }
        return materials.Values;
    }

    const TerrainRenderer::TileProxy& TerrainRenderer::ProxyOf( const TerrainDrawData& t, const bool deferred )
    {
        TileProxy& proxy = m_TileProxies[t.Landscape.Entity];
        if ( proxy.Materials != nullptr && proxy.Overrides == t.Overrides && SameOwner( proxy.Heightmap, t.Heightmap ) &&
             SameOwner( proxy.Weightmap, t.Weights.Weightmap ) && proxy.HeightRevision == t.Landscape.HeightRevision &&
             proxy.WeightRevision == t.Weights.Revision && proxy.Deferred == deferred &&
             proxy.ForwardVirtual == m_ForwardVirtual )
            return proxy;

        DESERT_PROFILE_PASS( "Terrain: Tile proxy" );
        const auto entry =
             m_Materials.try_emplace( TerrainTextureKey( *t.Overrides, t.Heightmap.get(), t.Weights.Weightmap.get() ) )
                  .first;
        ProgramMaterials& materials = entry->second;
        if ( !materials.Forward )
        {
            materials.Forward       = std::make_unique<DataDrivenMaterial>( "Terrain" );
            materials.GBuffer       = std::make_unique<DataDrivenMaterial>( "TerrainGBuffer" );
            materials.Depth         = std::make_unique<DataDrivenMaterial>( "TerrainDepth" );
            materials.Shadow        = std::make_unique<DataDrivenMaterial>( "TerrainShadow" );
            materials.VirtualShadow = std::make_unique<DataDrivenMaterial>( "TerrainVirtualShadow" );
        }
        // The forward VSM permutation is made on the first frame that draws with it (a toggle rebuilds the proxy).
        if ( m_ForwardVirtual && !materials.ForwardVsm )
            materials.ForwardVsm = std::make_unique<DataDrivenMaterial>( m_PipelineVsm->GetSpecification().Shader );
        const std::string layerKey = TerrainTextureKey( *t.Overrides, nullptr, nullptr );

        proxy.Overrides      = t.Overrides;
        proxy.Heightmap      = t.Heightmap;
        proxy.Weightmap      = t.Weights.Weightmap;
        proxy.HeightRevision = t.Landscape.HeightRevision;
        proxy.WeightRevision = t.Weights.Revision;
        proxy.Deferred       = deferred;
        proxy.ForwardVirtual = m_ForwardVirtual;
        proxy.Key            = &entry->first;
        proxy.Materials      = &materials;
        proxy.Layers         = &m_LayerInputs[layerKey];
        proxy.LayerTag       = "landscape material " + layerKey;

        // The overrides that are samplers of the surface program: the layer inputs' maps and layer infos are no
        // samplers, they reach the shader through the arrays and the rows' slice / input tables.
        proxy.Samplers.clear();
        const DataDrivenMaterial* surface = deferred ? materials.GBuffer.get() : ( materials.*ForwardProgram() ).get();
        const auto&               schema  = surface->GetSchema();
        for ( uint32_t index = 0; index < static_cast<uint32_t>( t.Overrides->Textures.size() ); ++index )
        {
            const std::string& name = t.Overrides->Textures[index].first;
            if ( t.Overrides->Textures[index].second == 0 )
                continue;
            const auto param = std::find_if( schema.Params.begin(), schema.Params.end(),
                                             [&]( const Core::Formats::ShaderParam& p ) { return p.Name == name; } );
            if ( param != schema.Params.end() && ( param->ElementIndex >= 0 || param->IsAssetRef() ) )
                continue;
            proxy.Samplers.push_back( index );
        }
        return proxy;
    }

    void TerrainRenderer::ApplySceneFrame( const SceneFrameBinding& scene )
    {
        // The scene's part of the forward program, by the ONE applier every lit material gets it from
        // (SceneFrameBinding::ApplyTo, the groups its layout declares): the cloud layer's shadow on the sun, the
        // sun (DirectionLightsUB), the point and spot lights, the cascades' ShadowUB and the sky look the ambient
        // reads (Mesh/AmbientIBL.glslh). Forward only: in Deferred the composite lights the terrain's G-buffer
        // texels with all of it, like every other surface. The maps (the cascade array, the local shadow atlas and
        // its views, the environment cubes and BRDF LUT, the reflection captures, the cloud shadow map) and the
        // light grid are pass parameters of the "TerrainPass" node (SceneViewInputs); the material carries the
        // blocks. Called after the lights got their ShadowView / ShadowMaskSlot (LOCAL-SHADOW-COVER: applied in
        // PrepareFrame, the point/spot blocks went up with ShadowView -1 and the forward terrain received no
        // local shadow at all), at the same point of the frame the meshes' forward materials take them.
        // Deferred: the G-buffer program takes the groups ITS layout declares from the same applier -- the world's
        // parameters (the snow layer, Common/WeatherSnow.glslh, is a surface property written into the G-buffer).
        // Both programs take the Camera group here: the view block (CameraUB) is the only place the terrain's
        // raster reads the view from - its matrices, the eye, PreExposure and the grass field lens (GPU-COST-26).
        const bool deferred = m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred;
        for ( const auto& group : m_FrameGroups )
        {
            const ProgramMaterials& materials = *group.Materials;
            scene.ApplyTo( deferred ? materials.GBuffer.get() : ( materials.*ForwardProgram() ).get() );
        }
    }

    void TerrainRenderer::PrepareLocalShadowViews( const std::vector<LocalShadowCasterLight>& localLights,
                                                   const std::span<const float>               cascadeWorldPerTexel )
    {
        // Every light of the frame has its entry, drawn or not: LocalShadowSignature answers for each of them.
        // The lists of earlier frames go back to the pool and come out of it with their capacity.
        ReleaseShadowDrawLists();
        for ( const LocalShadowCasterLight& light : localLights )
            m_LocalLightDraws.push_back(
                 { .Light = light, .Draws = TakeDrawList(), .Signature = ShadowSignatureHash{}.Value } );
        for ( size_t v = 0; v < cascadeWorldPerTexel.size(); ++v )
            m_CascadeDraws.push_back( TakeDrawList() );
        if ( m_Queue.empty() )
            return;

        // ── Each sun cascade's draws: the camera's tiles and LOD, diced to the cascade's own texels ──────────
        //
        // The camera's rows dice the displaced landscape to DicingRate PIXELS of the camera (16 sub-cells a side
        // at the eye); a far cascade's texel is metres wide, and drawing the camera's dice into every cascade
        // was the largest share of the frame (FPS-MAC p5: the four cascades 182 ms of a 520 ms GPU frame at
        // FIELD_Barn's eye, 25 ms without the dicing). LandscapeCasterDicedRow keeps the camera's vertices up
        // to the finest dice the cascade resolves; a row that changes goes after the camera's in its group
        // buffer, as the lights' rows do (instance row only - the shadow program reads no param row).
        const int dicingRate = m_SceneRenderer->GetDisplacementDicingRate();
        for ( std::size_t v = 0; v < cascadeWorldPerTexel.size(); ++v )
        {
            if ( cascadeWorldPerTexel[v] <= 0.0f )
                continue;
            std::vector<FrameDraw>& draws = m_CascadeDraws[v];
            draws.reserve( m_FrameDraws.size() );
            for ( std::size_t i = 0; i < m_Queue.size(); ++i )
            {
                const FrameDraw&         camera = m_FrameDraws[i];
                const LandscapeTileDraw& l      = m_Queue[i].Landscape;
                const TerrainInstance    row    = LandscapeCasterDicedRow( m_FrameInstances[i], l.SpacingCm,
                                                                           cascadeWorldPerTexel[v], dicingRate );
                const uint32_t           count =
                     LandscapeDicedVertexCount( l.QuadsPerTile, static_cast<uint32_t>( row.Params.w ), row );
                if ( count == camera.VertexCount )
                {
                    draws.push_back( camera );
                    continue;
                }
                FrameGroup& group = m_FrameGroups[camera.Group];
                FrameDraw   draw  = camera;
                draw.Row          = static_cast<uint32_t>( group.Instances.size() );
                draw.VertexCount  = count;
                SplitDraw( draw, l.QuadsPerTile, static_cast<uint32_t>( row.Params.w ), row );
                draws.push_back( draw );
                group.Instances.push_back( row );
            }
        }

        // ── Each point/spot light's own draws: the tiles in its reach, at the LOD its position asks for ─────────
        //
        // A local view's depth is cached until something in its reach changes
        // (MeshRenderer::PlanLocalShadowCache), so its detail must not follow the camera: with the camera's LOD
        // every step of the camera re-keyed and redrew every local view that reached terrain. The rows go after
        // the camera's in the same group buffers (one upload, before any draw); a local row is an instance row
        // only — the shadow program reads no param row.
        if ( !m_LocalLightDraws.empty() )
        {
            const uint64_t   queueKey = LightQueueKey();
            const uint64_t   frame    = m_PrepareSerial;
            constexpr size_t kNoSlot  = ~size_t{ 0 };
            const auto keyed = [queueKey]( const LightDrawCacheEntry& entry, const LocalShadowCasterLight& light )
            {
                return entry.QueueKey == queueKey && entry.Reach == light.Reach &&
                       entry.Origin == light.Lod.Origin && entry.Projection == light.Lod.Projection;
            };
            // The lights an entry holds first, so a light that moved never takes the entry of one that did not.
            m_LightCacheSlots.assign( m_LocalLightDraws.size(), kNoSlot );
            for ( size_t li = 0; li < m_LocalLightDraws.size(); ++li )
            {
                for ( size_t e = 0; e < m_LightDrawCache.size(); ++e )
                {
                    if ( !keyed( m_LightDrawCache[e], m_LocalLightDraws[li].Light ) )
                        continue;
                    m_LightCacheSlots[li]         = e;
                    m_LightDrawCache[e].UsedFrame = frame;
                    break;
                }
            }
            // The others build their draws into an entry no light of this frame uses.
            for ( size_t li = 0; li < m_LocalLightDraws.size(); ++li )
            {
                if ( m_LightCacheSlots[li] != kNoSlot )
                    continue;
                size_t slot = 0;
                while ( slot < m_LightDrawCache.size() && m_LightDrawCache[slot].UsedFrame == frame )
                    ++slot;
                if ( slot == m_LightDrawCache.size() )
                    m_LightDrawCache.emplace_back();
                m_LightCacheSlots[li]               = slot;
                LightDrawCacheEntry&          entry = m_LightDrawCache[slot];
                const LocalShadowCasterLight& light = m_LocalLightDraws[li].Light;
                entry.Origin                        = light.Lod.Origin;
                entry.Projection                    = light.Lod.Projection;
                entry.Reach                         = light.Reach;
                entry.QueueKey                      = queueKey;
                entry.UsedFrame                     = frame;
                entry.Draws.clear();
                entry.Rows.clear();

                m_LightTileLods.clear();
                for ( const auto& t : m_Queue )
                    if ( TileMeetsDisc( t.Landscape, light.Lod.Origin, light.Reach ) )
                        SetTileLods( m_LightTileLods, KeyOf( t.Landscape, 0, 0 ),
                                     TileSectionLods( t.Landscape, light.Lod ) );
                SealTileLods( m_LightTileLods );

                ShadowSignatureHash hash;
                for ( std::size_t i = 0; i < m_Queue.size(); ++i )
                {
                    const TerrainDrawData&   t = m_Queue[i];
                    const LandscapeTileDraw& l = t.Landscape;
                    if ( FindTileLods( m_LightTileLods, KeyOf( l, 0, 0 ) ) == nullptr )
                        continue;
                    // The tile's row with no LOD (the same layer inputs as the camera's row; the shadow program
                    // reads none of them, the row is whole) and the light's LODs on it.
                    TerrainInstance instance = m_LightBaseRows[i];
                    SetInstanceLods( instance, TileLodsOf( l, m_LightTileLods ) );

                    FrameDraw draw;
                    draw.Group = m_FrameDraws[i].Group;
                    draw.VertexCount =
                         LandscapeLodVertexCount( l.QuadsPerTile, static_cast<uint32_t>( instance.Params.w ) );
                    SplitDraw( draw, l.QuadsPerTile, static_cast<uint32_t>( instance.Params.w ), instance );
                    entry.Draws.push_back( draw );
                    entry.Rows.push_back( instance );

                    // What TerrainShadow.shader reads: the placement and LOD fields of the row (not the layer
                    // colours), the heightmap and its texels' revision, and the weightmap where the tile has a
                    // visibility layer (its holes).
                    hash.Add( instance.Params );
                    hash.Add( instance.Params2 );
                    hash.Add( instance.LandscapeFrame );
                    hash.Add( instance.LandscapeTile );
                    hash.Add( instance.Sections );
                    for ( const glm::vec4& lods : instance.SectionLods )
                        hash.Add( lods );
                    hash.Add( draw.VertexCount );
                    hash.Add( t.Heightmap );
                    hash.Add( l.HeightRevision );
                    if ( t.Weights.VisibilityLayer >= 0 )
                    {
                        hash.Add( t.Weights.Weightmap );
                        hash.Add( t.Weights.Revision );
                    }
                }
                entry.Signature = hash.Value;
            }
            // Every light's rows after the camera's in its groups' buffers, in light order (as they were built).
            for ( size_t li = 0; li < m_LocalLightDraws.size(); ++li )
            {
                LocalLightDraws&           local = m_LocalLightDraws[li];
                const LightDrawCacheEntry& entry = m_LightDrawCache[m_LightCacheSlots[li]];
                for ( size_t k = 0; k < entry.Draws.size(); ++k )
                {
                    FrameDraw   draw  = entry.Draws[k];
                    FrameGroup& group = m_FrameGroups[draw.Group];
                    draw.Row          = static_cast<uint32_t>( group.Instances.size() );
                    local.Draws.push_back( draw );
                    group.Instances.push_back( entry.Rows[k] );
                }
                local.Signature = entry.Signature;
            }
        }

        // ── The instance rows, the camera's and the lights', once, still before any draw ──────────────
        const bool deferred = m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred;
        for ( const auto& group : m_FrameGroups )
        {
            ProgramMaterials&   materials = *group.Materials;
            DataDrivenMaterial* surface =
                 deferred ? materials.GBuffer.get() : ( materials.*ForwardProgram() ).get();
            // ONE upload per group (UploadGroupRows): the first program's buffer takes the rows, the other three
            // read that same buffer - the rows are identical by construction.
            UploadGroupRows( materials.InstanceUpload, RowBlocksOf( materials, surface ).Instances,
                             group.Instances.data(),
                             static_cast<uint32_t>( group.Instances.size() * sizeof( TerrainInstance ) ) );
        }
    }

    Common::BoolResultStr TerrainRenderer::AddDicePoolPass( RDG::Builder& graph, FrameTextures& textures )
    {
        m_DicePool            = {};
        m_DicePoolCornerNodes = {};
        // The camera's kept ring cells (AddDiceCullPass writes them), bound by every terrain draw as the pool is:
        // one zero word when no row is culled.
        m_DiceCells = graph.CreateBuffer(
             RDG::BufferDesc{ uint64_t{ std::max<uint32_t>( m_DiceCullCells, 1u ) } * sizeof( uint32_t ) },
             "Landscape.DiceCull.Cells" );
        if ( m_DiceCullJobs.empty() )
        {
            const std::vector<std::byte> zero( sizeof( uint32_t ), std::byte{ 0 } );
            graph.QueueBufferUpload( m_DiceCells, zero );
        }
        if ( m_FrameDraws.empty() )
            return BOOLSUCCESS;
        // The row's first vertex rides as a float (TerrainInstance.DicePool.x): exact below 2^24 vertices (512
        // MB).
        if ( m_DicePoolVertices >= ( 1u << 24 ) )
            return Common::MakeFormattedError<bool>( "{}: {} pooled vertices exceed the 2^24 a row can address",
                                                     kDicePoolPass, m_DicePoolVertices );
        const uint64_t vertices    = std::max<uint32_t>( m_DicePoolVertices, 1u );
        const uint64_t poolBytes   = vertices * 2u * sizeof( glm::vec4 );
        const uint64_t cornerCount = std::max<uint32_t>( m_DicePoolCorners, 1u );
        const uint64_t cornerBytes = cornerCount * 2u * sizeof( glm::vec4 );
        if ( m_DicePoolJobs.empty() )
        {
            m_DicePool = graph.CreateBuffer( RDG::BufferDesc{ poolBytes }, "Landscape.DicePool" );
            // No tile is diced this frame: nothing reads the pool, but every terrain vertex stage binds it.
            const std::array<std::byte, 2 * sizeof( glm::vec4 )> zero{};
            graph.QueueBufferUpload( m_DicePool, zero );
            return BOOLSUCCESS;
        }

        if ( !m_DicePoolPipeline )
        {
            const auto shaders = Runtime::ResourceRegistry::GetShaderService();
            const auto shader  = shaders ? shaders->GetByName( std::string( kDicePoolShader ) ) : nullptr;
            if ( !shader )
                return Common::MakeFormattedError<bool>(
                     "{}: compute shader '{}' is not registered (Programs/Terrain/{}.shader)", kDicePoolPass,
                     kDicePoolShader, kDicePoolShader );
            const auto pipeline =
                 ComputePipeline::Create( { .Shader = shader, .DebugName = std::string( kDicePoolShader ) } );
            if ( !pipeline )
                return Common::MakeFormattedError<bool>( "{}: {}", kDicePoolPass, pipeline.GetError() );
            m_DicePoolPipeline = pipeline.GetValue();
        }

        // GPU-TERRAIN-HZB: the kept pool is evaluated only when a job's key differs from what it holds.
        if ( m_DicePoolCache )
        {
            m_DicePoolFrameKeys.clear();
            for ( const DicePoolJob& job : m_DicePoolJobs )
            {
                DicePoolKey key;
                key.Row              = job.Row;
                key.Heightmap        = job.Heightmap.get();
                key.Weightmap        = job.Weightmap.get();
                key.LayerHeights     = job.LayerHeights.get();
                key.HeightGeneration = job.Heightmap ? job.Heightmap->GetResourceGeneration() : 0u;
                const auto* const weight2d = dynamic_cast<const Image2D*>( job.Weightmap.get() );
                key.WeightGeneration = weight2d != nullptr ? weight2d->GetResourceGeneration() : 0u;
                key.LayerGeneration  = job.LayerHeights ? job.LayerHeights->GetResourceGeneration() : 0u;
                key.HeightRevision   = job.HeightRevision;
                key.WeightRevision   = job.WeightRevision;
                key.Vertices         = job.Vertices;
                key.Corners          = job.Corners;
                key.FirstCorner      = job.FirstCorner;
                m_DicePoolFrameKeys.push_back( key );
            }
            if ( const Common::BoolResultStr imported = ImportPersistentDicePool( graph, poolBytes, cornerBytes );
                 !imported )
                return imported;
            if ( m_DicePoolKeysValid &&
                 std::equal( m_DicePoolFrameKeys.begin(), m_DicePoolFrameKeys.end(), m_DicePoolKeys.begin(),
                             m_DicePoolKeys.end(), &TerrainRenderer::SameDicePoolKey ) )
                return BOOLSUCCESS; // the kept pool is this frame's: no dispatch
            m_DicePoolKeysValid = false;
        }
        else
        {
            m_DicePool = graph.CreateBuffer( RDG::BufferDesc{ poolBytes }, "Landscape.DicePool" );
            m_DicePoolCornerNodes =
                 graph.CreateBuffer( RDG::BufferDesc{ cornerBytes }, "Landscape.DicePool.Corners" );
        }

        std::vector<std::byte> rowBytes( m_DicePoolJobs.size() * sizeof( TerrainInstance ) );
        for ( std::size_t i = 0; i < m_DicePoolJobs.size(); ++i )
            std::memcpy( rowBytes.data() + i * sizeof( TerrainInstance ), &m_DicePoolJobs[i].Row,
                         sizeof( TerrainInstance ) );
        const RDG::BufferRef rows =
             graph.CreateBuffer( RDG::BufferDesc{ rowBytes.size() }, "Landscape.DicePool.Rows" );
        graph.QueueBufferUpload( rows, rowBytes );

        // The diced tiles' corner nodes (TERRAIN-COST-2): every sub-vertex reads its cell's three from here
        // instead of evaluating them (LandscapeDicePool.shader, LandscapeDicePoolCornerCount).
        const RDG::BufferRef corners = m_DicePoolCornerNodes;

        // Two nodes, one binding block and one dispatch per diced tile each: the corner nodes (Tile.w = 0), then -
        // after the graph's barrier on the corners - the pool's sub-vertices from them (Tile.w = 1), every tile
        // writing its own range of the one corner buffer and of the one pool. Both bind the shader's every slot;
        // the corner dispatch writes the corners only, the vertex dispatch reads them.
        const auto& layout = m_DicePoolLayout.Get( m_DicePoolPipeline->GetSpecification().Shader );
        const auto  declare =
             [&]( const std::string_view name, const uint32_t phase, const RDG::Access cornerAccess )
        {
            ComputeNodeDeclaration  node( graph.GetFrameArena() );
            std::vector<glm::uvec4> pushes;
            std::vector<uint32_t>   groups;
            node.Name = name; // kDicePoolCornerPass / kDicePoolPass: static
            for ( std::size_t i = 0; i < m_DicePoolJobs.size(); ++i )
            {
                const DicePoolJob& job = m_DicePoolJobs[i];
                // Named before the call (MSVC: no conditional inline in a by-value argument).
                const std::string weightmapName =
                     job.HasWeightmap ? std::format( "Landscape.Weightmap.{}",
                                                     static_cast<const void*>( job.Weightmap.get() ) )
                                      : std::string( "Landscape.DicePool.WeightmapFallback" );
                node.Access.Bindings( layout, Renderer::GetPipelineRouteFill( *m_DicePoolPipeline ) )
                     .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::uvec4 ) ) )
                     .Storage( DicePoolShaderParameters::LandscapeDicePoolRows, rows, RDG::Access::StorageRead )
                     .Texture( DicePoolShaderParameters::HeightmapTexture, job.Heightmap,
                               RDG::Access::SampledCompute,
                               std::format( "Landscape.Heightmap.{}",
                                            static_cast<const void*>( job.Heightmap.get() ) ) )
                     .Texture( DicePoolShaderParameters::WeightmapTexture, job.Weightmap,
                               RDG::Access::SampledCompute, weightmapName )
                     .Texture( DicePoolShaderParameters::LayerHeightTexture, job.LayerHeights,
                               RDG::Access::SampledCompute,
                               std::format( "Landscape.LayerHeights.{}",
                                            static_cast<const void*>( job.LayerHeights.get() ) ) )
                     .Sampler( DicePoolShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                     .Sampler( DicePoolShaderParameters::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
                     .Sampler( DicePoolShaderParameters::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() )
                     .Storage( DicePoolShaderParameters::LandscapeDicePoolVertices, m_DicePool,
                               RDG::Access::StorageWrite )
                     .Storage( DicePoolShaderParameters::LandscapeDicePoolCorners, corners, cornerAccess );
                const uint32_t items = phase == 0u ? job.Corners : job.Vertices;
                pushes.emplace_back( static_cast<uint32_t>( i ), items, job.FirstCorner, phase );
                groups.push_back( ( items + kDicePoolLocalSize - 1u ) / kDicePoolLocalSize );
            }
            node.Record = [pipeline = m_DicePoolPipeline, pushes = std::move( pushes ),
                           groups = std::move( groups )]( RDG::PassContext& context,
                                                          const FrameGraphRefs& ) -> Common::BoolResultStr
            {
                for ( uint32_t block = 0; block < pushes.size(); ++block )
                {
                    RDG::PassBindings bindings( context, context.GetBindingBlock( block ) );
                    bindings.PushConstants( &pushes[block], static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
                    if ( const Common::BoolResultStr dispatched =
                              Renderer::DispatchCompute( bindings, *pipeline, groups[block], 1, 1 );
                         !dispatched )
                        return dispatched;
                }
                return BOOLSUCCESS;
            };
            return node;
        };
        ComputeNodeList nodes = MakeComputeNodeList( graph.GetFrameArena() );
        nodes.push_back( declare( kDicePoolCornerPass, 0u, RDG::Access::StorageWrite ) );
        nodes.push_back( declare( kDicePoolPass, 1u, RDG::Access::StorageRead ) );
        if ( !AddComputeNodes( graph, textures, std::move( nodes ) ) )
        {
            m_DicePool = {};
            return Common::MakeFormattedError<bool>( "{}: the node could not be declared", kDicePoolPass );
        }
        if ( m_DicePoolCache )
        {
            m_DicePoolKeys.swap( m_DicePoolFrameKeys );
            m_DicePoolKeysValid = true;
        }
        return BOOLSUCCESS;
    }

    bool TerrainRenderer::SameDicePoolKey( const DicePoolKey& a, const DicePoolKey& b ) noexcept
    {
        return std::memcmp( &a.Row, &b.Row, sizeof( TerrainInstance ) ) == 0 && a.Heightmap == b.Heightmap &&
               a.Weightmap == b.Weightmap && a.LayerHeights == b.LayerHeights &&
               a.HeightGeneration == b.HeightGeneration && a.WeightGeneration == b.WeightGeneration &&
               a.LayerGeneration == b.LayerGeneration && a.HeightRevision == b.HeightRevision &&
               a.WeightRevision == b.WeightRevision && a.Vertices == b.Vertices && a.Corners == b.Corners &&
               a.FirstCorner == b.FirstCorner;
    }

    Common::BoolResultStr TerrainRenderer::ImportPersistentDicePool( RDG::Builder& graph, const uint64_t poolBytes,
                                                                     const uint64_t cornerBytes )
    {
        if ( poolBytes > UINT32_MAX || cornerBytes > UINT32_MAX )
            return Common::MakeFormattedError<bool>( "{}: a {} / {} byte pool exceeds a storage buffer's 4 GB",
                                                     kDicePoolPass, poolBytes, cornerBytes );
        // Grown, never shrunk: a bigger buffer than this frame's pool is read only up to its rows' vertices.
        if ( !m_DicePoolBuffer || m_DicePoolBytes < poolBytes )
        {
            m_DicePoolKeysValid = false;
            m_DicePoolBytes     = 0u;
            m_DicePoolBuffer    = ShaderResources::StorageBuffer::CreateDeviceLocal(
                 "Landscape.DicePool", static_cast<uint32_t>( poolBytes ), 0u );
            if ( !m_DicePoolBuffer )
                return Common::MakeFormattedError<bool>( "{}: the kept pool ({} bytes) was not made",
                                                         kDicePoolPass, poolBytes );
            m_DicePoolBytes = poolBytes;
        }
        if ( !m_DicePoolCornerBuffer || m_DicePoolCornerBytes < cornerBytes )
        {
            m_DicePoolKeysValid    = false;
            m_DicePoolCornerBytes  = 0u;
            m_DicePoolCornerBuffer = ShaderResources::StorageBuffer::CreateDeviceLocal(
                 "Landscape.DicePool.Corners", static_cast<uint32_t>( cornerBytes ), 0u );
            if ( !m_DicePoolCornerBuffer )
                return Common::MakeFormattedError<bool>( "{}: the kept corner nodes ({} bytes) were not made",
                                                         kDicePoolPass, cornerBytes );
            m_DicePoolCornerBytes = cornerBytes;
        }
        Common::BoolResultStr imported = Renderer::ImportBuffer( m_DicePoolBuffer, m_DicePoolImport );
        if ( imported )
            imported = Renderer::ImportBuffer( m_DicePoolCornerBuffer, m_DicePoolCornerImport );
        if ( !imported )
        {
            m_DicePoolKeysValid = false;
            return imported;
        }
        m_DicePool            = graph.RegisterExternal( m_DicePoolImport, "Landscape.DicePool" );
        m_DicePoolCornerNodes = graph.RegisterExternal( m_DicePoolCornerImport, "Landscape.DicePool.Corners" );
        return BOOLSUCCESS;
    }

    void TerrainRenderer::QueueDiceCull( FrameDraw& view, TerrainInstance& row, const uint32_t firstCorner )
    {
        // The fade square's cells (ring 0: every diced cell) are the cull's lanes and each level's list stride.
        const uint32_t square =
             LandscapeDiceRectArea( LandscapeDiceRingRect( row, 0, static_cast<int>( view.Split.Cells ) ) );
        if ( square == 0u )
            return;
        row.DicePool.z      = static_cast<float>( m_DiceCullCells );
        view.DiceCull       = static_cast<uint32_t>( m_DiceCullJobs.size() ) * kDiceLevels;
        const float center  = row.Displacement.y;
        const float reachCm = row.Displacement.x * std::max( std::abs( center ), std::abs( 1.0f - center ) );
        m_DiceCullJobs.push_back( { .Row = row, .Cells = square, .FirstCorner = firstCorner, .Reach = reachCm } );
        m_DiceCullCells += square * kDiceLevels;
    }

    RDG::BufferRef TerrainRenderer::CameraDiceCullArgs() const
    {
        if ( !m_DiceCulled )
            return {};
        return m_DiceCullArgs;
    }

    void TerrainRenderer::ReportDiceCullCensus( const uint64_t frameIndex )
    {
        // The pattern of InstanceCullValidation: the slot's frame has completed (the frame start waited its fence).
        const uint32_t slots = std::max( 1u, EngineContext::GetInstance().GetMaxFramesInFlight() );
        if ( m_DiceCullCensus.size() != slots )
        {
            m_DiceCullCensus.clear();
            m_DiceCullCensus.resize( slots );
        }
        DiceCullCensus& slot = m_DiceCullCensus[static_cast<std::size_t>( frameIndex % slots )];
        if ( slot.Args.Physical && slot.Jobs > 0u )
        {
            const std::span<const std::byte> bytes   = slot.Args.Physical->ReadForHost();
            const std::size_t                records = std::size_t{ slot.Jobs } * kDiceLevels;
            if ( bytes.size() >= records * kDiceCullRecordWords * sizeof( uint32_t ) )
            {
                std::vector<uint32_t> words( records * kDiceCullRecordWords );
                std::memcpy( words.data(), bytes.data(), words.size() * sizeof( uint32_t ) );
                uint64_t vertices = 0u;
                uint64_t kept     = 0u;
                for ( std::size_t r = 0; r < records; ++r )
                {
                    const uint32_t dice = 1u << static_cast<uint32_t>( r % kDiceLevels );
                    vertices += words[r * kDiceCullRecordWords];
                    kept += words[r * kDiceCullRecordWords] / ( dice * dice * 6u );
                }
                const uint64_t triangles = vertices / 3u + slot.GridTriangles;
                const uint64_t logged    = m_LoggedCulledTriangles;
                if ( triangles * 10u > logged * 11u || triangles * 11u < logged * 10u )
                {
                    LOG_INFO( "[Terrain] triangles in view {} (blocks kept {} of {}: cells of {} culled rows; "
                              "rings {}, grid {})",
                              triangles, kept, slot.Tested, slot.Jobs, vertices / 3u, slot.GridTriangles );
                    m_LoggedCulledTriangles = triangles;
                }
            }
        }
        slot = DiceCullCensus{};
    }

    Common::BoolResultStr TerrainRenderer::AddDiceCullPass( RDG::Builder& graph, FrameTextures& textures,
                                                            const ViewFrame&      frame,
                                                            const DiceOcclusion&  occlusion )
    {
        m_DiceCulled   = false;
        m_DiceCullArgs = {};
        ReportDiceCullCensus( frame.FrameIndex );
        if ( m_DiceCullJobs.empty() || !m_DiceCells.IsValid() )
            return BOOLSUCCESS;
        // Refused, the rings stay the procedural decode; the list still gets its content (every draw binds it).
        const auto zeroCells = [&]()
        {
            const std::vector<std::byte> zero( std::size_t{ m_DiceCullCells } * sizeof( uint32_t ),
                                               std::byte{ 0 } );
            graph.QueueBufferUpload( m_DiceCells, zero );
        };
        if ( m_DiceCullCells >= ( 1u << 24 ) || !m_DicePoolCornerNodes.IsValid() )
        {
            zeroCells();
            return Common::MakeFormattedError<bool>(
                 "{}: {} list cells (DicePool.z is a float, exact below 2^24) or no corner nodes", kDiceCullPass,
                 m_DiceCullCells );
        }
        const auto ensure = [&]() -> Common::BoolResultStr
        {
            if ( !m_DiceCullPipeline )
            {
                const auto shaders = Runtime::ResourceRegistry::GetShaderService();
                const auto shader  = shaders ? shaders->GetByName( std::string( kDiceCullShader ) ) : nullptr;
                if ( !shader )
                    return Common::MakeFormattedError<bool>(
                         "{}: compute shader '{}' is not registered (Programs/Terrain/{}.shader)", kDiceCullPass,
                         kDiceCullShader, kDiceCullShader );
                const auto pipeline =
                     ComputePipeline::Create( { .Shader = shader, .DebugName = std::string( kDiceCullShader ) } );
                if ( !pipeline )
                    return Common::MakeFormattedError<bool>( "{}: {}", kDiceCullPass, pipeline.GetError() );
                m_DiceCullPipeline = pipeline.GetValue();
            }
            return BOOLSUCCESS;
        };
        if ( Common::BoolResultStr ready = ensure(); !ready )
        {
            zeroCells();
            return ready;
        }

        std::vector<std::byte> rowBytes( m_DiceCullJobs.size() * sizeof( TerrainInstance ) );
        std::vector<uint32_t>  records( m_DiceCullJobs.size() * kDiceLevels * kDiceCullRecordWords, 0u );
        uint64_t               tested = 0u;
        for ( std::size_t i = 0; i < m_DiceCullJobs.size(); ++i )
        {
            std::memcpy( rowBytes.data() + i * sizeof( TerrainInstance ), &m_DiceCullJobs[i].Row,
                         sizeof( TerrainInstance ) );
            tested += m_DiceCullJobs[i].Cells;
            for ( uint32_t level = 0; level < kDiceLevels; ++level )
            {
                uint32_t* record = records.data() + ( i * kDiceLevels + level ) * kDiceCullRecordWords;
                record[1]        = 1u;                                          // instanceCount
                record[2]        = kLandscapeDiceCulledVertex * ( level + 1u ); // firstVertex: the level
            }
        }
        const RDG::BufferRef rows =
             graph.CreateBuffer( RDG::BufferDesc{ rowBytes.size() }, "Landscape.DiceCull.Rows" );
        graph.QueueBufferUpload( rows, rowBytes );
        std::vector<std::byte> recordBytes( records.size() * sizeof( uint32_t ) );
        std::memcpy( recordBytes.data(), records.data(), recordBytes.size() );
        m_DiceCullArgs = graph.CreateBuffer( RDG::BufferDesc{ recordBytes.size() }, "Landscape.DiceCull.Args" );
        graph.QueueBufferUpload( m_DiceCullArgs, recordBytes );
        const std::array<glm::mat4, 2> matrices{ frame.JitteredViewProjection, occlusion.PrevViewProjection };
        std::vector<std::byte>         viewBytes( sizeof( matrices ) );
        std::memcpy( viewBytes.data(), matrices.data(), viewBytes.size() );
        const RDG::BufferRef view =
             graph.CreateBuffer( RDG::BufferDesc{ viewBytes.size() }, "Landscape.DiceCull.View" );
        graph.QueueBufferUpload( view, viewBytes );

        // Last frame's HZB only while the eye stays where it was (kDiceHzbStillCm), with a history of this view.
        const bool      still = glm::length( frame.CameraPosition - frame.PrevCameraPosition ) <= kDiceHzbStillCm;
        const bool      tests = occlusion.Hzb.IsValid() && frame.HistoryValid() && still;
        RDG::TextureRef hzb   = occlusion.Neutral;
        DiceCullPush    base;
        if ( tests )
        {
            hzb      = occlusion.Hzb;
            base.Hzb = glm::uvec4( occlusion.Extent, 0u );
        }

        const auto&               layout = m_DiceCullLayout.Get( m_DiceCullPipeline->GetSpecification().Shader );
        ComputeNodeDeclaration    node( graph.GetFrameArena() );
        std::vector<DiceCullPush> pushes;
        std::vector<uint32_t>     groups;
        node.Name = kDiceCullPass;
        for ( std::size_t i = 0; i < m_DiceCullJobs.size(); ++i )
        {
            const DiceCullJob& job = m_DiceCullJobs[i];
            // The block's push range is declared with its bindings (RDGPassBindings validates it against the
            // shader's PushConstants block: 48 bytes) - the Record below pushes exactly this many.
            node.Access.Bindings( layout, Renderer::GetPipelineRouteFill( *m_DiceCullPipeline ) )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( DiceCullPush ) ) )
                 .Storage( DiceCullShaderParameters::LandscapeDiceCullRows, rows, RDG::Access::StorageRead )
                 .Storage( DiceCullShaderParameters::LandscapeDicePoolCorners, m_DicePoolCornerNodes,
                           RDG::Access::StorageRead )
                 .Storage( DiceCullShaderParameters::LandscapeDiceCells, m_DiceCells, RDG::Access::StorageWrite )
                 .Storage( DiceCullShaderParameters::LandscapeDiceCullArgs, m_DiceCullArgs,
                           RDG::Access::StorageWrite )
                 .Storage( DiceCullShaderParameters::LandscapeDiceCullView, view, RDG::Access::StorageRead )
                 .Sampler( DiceCullShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                 .Texture( DiceCullShaderParameters::LandscapeDiceHzbTexture, hzb, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() );
            DiceCullPush push = base;
            push.Tile   = glm::uvec4( static_cast<uint32_t>( i ), job.Cells, job.FirstCorner,
                                      static_cast<uint32_t>( i ) * kDiceLevels );
            push.Bounds = glm::vec4( job.Reach, 0.0f, 0.0f, 0.0f );
            pushes.push_back( push );
            groups.push_back( ( job.Cells + kDiceCullLocalSize - 1u ) / kDiceCullLocalSize );
        }
        node.Record = [pipeline = m_DiceCullPipeline, pushes = std::move( pushes ),
                       groups = std::move( groups )]( RDG::PassContext&     context,
                                                      const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            for ( uint32_t block = 0; block < pushes.size(); ++block )
            {
                RDG::PassBindings bindings( context, context.GetBindingBlock( block ) );
                bindings.PushConstants( &pushes[block], static_cast<uint32_t>( sizeof( DiceCullPush ) ) );
                if ( const Common::BoolResultStr dispatched =
                          Renderer::DispatchCompute( bindings, *pipeline, groups[block], 1, 1 );
                     !dispatched )
                    return dispatched;
            }
            return BOOLSUCCESS;
        };
        ComputeNodeList nodes = MakeComputeNodeList( graph.GetFrameArena() );
        nodes.push_back( std::move( node ) );
        if ( !AddComputeNodes( graph, textures, std::move( nodes ) ) )
        {
            m_DiceCullArgs = {};
            zeroCells();
            return Common::MakeFormattedError<bool>( "{}: the node could not be declared", kDiceCullPass );
        }
        DiceCullCensus& slot =
             m_DiceCullCensus[static_cast<std::size_t>( frame.FrameIndex % m_DiceCullCensus.size() )];
        graph.Extract( m_DiceCullArgs, slot.Args, RDG::Access::HostRead );
        slot.Jobs          = static_cast<uint32_t>( m_DiceCullJobs.size() );
        slot.Tested        = tested;
        slot.GridTriangles = m_DiceCullGridTriangles;
        m_DiceCulled       = true;
        return BOOLSUCCESS;
    }

    namespace
    {
        // SETUP of a node drawing the terrain: one binding block per executor (TerrainRenderer::GroupExecutors),
        // in order - the layout kept for the shader of @p pipeline, the one EVERY group's draws record with
        // (RecordDraws), the material's route fill, and @p view's scene/view inputs where the layout has their
        // slots (none: a program that samples none). Returns how many it declared.
        template <typename Declaration, typename Executors>
        uint32_t DeclareGroupBlocks( Declaration& declaration, const GraphicsPipeline* pipeline,
                                     ShaderBindingLayoutCache& layoutCache, const Executors& executors,
                                     const SceneViewInputs* view, const RDG::BufferRef dicePool,
                                     const RDG::BufferRef diceCells, const RDG::BufferRef diceCull )
        {
            // TERRAIN-DICE-CULL: the camera's rasters draw their rings from the cull's records.
            if ( diceCull.IsValid() )
                declaration.Read( diceCull, RDG::Access::IndirectArgs );
            const SlottedLayout& slotted = layoutCache.GetSlotted( pipeline->GetSpecification().Shader );
            const std::shared_ptr<const RDG::ShaderBindingLayout>& layout = slotted.Layout;
            for ( const MaterialExecutor* executor : executors )
            {
                auto block = declaration.Bindings( layout, executor->GetRouteFill() );
                // Every terrain vertex stage reads the frame's dice pool (TerrainVertex.glslh).
                block.Storage( TerrainShaderParameters::LandscapeVertexPool, dicePool, RDG::Access::StorageRead );
                block.Storage( TerrainShaderParameters::LandscapeDiceCells, diceCells, RDG::Access::StorageRead );
                if ( view != nullptr && SamplesSceneViewInputs( slotted.Slots ) )
                {
                    BindSceneViewInputs( block, *view, slotted.Slots );
                }
            }
            return static_cast<uint32_t>( executors.size() );
        }

        // RENDER-THREADS M1/M2: every node the terrain draws in declares the executors RecordDraws applies
        // (exactly @p executors), so their view sets and block copies exist before the graph records and the exec
        // runs sealed. A node shared with the meshes (a shadow view, a virtual shadow level, the rain occlusion)
        // declares them too: the meshes' lists declare theirs (MeshDrawList::DeclareBlocks).
        template <typename Declaration, typename Executors>
        void PrepareGroupExecutors( Declaration& declaration, const Executors& executors )
        {
            for ( const MaterialExecutor* executor : executors )
                declaration.Prepares( *executor );
        }
    } // namespace

    namespace
    {
        // CPU-FRAME-ARENA 3 (UE FMemStack: a thread's scratch, kept): RecordDraws' per-draw push block.
        thread_local std::vector<std::byte> t_TerrainRecordPush;
    } // namespace

    const MaterialExecutor* TerrainRenderer::GroupExecutorRange::Iterator::operator*() const
    {
        return ( m_Group->Materials->*m_Program )->GetMaterialExecutor();
    }

    TerrainRenderer::GroupExecutorRange
    TerrainRenderer::GroupExecutors( std::unique_ptr<DataDrivenMaterial> ProgramMaterials::*program ) const
    {
        return GroupExecutorRange( m_FrameGroups, program );
    }

    // ── Record. The push constant is per-draw state, snapshotted by Vulkan at record: the row index AND the
    // clip-from-world matrix (and a light view's slope term), so one material serves the camera and every
    // cascade. ────────────────────────────────────────────────────────────────────────────────────────────
    // RENDER-THREADS M2b: the values travel BY VALUE, the group material is only read. Each draw's push block is
    // a copy of the material's with the draw's fields over it (DrawPushFields, placed by name through the cell's
    // reconciled layout exactly as Material::WritePushField places them), handed to the draw: two nodes recording
    // the same group material (the shadow views share one Shadow material) read one unchanged material. UE: the
    // landscape's mesh draw commands carry their own shader bindings, the record writes nothing shared.
    // Group g's draws go through the block the node's setup declared for its material (firstBlock + g). The
    // first refused draw is the node's error.
    Common::BoolResultStr TerrainRenderer::RecordDraws(
         const RDG::PassContext& context, const uint32_t firstBlock, const GraphicsPipeline& pipeline,
         std::unique_ptr<DataDrivenMaterial> ProgramMaterials::*program, const glm::mat4& clipFromWorld,
         const std::vector<FrameDraw>& draws, const std::optional<glm::vec2>& shadowSlope,
         const std::optional<VirtualShadowIndirect>& indirect, const RDG::BufferRef diceCull )
    {
        // CPU-FRAME-ARENA 3: the groups' blocks in this thread's kept slots (RDG::OpenBindingBlocks), the push
        // block in its kept scratch (rewritten whole per draw, read by that draw's call alone), and each cell
        // shader's layout placed once per shader code (DrawPushFields::ShaderPlacements, CPU-FRAME-ARENA 4), no
        // field found by name per draw or per record: a steady frame records the terrain with no allocation.
        const RDG::OpenBindingBlocks      blocks( context, firstBlock, m_FrameGroups.size() );
        std::vector<std::byte>&           push       = t_TerrainRecordPush;
        DrawPushFields::ShaderPlacements& placements = DrawPushFields::ThreadShaderPlacements();
        for ( size_t k = 0; k < draws.size(); ++k )
        {
            const FrameDraw&        draw     = draws[k];
            const auto*             material = ( m_FrameGroups[draw.Group].Materials->*program ).get();
            const MaterialExecutor* executor = material->GetMaterialExecutor();
            const RDG::PassBindings& groupBlock = blocks.At( draw.Group );
            const Common::Memory::Buffer& block = executor->GetPushConstantBuffer();
            push.assign( block.Size, std::byte{ 0 } );
            if ( block.Data != nullptr && block.Size != 0u )
                std::memcpy( push.data(), block.Data, block.Size );
            DrawPushFields fields;
            fields.MaterialIndex( draw.Row ).Transform( clipFromWorld );
            if ( shadowSlope )
                fields.ShadowSlope( shadowSlope->x, shadowSlope->y );
            // Material::GetMaterialLayout's layout: its executor's shader's (none: nothing placed).
            const DrawPushFields::Placement& placement = placements.Of( executor->GetShader().get() );
            if ( auto written = fields.WriteInto( push, placement ); !written )
                return Common::MakeFormattedError( "{}: terrain draw {} ('{}'): {}", context.GetPassName(), k,
                                                   executor->GetDubugName(), written.GetError() );
            if ( indirect )
            {
                // The level's cull kept or dropped this tile (AppendVirtualShadowDraws: record k of the
                // terrain's).
                const uint64_t offset = ( uint64_t{ indirect->FirstRecord } + k ) * kMeshDrawArgumentsBytes;
                if ( auto drawn = Renderer::DrawProceduralIndirect( groupBlock, pipeline, executor,
                                                                    indirect->Arguments, offset, push );
                     !drawn.IsSuccess() )
                    return drawn;
                continue;
            }
            // TERRAIN-GPU-2 (UE FLandscapeSharedBuffers): the dice rings - the procedural decode's head, their
            // sub-vertices the dice pool's - then every other cell through the grid's shared index buffer, one
            // vertex a grid node, where the decode evaluated each node six times (FPS-FIELD-BARN: 5.5 M vertex
            // invocations of the camera's tiles, ten heightmap fetches each).
            // TERRAIN-DICE-CULL: a camera draw whose rings the cull kept draws one indirect record a ring level,
            // finest first as the decode orders them, of the kept cells alone (LandscapeDecodeCulledVertex).
            if ( draw.Split.RingVertices > 0u && draw.DiceCull != kNoDiceCull && diceCull.IsValid() )
            {
                for ( uint32_t level = kDiceLevels; level-- > 0u; )
                {
                    const uint64_t offset =
                         ( uint64_t{ draw.DiceCull } + level ) * kDiceCullRecordWords * sizeof( uint32_t );
                    if ( auto drawn = Renderer::DrawProceduralIndirect( groupBlock, pipeline, executor,
                                                                        diceCull, offset, push );
                         !drawn.IsSuccess() )
                        return drawn;
                }
            }
            else if ( draw.Split.RingVertices > 0u )
            {
                if ( auto drawn = Renderer::DrawProcedural( groupBlock, pipeline, executor,
                                                            draw.Split.RingVertices, 1, push );
                     !drawn.IsSuccess() )
                    return drawn;
            }
            // TERRAIN-LOD-1 (UE: each section with its own LOD's index buffer): every section's cells on its own
            // grid, every range of one grid one indexed call (the ranges are disjoint cells, so their order is
            // free: a section row's alternating grids are not one call each); the instance names the grid
            // (kLandscapeGridLodInstances).
            const LandscapeGridRanges cells = LandscapeGridRangesOf( draw.Split );
            if ( cells.Count == 0u )
                continue; // the rings cover the whole tile
            std::array<ProceduralIndexedRange, kLandscapeMaxSections * kLandscapeMaxSections * 4> ranges{};
            std::array<bool, kLandscapeMaxSections * kLandscapeMaxSections * 4>                   placed{};
            for ( uint32_t first = 0; first < cells.Count; ++first )
            {
                if ( placed[first] )
                    continue;
                const uint32_t side  = cells.Ranges[first].Cells;
                uint32_t       count = 0u;
                for ( uint32_t r = first; r < cells.Count; ++r )
                {
                    if ( placed[r] || cells.Ranges[r].Cells != side )
                        continue;
                    placed[r]                       = true;
                    const LandscapeGridRange& range = cells.Ranges[r];
                    ranges[count].IndexCount        = range.IndexCount;
                    ranges[count].FirstIndex        = range.FirstIndex;
                    ranges[count].VertexOffset      = static_cast<int32_t>( range.FirstRow * ( side + 1u ) );
                    ranges[count].InstanceCount     = range.Rows;
                    ranges[count].FirstInstance =
                         kLandscapeGridFirstInstance + range.GridLod * kLandscapeGridLodInstances;
                    ++count;
                }
                const auto grid = m_GridIndices.find( side );
                if ( grid == m_GridIndices.end() || !grid->second )
                {
                    return Common::MakeFormattedError(
                         "{}: terrain draw {} ('{}'): no index buffer for the {}-cell grid", context.GetPassName(),
                         k, executor->GetDubugName(), side );
                }
                const std::span<const ProceduralIndexedRange> gridRanges( ranges.data(), count );
                if ( auto indexed = Renderer::DrawProceduralIndexed( groupBlock, pipeline, executor,
                                                                     *grid->second, gridRanges, push );
                     !indexed.IsSuccess() )
                    return indexed;
            }
        }
        return BOOLSUCCESS;
    }

    GraphicsPipeline* TerrainRenderer::BasePassPipeline()
    {
        GraphicsPipeline* const solid = ForwardPipeline().get();
#if DESERT_DEV_INSTRUMENTS
        if ( !m_Wireframe || solid == nullptr || m_WireframeRefused )
            return solid;
        std::shared_ptr<GraphicsPipeline>& twin = m_ForwardVirtual ? m_WireframeVsmPipeline : m_WireframePipeline;
        if ( !twin )
        {
            // The solid spec with the line polygon mode (device feature fillModeNonSolid), writing its own depth:
            // the wireframe view is not prepassed (MeshRendererForward's StaticMeshWireframe).
            GraphicsPipelineSpecification spec = solid->GetSpecification();
            spec.DebugName += "|Wireframe";
            spec.PolygonMode       = PrimitivePolygonMode::Wireframe;
            spec.DepthWriteEnabled = true;
            const auto created     = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
            if ( !created )
            {
                m_WireframeRefused = true;
                LOG_ERROR( "[TerrainRenderer] the wireframe view draws the ground solid: {}", created.GetError() );
                return solid;
            }
            twin = created.GetValue();
        }
        return twin.get();
#else
        return solid;
#endif
    }

    SystemRasterPass TerrainRenderer::GeometryPass()
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb || !m_Pipeline )
            return {};

        // Right after the meshes in AddFrameBasePass, on the same scene framebuffer: merges into the open render
        // pass (depth shared, no clear) so terrain and meshes depth-resolve against each other.
        SystemRasterPass pass{ .Name        = "TerrainPass",
                               .ExecuteFunc = [this]( RDG::PassContext&     context,
                                                      const FrameGraphRefs& refs ) -> Common::BoolResultStr
                               {
                                   // Forward path only. In Deferred the terrain is in the G-buffer
                                   // (RenderGBufferManual) and lit by the composite; drawing it here too would
                                   // light the ground twice, two different ways.
                                   if ( m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred )
                                       return BOOLSUCCESS;
                                   const auto* camera = m_SceneRenderer->GetMainCamera();
                                   if ( ( camera == nullptr ) || m_ViewDraws.empty() || m_ForwardRefused )
                                       return BOOLSUCCESS;
                                   // Through the blocks declared below (the scene/view inputs Terrain.shader
                                   // samples - the cloud shadow map - are entries of them).
                                   (void)refs;
                                   return RecordDraws( context, 0, *BasePassPipeline(), ForwardProgram(),
                                                       m_SceneRenderer->GetViewFrame()->JitteredViewProjection,
                                                       m_ViewDraws, std::nullopt, std::nullopt,
                                                       CameraDiceCullArgs() );
                               },
                               .TargetFramebuffer = targetFb };
        pass.Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& refs )
        {
            // Exactly when the exec draws: one block per Forward material of the frame's groups, each binding the
            // scene/view inputs its shader has slots for (a block entry IS the read).
            if ( m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred ||
                 m_SceneRenderer->GetMainCamera() == nullptr || m_ViewDraws.empty() || m_ForwardRefused )
            {
                return;
            }
            const SceneViewInputs                      view      = SceneViewInputsOf( refs );
            const GroupExecutorRange                   executors = GroupExecutors( ForwardProgram() );
            PrepareGroupExecutors( declared, executors );
            (void)DeclareGroupBlocks( declared, BasePassPipeline(),
                                      m_ForwardVirtual ? m_ForwardVsmLayout : m_ForwardLayout, executors, &view,
                                      m_DicePool, m_DiceCells, CameraDiceCullArgs() );
        };
        return pass;
    }

    Common::BoolResultStr TerrainRenderer::RenderGBufferManual( const RDG::PassContext& context )
    {
        const auto& gbuffer = m_SceneRenderer->GetGBuffer();
        const auto* camera  = m_SceneRenderer->GetMainCamera();
        const GraphicsPipeline* const pipeline = GBufferPassPipeline();
        if ( !gbuffer || ( camera == nullptr ) || pipeline == nullptr || m_ViewDraws.empty() )
            return BOOLSUCCESS;

        // LOAD, not clear: the meshes' G-buffer fill ran just before and cleared it. The graph opens the
        // render pass (SceneRenderer::AddFrameTerrainGBuffer); its depth is what the terrain tests against -
        // under an early-Z pass the ground's own prepassed depth ("Deferred: Terrain Depth Prepass"), tested
        // EQUAL without writes and without the hole discard (DESERT_TERRAIN_DEPTH_EQUAL, early fragment tests), so
        // a texel the grass or a mesh covers is rejected before the layers are fetched. Through the blocks
        // DeclareGBufferDraws declared (both pipelines run TerrainGBuffer, the permutation only drops the discard,
        // so one layout).
        return RecordDraws( context, 0, *pipeline, &ProgramMaterials::GBuffer,
                            m_SceneRenderer->GetViewFrame()->JitteredViewProjection, m_ViewDraws,
                            std::nullopt, std::nullopt, CameraDiceCullArgs() );
    }

    void TerrainRenderer::DeclareGBufferDraws( RDG::PassBuilder& pass, const SceneViewInputs& view )
    {
        // Exactly when RenderGBufferManual draws. TerrainGBuffer.shader samples the view's DBuffer alone.
        const GraphicsPipeline* const pipeline = GBufferPassPipeline();
        if ( !m_SceneRenderer->GetGBuffer() || m_SceneRenderer->GetMainCamera() == nullptr ||
             pipeline == nullptr || m_ViewDraws.empty() )
        {
            return;
        }
        const GroupExecutorRange executors = GroupExecutors( &ProgramMaterials::GBuffer );
        PrepareGroupExecutors( pass, executors );
        (void)DeclareGroupBlocks( pass, pipeline, m_GBufferLayout, executors, &view, m_DicePool, m_DiceCells,
                                  CameraDiceCullArgs() );
    }

    bool TerrainRenderer::PrepassesGBuffer() const
    {
        return m_DepthPrepass && m_GBufferDepthPipeline && m_GBufferPrepassedPipeline;
    }

    const GraphicsPipeline* TerrainRenderer::GBufferPassPipeline() const
    {
        if ( PrepassesGBuffer() )
            return m_GBufferPrepassedPipeline.get();
        return m_GBufferPipeline.get();
    }

    const GraphicsPipeline* TerrainRenderer::ViewDepthPipeline() const
    {
        if ( m_SceneRenderer->GetMainCamera() == nullptr || m_ViewDraws.empty() )
            return nullptr;
        // Deferred: under the frame's early-Z pass, into the G-buffer depth ("TerrainGBuffer" then tests it).
        // A frame whose tiles have no visibility layer discards nothing: the early-tested twin (UE: a landscape
        // without holes is opaque in the depth pass). Same layout and declarations, so m_DepthLayout holds.
        const bool noHoles = !m_FrameHasHoles && m_DepthNoHolesPipeline && m_GBufferDepthNoHolesPipeline;
        if ( m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred )
        {
            if ( !m_SceneRenderer->GetGBuffer() || !PrepassesGBuffer() )
                return nullptr;
            if ( noHoles )
                return m_GBufferDepthNoHolesPipeline.get();
            return m_GBufferDepthPipeline.get();
        }
        // Forward: the frames that draw the terrain at all ("TerrainPass" draws the same m_ViewDraws,
        // CloserOrEqual against this depth); the wireframe view's base pass writes its own depth
        // (BasePassPipeline): not prepassed, as the meshes'.
        if ( m_ForwardRefused || WireframeView() )
            return nullptr;
        if ( noHoles )
            return m_DepthNoHolesPipeline.get();
        return m_DepthPipeline.get();
    }

    void TerrainRenderer::DeclareDepthDraws( RDG::PassBuilder& pass )
    {
        // Exactly when RenderDepthManual draws (ViewDepthPipeline). TerrainDepth.shader samples no scene/view
        // input; both its pipelines run the one shader, so one layout.
        const GraphicsPipeline* const pipeline = ViewDepthPipeline();
        if ( pipeline == nullptr )
            return;
        const GroupExecutorRange executors = GroupExecutors( &ProgramMaterials::Depth );
        PrepareGroupExecutors( pass, executors );
        (void)DeclareGroupBlocks( pass, pipeline, m_DepthLayout, executors, nullptr, m_DicePool, m_DiceCells,
                                  CameraDiceCullArgs() );
    }

    Common::BoolResultStr TerrainRenderer::RenderDepthManual( const RDG::PassContext& context )
    {
        const GraphicsPipeline* const pipeline = ViewDepthPipeline();
        if ( pipeline == nullptr )
            return BOOLSUCCESS;
        // The base pass's very matrix (GeometryPass / "TerrainGBuffer"), so both rasterise the same depth.
        return RecordDraws( context, 0, *pipeline, &ProgramMaterials::Depth,
                            m_SceneRenderer->GetViewFrame()->JitteredViewProjection, m_ViewDraws,
                            std::nullopt, std::nullopt, CameraDiceCullArgs() );
    }

    const std::vector<TerrainRenderer::FrameDraw>& TerrainRenderer::ShadowViewDraws( const uint32_t view ) const
    {
        for ( const LocalLightDraws& local : m_LocalLightDraws )
            if ( local.Light.Owns( view ) )
                return local.Draws;
        // A cascade (or its movable layer, kCascadeMovableViewBase + v with the base at the cascade count): the
        // camera's tiles at the cascade's dicing. A view no cascade was prepared for draws the camera's.
        const std::size_t cascades = m_CascadeDraws.size();
        const std::size_t cascade  = view >= cascades && view < 2 * cascades ? view - cascades : view;
        if ( cascade < cascades && !m_CascadeDraws[cascade].empty() )
            return m_CascadeDraws[cascade];
        return m_FrameDraws;
    }

    std::optional<uint64_t> TerrainRenderer::LocalShadowSignature( const LocalShadowCasterLight& light ) const
    {
        for ( const LocalLightDraws& local : m_LocalLightDraws )
            if ( local.Light.FirstView == light.FirstView )
                return local.Signature;
        // PrepareFrame was not handed this light (SceneRenderer hands it MeshRenderer::GetLocalShadowCasterLights,
        // the list this key is asked for): what would be drawn into its views is unknown, so they are not cached,
        // and RecordShadowView would draw the camera's tiles into them. Said, not hidden.
        LOG_ERROR( "[Shadows] TerrainRenderer: local shadow view {} was not prepared this frame - its terrain is "
                   "drawn at the camera's LOD and its depth is not cached",
                   light.FirstView );
        return std::nullopt;
    }

    std::optional<uint64_t> TerrainRenderer::CascadeShadowSignature( const uint32_t view ) const
    {
        ShadowSignatureHash           hash;
        const std::vector<FrameDraw>& draws = ShadowViewDraws( view );
        hash.Add( draws.size() );
        // A cascade's draws are parallel to the queue (PrepareLocalShadowViews pushes one per tile, as
        // PrepareFrame does for the camera's); anything else is not a cascade's list and is not cached.
        if ( !draws.empty() && draws.size() != m_Queue.size() )
            return std::nullopt;
        for ( std::size_t i = 0; i < draws.size(); ++i )
        {
            const FrameDraw&       draw = draws[i];
            const TerrainDrawData& t    = m_Queue[i];
            if ( draw.Group >= m_FrameGroups.size() || draw.Row >= m_FrameGroups[draw.Group].Instances.size() )
                return std::nullopt;
            // The whole row TerrainShadow.shader's vertex stage reads: placement, LOD grid and borders, the
            // visibility layer, the displacement and its dice (the camera's rings, the cascade's dice level, the
            // row's first pool vertex).
            hash.Add( m_FrameGroups[draw.Group].Instances[draw.Row] );
            hash.Add( draw.VertexCount );
            hash.Bytes( m_FrameGroups[draw.Group].Key->data(), m_FrameGroups[draw.Group].Key->size() );
            // The heightmap's texels (sculpting edits them in place: HeightRevision) and the image itself (its
            // device generation: a recreated one at the same address is another image).
            hash.Add( t.Heightmap.get() );
            if ( t.Heightmap )
                hash.Add( t.Heightmap->GetResourceGeneration() );
            hash.Add( t.Landscape.HeightRevision );
            // The weightmap: its visibility channel cuts the holes, its layer weights drive the displacement.
            hash.Add( t.Weights.Weightmap.get() );
            hash.Add( t.Weights.Revision );
            hash.Add( t.Weights.VisibilityLayer );
        }
        // The dice pool every diced row reads its displaced vertices from ("Landscape: Dice Pool", evaluated from
        // the camera's rows): each job's row, its maps and the layer heights it samples.
        if ( !draws.empty() )
            for ( const DicePoolJob& job : m_DicePoolJobs )
            {
                hash.Add( job.Row );
                hash.Add( job.Vertices );
                hash.Add( job.Heightmap.get() );
                hash.Add( job.Weightmap.get() );
                hash.Add( job.LayerHeights.get() );
                if ( job.LayerHeights )
                    hash.Add( job.LayerHeights->GetResourceGeneration() );
            }
        return hash.Value;
    }

    std::optional<uint64_t> TerrainRenderer::CascadeShadowContent( const uint32_t   view,
                                                                   const glm::mat4& casterViewProj ) const
    {
        // THE LANDSCAPE IN THE BOX, as UE's cached CSM invalidates a cascade (by the static primitives its bounds
        // meet): the tiles whose caster box reaches @p casterViewProj, each by its identity, its placement, its
        // maps (a sculpted height, a repainted weight or a recreated image changes it) and its material's texture
        // set. Nothing of this frame's draws: no rows, no dice, no LOD (a new ring or dice does not change it) and
        // not the view's draw list, so the box the cache was drawn with, asked again on the next frame, answers
        // the same while nothing in it was edited - the cascade scrolls. The tiles are walked in the queue's order
        // (the landscape system's, frame to frame the same); the texture set is the camera's draw's (parallel to
        // the queue), the key string of the set, not the frame's group index.
        if ( m_ShadowPipeline == nullptr || ShadowViewDraws( view ).empty() )
            return ShadowSignatureHash{}.Value; // nothing drawn into the view: the hash of nothing
        if ( m_FrameDraws.size() != m_Queue.size() )
            return std::nullopt;
        // The tiles the view's casters reach: a sun cascade's without its sun side (CascadeCasterFrustum, pancaked).
        const Core::Frustum box = IsDirectionalCasterView( casterViewProj ) ? CascadeCasterFrustum( casterViewProj )
                                                                            : Core::Frustum( casterViewProj, glm::mat4( 1.0f ) );
        ShadowSignatureHash hash;
        // Four tiles' caster boxes per test (Intersects4, Intersects bit for bit): at every i % 4 == 0 the next
        // four boxes go into one AABBQuad; tile i reads bit i % 4 (bits past the queue never read).
        uint32_t reached4 = 0u;
        for ( std::size_t i = 0; i < m_Queue.size(); ++i )
        {
            if ( ( i & 3u ) == 0u )
            {
                Common::Math::AABBQuad quad;
                for ( std::size_t lane = 0; lane < 4u && i + lane < m_Queue.size(); ++lane )
                    quad.Set( lane, TileCasterBox( m_Queue[i + lane].Landscape ) );
                reached4 = box.Intersects4( quad );
            }
            const TerrainDrawData& t = m_Queue[i];
            if ( ( ( reached4 >> ( i & 3u ) ) & 1u ) == 0u )
                continue;
            const FrameDraw& draw = m_FrameDraws[i];
            if ( draw.Group >= m_FrameGroups.size() || m_FrameGroups[draw.Group].Key == nullptr )
                return std::nullopt;
            hash.Bytes( m_FrameGroups[draw.Group].Key->data(), m_FrameGroups[draw.Group].Key->size() );
            hash.Add( t.Landscape.Entity );
            hash.Add( t.Landscape.FirstSampleX );
            hash.Add( t.Landscape.FirstSampleZ );
            hash.Add( t.Landscape.NeighbourMask );
            hash.Add( t.Landscape.OriginX );
            hash.Add( t.Landscape.OriginZ );
            hash.Add( t.Landscape.BaseY );
            hash.Add( t.Landscape.SpacingCm );
            hash.Add( t.Landscape.ZScale );
            hash.Add( t.Landscape.QuadsPerTile );
            hash.Add( t.Heightmap.get() );
            if ( t.Heightmap )
                hash.Add( t.Heightmap->GetResourceGeneration() );
            hash.Add( t.Landscape.HeightRevision );
            hash.Add( t.Weights.Weightmap.get() );
            hash.Add( t.Weights.Revision );
            hash.Add( t.Weights.VisibilityLayer );
        }
        return hash.Value;
    }

    uint32_t TerrainRenderer::DeclareShadowView( RenderPassDeclaration& declared, const uint32_t view )
    {
        if ( !m_ShadowPipeline || ShadowViewDraws( view ).empty() )
            return 0;
        // A depth-only caster samples no scene/view input.
        const GroupExecutorRange executors = GroupExecutors( &ProgramMaterials::Shadow );
        PrepareGroupExecutors( declared, executors );
        return DeclareGroupBlocks( declared, m_ShadowPipeline.get(), m_ShadowLayout, executors, nullptr,
                                   m_DicePool, m_DiceCells, RDG::BufferRef{} );
    }

    Common::BoolResultStr TerrainRenderer::RecordShadowView( const RDG::PassContext& context, const uint32_t view,
                                                             const uint32_t           firstBlock,
                                                             const glm::mat4&                 casterViewProj,
                                                             const CascadeCasterBias&         bias,
                                                             const std::span<const glm::mat4> region )
    {
        const std::vector<FrameDraw>& viewDraws = ShadowViewDraws( view );
        if ( !m_ShadowPipeline || viewDraws.empty() )
            return BOOLSUCCESS;
        // A SCROLLED CASCADE fills only its strips (MeshRenderer::PlanCascadeCache), one a call under the strip's
        // scissor (MeshRenderer's scroll pass): the rest of the layer is the cache copied in. A tile's draw keeps
        // the patches of its grid a strip reaches and its rings when they reach it (StripPatchesOf); a tile none
        // reaches is left out. A cascade's draws are parallel to the queue (CascadeShadowSignature) and to the
        // camera's rows (PrepareFrame), whose LOD, borders and displacement they draw.
        std::vector<FrameDraw> regionDraws;
        if ( !region.empty() && viewDraws.size() == m_Queue.size() && m_FrameInstances.size() == m_Queue.size() )
        {
            std::vector<Core::Frustum> strips;
            strips.reserve( region.size() );
            for ( const glm::mat4& strip : region )
                strips.push_back( CascadeCasterFrustum( strip ) ); // a scrolled sun cascade's: pancaked
            for ( std::size_t i = 0; i < viewDraws.size(); ++i )
            {
                const FrameDraw&   draw = viewDraws[i];
                const StripPatches patches =
                     StripPatchesOf( strips, m_Queue[i].Landscape, m_FrameInstances[i], draw.Split );
                // The rings ride with the first window.
                bool rings = patches.Rings;
                for ( const glm::ivec4& window : patches.Windows )
                {
                    FrameDraw part    = draw;
                    part.Split.Window = window;
                    if ( !rings )
                        part.Split.RingVertices = 0u;
                    rings = false;
                    regionDraws.push_back( part );
                }
                if ( rings )
                {
                    // No patch of the grid outside the rings is reached: the rings alone.
                    FrameDraw part    = draw;
                    part.Split.Window = glm::ivec4( 0 );
                    regionDraws.push_back( part );
                }
            }
            if ( regionDraws.empty() )
                return BOOLSUCCESS;
        }
        const std::vector<FrameDraw>& draws = regionDraws.empty() ? viewDraws : regionDraws;
        // The view's slope term, as the mesh casters get it (their CameraUB, MaterialShadow): a push value
        // like the matrix beside it, so the one shadow material per group still serves every view. The
        // constant part is already in casterViewProj (Graphic::CasterViewProj). A point/spot view's pair is
        // the light's (Graphic::LocalCasterBias), the same the meshes in that view take: the terrain drawn at
        // the light's LOD is pushed back by the rule every caster is, not by a constant of its own.
        // Its own row inside the view's: the terrain's share of the shadow cost, summed over views.
        DESERT_PROFILE_PASS( "TerrainShadowView" );
        // Through the blocks DeclareShadowView declared on this view's node.
        // A sun cascade (orthographic: IsDirectionalCasterView) draws pancaked; a local light's view clips.
        auto* caster = m_ShadowPipeline.get();
        if ( IsDirectionalCasterView( casterViewProj ) && m_CascadeShadowPipeline )
            caster = m_CascadeShadowPipeline.get();
        return RecordDraws( context, firstBlock, *caster, &ProgramMaterials::Shadow, casterViewProj,
                            draws, glm::vec2( bias.SlopeBias, bias.SlopeClamp ) );
    }

    void TerrainRenderer::AppendVirtualShadowEntries( std::vector<ShadowCasterEntry>& entries ) const
    {
        // A tile's depth changes with its heightmap's pixels (HeightRevision), its holes (the weightmap's Revision
        // and its visibility layer), where it sits and the grid it is drawn with: its LOD grid (Params.w) and the
        // whole LODs of its sections and of the neighbours' its borders stitch to (SectionLods), the main camera's
        // (the virtual shadow rows, PrepareVirtualShadowRows, are parallel to the queue). A grid change redraws
        // the tile's pages (UE: a landscape LOD change invalidates its virtual shadow pages); the morph inside one
        // grid is not tracked (REMAINDER).
        for ( size_t index = 0; index < m_Queue.size(); ++index )
        {
            const TerrainDrawData&   tile = m_Queue[index];
            const LandscapeTileDraw& at   = tile.Landscape;
            uint64_t                 hash = 1469598103934665603ull;
            const auto               add  = [&hash]( const auto& value )
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): FNV-1a over the value's bytes
                const auto* bytes = reinterpret_cast<const unsigned char*>( &value );
                for ( std::size_t i = 0; i < sizeof( value ); ++i )
                {
                    hash ^= bytes[i];
                    hash *= 1099511628211ull;
                }
            };
            add( tile.Heightmap.get() );
            add( at.OriginX );
            add( at.OriginZ );
            add( at.BaseY );
            add( at.SpacingCm );
            add( at.ZScale );
            add( at.FirstSampleX );
            add( at.FirstSampleZ );
            add( at.QuadsPerTile );
            add( at.NeighbourMask );
            add( at.Entity );
            add( at.HeightRevision );
            add( tile.Weights.Weightmap.get() );
            add( tile.Weights.Revision );
            add( tile.Weights.VisibilityLayer );
            const VsmTileBox box = TileBox( at );
            if ( m_VsmInstances.size() == m_Queue.size() )
            {
                const TerrainInstance& instance = m_VsmInstances[index];
                add( instance.Params.w );
                for ( const glm::vec4& lods : instance.SectionLods )
                    add( glm::floor( lods ) );
                // THE DISPLACED RELIEF (TURN-CACHE-2, LandscapeVirtualShadowRow): its own entry, over the square the
                // fade's envelope reaches around the anchor, keyed by the tile, the material's values and the anchor
                // - never the eye. Not the camera's dice rings and their clip either (they re-triangulate the same
                // surface; REMAINDER-TURN-CACHE-2), nor whether the camera dices the tile yet (a cell past the
                // envelope is its plain grid either way). A new anchor redraws the old square's pages and the new's.
                const float fadeEnd = instance.Displacement.w;
                if ( instance.Displacement.x > 0.0f && instance.Params.w < 0.5f &&
                     TileMeetsVsmEnvelope( at, m_VsmAnchor, fadeEnd, kLandscapeVsmAnchorReachCm ) )
                {
                    // The relief's key goes on from the tile's (its placement, maps and LOD), in its own key space.
                    const uint64_t tileKey = hash;
                    hash ^= 0x52454C494546ull; // "RELIEF"
                    add( instance.Displacement );
                    add( instance.DisplacementTiling );
                    add( instance.InputTileSize );
                    add( instance.InputBlend );
                    add( m_VsmAnchor );
                    add( kLandscapeVsmAnchorReachCm );
                    const uint64_t relief = hash;
                    hash                  = tileKey;
                    const float reach     = fadeEnd + kLandscapeVsmAnchorReachCm;
                    entries.push_back(
                         { .Signature = relief,
                           .Min       = glm::vec3( std::max( box.Min.x, m_VsmAnchor.x - reach ), box.Min.y,
                                                   std::max( box.Min.z, m_VsmAnchor.z - reach ) ),
                           .Max       = glm::vec3( std::min( box.Max.x, m_VsmAnchor.x + reach ), box.Max.y,
                                                   std::min( box.Max.z, m_VsmAnchor.z + reach ) ) } );
                }
            }
            entries.push_back( { .Signature = hash, .Min = box.Min, .Max = box.Max } );
        }
    }

    void TerrainRenderer::AppendVirtualShadowDraws( std::vector<VirtualShadowCasterDraw>& draws ) const
    {
        // RecordVirtualShadow draws m_VsmDraws in order, one per queued tile (PrepareVirtualShadowRows).
        DESERT_VERIFY( m_VsmDraws.empty() || m_VsmDraws.size() == m_Queue.size(),
                       "the terrain's virtual shadow draws are one per queued tile" );
        for ( size_t k = 0; k < m_VsmDraws.size(); ++k )
        {
            const VsmTileBox box = TileBox( m_Queue[k].Landscape );
            draws.push_back( { .Min       = box.Min,
                               .Max       = box.Max,
                               .Arguments = { m_VsmDraws[k].VertexCount, 0u, 0u, 0u, 0u } } );
        }
    }

    Core::ShaderVariant TerrainRenderer::ForwardVsmVariant()
    {
        return Core::ShaderVariant{ .Defines = { std::string( kLocalShadowVsmDefine ) }, .ShippedByCook = true };
    }

    Core::ShaderVariant TerrainRenderer::DepthNoHolesVariant()
    {
        return Core::ShaderVariant{ .Defines       = { std::string( "DESERT_TERRAIN_NO_HOLES" ) },
                                    .ShippedByCook = true };
    }

    Core::ShaderVariant TerrainRenderer::GBufferDepthEqualVariant()
    {
        return Core::ShaderVariant{ .Defines       = { std::string( "DESERT_TERRAIN_DEPTH_EQUAL" ) },
                                    .ShippedByCook = true };
    }

    void TerrainRenderer::EnsureDepthNoHolesPipelines()
    {
        if ( m_DepthNoHolesRefused || ( m_DepthNoHolesPipeline && m_GBufferDepthNoHolesPipeline ) )
            return;
        const Core::ShaderVariant variant = DepthNoHolesVariant();
        auto acquired = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( "TerrainDepth", variant );
        std::string error;
        if ( acquired )
        {
            const std::shared_ptr<Shader> shader = acquired.ExtractValue();
            m_DepthNoHolesPipeline =
                 CreateTerrainPipeline( m_SceneRenderer, shader, "TerrainDepthPipeline|DESERT_TERRAIN_NO_HOLES",
                                        SceneDepthLayout(), error, TerrainDepthState::Shader );
            if ( m_DepthNoHolesPipeline )
                m_GBufferDepthNoHolesPipeline = CreateTerrainPipeline(
                     m_SceneRenderer, shader, "TerrainGBufferDepthPipeline|DESERT_TERRAIN_NO_HOLES",
                     GBufferDepthLayout(), error, TerrainDepthState::Shader );
        }
        else
            error = std::format( "the permutation was refused: {}", acquired.GetError() );
        if ( m_DepthNoHolesPipeline && m_GBufferDepthNoHolesPipeline )
            return;
        m_DepthNoHolesPipeline.reset();
        m_GBufferDepthNoHolesPipeline.reset();
        m_DepthNoHolesRefused = true;
        LOG_ERROR(
             "[TerrainRenderer] TerrainDepth|DESERT_TERRAIN_NO_HOLES is not built ({}): the depth prepass of a "
             "landscape without holes keeps the discarding program (exact, late depth test)",
             error );
    }

    bool TerrainRenderer::EnsureForwardVsmPipeline()
    {
        if ( m_PipelineVsm )
            return true;
        if ( m_PipelineVsmRefused )
            return false;
        auto acquired = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( "Terrain", ForwardVsmVariant() );
        std::string error;
        if ( acquired )
            m_PipelineVsm = CreateTerrainPipeline( m_SceneRenderer, acquired.ExtractValue(),
                                                   "TerrainPipeline|DESERT_LOCAL_SHADOW_VSM", SceneTargetLayout(), error,
                                                   TerrainDepthState::BasePass );
        else
            error = std::format( "its DESERT_LOCAL_SHADOW_VSM permutation was refused: {}", acquired.GetError() );
        if ( !m_PipelineVsm )
        {
            m_PipelineVsmRefused = true;
            LOG_ERROR( "[TerrainRenderer] the forward terrain is not drawn under Shadows.Method VirtualMaps: {}",
                       error );
            return false;
        }
        return true;
    }

    const GraphicsPipeline* TerrainRenderer::VirtualShadowPipeline()
    {
        if ( m_VirtualShadowPipeline || m_VirtualShadowRefused )
            return m_VirtualShadowPipeline.get();
        const auto refuse = [this]( const std::string& why ) -> const GraphicsPipeline*
        {
            m_VirtualShadowRefused = true;
            LOG_ERROR( "[TerrainRenderer] the terrain casts no virtual shadow: {}", why );
            return nullptr;
        };
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "TerrainVirtualShadow" );
        if ( !shader )
            return refuse( "missing shader 'TerrainVirtualShadow'" );

        GraphicsPipelineSpecification spec;
        spec.DebugName = "TerrainVirtualShadowPipeline";
        spec.Shader    = shader;
        ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
        // The level's node has no attachment (RDG PassBuilder::RenderArea): no colour, no depth, and so no depth
        // test; the fragment keeps the nearest occluder itself (imageAtomicMax). Depth clamp, as the mesh casters
        // (MeshRenderer::EnsureVirtualShadowPipelines): ground nearer the sun than the level's range still writes.
        spec.TargetLayout      = RenderTargetLayout{};
        spec.BlendEnable       = false;
        spec.DepthTestEnabled  = false;
        spec.DepthWriteEnabled = false;
        spec.DepthClampEnable  = true;
        spec.CullMode          = CullMode::None;
        const auto pipeline    = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
            return refuse( pipeline.GetError() );
        m_VirtualShadowPipeline = pipeline.GetValue();
        return m_VirtualShadowPipeline.get();
    }

    uint32_t TerrainRenderer::DeclareVirtualShadow( RDG::PassBuilder&                   pass,
                                                    const VirtualShadowBlockParameters& parameters )
    {
        if ( m_VsmDraws.empty() )
            return 0;
        const GraphicsPipeline* pipeline = VirtualShadowPipeline();
        if ( pipeline == nullptr )
            return 0;
        const SlottedLayout& slotted = m_VirtualShadowLayout.GetSlotted( pipeline->GetSpecification().Shader );
        const GroupExecutorRange executors = GroupExecutors( &ProgramMaterials::VirtualShadow );
        PrepareGroupExecutors( pass, executors );
        for ( const MaterialExecutor* executor : executors )
        {
            auto block = pass.Bindings( slotted.Layout, executor->GetRouteFill() );
            block.Storage( TerrainShaderParameters::LandscapeVertexPool, m_DicePool, RDG::Access::StorageRead );
            block.Storage( TerrainShaderParameters::LandscapeDiceCells, m_DiceCells, RDG::Access::StorageRead );
            parameters( block, slotted );
        }
        return static_cast<uint32_t>( executors.size() );
    }

    Common::BoolResultStr TerrainRenderer::RecordVirtualShadow( const RDG::PassContext& context,
                                                                const uint32_t          firstBlock,
                                                                const glm::mat4&        levelViewProj,
                                                                const RDG::BufferRef    arguments,
                                                                const uint32_t          firstRecord )
    {
        if ( !m_VirtualShadowPipeline || m_VsmDraws.empty() )
            return BOOLSUCCESS;
        // No caster bias in the virtual shadow map: its receivers are offset along their normals instead
        // (kVsmNormalBiasTexels, "VSM: Project").
        DESERT_PROFILE_PASS( "TerrainVirtualShadow" );
        return RecordDraws( context, firstBlock, *m_VirtualShadowPipeline, &ProgramMaterials::VirtualShadow,
                            levelViewProj, m_VsmDraws, glm::vec2( 0.0f, 0.0f ),
                            VirtualShadowIndirect{ .Arguments = arguments, .FirstRecord = firstRecord } );
    }

} // namespace Desert::Graphic::System
