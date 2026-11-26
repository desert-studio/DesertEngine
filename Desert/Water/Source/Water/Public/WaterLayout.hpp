#pragma once

// THE WATER'S GPU ROWS THAT PUBLIC HEADERS NAME, declared once: Tools/ShaderProtocolGen writes their GLSL into
// Generated/Water/*.glslh and checks every std430 layout against these declarations at compile time. The blocks
// stay the programs' own (their bindings belong to the program); they write `{ DESERT_<GlslName>_MEMBERS … }`.
// The private rows (the passes' uniform blocks) are Water/Private/WaterLayout.hpp's.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>
#include <Water/Public/GerstnerWaterWaves.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace Desert::Water
{
    /// The wave lanes a frame draws (WaterWaveTable; WATER_MAX_WAVE_SETS of Generated/Water/WaterMeshTiles.glslh).
    inline constexpr std::size_t kMaxWaveLanes = 4;

    // A sample of the world's floor and shore (WaterShoreField; Water/WaterDepthField.glslh WaterShoreAt).
#define DESERT_WATER_SHORE_SAMPLE_FIELDS( X )                                                                     \
    X( float, GroundCm, "GroundCm", 0.0f )                                                                        \
    X( float, ShoreDistanceCm, "ShoreDistanceCm", 0.0f )                                                          \
    X( glm::vec2, ShoreDirection, "ShoreDirection", glm::vec2( 0.0f ) )
    DESERT_SHADER_STRUCT( WaterShoreSample, "WaterShoreSample", Std430, DESERT_WATER_SHORE_SAMPLE_FIELDS );
#undef DESERT_WATER_SHORE_SAMPLE_FIELDS

    inline constexpr const Graphic::ShaderProtocols::ShaderStructDecl* kWaterShoreSampleStructs[] = {
         &kWaterShoreSampleStruct };
    inline constexpr Graphic::ShaderProtocols::ShaderIncludeDecl kWaterShoreSampleShaderInclude{
         .Path         = "Generated/Water/WaterShoreSample.glslh",
         .SourceHeader = "Desert/Water/Source/Water/Public/WaterLayout.hpp",
         .Structs      = kWaterShoreSampleStructs,
    };
} // namespace Desert::Water

namespace Desert::Graphic::System
{
    // ---- THE WATER MESH'S TILE BUFFER (Mesh/Surface/WaterMeshTiles.glslh WaterTiles, binding 24): the frame's
    // waves block leads, the tile rows follow as the block's runtime array.

