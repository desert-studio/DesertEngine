#pragma once

#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeEditLayers.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Desert::World::Landscape
{
    /**
     * @brief One control point of a landscape spline (UE 5.8: ULandscapeSplineControlPoint,
     * Classes/LandscapeSplineControlPoint.h:50-109), world centimetres.
     *
     * UE's semantics, one for one: Width is the HALF-width of the flat band at this point, SideFalloff the
     * distance beyond it over which the deformation fades out, EndFalloff the fade past a point that ends the
     * spline (a point with one segment). Rotation is not stored: UE's AutoCalcRotation derives it from the
     * connected segments, and so does LandscapeSplineSegmentPoints — the only rotation UE shows by default.
     */
    struct LandscapeSplineControlPoint
    {
        glm::vec3 Location    = glm::vec3( 0.0f );
        float     Width       = 1000.0f; ///< UE default: 1000 cm half-width.
        float     SideFalloff = 1000.0f; ///< UE default.
        float     EndFalloff  = 2000.0f; ///< UE default.
        /// UE LayerWidthRatio: the painted band's half-width is Width * this, with the same SideFalloff past it
        /// (LandscapeSplineSegment.cpp UpdateSplinePoints: LayerLeft/LayerRight). Goes linearly along a segment.
        float LayerWidthRatio = 1.0f;
    };

    /**
     * @brief One segment between two control points (UE 5.8: ULandscapeSplineSegment,
     * Classes/LandscapeSplineSegment.h:194-216). Start/End index the owner's ControlPoints.
     */
    struct LandscapeSplineSegment
    {
        uint32_t Start        = 0u;
        uint32_t End          = 0u;
        bool     RaiseTerrain = true; ///< UE bRaiseTerrain: the band may lift the ground up to the spline.
        bool     LowerTerrain = true; ///< UE bLowerTerrain: the band may cut the ground down to the spline.
        /// UE LayerName: the landscape paint layer the segment writes along its band (weight = the band's alpha
        /// over Width * LayerWidthRatio + SideFalloff) into the Splines edit layer; empty paints nothing.
        std::string LayerName;
    };

    /**
     * @brief A landscape's splines (UE: ULandscapeSplinesComponent on the landscape root, one set per landscape).
     * Authoring data: the heights they make live in the stack's Splines edit layer on the tiles
     * (RasterizeLandscapeSplines), and a cooked landscape reads only the merged heights.
     */
    struct LandscapeSplines
    {
        std::vector<LandscapeSplineControlPoint> ControlPoints;
        std::vector<LandscapeSplineSegment>      Segments;

        [[nodiscard]] bool Empty() const
        {
            return Segments.empty() && ControlPoints.empty();
        }
        friend bool operator==( const LandscapeSplines&, const LandscapeSplines& ) = default;
    };

    /// Refuses a non-finite location, a negative or non-finite width / falloff, a segment naming a point that
    /// does not exist or joining a point to itself — naming it.
    Common::BoolResultStr ValidateLandscapeSplines( const LandscapeSplines& splines );

    /// UE's FLandscapeSplineInterpPoint (LandscapeSplineSegment.h:22): the centre of the band, its left and
    /// right edges (Width), and the edges of the falloff (Width + SideFalloff), world cm.
    struct LandscapeSplineInterpPoint
    {
        glm::vec3 Center       = glm::vec3( 0.0f );
        glm::vec3 Left         = glm::vec3( 0.0f );
        glm::vec3 Right        = glm::vec3( 0.0f );
        glm::vec3 FalloffLeft  = glm::vec3( 0.0f );
        glm::vec3 FalloffRight = glm::vec3( 0.0f );
        float     Width        = 0.0f;
        float     SideFalloff  = 0.0f;
        float     LayerWidth   = 0.0f; ///< UE LayerLeft/LayerRight: Width * LayerWidthRatio.
    };

    /**
     * @brief Segment @p segment's curve sampled at most @p stepCm apart (UE:
     * ULandscapeSplineSegment::UpdateSplinePoints).
     *
     * The curve is UE's: a cubic Hermite between the two points, each end's tangent along the point's
     * auto-calculated rotation (the direction through its connected segments) with the length of the segment's
     * chord (UE's default TangentLen). Width and SideFalloff go linearly from the start point's to the end's.
     */
    std::vector<LandscapeSplineInterpPoint> LandscapeSplineSegmentPoints( const LandscapeSplines& splines,
                                                                          size_t segment, float stepCm );

    /**
     * @brief Re-derives procedural layer @p layer's heights on @p tile from @p splines, and merges the tile.
     *
     * UE: ULandscapeEditLayerSplines + LandscapeSplineRaster.cpp RasterizeSegmentPoints. For every sample the
     * band nearest to it (largest alpha) wins: alpha is 1 within Width of the curve, fades with UE's smooth
     * step over SideFalloff (and over EndFalloff past a spline end), and the ground BELOW the layer — the
     * stack's layers under @p layer merged as MergeLandscapeEditLayers merges them — moves toward the curve's
     * height by alpha, up only when the segment Raises and down only when it Lowers. The layer stores the
     * difference, so removing every spline leaves mid ("no change") and the tile returns byte for byte.
     *
     * PAINT (UE: the weightmap pass of RasterizeSegmentPoints): a segment with a LayerName writes that layer's
     * weight into the Splines layer, 255 * alpha of the same smooth step measured from Width * LayerWidthRatio;
     * where several segments paint one layer the largest weight wins. The merge then lays the Splines layer's
     * paint OVER the paint below (premultiplied "over"), so the road shows its layer with soft edges.
     *
     * Refuses an invalid stack or splines, a @p layer the stack does not name as its Splines layer, and what
     * the merge refuses (a LayerName the landscape has no paint layer for, naming it); the tile is then
     * unchanged.
     */
    Common::BoolResultStr RasterizeLandscapeSplines( const LandscapeSplines&             splines,
                                                     const LandscapeEditLayerStack&      stack,
                                                     std::span<const LandscapeLayerRule> rules,
                                                     const Common::UUID& layer, const LandscapeFrame& frame,
                                                     LandscapeTileData& tile );
} // namespace Desert::World::Landscape
