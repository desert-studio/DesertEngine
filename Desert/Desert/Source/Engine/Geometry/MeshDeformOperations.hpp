#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>

namespace Desert::Geometry
{
    // DEFORM OPERATIONS - UE's Deform palette of Modeling Mode, the operators behind USmoothMeshTool
    // (Smoothing Type = Iterative, FIterativeSmoothingOp) and UOffsetMeshTool (Offset Type = Iterative,
    // FIterativeOffsetMeshOp).
    //
    // Same contract as the other mesh operation headers (DynamicMeshUVOperations.hpp): pure functions, the
    // input is not touched, the result is a NEW mesh with the same topology and moved vertex positions; its
    // normals (overlay or per-vertex) are recomputed from the new positions and its tangents rebuilt, as UE
    // treats both as derived data. UVs, colours and polygroups are carried unchanged. A refusal names what and
    // why. Units are centimetres.

    struct DeformOutcome
    {
        DynamicMesh3 Mesh;
        int32_t      MovedVertices  = 0; // vertices the operation was allowed to move
        int32_t      PinnedVertices = 0; // vertices held in place (boundary / UV seams)
        std::string  Report;             // what the tool did, for the log
    };

    // The one-ring centroid a smoothing step moves a vertex towards (UE FMeshWeights).
    enum class SmoothWeightType : uint8_t
    {
        Uniform, // UE FMeshWeights::UniformCentroid: plain average of the one-ring neighbours
        Cotan,   // UE FMeshWeights::CotanCentroidSafe: cotangent weights, uniform where a face is degenerate
    };
    [[nodiscard]] const char* ToString( SmoothWeightType type );

    // ---- Smooth (UE USmoothMeshTool, Smoothing Type = Iterative; UIterativeSmoothProperties) ----
    // Steps rounds of p <- lerp( p, centroid( one-ring ), SmoothingPerStep ), all vertices from the previous
    // round's positions (Jacobi, as UE's double buffer).
    struct SmoothSettings
    {
        SmoothWeightType WeightType       = SmoothWeightType::Uniform;
        float            SmoothingPerStep = 0.8f;  // UE SmoothingPerStep, 0..1 (the step's speed)
        int32_t          Steps            = 10;    // UE Steps
        bool             SmoothBoundary   = true;  // UE bSmoothBoundary: off pins open-boundary vertices
        bool             PinUVSeams       = false; // UV-seam vertices held, so UV islands keep their outline

        bool operator==( const SmoothSettings& ) const = default;
    };
    [[nodiscard]] Common::ResultStr<DeformOutcome> Smooth( const DynamicMesh3&   mesh,
                                                           const SmoothSettings& settings );

    // ---- Offset (UE UOffsetMeshTool, Offset Type = Iterative; UIterativeOffsetProperties) ----
    // Steps rounds of: every movable vertex moves Distance / Steps along its current vertex normal (area- and
    // angle-weighted, recomputed each round), then one smoothing pass of SmoothingPerStep with WeightType
    // keeps the shell from folding on concave regions. A negative Distance moves inwards.
    struct OffsetSettings
    {
        float            Distance         = 10.0f; // cm, UE OffsetDistance
        int32_t          Steps            = 10;    // UE Steps
        float            SmoothingPerStep = 0.2f;  // UE SmoothingPerStep, 0..1; 0 = pure offset
        SmoothWeightType WeightType       = SmoothWeightType::Uniform;
        bool             OffsetBoundary   = true; // UE bOffsetBoundaries: off pins open-boundary vertices
        bool             PinUVSeams       = false;

        bool operator==( const OffsetSettings& ) const = default;
    };
    [[nodiscard]] Common::ResultStr<DeformOutcome> Offset( const DynamicMesh3&   mesh,
                                                           const OffsetSettings& settings );
} // namespace Desert::Geometry
