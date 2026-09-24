// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModePaintTools.cpp:258-445
// (FLandscapeToolStrokePaint::Apply) and Engine/Source/Runtime/Landscape/Classes/LandscapeLayerInfoObject.h:73-82,
// adapted: one weight plane per layer per tile instead of a component's RGBA weightmap channels, no edit layers,
// no allow-list mode, pressure 1, normalisation on the CPU after every write (see LandscapeNormalizeWeights).
#pragma once

#include <Engine/World/Landscape/LandscapeBrush.hpp>
#include <Engine/World/Landscape/LandscapeEditCache.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Desert::World::Landscape
{
    /**
     * @brief UE's ULandscapeLayerInfoObject reduced to what painting and blending read: the layer's name (the
     * key a tile's LandscapeWeightLayer carries), its Hardness and bNoWeightBlend.
     *
     * UE 5.7 deprecated bNoWeightBlend for BlendMethod (None = not weight-blended); the one bit is what the
     * card asks for and what both enums reduce to for normalisation, so the bit is kept.
     */
    struct LandscapeLayerRule
    {
        std::string Name;
        /// UE: "how much a layer resists being painted over", 0..1, default 0.5. Here: when a painted layer
        /// takes weight from the others, softer layers give first, in proportion to weight · (1 - Hardness);
        /// only what they cannot give is then taken from the harder ones, in proportion to what they have left.
        float Hardness = 0.5f;
        /// UE: bNoWeightBlend — the layer is neither normalised nor counted in the others' sum.
        bool NoWeightBlend = false;
    };

    /**
     * @brief Sets layer @p painted to @p value at one sample and renormalises the other weight-blended layers
     * so the weight-blended sum is 255 — UE's weight-adjust (legacy FLandscapeEditDataInterface::SetAlphaData
     * bWeightAdjust; in 5.8 the same invariant is enforced by the edit-layer merge on the GPU).
     *
     * @p weights holds every layer's weight at this sample, indexed as @p rules. A NoWeightBlend layer is set
     * and nothing else moves. When the painted layer LOSES weight and no other weight-blended layer holds any,
     * the weight has nowhere to go and the painted layer keeps its old value: the sum stays 255. Rounding
     * residue goes to the heaviest other layer, so the sum is exact, not approximately 255.
     */
    void LandscapeNormalizeWeights( std::span<uint8_t> weights, std::span<const LandscapeLayerRule> rules,
                                    size_t painted, uint8_t value );

    /// UE's paint-tool settings that the stroke reads (ULandscapeEditorObject).
    struct LandscapePaintSettings
    {
        /// The target layer (UE: the selected target layer's LayerInfo).
        std::string Layer;
        /// UE: bUseWeightTargetValue / WeightTargetValue — lerp towards a target weight instead of adding.
        bool  UseTargetValue = false;
        float TargetValue    = 1.0f;
        /// UE: bDisablePaintingStartupSlowdown.
        bool DisableStartupSlowdown = false;
    };

    /// One tile's weight layers before and after a stroke — the stroke's undo record for that tile.
    struct LandscapePaintTileRecord
    {
        int32_t                           TileX = 0;
        int32_t                           TileZ = 0;
        std::vector<LandscapeWeightLayer> Before;
        std::vector<LandscapeWeightLayer> After;
    };

    struct LandscapePaintRecord
    {
        std::vector<LandscapePaintTileRecord> Tiles;
    };

    /**
     * @brief One press of the Paint tool (UE's FLandscapeToolStrokePaint): applied once per frame while held,
     * one undo transaction for the whole press.
     *
     * The stroke snapshots each tile's layers the first time it touches the tile. That snapshot is UE's
     * OriginalData (the value every step adds to, so holding the brush still does not keep stacking) and the
     * undo record's Before. Neighbouring tiles share their edge samples; each copy is computed from the same
     * snapshot and brush weight, so both stay equal without a cross-tile pass.
     */
    class LandscapePaintStroke
    {
    public:
        LandscapePaintStroke( const LandscapeRoot& root, LandscapeTileLookup lookup,
                              std::vector<LandscapeLayerRule> rules );

        /// FLandscapeToolStrokePaint::Apply. @p invert is UE's bInvert (Shift): erase instead of paint.
        /// Refuses a target layer the rules do not name, a tile layer the rules do not name, and a tile that
        /// would need a fifth layer — naming each. A touched tile that is not loaded is skipped, as UE's cache
        /// skips unloaded components.
        Common::BoolResultStr Apply( const LandscapeBrushWeights& weights, const LandscapeBrushSettings& brush,
                                     const LandscapePaintSettings& paint, bool invert );

        bool Touched() const
        {
            return !m_Tiles.empty();
        }

        /// The undo record for every tile the stroke touched. Refuses a stroke that touched nothing.
        Common::ResultStr<LandscapePaintRecord> Finish() const;

    private:
        struct TileState
        {
            int32_t                           TileX = 0;
            int32_t                           TileZ = 0;
            std::vector<LandscapeWeightLayer> Original;
            std::vector<float>                Influence; ///< UE's TotalInfluenceMap, per sample of the tile.
        };

        TileState&                StateFor( int32_t tileX, int32_t tileZ, const LandscapeTileData& tile );
        const LandscapeLayerRule* Rule( std::string_view name ) const;

        LandscapeRoot                   m_Root;
        LandscapeTileLookup             m_Lookup;
        std::vector<LandscapeLayerRule> m_Rules;
        std::vector<TileState>          m_Tiles;
    };

    /// Writes the Before (undo) or After (redo) layers of @p record back into its tiles. A tile that is not
    /// loaded is refused by name: silently skipping it would leave half a stroke undone.
    Common::BoolResultStr ApplyLandscapePaintRecord( LandscapeTileLookup         lookup,
                                                     const LandscapePaintRecord& record, bool before );
} // namespace Desert::World::Landscape
