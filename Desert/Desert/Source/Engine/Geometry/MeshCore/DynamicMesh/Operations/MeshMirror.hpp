// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/MeshMirror.h:12-68 and
// Private/Operations/MeshMirror.cpp:17-407, adapted: namespace Desert::Geometry, UE Core as std/glm, no
// FProgressCancel (the operation runs synchronously on the editor's copy); MirrorAndAppend reports a mirrored
// triangle that cannot join the mesh instead of check()ing it. FDynamicMeshEditor::CopyAttributes with
// FMeshIndexMappings (DynamicMeshEditor.cpp) is folded into MirrorAndAppend as per-overlay element maps covering
// what the attribute set carries here: every UV and normal layer (tangents included), the primary colours and the
// material ID.
#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/MathUtil.hpp"

#include <cstdint>

namespace Desert::Geometry
{
    enum class MeshMirrorNormalMode : uint8_t
    {
        /** Normals are split and mirrored across the mirror plane. */
        MirrorNormals = 0,
        /** Normals are averaged with their mirrored normal across the mirror plane. */
        AverageMirrorNormals = 1,
    };

    class MeshMirror
    {
    public:
        MeshMirror( DynamicMesh3* MeshIn, const glm::dvec3& Origin, const glm::dvec3& Normal )
             : m_Mesh( MeshIn ), m_PlaneOrigin( Origin ), m_PlaneNormal( Normal )
        {
        }

        DynamicMesh3* m_Mesh = nullptr;
        glm::dvec3    m_PlaneOrigin{ 0.0 };
        glm::dvec3    m_PlaneNormal{ 0.0, 0.0, 1.0 };

        /** Tolerance distance for considering a vertex to be "on the plane". */
        double m_PlaneTolerance = static_cast<double>( ZeroTolerance<float> ) * 10.0;
        /** Whether, when using MirrorAndAppend, vertices on the mirror plane should be welded. */
        bool m_bWeldAlongPlane = true;
        /** The normal compute method for welded vertices along the mirror plane. */
        MeshMirrorNormalMode m_WeldNormalMode = MeshMirrorNormalMode::MirrorNormals;
        /** Whether, when welding, a point lying in the plane without an edge in the plane may become a bowtie. */
        bool m_bAllowBowtieVertexCreation = false;

        /** Alters the existing mesh to be mirrored across the mirror plane. */
        void Mirror();

        /** Appends a mirrored copy of the mesh to the mesh.
         *  @return false when a mirrored triangle cannot join the mesh (an edge in the plane that already has a
         *  triangle on each side): m_FailedTriangle names the source triangle, m_FailureCode the AppendTriangle
         *  result, and the mesh is left part-way (callers work on a copy). */
        bool MirrorAndAppend();

        int m_FailedTriangle = DynamicMesh3::InvalidID;
        int m_FailureCode    = 0;
    };
} // namespace Desert::Geometry
