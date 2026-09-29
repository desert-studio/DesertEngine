#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Desert::World::Landscape
{
    class LandscapeTileData;
    struct LandscapeRect;
    struct LandscapeWeightLayer;
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
     * @brief One edit layer of a landscape (UE 5.8: ULandscapeEditLayerBase,
     * Classes/LandscapeEditLayer.h:345-360).
     *
     * WHO OWNS WHAT (the split UE spreads across ALandscape, the layer object and every component):
     *   * the STACK — which layers exist, their order, name, visibility, lock and alphas — is ONE value,
     *     owned by the landscape root (UE: ALandscape). A tile never knows the order;
     *   * a layer's DATA on one tile — its heights and weights there — lives on that tile
     *     (LandscapeTileData::EditLayers, keyed by Guid; UE: ULandscapeComponent::LayersData), and only on the
     *     tiles the layer touched;
     *   * the tile's final samples and weights are the MERGE of the two (MergeLandscapeEditLayers) — a derived
     *     value, never a second source: a tile that carries edit layers refuses every other write.
     *
     * Edit layers are authoring data: a cooked or runtime landscape reads only the merged result, as UE's does.
     */
    struct LandscapeEditLayer
    {
        Common::UUID Guid;            ///< The key a tile's LandscapeEditLayerTileData carries. Never null.
        std::string  Name;            ///< What the panel shows. Not a key: two layers may be renamed freely.
        bool         Visible = true;  ///< UE bVisible. An invisible layer contributes nothing (alpha 0).
        bool         Locked  = false; ///< UE bLocked. Brushes refuse a locked layer; the merge ignores it.
        /// UE HeightmapAlpha, -1..1: the layer's height deltas are scaled by it (negative inverts them).
        float HeightAlpha = 1.0f;
        /// UE WeightmapAlpha, 0..1: the layer's weights, and how much they cover the layers below, scale by it.
        float WeightAlpha = 1.0f;
    };

    /// The stack, bottom first: Layers[0] is merged first and every later layer goes over it.
    struct LandscapeEditLayerStack
    {
        std::vector<LandscapeEditLayer> Layers;

        [[nodiscard]] const LandscapeEditLayer* Find( const Common::UUID& guid ) const;
    };

    /// Refuses a null or repeated Guid, an empty name, and an alpha outside its range or not finite — naming it.
    Common::BoolResultStr ValidateLandscapeEditLayerStack( const LandscapeEditLayerStack& stack );

    /**
     * @brief Re-derives @p tile's samples and weights inside @p rect from its edit layers and @p stack — and
     * nowhere else: samples outside @p rect are not read and not written.
     *
     * HEIGHTS (UE: LandscapeEditLayersHeightmaps.usf MergeEditLayerPS, the ADDITIVE mode): every layer stores
     * its heights relative to kLandscapeMidSample, the canvas starts at kLandscapeMidSample, and each visible
     * layer bottom-up adds (height - mid) * HeightAlpha, clamped to the u16 range after every layer as UE clamps
     * every pass. The base layer at alpha 1 therefore IS the absolute height, and every layer above is a delta;
     * the sum does not depend on the order.
     *
     * WEIGHTS: a premultiplied-alpha "over" per sample (UE 5.5+'s premultiplied weight blending, without
     * blend groups): a layer with weight-blended weights w_k and alpha a covers c = a * sum(w_k) / 255 of what
     * lies below, so every weight-blended result becomes out_k * (1 - c) + w_k * a. The weight-blended sum
     * therefore never rises above 255 and is never scaled UP, the rule LandscapeNormalizeWeights keeps for a
     * brush — and the order matters: the upper layer's paint wins. A NoWeightBlend layer (and the visibility
     * layer) adds, clamped to 255, as UE's additive mode does.
     *
     * Refuses an invalid stack or rectangle, a tile layer whose Guid the stack does not name (a layer removed
     * from the stack is removed from its tiles first), a weight layer @p rules do not name, and a result that
     * would need a ninth weight layer on the tile — in every refusal the tile is unchanged.
     */
    Common::BoolResultStr MergeLandscapeEditLayers( const LandscapeEditLayerStack&      stack,
                                                    std::span<const LandscapeLayerRule> rules,
                                                    const LandscapeRect& rect, LandscapeTileData& tile );

    /**
     * @brief What a brush edits (UE: ALandscape::GetEditingLayer): the root's stack, the paint rules the merge
     * reads, and the Guid of the layer the brush writes. A brush writes that layer's data and re-merges the
     * rectangle it wrote; the tile's samples and weights are never written by a brush directly.
     */
    struct LandscapeEditLayerTarget
    {
        LandscapeEditLayerStack         Stack;
        std::vector<LandscapeLayerRule> Rules;
        Common::UUID                    Layer;
    };

    /// Refuses an invalid stack, a layer the stack does not name and a Locked layer (UE: a locked layer
    /// cannot be edited) — naming it.
    Common::BoolResultStr CheckLandscapeEditLayerTarget( const LandscapeEditLayerTarget& target );

    /// Layer @p layer's height at (x, z) of @p tile, relative to kLandscapeMidSample: its plane there, or mid
    /// ("no change") where the layer holds no heights on the tile.
    [[nodiscard]] uint16_t LandscapeEditLayerHeight( const LandscapeTileData& tile, const Common::UUID& layer,
                                                     uint32_t x, uint32_t z );

    /**
     * @brief Writes @p values (row-major, X fastest, @p rect.Area() of them) into the target layer's height
     * plane on @p tile — creating the plane at mid where the layer had none — and merges @p rect.
     *
     * Refuses, leaving the tile unchanged: a target CheckLandscapeEditLayerTarget refuses, a tile carrying no
     * edit layers (its samples are not a merge), a rectangle outside the tile, a value count that does not
     * match, and anything the merge refuses.
     */
    Common::BoolResultStr WriteLandscapeEditLayerHeights( const LandscapeEditLayerTarget& target,
                                                          const LandscapeRect&            rect,
                                                          std::span<const uint16_t>       values,
                                                          LandscapeTileData&              tile );

    /// Replaces the target layer's weight planes on @p tile with @p weights and merges @p rect. The same
    /// refusals as WriteLandscapeEditLayerHeights, plus every plane SetEditLayer refuses.
    Common::BoolResultStr WriteLandscapeEditLayerWeights( const LandscapeEditLayerTarget&   target,
                                                          std::vector<LandscapeWeightLayer> weights,
                                                          const LandscapeRect& rect, LandscapeTileData& tile );
} // namespace Desert::World::Landscape
