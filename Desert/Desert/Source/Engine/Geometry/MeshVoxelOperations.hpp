#pragma once

#include "Engine/Geometry/MeshDeformOperations.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstdint>

// Modeling Mode -> VoxOps: UE 5.8 VoxWrap (UVoxelSolidifyMeshesTool, operator FVoxelSolidifyMeshesOp in
// ModelingOperators/Private/CompositionOps/VoxelSolidifyMeshesOp.cpp, implicit TImplicitSolidify in
// GeometryCore/Public/Implicit/Solidify.h): the fast winding number of the input sampled on a grid, its
// WindingThreshold iso-surface meshed by Marching Cubes (MeshCore/Implicit/MarchingCubes.hpp) - a closed,
// manifold mesh whatever the input's holes and self-intersections. Same contract as MeshDeformOperations.hpp
// (pure, input untouched, result is a NEW mesh in the input's object space) so MeshProcessingTool previews it.
namespace Desert::Geometry
{
    // UE UVoxelProperties + UVoxelSolidifyMeshesToolProperties (defaults as UE's).
    struct VoxWrapSettings
    {
        int32_t VoxelCount         = 128;   // UE VoxelCount: cells along the longest side of the bounds
        float   WindingThreshold   = 0.5f;  // UE WindingThreshold, -10..10
        float   ExtendBounds       = 1.0f;  // UE ExtendBounds, cm beyond the input bounds the grid reaches
        int32_t SurfaceSearchSteps = 5;     // UE SurfaceSearchSteps: bisection steps per surface vertex, 0..10
        bool    SolidAtBoundaries  = true;  // UE bSolidAtBoundaries: the grid's border closes the surface
        bool    ThickenShells      = false; // UE bApplyThickenShells: open shells get a slab before solidify
        float   ShellThickness     = 5.0f;  // UE ThickenShells, cm (at least two cells, as UE)

        bool operator==( const VoxWrapSettings& ) const = default;
    };

    // Refuses an empty mesh, VoxelCount outside 8..1024, SurfaceSearchSteps outside 0..10, a negative thickness.
    [[nodiscard]] Common::ResultStr<DeformOutcome> VoxWrap( const DynamicMesh3& mesh, const VoxWrapSettings& settings );
} // namespace Desert::Geometry
