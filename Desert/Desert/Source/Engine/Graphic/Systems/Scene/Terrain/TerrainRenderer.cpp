#include "TerrainRenderer.hpp"
#include <Engine/Graphic/MeshDrawArguments.hpp>

#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/RenderPass.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/Materials/MaterialBinder.hpp>
#include <Engine/Graphic/Materials/DrawPushFields.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Core/Units.hpp>

#include <algorithm>
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
    };
    constexpr std::string_view kDicePoolPass      = "Landscape: Dice Pool";
    constexpr std::string_view kDicePoolShader    = "LandscapeDicePool";
    constexpr uint32_t         kDicePoolLocalSize = 64; // LocalSize of the shader
} // namespace

namespace Desert::Graphic::System
{
    namespace
    {
        // THE VIEW IS THE VIEW BLOCK'S (GPU-COST-26; UE: the landscape's base pass reads
        // FViewUniformShaderParameters like every pass of the view): the surface programs read their matrices, the
        // eye, PreExposure and the grass field lens from CameraUB (Common/CameraUB.glslh,
        // ShaderProtocols::MakeCameraUB of this frame), written by SceneFrameBinding::ApplyTo in ApplySceneFrame.
        // The pass keeps no copy of the view: everything per-terrain rides as a TerrainInstance row
        // (TerrainBatch.hpp).

        // A landscape tile is placed by its LandscapeTileDraw: the shader builds world positions from the
        // root's origin and GLOBAL sample indices (see LandscapeTileDraw for why). The LOD
        // half is the tile's LandscapeTileLods; the grid it is drawn with is the floor of its own LOD, and
        // every vertex morphs from there by its blended one (LandscapeLod.glslh).
        TerrainInstance LandscapeInstance( const TerrainDrawData& t, const LandscapeTileLod& lod,
                                           const TerrainLayerInputs& layers )
        {
            const LandscapeTileDraw& l = t.Landscape;

            TerrainInstance instance;
            instance.Params =
                 glm::vec4( static_cast<float>( l.QuadsPerTile ) * l.SpacingCm, lod.Center,
                            static_cast<float>( t.Weights.VisibilityLayer + 1 ), std::floor( lod.Center ) );
            instance.Params2        = glm::vec4(
                 1.0f / std::max( 0.01f, kLandscapeLodBlendRange ), static_cast<float>( t.Weights.LayerCount ),
                 static_cast<float>( World::Landscape::LandscapeWeightmapPageCount( t.Weights.LayerCount ) ),
                 static_cast<float>( l.NeighbourMask ) );
            instance.GroundColor    = t.Weights.Ground;
            instance.LandscapeFrame = glm::vec4( l.OriginX, l.BaseY, l.OriginZ, l.SpacingCm );
            instance.LandscapeTile =
                 glm::vec4( static_cast<float>( l.FirstSampleX ), static_cast<float>( l.FirstSampleZ ),
                            static_cast<float>( l.QuadsPerTile ), l.ZScale );
            instance.LodEdges   = lod.Edges;
            instance.LodCorners = lod.Corners;
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
            // Each channel's material input, matched by its layer's NAME (UE's LayerBlend LayerName), and the
            // ground's by the root's first layer's.
            for ( size_t c = 0; c < t.Weights.LayerNames.size(); ++c )
                instance.LayerInput[c / World::Landscape::kLandscapeWeightmapChannels][static_cast<glm::length_t>(
                     c % World::Landscape::kLandscapeWeightmapChannels )] =
                     static_cast<float>( layers.InputOf( t.Weights.LayerNames[c] ) );
            instance.InputSlices   = layers.Slices;
            instance.GroundInput.x = static_cast<float>( layers.InputOf( t.Weights.GroundLayer ) );
            return instance;
        }

