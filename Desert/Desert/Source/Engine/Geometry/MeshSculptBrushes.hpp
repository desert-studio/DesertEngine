#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"

#include <Common/Core/ResultStr.hpp>

#include <glm/vec3.hpp>

#include <cstdint>

namespace Desert::Geometry
{
    // VERTEX SCULPT BRUSHES - the brush operators of UE's UMeshVertexSculptTool (Modeling Mode -> Deform ->
    // VSculpt): one stamp moves the vertices inside the brush sphere IN PLACE on a mutable sculpt mesh (the tool
    // owns that copy for the length of a stroke and commits it once at the stroke's end). Positions are object
    // space of the sculpted mesh, in centimetres.

    // UE EMeshSculptBrushType, the two ported so far.
    enum class SculptBrush : uint8_t
    {
        Move,   // UE FMoveBrushOp: the vertices follow the brush, weighted by the falloff (Strength ignored)
        Smooth, // UE FSmoothBrushOp: each vertex pulled towards its one-ring centroid by Strength * falloff
    };
    [[nodiscard]] const char* ToString( SculptBrush brush );

    // UE EMeshSculptFalloffType (the spherical ones; MeshSculptFalloffs.cpp). t runs 0 at the inner edge of the
    // falloff band to 1 at the radius; inside the band's inner edge the weight is 1.
    enum class SculptFalloff : uint8_t
    {
        Smooth,  // 1 - smoothstep(t)
        Linear,  // 1 - t
        Inverse, // (1 - t)^2
        Round,   // sqrt(1 - t^2)
    };
    [[nodiscard]] const char* ToString( SculptFalloff falloff );

    // UE USculptBrushProperties: BrushSize (here the radius in cm, UE's world-space BrushRadius), BrushStrength
    // (UE StrengthScale) and BrushFalloffAmount - the outer fraction of the radius over which the weight falls
    // to zero (0 = a hard-edged sphere, 1 = falls off from the centre).
    struct SculptSettings
    {
        SculptBrush   Brush         = SculptBrush::Move;
        SculptFalloff FalloffType   = SculptFalloff::Smooth;
        float         Radius        = 25.0f; // cm
        float         Strength      = 0.5f;  // 0..1
        float         FalloffAmount = 0.5f;  // 0..1, UE BrushFalloffAmount default

        bool operator==( const SculptSettings& ) const = default;
    };
    inline constexpr float kSculptMinRadius = 0.1f;
    inline constexpr float kSculptMaxRadius = 10000.0f;

    // The brush weight of a vertex `distance` cm from the brush centre (0 beyond the radius).
    [[nodiscard]] double SculptFalloffWeight( const SculptSettings& settings, double distance );

    // One brush stamp (UE FSculptBrushStamp): the brush centre now and at the previous stamp of the stroke.
    struct SculptStamp
    {
        glm::dvec3 Center{ 0.0 };
        glm::dvec3 PrevCenter{ 0.0 };
    };

    // Applies one stamp to `mesh` in place (positions, then overlay / vertex normals from them). Move weighs
    // each vertex by its distance to PrevCenter - the surface that was under the brush - and carries it by
    // Center - PrevCenter; Smooth weighs by the distance to Center, reading the one-ring centroids from the
    // positions before the stamp (Jacobi, as UE's op reads its ROI snapshot). Returns the vertices moved.
    // Refused for a non-positive radius or a strength outside 0..1.
    [[nodiscard]] Common::ResultStr<int32_t> ApplySculptStamp( DynamicMesh3& mesh, const SculptSettings& settings,
                                                               const SculptStamp& stamp );

    // The stroke ends: the tangent frames are rebuilt from the final positions (derived data, as the Deform
    // operators treat it; once per stroke instead of per stamp).
    [[nodiscard]] Common::BoolResultStr FinishSculptStroke( DynamicMesh3& mesh );
} // namespace Desert::Geometry
