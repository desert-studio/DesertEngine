#pragma once

// Ported from UE 5.8
// Engine/Plugins/Runtime/MeshModelingToolset/Source/ModelingComponentsEditorOnly/Public/Operations/SubdividePoly.h:15-133
// and Private/Operations/SubdividePoly.cpp:1-1466, adapted: UE Core as std/glm, namespace Desert::Geometry, no
// UENUM/UE_API; a failure is returned by name (Failure()) instead of an ensure + false. The refiner is the same
// OpenSubdiv 3.6 Far::TopologyRefiner UE uses (ThirdParty/OpenSubdiv, BuildScripts/ThirdParty/OpenSubdiv.lua).
// NOT ported: skin weights, bones, morph targets and weight layers (the attribute set here has none of them),
// ESubdivisionOutputNormals::None and ESubdivisionOutputUVs (UVs are always interpolated: ToRenderMesh refuses a
// triangle with no normal, and a mesh that loses its UVs cannot be textured again without a separate unwrap), and
// the HAVE_OPENSUBDIV no-op branch (every platform we build has the library). CHANGED: the Loop scheme numbers
// the refiner's vertices densely through a vertex map, where UE passes MaxVertexID and fills VertexIndicesItr's
// count of values (SubdividePoly.cpp:618, :805), which only agree on a compact mesh.

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/GroupTopology.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace Desert::Geometry
{
    // UE's ESubdivisionScheme (SubdividePoly.h:19-31).
    enum class SubdivisionScheme : uint8_t
    {
        // Subdivides like Catmull-Clark, but does not smooth the result (the vertices stay in their original
        // planes).
        Bilinear,
        // The quad-dominant scheme; the group topology is the cage it starts from.
        CatmullClark,
        // Charles Loop's scheme; runs on the triangles directly and does not use the group topology.
        Loop,
    };

    // UE's ESubdivisionBoundaryScheme (SubdividePoly.h:33-50).
    enum class SubdivisionBoundaryScheme : uint8_t
    {
        // Corners with only one adjoining face are smoothed with the rest of the boundary (a square patch gets
        // rounded corners).
        SmoothCorners,
        // Corners with only one adjoining face are passed through.
        SharpCorners,
    };

    // UE's ESubdivisionOutputNormals (SubdividePoly.h:56-67) without None.
    enum class SubdivisionOutputNormals : uint8_t
    {
        // The primary normal overlay is interpolated like any other face-varying value.
        Interpolated,
        // Per-vertex normals are recomputed on the result.
        Generated,
    };

    [[nodiscard]] const char* ToString( SubdivisionScheme scheme );
    [[nodiscard]] const char* ToString( SubdivisionBoundaryScheme scheme );
    [[nodiscard]] const char* ToString( SubdivisionOutputNormals normals );

    // Interprets a GroupTopology as a poly mesh (or, for Loop, the triangles themselves) and refines it
    // `level` times. ComputeTopologySubdivision builds the refined topology once; ComputeSubdividedMesh then
    // interpolates positions, polygroups, material IDs, polygroup layers, UV layers, colours and (Interpolated)
    // normals onto it.
    class SubdividePoly
    {
    public:
        SubdividePoly( const GroupTopology& topology, const DynamicMesh3& originalMesh, int level );
        ~SubdividePoly();
        SubdividePoly( const SubdividePoly& )            = delete;
        SubdividePoly& operator=( const SubdividePoly& ) = delete;

        // UE's ETopologyCheckResult (SubdividePoly.h:98-106). InsufficientGroups is declared there and returned
        // by nothing, so it is not ported.
        enum class TopologyCheckResult : uint8_t
        {
            Ok,
            NoGroups,
            UnboundedPolygroup,
            MultiBoundaryPolygroup,
            DegeneratePolygroup,
        };
        [[nodiscard]] TopologyCheckResult ValidateTopology() const;

        // False with Failure() naming the cause: a level below 1, a polygroup the cage cannot use, or a
        // descriptor OpenSubdiv rejects.
        [[nodiscard]] bool ComputeTopologySubdivision();

        // False with Failure() naming the cause; requires ComputeTopologySubdivision to have succeeded.
        [[nodiscard]] bool ComputeSubdividedMesh( DynamicMesh3& outMesh );

        [[nodiscard]] const std::string& Failure() const
        {
            return m_Failure;
        }

        SubdivisionScheme         m_SubdivisionScheme       = SubdivisionScheme::CatmullClark;
        SubdivisionBoundaryScheme m_BoundaryScheme          = SubdivisionBoundaryScheme::SharpCorners;
        SubdivisionOutputNormals  m_NormalComputationMethod = SubdivisionOutputNormals::Generated;
        bool                      m_bNewPolyGroups          = false;

        // Wrapper around OpenSubdiv::Far::TopologyRefiner, so this header does not include OpenSubdiv's.
        class RefinerImpl;

    private:
        const GroupTopology&         m_GroupTopology;
        const DynamicMesh3&          m_OriginalMesh;
        int                          m_Level;
        std::unique_ptr<RefinerImpl> m_Refiner;
        std::string                  m_Failure;
    };

    [[nodiscard]] const char* ToString( SubdividePoly::TopologyCheckResult result );
} // namespace Desert::Geometry