    // One drawn quadtree tile.
#define DESERT_WATER_TILE_ROW_FIELDS( X )                                                                         \
    /* xy = lower (X, Z) corner relative to the zone origin, z = still level Y, w = side */                       \
    X( glm::vec4, Tile, "Tile", glm::vec4( 0.0f ) )                                                               \
    /* x = LOD level, y = morphs, z = can morph twice, w = height LOD factor */                                   \
    X( glm::vec4, Lod, "Lod", glm::vec4( 0.0f ) )                                                                 \
    /* xy = the zone's world (X, Z) origin, z = LOD scale (cm), w = quads per side */                             \
    X( glm::vec4, Zone, "Zone", glm::vec4( 0.0f ) )                                                               \
    /* The tile's body (UE WaterBodyIndex; TileWalk::Bodies): x = its material's row, y = 1 for a river, -1 for   \
       an ocean drawn under a river's cut leaf, 2 for a fall's stretch, z = 1 + its wave set in                   \
       WaterWavesBlock::Sets (0 = flat), w = the body's index in its zone. */                                     \
    X( glm::vec4, Body, "Body", glm::vec4( 0.0f ) )                                                               \
    /* The body's medium (WaterBodyLane::Medium): y = its river's two-phase flow period (s; 0 off a river); on a  \
       river's tile x = its mouth's radius (cm) and zw = the mouth's world (X, Z); on a fall's row zw = the       \
       water's speeds at its two samples (cm/s). */                                                               \
    X( glm::vec4, Medium, "Medium", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( WaterTileRow, "WaterTileRow", Std430, DESERT_WATER_TILE_ROW_FIELDS );
#undef DESERT_WATER_TILE_ROW_FIELDS

    // One Gerstner wave of a wave set.
#define DESERT_WATER_WAVE_ROW_FIELDS( X )                                                                         \
    /* xy = direction, z = wavelength (cm), w = amplitude (cm) */                                                 \
    X( glm::vec4, Wave, "Wave", glm::vec4( 0.0f ) )                                                               \
    /* x = steepness */                                                                                           \
    X( glm::vec4, Form, "Form", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( WaterWaveRow, "WaterWaveRow", Std430, DESERT_WATER_WAVE_ROW_FIELDS );
#undef DESERT_WATER_WAVE_ROW_FIELDS

    // WATER_MAX_WAVES / WATER_MAX_WAVE_SETS of the generated include: the most waves one set draws and the most
    // sets one frame draws (WaterRenderer refuses more, out loud).
    inline constexpr auto kWaterMaxWaves    = static_cast<uint32_t>( Water::kMaxDrawnWaves );
    inline constexpr auto kWaterMaxWaveSets = static_cast<uint32_t>( Water::kMaxWaveLanes );

    // One body wave set (UE: a water body's wave buffer).
    using WaterWaveRowArray = std::array<WaterWaveRow, kWaterMaxWaves>;
#define DESERT_WATER_WAVE_SET_FIELDS( X )                                                                         \
    /* x = Gerstner wave count, y = 1 if it IS the frame's FFT ocean, zw = WaveOrigin (X, Z) */                   \
    X( glm::vec4, Header, "Header", glm::vec4( 0.0f ) )                                                           \
    /* x = peak omega (rad/s), y = significant height (cm) the shore wave is made of */                           \
    X( glm::vec4, Swell, "Swell", glm::vec4( 0.0f ) )                                                             \
    X( WaterWaveRowArray, Rows, "Rows", {} )
    DESERT_SHADER_STRUCT( WaterWaveSet, "WaterWaveSet", Std430, DESERT_WATER_WAVE_SET_FIELDS );
#undef DESERT_WATER_WAVE_SET_FIELDS

    // The head of the WaterTiles block, member for member (its GLSL names are the block's).
    using WaterVec4x4       = std::array<glm::vec4, 4>;
    using WaterVec4x3       = std::array<glm::vec4, 3>;
    using WaterWaveSetArray = std::array<WaterWaveSet, kWaterMaxWaveSets>;
#define DESERT_WATER_WAVES_BLOCK_FIELDS( X )                                                                      \
    /* x = the set count, y = 1 when a set IS the FFT ocean, z = its choppiness */                                \
    X( glm::vec4, Header, "WaterWaveHeader", glm::vec4( 0.0f ) )                                                  \
    /* the shore field: xy = lower world (X, Z), z = 1 / texel (1/cm), w = the level its heights are over */      \
    X( glm::vec4, ShoreFrame, "WaterShoreFrame", glm::vec4( 0.0f ) )                                              \
    /* x = the open sea's floor (world Y, cm), y = the beach's slope, z = the skirt's slope */                    \
    X( glm::vec4, ShoreShape, "WaterShoreShape", glm::vec4( 0.0f ) )                                              \
    /* x = 1 / patch side, y = the band's significant height (cm), z = its peak omega (rad/s) */                  \
    X( WaterVec4x4, FFTCascade, "WaterFFTCascade", {} )                                                           \
    /* xy = cos, sin of the lattice's turn */                                                                     \
    X( WaterVec4x4, FFTTurn, "WaterFFTTurn", {} )                                                                 \
    /* x = 1 when a river is in the frame, zw = WaterRiverTexture's size in texels */                             \
    X( glm::vec4, Flow, "WaterFlowFrame", glm::vec4( 0.0f ) )                                                     \
    /* WaterRiverTexture: xy = lower world (X, Z), zw = 1 / texel along X and Z (1/cm) */                         \
    X( glm::vec4, River, "WaterRiverFrame", glm::vec4( 0.0f ) )                                                   \
    /* the foam's levels: xy = the window's lower texel on the level's grid, z = 1 / texel (1/cm) */              \
    X( WaterVec4x3, FoamOrigin, "WaterFoamOrigin", {} )                                                           \
    /* x = a level's side in texels, y = 1 when the map holds foam */                                             \
    X( glm::vec4, FoamShape, "WaterFoamShape", glm::vec4( 0.0f ) )                                                \
    X( WaterWaveSetArray, Sets, "WaterWaveSets", {} )
    DESERT_SHADER_STRUCT( WaterWavesBlock, "WaterWavesBlock", Std430, DESERT_WATER_WAVES_BLOCK_FIELDS );
#undef DESERT_WATER_WAVES_BLOCK_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kWaterMeshTilesStructs[] = {
         &kWaterTileRowStruct, &kWaterWaveRowStruct, &kWaterWaveSetStruct, &kWaterWavesBlockStruct };
    inline constexpr ShaderProtocols::ShaderConstantDecl kWaterMeshTilesConstants[] = {
         { .Kind  = ShaderProtocols::ShaderConstantDecl::Type::Int,
           .Name  = "WATER_MAX_WAVES",
           .Value = kWaterMaxWaves },
         { .Kind  = ShaderProtocols::ShaderConstantDecl::Type::Int,
           .Name  = "WATER_MAX_WAVE_SETS",
           .Value = kWaterMaxWaveSets },
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kWaterMeshTilesShaderInclude{
         .Path         = "Generated/Water/WaterMeshTiles.glslh",
         .SourceHeader = "Desert/Water/Source/Water/Public/WaterLayout.hpp",
         .Structs      = kWaterMeshTilesStructs,
         .Constants    = kWaterMeshTilesConstants,
    };

    // ---- THE CAUSTICS' FRAME ROW (Common/WaterCaustics.glslh WaterCausticsFrameUB, std430): the head, followed
    // by Shape.x WaterCausticsBodyRow (the block's runtime array WaterCausticsBodies). Read by every receiver with
    // the map (SceneViewInputs::WaterCausticsFrame) and by the splat.
#define DESERT_WATER_CAUSTICS_FRAME_HEAD_FIELDS( X )                                                              \
    /* xy = the map's lower world (X, Z), z = 1 / side (cm), w = 1 when there is a map */                         \
    X( glm::vec4, Window, "WaterCausticsWindow", glm::vec4( 0.0f ) )                                              \
    /* x = the body rows that follow */                                                                           \
    X( glm::vec4, Shape, "WaterCausticsShape", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( WaterCausticsFrameHead, "WaterCausticsFrameHead", Std430,
                          DESERT_WATER_CAUSTICS_FRAME_HEAD_FIELDS );
#undef DESERT_WATER_CAUSTICS_FRAME_HEAD_FIELDS

    // One body of the frame whose footprint meets the map's square.
#define DESERT_WATER_CAUSTICS_BODY_ROW_FIELDS( X )                                                                \
    /* xy = min (X, Z), zw = max (cm) */                                                                          \
    X( glm::vec4, Footprint, "Footprint", glm::vec4( 0.0f ) )                                                     \
    /* x = the still surface Y (cm), y = its wave lane (1 + the frame's wave set, 0 = flat; WaterWaveSetOf),      \
       z = its share of the beach slope (1 = an ocean, 0 = a river) */                                            \
    X( glm::vec4, Level, "Level", glm::vec4( 0.0f ) )                                                             \
    /* its material's (WaterSurface.shader Caustics*): x = the depth the map comes in over, y = the depth its     \
       contrast falls by 1/e over (cm), z = its strength (1 = the map as computed) */                             \
    X( glm::vec4, Look, "Look", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( WaterCausticsBodyRow, "WaterCausticsBody", Std430,
                          DESERT_WATER_CAUSTICS_BODY_ROW_FIELDS );
#undef DESERT_WATER_CAUSTICS_BODY_ROW_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kWaterCausticsStructs[] = {
         &kWaterCausticsFrameHeadStruct, &kWaterCausticsBodyRowStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kWaterCausticsShaderInclude{
         .Path         = "Generated/Water/WaterCaustics.glslh",
         .SourceHeader = "Desert/Water/Source/Water/Public/WaterLayout.hpp",
         .Structs      = kWaterCausticsStructs,
    };

    // ---- THE FOAM UPDATE'S ROWS (Programs/Water/WaterFoamUpdate.shader WaterFoamBodies / WaterFoamPlunges).

    // One body's foam: each texel of the map follows the body whose footprint holds it.
#define DESERT_WATER_FOAM_BODY_FIELDS( X )                                                                        \
    /* min X, min Z, max X, max Z */                                                                              \
    X( glm::vec4, Footprint, "Footprint", glm::vec4( 0.0f ) )                                                     \
    /* x = still Y, y = its wave set (-1: none), z = 1 + its river of the frame (0: none), w = its beach's slope  \
     */                                                                                                           \
    X( glm::vec4, Level, "Level", glm::vec4( 0.0f ) )                                                             \
    /* x = jacobian threshold, y = softness, z = whitecap gain */                                                 \
    X( glm::vec4, Source, "Source", glm::vec4( 0.0f ) )                                                           \
    /* x = foam, y = bubbles, z = wetness (s) */                                                                  \
    X( glm::vec4, Decay, "Decay", glm::vec4( 0.0f ) )                                                             \
    /* xy = the wind's surface drift (cm/s) */                                                                    \
    X( glm::vec4, Drift, "Drift", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( WaterFoamBody, "WaterFoamBody", Std430, DESERT_WATER_FOAM_BODY_FIELDS );
#undef DESERT_WATER_FOAM_BODY_FIELDS

    // One fall's plunge (Water::FallPlunge): the white boil the fall lands in and the outflow that spreads its
    // foam.
#define DESERT_WATER_FOAM_PLUNGE_FIELDS( X )                                                                      \
    /* xyz = Foot, w = Radius (cm) */                                                                             \
    X( glm::vec4, FootRadius, "FootRadius", glm::vec4( 0.0f ) )                                                   \
    /* xy = Across.xz, z = HalfWidth (cm), w = OutflowSpeed (cm/s) */                                             \
    X( glm::vec4, AcrossHalf, "AcrossHalf", glm::vec4( 0.0f ) )                                                   \
    /* xy = Downstream, z = LandingSpeed (cm/s), w = 0 */                                                         \
    X( glm::vec4, Landing, "Landing", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( WaterFoamPlunge, "WaterFoamPlunge", Std430, DESERT_WATER_FOAM_PLUNGE_FIELDS );
#undef DESERT_WATER_FOAM_PLUNGE_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kWaterFoamRowsStructs[] = {
         &kWaterFoamBodyStruct, &kWaterFoamPlungeStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kWaterFoamRowsShaderInclude{
         .Path         = "Generated/Water/WaterFoamRows.glslh",
         .SourceHeader = "Desert/Water/Source/Water/Public/WaterLayout.hpp",
         .Structs      = kWaterFoamRowsStructs,
    };
} // namespace Desert::Graphic::System
