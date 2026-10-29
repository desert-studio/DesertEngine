#pragma once

#include "Engine/Geometry/MeshCore/BoxTypes.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"

#include <cstdint>
#include <functional>

// Marching Cubes over a scalar field, ported from UE 5.8 GeometryCore Generators/MarchingCubes.h (FMarchingCubes)
// and Private/Generators/MarchingCubes.cpp (its EdgeIndices / EdgeTable / TriTable, copied verbatim), adapted:
// UE Core as std/glm, ParallelFor as the engine JobSystem. Cell corner layout, the "f < IsoValue sets the corner
// bit" convention, the triangle order out of TriTable and find_iso (pull an on-corner root 1e-6 inside the edge so
// the result stays manifold; Bisection / Lerp root modes) are UE's.
//
// Difference from UE: only the full-grid sweep (UE Generate) is here, run in three parallel passes - corner
// values, edge roots, cell triangles - with the edge vertices keyed by (lower corner, axis) so neighbouring cells
// share them (UE's edge hash). UE's GenerateContinuation (surface-following from seeds) finds a subset of the
// same surface; the full sweep finds every component, which VoxWrap wants anyway.
namespace Desert::Geometry
{
    enum class MarchingCubesRootMode : uint8_t
    {
        SingleLerp, // UE ERootfindingModes::SingleLerp
        LerpSteps,  // UE ERootfindingModes::LerpSteps
        Bisection,  // UE ERootfindingModes::Bisection
    };

    struct MarchingCubes
    {
        AxisAlignedBox3d Bounds;            // the region swept; cells cover it from Bounds.Min
        double           CubeSize = 0.1;    // cell edge length, world units
        double           IsoValue = 0.0;    // the surface is Implicit == IsoValue; below it is outside
        MarchingCubesRootMode RootMode      = MarchingCubesRootMode::SingleLerp;
        int32_t               RootModeSteps = 5;
        // Must be safe to call from several threads at once (the sweep is parallel).
        std::function<double( const glm::dvec3& )> Implicit;
        // Polled between slabs; true abandons the sweep (UE CancelF).
        std::function<bool()> Cancel;

        // The triangles Generate could not add (non-manifold at a coincident vertex; UE ensures they never occur).
        int32_t RejectedTriangles = 0;
        bool    Cancelled         = false;

        // UE FMarchingCubes::Generate: sweeps every cell of Bounds and returns the zero-iso surface.
        [[nodiscard]] DynamicMesh3 Generate();

        static const uint8_t  EdgeIndices[12][2];
        static const uint16_t EdgeTable[256];
        static const uint64_t TriTable[256];
    };
} // namespace Desert::Geometry
