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

    // UE ESmoothMeshToolSmoothType: which solver USmoothMeshTool runs.
    enum class SmoothType : uint8_t
    {
        Iterative, // explicit: Steps rounds of p <- lerp( p, centroid( one-ring ), SmoothingPerStep ) (Jacobi)
        Implicit,  // one sparse solve: argmin |L x|^2 + (1/s) |x - (p + VolumeCorrection n)|^2,
                   // s = Smoothness^2 * VertexCount / 50, n = the input's vertex normal
        Diffusion, // implicit heat flow: Steps backward-Euler solves ( I + SmoothingPerStep * L ) x' = x
    };
    [[nodiscard]] const char* ToString( SmoothType type );

    // ---- Smooth (UE USmoothMeshTool; UIterativeSmoothProperties / UImplicitSmoothProperties /
    // UDiffusionSmoothProperties). L is the mesh Laplacian of WeightType (Cotan = UE's ClampedCotangent:
    // symmetric, negative cotangents clamped to zero), so the implicit systems are symmetric positive definite and
    // factor once with SparseLDLT for all three coordinates. Pinned vertices are held by the solve.
    struct SmoothSettings
    {
        SmoothType       Type             = SmoothType::Iterative;
        SmoothWeightType WeightType       = SmoothWeightType::Uniform;
        float            SmoothingPerStep = 0.8f;  // Iterative / Diffusion: UE SmoothingPerStep, 0..1
        int32_t          Steps            = 10;    // Iterative / Diffusion: UE Steps
        float            Smoothness       = 0.2f;  // Implicit: UE Smoothness, 0..1, non-linear (squared)
        float            VolumeCorrection = 0.0f;  // Implicit: UE VolumeCorrection, cm along the input normal
                                                   // that each fairing target is pushed out, against shrink;
                                                   // 0..10 (UE ClampMax; UE's SmoothSpeed is a hidden
                                                   // property its CotanSmoothingOp never reads - not ported)
        bool             SmoothBoundary   = true;  // UE bSmoothBoundary: off pins open-boundary vertices
        bool             PinUVSeams       = false; // UV-seam vertices held, so UV islands keep their outline

        bool operator==( const SmoothSettings& ) const = default;
    };
    [[nodiscard]] Common::ResultStr<DeformOutcome> Smooth( const DynamicMesh3&   mesh,
                                                           const SmoothSettings& settings );

    // UE EOffsetMeshToolOffsetType: which operator UOffsetMeshTool runs.
    enum class OffsetType : uint8_t
    {
        Iterative, // UE FIterativeOffsetMeshOp: Steps rounds of normal move + smoothing
        Implicit,  // UE FLaplacianOffsetMeshOp: one sparse solve keeping the Laplacian (detail) of the input,
                   // argmin |L x - L p|^2 + w |x - (p + Distance n)|^2, w = max( Smoothness^2 * VertexCount / 50,
                   // 0.001 ) / 100; open-boundary vertices held (UE bFixedBoundary)
    };
    [[nodiscard]] const char* ToString( OffsetType type );

    // ---- Offset (UE UOffsetMeshTool; UIterativeOffsetProperties / UImplicitOffsetProperties) ----
    // Iterative: Steps rounds of: every movable vertex moves Distance / Steps along its current vertex normal (area- and
    // angle-weighted, recomputed each round), then one smoothing pass of SmoothingPerStep with WeightType
    // keeps the shell from folding on concave regions. A negative Distance moves inwards.
    struct OffsetSettings
    {
        OffsetType       Type             = OffsetType::Iterative;
        float            Distance         = 10.0f; // cm, UE OffsetDistance
        int32_t          Steps            = 10;    // UE Steps
        float            SmoothingPerStep = 0.2f;  // Iterative: UE SmoothingPerStep, 0..1; 0 = pure offset
        float            Smoothness       = 0.2f;  // Implicit: UE Smoothness, 0..100 (UI 0..1), squared
        SmoothWeightType WeightType       = SmoothWeightType::Uniform;
        bool             OffsetBoundary   = true; // Iterative: UE bOffsetBoundaries: off pins open boundary
        bool             PinUVSeams       = false;

        bool operator==( const OffsetSettings& ) const = default;
    };
    [[nodiscard]] Common::ResultStr<DeformOutcome> Offset( const DynamicMesh3&   mesh,
                                                           const OffsetSettings& settings );
} // namespace Desert::Geometry
