#pragma once

// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModeBrushes.cpp:355-455,1009-1135,
// adapted: FLandscapeBrushCircle and its four falloff subclasses become one enum and a free function (no
// virtuals, no UObject, no EdMode); ApplyBrush's sample loop works in the root's sample lattice from
// centimetre positions instead of reading UISettings and ULandscapeInfo; the per-tile view is added because
// our tiles are separate blobs where UE addresses one landscape-wide vertex grid.

#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/vec2.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Desert::World::Landscape
{
    /**
     * @file
     * @brief The circle brush's weight over landscape samples — UE's FLandscapeBrushCircle, maths only.
     *
     * WHERE THE WEIGHTS LIVE. UE addresses one landscape-wide vertex grid, and a vertex on a component edge
     * is ONE vertex. Here each tile is its own blob and the shared edge row is stored twice (LandscapeLayout.hpp,
     * LandscapeTileSamples). The brush is therefore evaluated ONCE, on the root's global sample lattice —
     * global index = tile · QuadsPerTile + local index — and a tile reads its weights from that lattice. A seam
     * sample is the same global index seen from both tiles, so the two copies cannot receive different weights:
     * the seam is correct by construction, not by a second evaluation that happens to agree.
     *
     * Alpha and Pattern brushes (FLandscapeBrushAlphaBase, :1140) are not here: they sample an alpha texture,
     * and the landscape has no brush-texture asset to sample.
     */

    /// UE's four circle falloffs: Circle_Linear, Circle_Smooth, Circle_Spherical, Circle_Tip.
    enum class LandscapeBrushFalloff : uint8_t
    {
        Linear,
        Smooth,
        Spherical,
        Tip,
    };

    /**
     * @brief UE's CalculateFalloff for @p shape: 1 inside @p radius, falling to 0 at @p radius + @p falloff.
     *
     * All three lengths are in the same unit (UE passes landscape quads). The comparisons are UE's to the
     * letter — Linear is 1 for Distance < Radius, Spherical and Tip for Distance <= Radius — so a sample
     * exactly on the inner circle weighs what it weighs in UE.
     */
    float LandscapeBrushFalloffWeight( LandscapeBrushFalloff shape, float distance, float radius, float falloff );

    /// What the brush UI hands over. The same four numbers UE reads from ULandscapeEditorObject.
    struct LandscapeBrushSettings
    {
        /// Outer radius in centimetres: the circle beyond which the weight is 0 (UE's BrushRadius).
        float RadiusCm = 2048.0f;
        /// Fraction of the radius that is falloff, 0..1 (UE's BrushFalloff): the inner, full-weight circle has
        /// radius (1 - FalloffFraction) · RadiusCm.
        float                 FalloffFraction = 0.5f;
        LandscapeBrushFalloff Shape           = LandscapeBrushFalloff::Smooth;
        /// Multiplies every weight. UE multiplies by ToolStrength in the tool; it is here so a tool reads one
        /// number per sample.
        float Strength = 1.0f;
    };

    /// Refuses settings the maths cannot honour: a radius that is not positive and finite, a falloff fraction
    /// outside [0, 1], a strength that is negative or not finite. Names the number.
    Common::BoolResultStr ValidateLandscapeBrush( const LandscapeBrushSettings& settings );

    /// UE caps one stroke step at ten interactor positions, keeping the first and last (CapInteractorPositions).
    inline constexpr size_t kLandscapeBrushMaxPositions = 10u;

    /**
     * @brief The brush's weights on the root's GLOBAL sample lattice, over the half-open rectangle
     *        [X0, X0 + Width) × [Z0, Z0 + Depth). Samples outside it weigh 0.
     *
     * Global indices are signed: a landscape extends either side of its root.
     */
    struct LandscapeBrushWeights
    {
        int32_t            X0    = 0;
        int32_t            Z0    = 0;
        uint32_t           Width = 0u;
        uint32_t           Depth = 0u;
        std::vector<float> Values;

        bool Empty() const
        {
            return Width == 0u || Depth == 0u;
        }

        /// The weight of global sample (@p gx, @p gz); 0 outside the rectangle.
        float At( int32_t gx, int32_t gz ) const;
    };

    /**
     * @brief UE's FLandscapeBrushCircle::ApplyBrush on the root's sample lattice.
     *
     * @p positionsCm are world X/Z positions of the brush centre (one per interactor position of this stroke
     * step). Each position stamps a circle; where circles overlap the larger weight wins, as in UE. The
     * rectangle is the union of UE's per-position bounds: floor(p - R) .. ceil(p + R) + 1, in samples.
     *
     * UE also clips to loaded components and refuses the whole step if any touched component is unloaded, to
     * keep shared edges equal. That needs the tile set, which this pure function does not know: the caller
     * checks LandscapeBrushTiles against the tiles it has.
     */
    Common::ResultStr<LandscapeBrushWeights> ComputeLandscapeBrush( const LandscapeRoot&          root,
                                                                    const LandscapeBrushSettings& settings,
                                                                    std::span<const glm::vec2>    positionsCm );

    /// One tile the brush reaches, and the LOCAL sample rectangle of it that the brush's rectangle covers.
    struct LandscapeBrushTile
    {
        int32_t       TileX = 0;
        int32_t       TileZ = 0;
        LandscapeRect Samples;
    };

    /// Every tile whose samples intersect @p weights' rectangle — including a tile that only shares an edge row
    /// with it, because that row is stored in both tiles and both copies must be written.
    std::vector<LandscapeBrushTile> LandscapeBrushTiles( const LandscapeRoot&         root,
                                                         const LandscapeBrushWeights& weights );

    /// The weight of local sample (@p x, @p z) of tile (@p tileX, @p tileZ).
    float LandscapeBrushTileWeight( const LandscapeRoot& root, const LandscapeBrushWeights& weights, int32_t tileX,
                                    int32_t tileZ, uint32_t x, uint32_t z );
} // namespace Desert::World::Landscape
