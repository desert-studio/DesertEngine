#pragma once

#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/World/Landscape/LandscapeSculpt.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace Desert::World::Landscape
{
    /**
     * @file
     * @brief UE's "New Landscape" (Manage mode): a landscape of TilesX x TilesZ tiles, centred on a location,
     * filled flat or generated from a seed, and optionally run through the ported Erosion / Hydro Erosion tools
     *        over the whole map.
     *
     * THE UE HALF is the frame: ULandscapeEditorObject's NewLandscape_* defaults (8 x 8 components of 63 quads,
     * scale 100, location 0), the component-count clamp and the centring offset of
     * FLandscapeEditorDetailCustomization_NewLandscape::OnCreateButtonClicked; a flat fill is UE's mid value.
     *
     * THE SEED is ours: UE's New Landscape fills flat or imports, and its noise (FNoiseParameter) has no seed —
     * it is a function of the global sample coordinate. So a seed here SHIFTS that coordinate: the whole map is
     * sampled at (seed offset + global sample), and the erosion field is laid at the same shifted coordinates,
     * which makes the rain pattern of the hydraulic pass follow the seed too. Nothing else is random: the fill,
     * the thermal loop and the hydraulic loop are UE's position-seeded, fixed-order passes, so one seed and one
     * settings block give one heightmap, byte for byte (LandscapeGenerator suite).
     *
     * CPU and pure: no entities, no scene, no files. The editor turns the result into a root and tile entities.
     */

    /// UE: ULandscapeEditorObject::ClampLandscapeSize's upper bounds — at most 256 components a side, and the
    /// whole side at most 8191 quads.
    inline constexpr int32_t  kLandscapeMaxTilesPerSide = 256;
    inline constexpr uint32_t kLandscapeMaxQuadsPerSide = 8191u;
    /// UE: NewLandscape_ComponentCount(8, 8).
    inline constexpr int32_t kLandscapeDefaultTilesPerSide = 8;

    /// UE's ClampLandscapeSize: 1 .. min(256, floor(8191 / quads per component)).
    int32_t ClampLandscapeTileCount( int32_t count, uint32_t quadsPerTile );

    enum class LandscapeGenerateFill : uint8_t
    {
        /// UE's New Landscape without an import: every sample at the mid value (height 0).
        Flat,
        /// UE's Noise tool field (FNoiseParameter, four octaves) over the whole map at a seeded offset.
        Noise,
    };

    struct LandscapeGenerateSettings
    {
        /// UE: NewLandscape_Location — the CENTRE of the new landscape, centimetres.
        glm::vec3 LocationCm = glm::vec3( 0.0f );
        /// UE: NewLandscape_QuadsPerSection (one section a component here), one of kLandscapeTileQuadsValues.
        uint32_t QuadsPerTile = kLandscapeDefaultTileQuads;
        /// UE: NewLandscape_ComponentCount.
        int32_t TilesX = kLandscapeDefaultTilesPerSide;
        int32_t TilesZ = kLandscapeDefaultTilesPerSide;
        /// UE: NewLandscape_Scale X/Y (100 = 1 m between samples).
        float SpacingCm = kLandscapeDefaultSpacingCm;
        /// UE: NewLandscape_Scale Z.
        float ZScale = kLandscapeDefaultZScale;

        LandscapeGenerateFill Fill = LandscapeGenerateFill::Flat;
        /// Shifts the noise and rain coordinates; the same seed is the same map.
        uint32_t Seed = 1u;
        /// Noise amplitude: a noise value of 1 lifts a sample this many centimetres (the four-octave sum
        /// reaches about ±1.9). Samples past the 16-bit range are clamped to it.
        float NoiseHeightCm = 12800.0f;
        /// Samples per noise period, UE's NoiseScale range 1..512.
        float NoiseScale = 256.0f;

        /// UE's ToolStrength for the two erosion passes, 0..1 (UE's slider default 0.3).
        float ErosionStrength = 0.3f;
        /// Runs FLandscapeToolStrokeErosion over the whole map: the thermal loop, then its noise pass.
        bool                     Erosion = false;
        LandscapeErosionSettings ErosionSettings;
        /// Runs FLandscapeToolStrokeHydraErosion over the whole map, after the thermal pass.
        bool                          HydroErosion = false;
        LandscapeHydroErosionSettings HydroSettings;
    };

    /// Refuses what the generator cannot honour, naming the field and the number: a tile size UE does not
    /// offer, a tile count outside UE's clamp, a non-positive spacing / Z scale, a noise scale outside 1..512,
    /// a non-finite height, a strength outside 0..1, or erosion settings the tools refuse.
    Common::BoolResultStr ValidateLandscapeGenerate( const LandscapeGenerateSettings& settings );

    struct LandscapeGeneratedTile
    {
        int32_t           TileX = 0;
        int32_t           TileZ = 0;
        LandscapeTileData Heights;
    };

    struct LandscapeGenerated
    {
        /// The frame: Origin is sample (0, 0) of tile (0, 0), i.e. LocationCm less half the landscape.
        LandscapeRoot Root;
        /// Row-major over the tile grid (Z then X), tiles (0, 0) .. (TilesX - 1, TilesZ - 1). Neighbouring tiles
        /// carry the same values on their shared edge row, cut from one map.
        std::vector<LandscapeGeneratedTile> Tiles;
        /// Iterations the erosion passes actually ran (they stop early once nothing changes); 0 when off.
        int32_t ErosionIterations      = 0;
        int32_t HydroErosionIterations = 0;
    };

    /// The whole landscape as one map, before it is cut into tiles: the fill, then the erosion passes.
    struct LandscapeGeneratedMap
    {
        uint32_t SamplesX = 0u; ///< TilesX · QuadsPerTile + 1
        uint32_t SamplesZ = 0u; ///< TilesZ · QuadsPerTile + 1
        /// Row-major, X fastest.
        std::vector<uint16_t> Samples;
        /// Iterations the erosion passes actually ran (they stop early once nothing changes); 0 when off.
        int32_t ErosionIterations      = 0;
        int32_t HydroErosionIterations = 0;
    };

    Common::ResultStr<LandscapeGeneratedMap> GenerateLandscapeMap( const LandscapeGenerateSettings& settings );

    /// The frame and the tiles of GenerateLandscapeMap's map. Refuses what ValidateLandscapeGenerate refuses.
    Common::ResultStr<LandscapeGenerated> GenerateLandscape( const LandscapeGenerateSettings& settings );
} // namespace Desert::World::Landscape