        // THE LANDSCAPE DISPLACEMENT of a tile's camera row (UE 5.4+ landscape displacement): the material's
        // values (Terrain.shader Displacement*), the main camera the fade is measured from, and the dice rings
        // around it - every cell of the LOD-0 grid the fade's square reaches, diced finer the larger it is on
        // screen. A grid past LOD 0 is never diced: its vertices are past LOD 1, where the relief is zero
        // (LandscapeDisplacementLodScale).
        void ApplyLandscapeDisplacement( TerrainInstance& instance, const LandscapeTileDraw& l,
                                         const DataDrivenMaterial& surface, const glm::vec3& camera,
                                         const glm::mat4& projection, const glm::mat4& viewProjection,
                                         const std::optional<glm::vec2>& slab, uint32_t viewHeight,
                                         int dicingRate )
        {
            const auto read = [&]( const std::string& name )
            {
                return MaterialBinder::ReadRowParam( surface.GetMaterialLayout(), surface.GetParamRow(), name )
                     .value_or( glm::vec4( 0.0f ) )
                     .x;
            };
            for ( uint32_t k = 0; k < World::Landscape::kLandscapeMaxWeightLayers; ++k )
            {
                const std::string layer                                            = std::format( "Layer{}", k );
                instance.InputTileSize[k / 4][static_cast<glm::length_t>( k % 4 )] = read( layer + "TileSize" );
                // LayerKBlend is an Int: the row holds its bits (MaterialRowSlotValue).
                instance.InputBlend[k / 4][static_cast<glm::length_t>( k % 4 )] =
                     static_cast<float>( std::bit_cast<int32_t>( read( layer + "Blend" ) ) );
            }
            instance.DisplacementTiling =
                 glm::vec4( read( "FarTilingDistance" ), read( "FarTilingScale" ), 0.0f, 0.0f );
            const float magnitude     = dicingRate > 0 ? read( "DisplacementMagnitude" ) : 0.0f;
            const float fadeStart     = std::max( read( "DisplacementFadeStart" ), 1.0f );
            const float fadeEnd       = std::max( read( "DisplacementFadeEnd" ), fadeStart + 1.0f );
            instance.Displacement     = glm::vec4( magnitude, read( "DisplacementCenter" ), fadeStart, fadeEnd );
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
                const float reach = nearCm;
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

        // The landscape material a tile drew with, for the landscape's ray tracing hit group: the values the
        // ground reads (Common/LandscapeSurface.glslh LandscapeMaterial) out of the drawn @p surface's row, and
        // each input's maps as the bindless slots of the images @p layers' arrays copied. Terrain.shader and
        // TerrainGBuffer.shader declare every one of these parameters (the ShaderCacheKey suite holds the two
        // Properties blocks equal); a cell without one is a broken shader, said, and its hits read zero for it.
        TerrainFrameMaterial FrameMaterialOf( const DataDrivenMaterial& surface, const TerrainLayerInputs& layers )
        {
            std::string missing;
            const auto  read = [&]( const std::string& name ) -> glm::vec4
            {
                const std::optional<glm::vec4> value =
                     MaterialBinder::ReadRowParam( surface.GetMaterialLayout(), surface.GetParamRow(), name );
                if ( !value )
                    missing += ( missing.empty() ? "" : ", " ) + name;
                return value.value_or( glm::vec4( 0.0f ) );
            };

            TerrainFrameMaterial material;
            material.Tint = read( "Tint" );
            material.Values.Tiling =
                 glm::vec4( read( "FarTilingDistance" ).x, read( "FarTilingScale" ).x,
                            read( "MacroVariationSize" ).x, read( "MacroVariationStrength" ).x );
            material.Values.Detail =
                 glm::vec4( read( "DetailNormalScale" ).x, read( "DetailNormalStrength" ).x, read( "MacroHueSize" ).x,
                            read( "MacroHueStrength" ).x );
            for ( uint32_t k = 0; k < World::Landscape::kLandscapeMaxWeightLayers; ++k )
            {
                const std::string layer      = std::format( "Layer{}", k );
                material.Values.LayerTint[k] = read( layer + "Tint" );
                // LayerKBlend is an Int: the row holds its bits (MaterialRowSlotValue).
                material.Values.LayerParams[k] = glm::vec4(
                     read( layer + "TileSize" ).x,
                     static_cast<float>( std::bit_cast<int32_t>( read( layer + "Blend" ).x ) ),
                     read( layer + "NormalStrength" ).x, 0.0f );
                for ( uint32_t kind = 0; kind < kTerrainLayerMapKinds; ++kind )
                {
                    const float slice = layers.Slices[k][static_cast<glm::length_t>( kind )];
                    if ( slice < 0.0f )
                        continue;
                    const Image2D* source = layers.Sources[kind][static_cast<size_t>( slice )];
                    material.LayerMaps[k][static_cast<glm::length_t>( kind )] = source->GetBindlessSlot().Index;
                }
            }
            if ( !missing.empty() )
                LOG_ERROR(
                     "[TerrainRenderer] '{}' has no {} parameter(s): the landscape's ray tracing hits read zero "
                     "for them",
                     surface.GetShaderName(), missing );
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

        struct TileKey
        {
            float   OriginX                            = 0.0f;
            float   OriginZ                            = 0.0f;
            int32_t X                                  = 0;
            int32_t Z                                  = 0;
            bool    operator==( const TileKey& ) const = default;
        };

        struct TileKeyHash
        {
            size_t operator()( const TileKey& k ) const
            {
                const size_t h = std::hash<float>{}( k.OriginX ) ^ ( std::hash<float>{}( k.OriginZ ) << 1u );
                return h ^ ( std::hash<int64_t>{}( ( static_cast<int64_t>( k.X ) << 32 ) ^ k.Z ) << 2u );
            }
        };

        TileKey KeyOf( const LandscapeTileDraw& l, int32_t dx, int32_t dz )
        {
            const auto quads = static_cast<int32_t>( std::max( l.QuadsPerTile, 1u ) );
            return { l.OriginX, l.OriginZ, l.FirstSampleX / quads + dx, l.FirstSampleZ / quads + dz };
        }

        // The tile's continuous LOD for @p view (the main camera, or a point/spot light: LocalShadowCasterLight):
        // its bounding sphere's screen size, UE's way. The sphere holds the tile's full height range, not its
        // actual heights — a taller sphere only means finer detail sooner.
        float TileLod( const LandscapeTileDraw& l, const Geometry::LODView& view )
        {
            const float     extent = static_cast<float>( l.QuadsPerTile ) * l.SpacingCm;
            const glm::vec3 center(
                 l.OriginX + ( static_cast<float>( l.FirstSampleX ) * l.SpacingCm ) + 0.5f * extent, l.BaseY,
                 l.OriginZ + ( static_cast<float>( l.FirstSampleZ ) * l.SpacingCm ) + 0.5f * extent );
            const float radius = glm::length( glm::vec3( 0.5f * extent, 256.0f * l.ZScale, 0.5f * extent ) );
            return LandscapeLodFromScreenSize( MakeLandscapeLodSettings( l.QuadsPerTile ),
                                               Geometry::BoundsScreenRadiusSquared( center, radius, view ) );
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

        using TileLodMap = std::unordered_map<TileKey, float, TileKeyHash>;

        // A tile's LOD and its borders' for one view, from that view's LODs of every tile it draws (a neighbour
        // the view does not draw has nothing to agree with: nullopt).
        LandscapeTileLod TileLodsOf( const LandscapeTileDraw& l, const TileLodMap& lods )
        {
            return LandscapeTileLods( lods.at( KeyOf( l, 0, 0 ) ),
                                      [&]( int dx, int dz ) -> std::optional<float>
                                      {
                                          const auto n = lods.find( KeyOf( l, dx, dz ) );
                                          return n == lods.end() ? std::nullopt
                                                                 : std::optional<float>( n->second );
                                      } );
        }
    } // namespace

    namespace
    {
        // Whether a pipeline draws the forward BASE PASS over the view's depth prepass: then it tests
        // CloserOrEqual (its own prepassed depth passes) and writes no depth (UE: the base pass after a full
        // depth prepass, DepthRead).
        enum class TerrainDepthState : uint8_t
        {
            Shader,   // the shader's own `State` block
            BasePass, // CloserOrEqual, no depth write
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
        return BOOLSUCCESS;
    }

    void TerrainRenderer::PrepareFrame( const ViewFrame& frame )
    {
        m_FrameGroups.clear();
        m_FrameDraws.clear();
        // The lights' draws are PrepareLocalShadowViews' (after UpdateLocalShadows gave the lights their views).
        m_LocalLightDraws.clear();
        m_FrameInstances.clear();
        m_FrameMaterials.clear();
        m_DicePoolJobs.clear();
        m_DicePoolVertices = 0;
        if ( m_Queue.empty() )
            return;

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
        std::unordered_map<std::string, size_t> groupIndex;
        std::unordered_set<std::string>         layerKeysThisFrame;
        m_FrameDraws.reserve( m_Queue.size() );
        m_FrameInstances.reserve( m_Queue.size() );
        m_FrameMaterials.reserve( m_Queue.size() );

        // Every tile's LOD first: a tile's borders take the max with its neighbours', so all must be known.
        TileLodMap tileLods;
        for ( const auto& t : m_Queue )
            tileLods[KeyOf( t.Landscape, 0, 0 )] =
                 TileLod( t.Landscape, Geometry::LODView{ frame.CameraPosition, frame.Projection } );

        // Each landscape's height slab for the view's footprint on its dice (ApplyLandscapeDisplacement): the
        // lowest and highest sample of its tiles within the across-edge reach of the ring diced 2 - no finer ring
        // reaches a tile past it.
        const uint32_t                            viewHeight = m_SceneRenderer->GetViewExtent().Height;
        const int                                 dicingRate = m_SceneRenderer->GetDisplacementDicingRate();
        std::map<std::array<float, 4>, glm::vec2> diceSlabs;
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
            const auto [slab, fresh] = diceSlabs.try_emplace( slabKey( l ), glm::vec2( l.MinY, l.MaxY ) );
            if ( !fresh )
                slab->second = glm::vec2( std::min( slab->second.x, l.MinY ), std::max( slab->second.y, l.MaxY ) );
        }

        for ( const auto& t : m_Queue )
        {
            const auto [it, inserted] = groupIndex.try_emplace(
                 TerrainTextureKey( t.Overrides, t.Heightmap.get(), t.Weights.Weightmap.get() ),
                 m_FrameGroups.size() );
            ProgramMaterials& materials = m_Materials[it->first];
            if ( !materials.Forward )
            {
                materials.Forward       = std::make_unique<DataDrivenMaterial>( "Terrain" );
                materials.GBuffer       = std::make_unique<DataDrivenMaterial>( "TerrainGBuffer" );
                materials.Depth         = std::make_unique<DataDrivenMaterial>( "TerrainDepth" );
                materials.Shadow        = std::make_unique<DataDrivenMaterial>( "TerrainShadow" );
                materials.VirtualShadow = std::make_unique<DataDrivenMaterial>( "TerrainVirtualShadow" );
            }
            if ( m_ForwardVirtual && !materials.ForwardVsm )
                materials.ForwardVsm =
                     std::make_unique<DataDrivenMaterial>( m_PipelineVsm->GetSpecification().Shader );

            // The material's layer inputs (TerrainLayerInputs): keyed by the material's own textures, not the
            // tile's, so every tile of a landscape shares one set of arrays; resolved once per frame per material.
            const std::string   layerKey = TerrainTextureKey( t.Overrides, nullptr, nullptr );
            TerrainLayerInputs& layers   = m_LayerInputs[layerKey];
            if ( !m_EmptyLayerArray )
                m_EmptyLayerArray = CreateTerrainEmptyLayerArray();
            if ( layerKeysThisFrame.insert( layerKey ).second )
                UpdateTerrainLayerInputs( materials.Forward->GetSchema(), t.Overrides,
                                          "landscape material " + layerKey, m_EmptyLayerArray, layers );

            if ( inserted )
            {
                m_FrameGroups.push_back( { it->first, {}, {} } );

                // Textures are identical for every terrain of this group BY CONSTRUCTION (the key), so they
                // are bound once, from the terrain that opened the group. Unset samplers keep the backend
                // white fallback, so this is purely additive.
                DataDrivenMaterial* surface = deferred ? materials.GBuffer.get() : ( materials.*ForwardProgram() ).get();
                const auto&         schema  = surface->GetSchema();
                for ( const auto& [name, handle] : t.Overrides.Textures )
                {
                    if ( handle == 0 )
                        continue;
                    // The layer inputs' maps and layer infos are no samplers: they reach the shader through
                    // the arrays bound below and the rows' slice / input tables.
                    const auto param =
                         std::find_if( schema.Params.begin(), schema.Params.end(),
                                       [&]( const Core::Formats::ShaderParam& p ) { return p.Name == name; } );
                    if ( param != schema.Params.end() && ( param->ElementIndex >= 0 || param->IsAssetRef() ) )
                        continue;
                    auto* tex = Runtime::ResourceRegistry::GetTextureService()->Get( Common::UUID( handle ) );
                    if ( tex == nullptr )
                        continue;
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key/handle names this
                    // exact type
                    // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast)
                    auto* img = static_cast<Image2D*>(
                         Runtime::ResourceRegistry::GetImageService()->Resolve( tex->GetImageHandle() ) );
                    // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
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
            }
            FrameGroup&         group   = m_FrameGroups[it->second];
            DataDrivenMaterial* surface = deferred ? materials.GBuffer.get() : ( materials.*ForwardProgram() ).get();

            surface->ApplyDefaults();
            for ( const auto& [name, value] : t.Overrides.Params )
                surface->SetParamRaw( name, value );

            TerrainInstance instance = LandscapeInstance( t, TileLodsOf( t.Landscape, tileLods ), layers );
            const auto      slab     = diceSlabs.find( slabKey( t.Landscape ) );
            ApplyLandscapeDisplacement(
                 instance, t.Landscape, *surface, frame.CameraPosition, frame.Projection, frame.ViewProjection,
                 slab != diceSlabs.end() ? std::optional<glm::vec2>( slab->second ) : std::nullopt, viewHeight,
                 dicingRate );
            // The tile's displaced sub-vertices, evaluated once this frame (AddDicePoolPass) for every view's
            // draw.
            if ( const uint32_t pooled = LandscapeDicePoolVertexCount(
                      t.Landscape.QuadsPerTile, static_cast<uint32_t>( instance.Params.w ), instance );
                 pooled > 0u )
            {
                instance.DicePool.x = static_cast<float>( m_DicePoolVertices );
                m_DicePoolJobs.push_back(
                     { .Row       = instance,
                       .Heightmap = t.Heightmap,
                       .Weightmap = t.Weights.Weightmap != nullptr ? std::shared_ptr<Image>( t.Weights.Weightmap )
                                                                   : FallbackTextures::Get().GetFallbackTexture2D(
                                                                          Core::Formats::ImageFormat::RGBA8F ),
                       .HasWeightmap = t.Weights.Weightmap != nullptr,
                       .LayerHeights = layers.Arrays[3] ? layers.Arrays[3] : m_EmptyLayerArray,
                       .Vertices     = pooled } );
                m_DicePoolVertices += pooled;
            }

            FrameDraw draw;
            draw.Group       = it->second;
            draw.Row         = static_cast<uint32_t>( group.Instances.size() );
            draw.VertexCount = LandscapeDicedVertexCount( t.Landscape.QuadsPerTile,
                                                          static_cast<uint32_t>( instance.Params.w ), instance );
            m_FrameDraws.push_back( draw );

            group.ParamRows.insert( group.ParamRows.end(), surface->GetParamRow().begin(),
                                    surface->GetParamRow().end() );
            group.Instances.push_back( instance );
            m_FrameInstances.push_back( instance );
            m_FrameMaterials.push_back( FrameMaterialOf( *surface, layers ) );
        }

        // ── Upload every group's material rows, still before any draw ───────────────────────────────────
        // (the instance rows go up in PrepareLocalShadowViews, once the lights' rows follow the camera's; the view
        // block goes on in ApplySceneFrame)
        for ( const auto& group : m_FrameGroups )
        {
            ProgramMaterials const& materials = m_Materials[group.Key];
            DataDrivenMaterial*     surface   = deferred ? materials.GBuffer.get() : ( materials.*ForwardProgram() ).get();
            for ( DataDrivenMaterial* material :
                  { surface, materials.Shadow.get(), materials.VirtualShadow.get(), materials.Depth.get() } )
            {
                // The depth-only programs (the casters, the view's depth prepass) have no Materials[] row block
                // (no Properties block): for them the slot is optional.
                const bool shadowProgram = material != surface;
                auto*      rows = shadowProgram ? material->Find<StorageBufferProperty>( kMaterialRowBlock )
                                                : material->Get<StorageBufferProperty>( kMaterialRowBlock );
                if ( rows )
                    if ( !group.ParamRows.empty() )
                        rows->SetRawData( group.ParamRows.data(),
                                          static_cast<uint32_t>( group.ParamRows.size() * sizeof( glm::vec4 ) ) );
            }
        }
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
            const ProgramMaterials& materials = m_Materials.at( group.Key );
            scene.ApplyTo( deferred ? materials.GBuffer.get() : ( materials.*ForwardProgram() ).get() );
        }
    }

    void TerrainRenderer::PrepareLocalShadowViews( const std::vector<LocalShadowCasterLight>& localLights,
                                                   const std::vector<float>& cascadeWorldPerTexel )
    {
        // Every light of the frame has its entry, drawn or not: LocalShadowSignature answers for each of them.
        m_LocalLightDraws.clear();
        for ( const LocalShadowCasterLight& light : localLights )
            m_LocalLightDraws.push_back(
                 { .Light = light, .Draws = {}, .Signature = ShadowSignatureHash{}.Value } );
        m_CascadeDraws.assign( cascadeWorldPerTexel.size(), {} );
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
        for ( LocalLightDraws& local : m_LocalLightDraws )
        {
            TileLodMap lightLods;
            for ( const auto& t : m_Queue )
                if ( TileMeetsDisc( t.Landscape, local.Light.Lod.Origin, local.Light.Reach ) )
                    lightLods[KeyOf( t.Landscape, 0, 0 )] = TileLod( t.Landscape, local.Light.Lod );

            ShadowSignatureHash hash;
            for ( std::size_t i = 0; i < m_Queue.size(); ++i )
            {
                const TerrainDrawData&   t = m_Queue[i];
                const LandscapeTileDraw& l = t.Landscape;
                if ( !lightLods.contains( KeyOf( l, 0, 0 ) ) )
                    continue;
                FrameGroup&           group    = m_FrameGroups[m_FrameDraws[i].Group];
                // The same layer inputs as the camera's row (resolved above this frame); the shadow program
                // reads none of them, the row is whole.
                const TerrainInstance instance =
                     LandscapeInstance( t, TileLodsOf( l, lightLods ),
                                        m_LayerInputs.at( TerrainTextureKey( t.Overrides, nullptr, nullptr ) ) );

                FrameDraw draw;
                draw.Group = m_FrameDraws[i].Group;
                draw.Row   = static_cast<uint32_t>( group.Instances.size() );
                draw.VertexCount =
                     LandscapeLodVertexCount( l.QuadsPerTile, static_cast<uint32_t>( instance.Params.w ) );
                local.Draws.push_back( draw );
                group.Instances.push_back( instance );

                // What TerrainShadow.shader reads: the placement and LOD fields of the row (not the layer
                // colours), the heightmap and its texels' revision, and the weightmap where the tile has a
                // visibility layer (its holes).
                hash.Add( instance.Params );
                hash.Add( instance.Params2 );
                hash.Add( instance.LandscapeFrame );
                hash.Add( instance.LandscapeTile );
                hash.Add( instance.LodEdges );
                hash.Add( instance.LodCorners );
                hash.Add( draw.VertexCount );
                hash.Add( t.Heightmap );
                hash.Add( l.HeightRevision );
                if ( t.Weights.VisibilityLayer >= 0 )
                {
                    hash.Add( t.Weights.Weightmap );
                    hash.Add( t.Weights.Revision );
                }
            }
            local.Signature = hash.Value;
        }

        // ── The instance rows, the camera's and the lights', once, still before any draw ──────────────
        const bool deferred = m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred;
        for ( const auto& group : m_FrameGroups )
        {
            ProgramMaterials const& materials = m_Materials[group.Key];
            DataDrivenMaterial*     surface   = deferred ? materials.GBuffer.get() : ( materials.*ForwardProgram() ).get();
            for ( DataDrivenMaterial* material :
                  { surface, materials.Shadow.get(), materials.VirtualShadow.get(), materials.Depth.get() } )
                if ( auto* instances =
                          material->Get<StorageBufferProperty>( TerrainShaderParameters::TerrainInstances ) )
                    instances->SetRawData(
                         group.Instances.data(),
                         static_cast<uint32_t>( group.Instances.size() * sizeof( TerrainInstance ) ) );
        }
    }

    Common::BoolResultStr TerrainRenderer::AddDicePoolPass( RDG::Builder& graph, FrameTextures& textures )
    {
        m_DicePool = {};
        if ( m_FrameDraws.empty() )
            return BOOLSUCCESS;
        // The row's first vertex rides as a float (TerrainInstance.DicePool.x): exact below 2^24 vertices (512
        // MB).
        if ( m_DicePoolVertices >= ( 1u << 24 ) )
            return Common::MakeFormattedError<bool>( "{}: {} pooled vertices exceed the 2^24 a row can address",
                                                     kDicePoolPass, m_DicePoolVertices );
        const uint64_t vertices = std::max<uint32_t>( m_DicePoolVertices, 1u );
        m_DicePool =
             graph.CreateBuffer( RDG::BufferDesc{ vertices * 2u * sizeof( glm::vec4 ) }, "Landscape.DicePool" );
        if ( m_DicePoolJobs.empty() )
        {
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

        std::vector<std::byte> rowBytes( m_DicePoolJobs.size() * sizeof( TerrainInstance ) );
        for ( std::size_t i = 0; i < m_DicePoolJobs.size(); ++i )
            std::memcpy( rowBytes.data() + i * sizeof( TerrainInstance ), &m_DicePoolJobs[i].Row,
                         sizeof( TerrainInstance ) );
        const RDG::BufferRef rows =
             graph.CreateBuffer( RDG::BufferDesc{ rowBytes.size() }, "Landscape.DicePool.Rows" );
        graph.QueueBufferUpload( rows, rowBytes );

        // One binding block and one dispatch per diced tile, all writing their own range of the one pool.
        ComputeNodeDeclaration node;
        node.Name                      = std::string( kDicePoolPass );
        const auto&             layout = m_DicePoolLayout.Get( m_DicePoolPipeline->GetSpecification().Shader );
        std::vector<glm::uvec4> pushes;
        std::vector<uint32_t>   groups;
        for ( std::size_t i = 0; i < m_DicePoolJobs.size(); ++i )
        {
            const DicePoolJob& job = m_DicePoolJobs[i];
            node.Access.Bindings( layout, Renderer::GetPipelineRouteFill( *m_DicePoolPipeline ) )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::uvec4 ) ) )
                 .Storage( DicePoolShaderParameters::LandscapeDicePoolRows, rows, RDG::Access::StorageRead )
                 .Texture(
                      DicePoolShaderParameters::HeightmapTexture, job.Heightmap, RDG::Access::SampledCompute,
                      std::format( "Landscape.Heightmap.{}", static_cast<const void*>( job.Heightmap.get() ) ) )
                 .Texture( DicePoolShaderParameters::WeightmapTexture, job.Weightmap, RDG::Access::SampledCompute,
                           job.HasWeightmap ? std::format( "Landscape.Weightmap.{}",
                                                           static_cast<const void*>( job.Weightmap.get() ) )
                                            : std::string( "Landscape.DicePool.WeightmapFallback" ) )
                 .Texture( DicePoolShaderParameters::LayerHeightTexture, job.LayerHeights,
                           RDG::Access::SampledCompute,
                           std::format( "Landscape.LayerHeights.{}",
                                        static_cast<const void*>( job.LayerHeights.get() ) ) )
                 .Sampler( DicePoolShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                 .Sampler( DicePoolShaderParameters::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
                 .Sampler( DicePoolShaderParameters::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() )
                 .Storage( DicePoolShaderParameters::LandscapeDicePoolVertices, m_DicePool,
                           RDG::Access::StorageWrite );
            pushes.emplace_back( static_cast<uint32_t>( i ), job.Vertices, 0u, 0u );
            groups.push_back( ( job.Vertices + kDicePoolLocalSize - 1u ) / kDicePoolLocalSize );
        }
        node.Record = [pipeline = m_DicePoolPipeline, pushes,
                       groups]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
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
        std::vector<ComputeNodeDeclaration> nodes;
        nodes.push_back( std::move( node ) );
        if ( !AddComputeNodes( graph, textures, std::move( nodes ) ) )
        {
            m_DicePool = {};
            return Common::MakeFormattedError<bool>( "{}: the node could not be declared", kDicePoolPass );
        }
        return BOOLSUCCESS;
    }

    namespace
    {
        // SETUP of a node drawing the terrain: one binding block per executor (TerrainRenderer::GroupExecutors),
        // in order - the layout kept for the shader of @p pipeline, the one EVERY group's draws record with
        // (RecordDraws), the material's route fill, and @p view's scene/view inputs where the layout has their
        // slots (none: a program that samples none). Returns how many it declared.
        template <typename Declaration>
        uint32_t DeclareGroupBlocks( Declaration& declaration, const GraphicsPipeline* pipeline,
                                     ShaderBindingLayoutCache&                   layoutCache,
                                     const std::vector<const MaterialExecutor*>& executors,
                                     const SceneViewInputs* view, const RDG::BufferRef dicePool )
        {
            const std::shared_ptr<const RDG::ShaderBindingLayout>& layout =
                 layoutCache.Get( pipeline->GetSpecification().Shader );
            for ( const MaterialExecutor* executor : executors )
            {
                auto block = declaration.Bindings( layout, executor->GetRouteFill() );
                // Every terrain vertex stage reads the frame's dice pool (TerrainVertex.glslh).
                block.Storage( TerrainShaderParameters::LandscapeVertexPool, dicePool, RDG::Access::StorageRead );
                if ( view != nullptr && SamplesSceneViewInputs( *layout ) )
                {
                    BindSceneViewInputs( block, *view, *layout );
                }
            }
            return static_cast<uint32_t>( executors.size() );
        }

        // RENDER-THREADS M1/M2: every node the terrain draws in declares the executors RecordDraws applies
        // (exactly @p executors), so their view sets and block copies exist before the graph records and the exec
        // runs sealed. A node shared with the meshes (a shadow view, a virtual shadow level, the rain occlusion)
        // declares them too: the meshes' lists declare theirs (MeshDrawList::DeclareBlocks).
        template <typename Declaration>
        void PrepareGroupExecutors( Declaration&                                declaration,
                                    const std::vector<const MaterialExecutor*>& executors )
        {
            for ( const MaterialExecutor* executor : executors )
                declaration.Prepares( *executor );
        }
    } // namespace

    std::vector<const MaterialExecutor*>
    TerrainRenderer::GroupExecutors( std::unique_ptr<DataDrivenMaterial> ProgramMaterials::*program ) const
    {
        std::vector<const MaterialExecutor*> executors;
        executors.reserve( m_FrameGroups.size() );
        for ( const auto& group : m_FrameGroups )
            executors.push_back( ( m_Materials.at( group.Key ).*program )->GetMaterialExecutor() );
        return executors;
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
         const std::optional<VirtualShadowIndirect>& indirect )
    {
        std::vector<std::unique_ptr<RDG::PassBindings>> blocks;
        blocks.reserve( m_FrameGroups.size() );
        for ( uint32_t group = 0; group < static_cast<uint32_t>( m_FrameGroups.size() ); ++group )
            blocks.push_back(
                 std::make_unique<RDG::PassBindings>( context, context.GetBindingBlock( firstBlock + group ) ) );
        std::vector<std::byte> push;
        for ( size_t k = 0; k < draws.size(); ++k )
        {
            const FrameDraw&        draw     = draws[k];
            const auto*             material = ( m_Materials.at( m_FrameGroups[draw.Group].Key ).*program ).get();
            const MaterialExecutor* executor = material->GetMaterialExecutor();
            const Common::Memory::Buffer& block = executor->GetPushConstantBuffer();
            push.assign( block.Size, std::byte{ 0 } );
            if ( block.Data != nullptr && block.Size != 0u )
                std::memcpy( push.data(), block.Data, block.Size );
            DrawPushFields fields;
            fields.MaterialIndex( draw.Row ).Transform( clipFromWorld );
            if ( shadowSlope )
                fields.ShadowSlope( shadowSlope->x, shadowSlope->y );
            if ( auto written = fields.WriteInto( push, material->GetMaterialLayout() ); !written )
                return Common::MakeFormattedError( "{}: terrain draw {} ('{}'): {}", context.GetPassName(), k,
                                                   executor->GetDubugName(), written.GetError() );
            if ( indirect )
            {
                // The level's cull kept or dropped this tile (AppendVirtualShadowDraws: record k of the
                // terrain's).
                const uint64_t offset = ( uint64_t{ indirect->FirstRecord } + k ) * kMeshDrawArgumentsBytes;
                if ( auto drawn = Renderer::DrawProceduralIndirect( *blocks[draw.Group], pipeline, executor,
                                                                    indirect->Arguments, offset, push );
                     !drawn.IsSuccess() )
                    return drawn;
                continue;
            }
            if ( auto drawn = Renderer::DrawProcedural( *blocks[draw.Group], pipeline, executor, draw.VertexCount,
                                                        1, push );
                 !drawn.IsSuccess() )
                return drawn;
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
                                   if ( ( camera == nullptr ) || m_FrameDraws.empty() || m_ForwardRefused )
                                       return BOOLSUCCESS;
                                   // Through the blocks declared below (the scene/view inputs Terrain.shader
                                   // samples - the cloud shadow map - are entries of them).
                                   (void)refs;
                                   return RecordDraws( context, 0, *BasePassPipeline(), ForwardProgram(),
                                                       m_SceneRenderer->GetViewFrame()->JitteredViewProjection,
                                                       m_FrameDraws );
                               },
                               .TargetFramebuffer = targetFb };
        pass.Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& refs )
        {
            // Exactly when the exec draws: one block per Forward material of the frame's groups, each binding the
            // scene/view inputs its shader has slots for (a block entry IS the read).
            if ( m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred ||
                 m_SceneRenderer->GetMainCamera() == nullptr || m_FrameDraws.empty() || m_ForwardRefused )
            {
                return;
            }
            const SceneViewInputs                      view      = SceneViewInputsOf( refs );
            const std::vector<const MaterialExecutor*> executors = GroupExecutors( ForwardProgram() );
            PrepareGroupExecutors( declared, executors );
            (void)DeclareGroupBlocks( declared, BasePassPipeline(),
                                      m_ForwardVirtual ? m_ForwardVsmLayout : m_ForwardLayout, executors, &view,
                                      m_DicePool );
        };
        return pass;
    }

