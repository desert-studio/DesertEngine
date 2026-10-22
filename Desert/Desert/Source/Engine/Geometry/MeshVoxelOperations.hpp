#pragma once

#include "Engine/Geometry/MeshDeformOperations.hpp"
#include "Engine/Geometry/ProgressCancel.hpp"

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
    // progress (UE FDynamicMeshOperator::CalculateResult's FProgressCancel): polled between the build steps and
    // between Marching Cubes slabs; once raised, VoxWrap stops and refuses with kVoxWrapCancelled.
    inline constexpr const char* kVoxWrapCancelled = "VoxWrap was cancelled";
    [[nodiscard]] Common::ResultStr<DeformOutcome> VoxWrap( const DynamicMesh3& mesh, const VoxWrapSettings& settings,
                                                            const ProgressCancel* progress = nullptr );

    // Modeling Mode -> VoxOps: UE 5.8 VoxBoolean (UVoxelCSGMeshesTool, operator FVoxelBooleanMeshesOp in
    // ModelingOperators/Private/CompositionOps/VoxelBooleanMeshesOp.cpp): each input becomes a signed distance
    // field on one grid over both (|distance| to the nearest triangle, negative where its fast winding number
    // says inside, so open or self-intersecting inputs still have a side), the two fields combine as a CSG of
    // implicits (UE TImplicitBoolean: Union = min, Intersect = max, Difference = max( A, -B )) and Marching
    // Cubes meshes the zero level set. Inputs are in ONE space (the caller bakes B into A's object space);
    // the result is a NEW closed mesh in that space.
    enum class VoxBooleanOperation : uint8_t
    {
        DifferenceAB, // UE EVoxelCSGOperation::DifferenceAB: A minus B
        DifferenceBA, // B minus A
        Intersect,
        Union,
    };

    [[nodiscard]] const char* ToString( VoxBooleanOperation operation );

    // UE UVoxelCSGMeshesToolProperties + UVoxelProperties (defaults as UE's).
    struct VoxBooleanSettings
    {
        VoxBooleanOperation Operation          = VoxBooleanOperation::DifferenceAB;
        int32_t             VoxelCount         = 128; // cells along the longest side of both inputs' bounds
        int32_t             SurfaceSearchSteps = 5;   // bisection steps per surface vertex, 0..10

        bool operator==( const VoxBooleanSettings& ) const = default;
    };

    // Refuses an input without triangles, VoxelCount outside 8..1024, SurfaceSearchSteps outside 0..10, an empty
    // result (e.g. an Intersect of inputs that do not overlap). Polls progress like VoxWrap; refuses with
    // kVoxBooleanCancelled once it is raised.
    inline constexpr const char*                   kVoxBooleanCancelled = "VoxBoolean was cancelled";
    [[nodiscard]] Common::ResultStr<DeformOutcome> VoxBoolean( const DynamicMesh3& a, const DynamicMesh3& b,
                                                               const VoxBooleanSettings& settings,
                                                               const ProgressCancel*     progress = nullptr );
} // namespace Desert::Geometry
