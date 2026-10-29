#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshDeformOperations.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstdint>

namespace Desert::Geometry
{
    // UE 5.8 MeshModelingTools RemoveOccludedTrianglesTool.h (EOcclusionCalculationUIMode): how a sample point
    // is decided to be hidden.
    //   WindingNumber - the point is inside the mesh: its generalized winding number (FastWindingTree) is above
    //                   the Winding Iso Value (UE GeneralizedWindingNumber, the default);
    //   RaycastOcclusionSamples - every test ray from the point hits the mesh: the six axis directions plus
    //                   Add Random Rays sphere directions, none escaping (UE RaycastOcclusionSamples).
    enum class JacketOcclusionMode : uint8_t
    {
        WindingNumber,
        RaycastOcclusionSamples,
    };
    [[nodiscard]] const char* ToString( JacketOcclusionMode mode );

    // UE EOcclusionTriangleSamplingUIMode: where a triangle is tested. A triangle is removed only when EVERY one
    // of its samples is hidden (UE TRemoveOccludedTriangles: vertices / centroids / both).
    enum class JacketTriangleSampling : uint8_t
    {
        Vertices,
        VerticesAndCentroids,
    };
    [[nodiscard]] const char* ToString( JacketTriangleSampling sampling );

    // UE URemoveOccludedTrianglesToolProperties (its defaults and ranges): Occlusion Mode, Triangle Sampling,
    // Winding Iso Value (0..1), Add Random Rays (0..1000, Raycast only), Add Triangle Samples (0..1000 extra
    // random points per triangle, each of which must also be hidden).
    struct JacketSettings
    {
        JacketOcclusionMode    OcclusionMode      = JacketOcclusionMode::WindingNumber;
        JacketTriangleSampling TriangleSampling   = JacketTriangleSampling::VerticesAndCentroids;
        float                  WindingIsoValue    = 0.5f;
        int32_t                AddRandomRays      = 0;
        int32_t                AddTriangleSamples = 0;

        bool operator==( const JacketSettings& ) const = default;
    };

    // UE URemoveOccludedTrianglesTool / TRemoveOccludedTriangles::Apply on the mesh alone (bOnlySelfOcclude):
    // every triangle whose samples are all hidden by the mesh itself is removed, with the vertices left
    // isolated; nothing else changes. Each sample is pushed off the surface along the vertex / triangle normal
    // by a tolerance relative to the mesh's size before it is tested. DeformOutcome::MovedVertices carries the
    // number of removed triangles. Refused when a setting is out of its range or the mesh has no triangle.
    [[nodiscard]] Common::ResultStr<DeformOutcome> Jacket( const DynamicMesh3&   input,
                                                           const JacketSettings& settings );
} // namespace Desert::Geometry