    Common::BoolResultStr TerrainRenderer::RenderGBufferManual( const RDG::PassContext& context )
    {
        const auto& gbuffer = m_SceneRenderer->GetGBuffer();
        const auto* camera  = m_SceneRenderer->GetMainCamera();
        if ( !gbuffer || ( camera == nullptr ) || !m_GBufferPipeline || m_FrameDraws.empty() )
            return BOOLSUCCESS;

        // LOAD, not clear: the meshes' G-buffer fill ran just before and cleared it. The graph opens the
        // render pass (SceneRenderer::AddFrameTerrainGBuffer); its depth is what the terrain tests against.
        // Through the blocks DeclareGBufferDraws declared.
        return RecordDraws( context, 0, *m_GBufferPipeline, &ProgramMaterials::GBuffer,
                            m_SceneRenderer->GetViewFrame()->JitteredViewProjection, m_FrameDraws );
    }

    void TerrainRenderer::DeclareGBufferDraws( RDG::PassBuilder& pass )
    {
        // Exactly when RenderGBufferManual draws. TerrainGBuffer.shader samples no scene/view input.
        if ( !m_SceneRenderer->GetGBuffer() || m_SceneRenderer->GetMainCamera() == nullptr || !m_GBufferPipeline ||
             m_FrameDraws.empty() )
        {
            return;
        }
        const std::vector<const MaterialExecutor*> executors = GroupExecutors( &ProgramMaterials::GBuffer );
        PrepareGroupExecutors( pass, executors );
        (void)DeclareGroupBlocks( pass, m_GBufferPipeline.get(), m_GBufferLayout, executors, nullptr, m_DicePool );
    }

