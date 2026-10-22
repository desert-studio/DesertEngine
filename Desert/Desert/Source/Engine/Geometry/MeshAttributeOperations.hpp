#pragma once

#include "Engine/Geometry/MeshDeformOperations.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstdint>

namespace Desert::Geometry
{
    // ATTRIBUTE OPERATIONS - UE's Attributes palette of Modeling Mode, the operators behind its tools; the first
    // is UNormalsTool (FEditNormalsOp, whole-mesh path).
    //
    // Same contract as MeshDeformOperations.hpp, whose outcome they share so one mesh-processing session previews
    // both: pure functions, the input is not touched, the result is a NEW mesh. Normals change the primary normal
    // overlay (its split topology and its values) and, with Invert, the triangles' winding; positions, UVs,
    // colours and polygroups are carried unchanged; tangents are rebuilt, as they derive from the normals.

    // UE ENormalCalculationMethod: how a vertex averages the normals of the triangles around it.
    enum class NormalCalculationMethod : uint8_t
    {
        AreaWeighted,       // larger triangles count more
        AngleWeighted,      // each triangle counts by its opening angle at the vertex
        AreaAngleWeighting, // both (UE's default)
    };
    [[nodiscard]] const char* ToString( NormalCalculationMethod method );

    // UE ESplitNormalMethod: where the normal overlay is split into hard edges.
    enum class SplitNormalMethod : uint8_t
    {
        UseExistingTopology, // keep the mesh's split edges, only recompute the values
        FaceNormalThreshold, // split where adjacent face normals differ by more than SharpEdgeAngleThreshold
        FaceGroupID,         // split along polygroup borders
        PerTriangle,         // every triangle flat (faceted)
        PerVertex,           // no split at all (fully smooth)
    };
    [[nodiscard]] const char* ToString( SplitNormalMethod method );

    // UE UEditNormalsProperties (defaults as UE's).
    struct NormalsSettings
    {
        bool                    RecomputeNormals        = true; // UE bRecomputeNormals
        NormalCalculationMethod CalculationMethod       = NormalCalculationMethod::AreaAngleWeighting;
        SplitNormalMethod       SplitMethod             = SplitNormalMethod::UseExistingTopology;
        float                   SharpEdgeAngleThreshold = 60.0f; // degrees, UE NormalSplitThreshold, 0..180
        bool                    AllowSharpVertices      = false; // UE bAllowSharpVertices (FaceNormalThreshold)
        bool FixInconsistentNormals = false; // UE bFixInconsistentNormals (FMeshRepairOrientation)
        bool                    InvertNormals           = false; // UE bInvertNormals: flips winding and normals

        bool operator==( const NormalsSettings& ) const = default;
    };

    // UE FMeshRepairOrientation: OrientComponents makes every connected component's winding agree with its seed
    // triangle; SolveGlobalOrientation then reverses each component that is seen from far away more from inside
    // than from outside. Returns the number of reversed triangles.
    int RepairOrientation( DynamicMesh3& mesh );

    // UE FEditNormalsOp::CalculateResultWholeMesh. Splitting always recomputes (UE: "if you split normals you must
    // always recompute as well"). Refuses an empty mesh or a threshold outside 0..180.
    // UE EMeshTangentsType (the computed ones; CopyExisting is UE's for a source that carries tangents).
    enum class TangentsMethod : uint8_t
    {
        FastMikkTSpace, // UE default: MikkTSpace-style wedge average (MeshTangents::ComputeMikkTStyleTangents)
        MikkTSpace,     // the MikkTSpace library (ThirdParty/mikktspace), as UE DynamicMeshMikkTWrapper
        PerTriangle,    // per triangle corner, no averaging
    };
    [[nodiscard]] const char* ToString( TangentsMethod method );

    // UE UMeshTangentsToolProperties (defaults as UE's). Only CalculationMethod changes the result; the rest is
    // the preview's line display.
    struct TangentsSettings
    {
        TangentsMethod CalculationMethod = TangentsMethod::FastMikkTSpace;
        bool           ShowTangents      = true;
        bool           ShowNormals       = false;
        float          LineLength        = 2.0f; // cm, UE UIMin 0.01 UIMax 25
        float          LineThickness     = 1.0f;
        bool           operator==( const TangentsSettings& ) const = default;
    };

    // UE FCalculateTangentsOp + UMeshTangentsTool::CopyToOverlays: computes the tangent frames from the primary
    // normals and UV layer 0 and writes them into the tangent / bitangent overlays (enabled when missing).
    [[nodiscard]] Common::ResultStr<DeformOutcome> ComputeTangents( const DynamicMesh3&     mesh,
                                                                    const TangentsSettings& settings );

    [[nodiscard]] Common::ResultStr<DeformOutcome> EditNormals( const DynamicMesh3&    mesh,
                                                                const NormalsSettings& settings );
} // namespace Desert::Geometry
