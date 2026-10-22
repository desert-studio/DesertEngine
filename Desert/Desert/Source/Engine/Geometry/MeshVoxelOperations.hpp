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

    // Modeling Mode -> VoxOps: UE 5.8 VoxMorph (UVoxelMorphologyMeshesTool, operator FVoxelMorphologyMeshesOp in
    // ModelingOperators/Private/CompositionOps/VoxelMorphologyMeshesOp.cpp, implicit TImplicitMorphology in
    // GeometryCore/Public/Implicit/Morphology.h): the input's signed distance field (as VoxBoolean's) offset by
    // Distance and meshed by Marching Cubes. Dilate grows the solid by Distance, Contract shrinks it; Close is a
    // Dilate whose result is contracted back (fills gaps and creases narrower than 2 x Distance), Open is a
    // Contract whose result is dilated back (removes parts thinner than 2 x Distance) - the second pass reads the
    // first pass's mesh, as UE's re-sampled level set. Convex edges come out rounded with radius Distance.
    enum class VoxMorphOperation : uint8_t
    {
        Dilate, // UE EMorphologyOperation::Dilate
        Contract,
        Close,
        Open,
    };

    [[nodiscard]] const char* ToString( VoxMorphOperation operation );

    // UE UVoxelMorphologyMeshesToolProperties + UVoxelProperties (defaults as UE's).
    struct VoxMorphSettings
    {
        VoxMorphOperation Operation          = VoxMorphOperation::Dilate;
        float             Distance           = 1.0f; // UE Distance, cm (> 0)
        int32_t           VoxelCount         = 128;  // cells along the longest side of the dilated bounds
        int32_t           SurfaceSearchSteps = 5;    // bisection steps per surface vertex, 0..10

        bool operator==( const VoxMorphSettings& ) const = default;
    };

    // Refuses an empty mesh, Distance <= 0, VoxelCount outside 8..1024, SurfaceSearchSteps outside 0..10, an empty
    // result (a Contract or Open thicker than the solid). Polls progress; refuses with kVoxMorphCancelled.
    inline constexpr const char*                   kVoxMorphCancelled = "VoxMorph was cancelled";
    [[nodiscard]] Common::ResultStr<DeformOutcome> VoxMorph( const DynamicMesh3&     mesh,
                                                             const VoxMorphSettings& settings,
                                                             const ProgressCancel*   progress = nullptr );
    // Modeling Mode -> VoxOps: UE 5.8 VoxBlend (UVoxelBlendMeshesTool, operator FVoxelBlendMeshesOp in
    // ModelingOperators/Private/CompositionOps/VoxelBlendMeshesOp.cpp, implicit TImplicitBlend in
    // GeometryCore/Public/Implicit/Blend.h): each input's signed distance field (as VoxBoolean's) passes through a
    // falloff - full inside, (1 - d / BlendFalloff)^BlendPower out to BlendFalloff from the surface, zero beyond -
    // and the two fields are summed (Union) or the second is taken from the first (Subtract), so surfaces nearer
    // than BlendFalloff melt into each other with a smooth bridge or fillet while the rest of each surface stays
    // where it was. Inputs are in ONE space (the caller bakes B into A's object space); the result is a NEW closed
    // mesh in that space.
    enum class VoxBlendOperation : uint8_t
    {
        Union,    // UE EVoxelBlendOperation::Union
        Subtract, // A minus B, the cut's rim blended
    };

    [[nodiscard]] const char* ToString( VoxBlendOperation operation );

    // UE UVoxelBlendMeshesToolProperties + UVoxelProperties (defaults as UE's).
    struct VoxBlendSettings
    {
        VoxBlendOperation Operation          = VoxBlendOperation::Union;
        float             BlendFalloff       = 10.0f; // UE BlendFalloff, cm (> 0): the reach of the blend
        float             BlendPower         = 2.0f;  // UE BlendPower (> 0): the blend's profile, higher = tighter
        int32_t           VoxelCount         = 128;   // cells along the longest side of both inputs' bounds
        int32_t           SurfaceSearchSteps = 5;     // bisection steps per surface vertex, 0..10

        bool operator==( const VoxBlendSettings& ) const = default;
    };

    // Refuses an input without triangles, BlendFalloff or BlendPower <= 0, VoxelCount outside 8..1024,
    // SurfaceSearchSteps outside 0..10, an empty result. Polls progress; refuses with kVoxBlendCancelled.
    inline constexpr const char*                   kVoxBlendCancelled = "VoxBlend was cancelled";
    [[nodiscard]] Common::ResultStr<DeformOutcome> VoxBlend( const DynamicMesh3& a, const DynamicMesh3& b,
                                                             const VoxBlendSettings& settings,
                                                             const ProgressCancel*   progress = nullptr );
} // namespace Desert::Geometry