    void TerrainRenderer::DeclareDepthDraws( RDG::PassBuilder& pass )
    {
        // Exactly when RenderDepthManual draws: the forward path's frames that draw the terrain at all
        // ("TerrainPass" draws the same m_FrameDraws, CloserOrEqual against this depth). TerrainDepth.shader samples
        // no scene/view input.
        if ( m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred ||
             m_SceneRenderer->GetMainCamera() == nullptr || !m_DepthPipeline || m_FrameDraws.empty() ||
             m_ForwardRefused || WireframeView() )
        {
            return;
        }
        const std::vector<const MaterialExecutor*> executors = GroupExecutors( &ProgramMaterials::Depth );
        PrepareGroupExecutors( pass, executors );
        (void)DeclareGroupBlocks( pass, m_DepthPipeline.get(), m_DepthLayout, executors, nullptr, m_DicePool );
    }

    Common::BoolResultStr TerrainRenderer::RenderDepthManual( const RDG::PassContext& context )
    {
        // The wireframe view's base pass writes its own depth (BasePassPipeline): not prepassed, as the meshes'.
        if ( m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred ||
             m_SceneRenderer->GetMainCamera() == nullptr || !m_DepthPipeline || m_FrameDraws.empty() ||
             m_ForwardRefused || WireframeView() )
            return BOOLSUCCESS;
        // The base pass's very matrix (GeometryPass), so both rasterise the same depth.
        return RecordDraws( context, 0, *m_DepthPipeline, &ProgramMaterials::Depth,
                            m_SceneRenderer->GetViewFrame()->JitteredViewProjection, m_FrameDraws );
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
            hash.Bytes( m_FrameGroups[draw.Group].Key.data(), m_FrameGroups[draw.Group].Key.size() );
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

    uint32_t TerrainRenderer::DeclareShadowView( RenderPassDeclaration& declared, const uint32_t view )
    {
        if ( !m_ShadowPipeline || ShadowViewDraws( view ).empty() )
            return 0;
        // A depth-only caster samples no scene/view input.
        const std::vector<const MaterialExecutor*> executors = GroupExecutors( &ProgramMaterials::Shadow );
        PrepareGroupExecutors( declared, executors );
        return DeclareGroupBlocks( declared, m_ShadowPipeline.get(), m_ShadowLayout, executors, nullptr,
                                   m_DicePool );
    }

    Common::BoolResultStr TerrainRenderer::RecordShadowView( const RDG::PassContext& context, const uint32_t view,
                                                             const uint32_t           firstBlock,
                                                             const glm::mat4&         casterViewProj,
                                                             const CascadeCasterBias& bias )
    {
        const std::vector<FrameDraw>& draws = ShadowViewDraws( view );
        if ( !m_ShadowPipeline || draws.empty() )
            return BOOLSUCCESS;
        // The view's slope term, as the mesh casters get it (their CameraUB, MaterialShadow): a push value
        // like the matrix beside it, so the one shadow material per group still serves every view. The
        // constant part is already in casterViewProj (Graphic::CasterViewProj). A point/spot view's pair is
        // the light's (Graphic::LocalCasterBias), the same the meshes in that view take: the terrain drawn at
        // the light's LOD is pushed back by the rule every caster is, not by a constant of its own.
        // Its own row inside the view's: the terrain's share of the shadow cost, summed over views.
        DESERT_PROFILE_PASS( "TerrainShadowView" );
        // Through the blocks DeclareShadowView declared on this view's node.
        return RecordDraws( context, firstBlock, *m_ShadowPipeline, &ProgramMaterials::Shadow, casterViewProj,
                            draws, glm::vec2( bias.SlopeBias, bias.SlopeClamp ) );
    }

    void TerrainRenderer::AppendVirtualShadowEntries( std::vector<ShadowCasterEntry>& entries ) const
    {
        // A tile's depth changes with its heightmap's pixels (HeightRevision), its holes (the weightmap's Revision
        // and its visibility layer), where it sits and the grid it is drawn with: its LOD grid (Params.w) and the
        // whole LODs its borders stitch to (LodEdges / LodCorners), the main camera's (PrepareFrame; the frame's
        // instances are parallel to the queue). A grid change redraws the tile's pages (UE: a landscape LOD change
        // invalidates its virtual shadow pages); the morph inside one grid is not tracked (REMAINDER).
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
            if ( m_FrameInstances.size() == m_Queue.size() )
            {
                const TerrainInstance& instance = m_FrameInstances[index];
                add( instance.Params.w );
                add( glm::floor( instance.LodEdges ) );
                add( glm::floor( instance.LodCorners ) );
                // A displaced tile's depth follows the main camera (the fade, the dice window).
                if ( instance.DisplacementView.w > 0.5f )
                {
                    add( instance.Displacement );
                    add( instance.DisplacementView );
                    add( instance.DiceWindow );
                    add( instance.DiceRings );
                }
            }

            const VsmTileBox box = TileBox( at );
            entries.push_back( { .Signature = hash, .Min = box.Min, .Max = box.Max } );
        }
    }

    void TerrainRenderer::AppendVirtualShadowDraws( std::vector<VirtualShadowCasterDraw>& draws ) const
    {
        // RecordVirtualShadow draws m_FrameDraws in order, one per queued tile (PrepareFrame).
        DESERT_VERIFY( m_FrameDraws.empty() || m_FrameDraws.size() == m_Queue.size(),
                       "the terrain's frame draws are one per queued tile" );
        for ( size_t k = 0; k < m_FrameDraws.size(); ++k )
        {
            const VsmTileBox box = TileBox( m_Queue[k].Landscape );
            draws.push_back( { .Min       = box.Min,
                               .Max       = box.Max,
                               .Arguments = { m_FrameDraws[k].VertexCount, 0u, 0u, 0u, 0u } } );
        }
    }

    Core::ShaderVariant TerrainRenderer::ForwardVsmVariant()
    {
        return Core::ShaderVariant{ .Defines = { std::string( kLocalShadowVsmDefine ) }, .ShippedByCook = true };
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
        if ( m_FrameDraws.empty() )
            return 0;
        const GraphicsPipeline* pipeline = VirtualShadowPipeline();
        if ( pipeline == nullptr )
            return 0;
        const std::shared_ptr<const RDG::ShaderBindingLayout>& layout =
             m_VirtualShadowLayout.Get( pipeline->GetSpecification().Shader );
        const std::vector<const MaterialExecutor*> executors = GroupExecutors( &ProgramMaterials::VirtualShadow );
        PrepareGroupExecutors( pass, executors );
        for ( const MaterialExecutor* executor : executors )
        {
            auto block = pass.Bindings( layout, executor->GetRouteFill() );
            block.Storage( TerrainShaderParameters::LandscapeVertexPool, m_DicePool, RDG::Access::StorageRead );
            parameters( block, *layout );
        }
        return static_cast<uint32_t>( executors.size() );
    }

    Common::BoolResultStr TerrainRenderer::RecordVirtualShadow( const RDG::PassContext& context,
                                                                const uint32_t          firstBlock,
                                                                const glm::mat4&        levelViewProj,
                                                                const RDG::BufferRef    arguments,
                                                                const uint32_t          firstRecord )
    {
        if ( !m_VirtualShadowPipeline || m_FrameDraws.empty() )
            return BOOLSUCCESS;
        // No caster bias in the virtual shadow map: its receivers are offset along their normals instead
        // (kVsmNormalBiasTexels, "VSM: Project").
        DESERT_PROFILE_PASS( "TerrainVirtualShadow" );
        return RecordDraws( context, firstBlock, *m_VirtualShadowPipeline, &ProgramMaterials::VirtualShadow,
                            levelViewProj, m_FrameDraws, glm::vec2( 0.0f, 0.0f ),
                            VirtualShadowIndirect{ .Arguments = arguments, .FirstRecord = firstRecord } );
    }

} // namespace Desert::Graphic::System
