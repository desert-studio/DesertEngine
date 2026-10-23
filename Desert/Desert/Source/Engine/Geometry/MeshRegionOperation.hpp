#pragma once

// Extrude / Push-Pull / Inset / Outset on a selected region of an DynamicMesh3, as UE's PolyEdit runs them on a
// copy of the mesh: FExtrudeOp (ModelingOperators/Private/DeformationOps/ExtrudeOp.cpp:94-146) drives
// OffsetMeshRegion, UPolyEditInsetOutsetActivity (PolyEditInsetOutsetActivity.cpp:181) drives InsetMeshRegion
// with the distance negated for Outset.
//
// TANGENTS. The ported region operations set normals and UVs on the triangles they create but never the tangent
// overlays: UE treats tangents as derived data and rebuilds them when the mesh description is built. A mesh that
// carries a tangent space here would otherwise leave the new triangles unset in it, and ToRenderMesh refuses such
// a mesh; so the result's tangents are recomputed from its normals and UV layer 0 (FMeshTangents, per triangle).

#include "Engine/Geometry/DynamicMeshSelection.hpp"
#include "Engine/Geometry/MeshDeformOperations.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/SubdividePoly.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace Desert::Geometry
{
    enum class RegionOperation : uint8_t
    {
        Extrude,
        PushPull,
        Inset,
        Outset,
    };

    [[nodiscard]] const char* ToString( RegionOperation operation );

    struct RegionOutcome
    {
        std::shared_ptr<const DynamicMesh3> Mesh;
        // The moved region (Extrude, Push/Pull) or the inset region (Inset, Outset), in the selection's mode.
        ElementSelection Selection{ ElementMode::Triangle };
        // What the operation did beyond the mesh itself, in numbers, for the log (Clean's counts). Empty when
        // the topology change says it all.
        std::string Report;
    };

    // Runs `operation` on a copy of `before`. `distance` is in cm: positive for Extrude / Inset / Outset, signed
    // for Push/Pull. Refused, by name and numbers, on a zero or wrongly signed distance, a selection covering no
    // whole triangle, the ported operation's own failure, or a tangent space with no UV layer 0 to derive from.
    [[nodiscard]] Common::ResultStr<RegionOutcome> RunRegionOperation( RegionOperation         operation,
                                                                       const DynamicMesh3&     before,
                                                                       const ElementSelection& selection,
                                                                       float                   distance );

    // Fill Hole, as UE's FHoleFillOp::CalculateResult runs it with the TriangleFan fill
    // (ModelingOperators/Private/CleaningOps/HoleFillOp.cpp:331-450): per open loop, the Newell plane of its
    // vertices (PolygonTriangulation::ComputePolygonPlane), negated as UE does, a fan in a new polygroup, the
    // plane normal on the fan and UVs projected on the plane at 1 / the mesh's largest bounds dimension
    // (HoleFillTool.cpp:218). An EDGE selection fills the loops through its edges; any other selection fills
    // every loop. The result selects the new triangles. Refused on a mesh with no open loop, a selected edge on
    // no loop, or a loop the filler rejects.
    [[nodiscard]] Common::ResultStr<RegionOutcome> FillHoles( const DynamicMesh3&     before,
                                                              const ElementSelection& selection );

    // Insert Edge Loop, as UE's UPolyEditInsertEdgeLoopActivity drives FEdgeLoopInsertionOp
    // (PolyEditInsertEdgeLoopActivity.cpp:30-75, EdgeLoopInsertionOp.cpp:25-66) with the PlaneCut mode and one
    // ProportionOffset: GroupEdgeInserter::InsertEdgeLoops across the group edge the EDGE selection lies on,
    // `position` in (0, 1) measured from that group edge's EndpointCorners.A (UE's unflipped direction). Every
    // crossed group is split in two new groups (a cube: 6 -> 10). The result selects the new loop's edges.
    // Refused, by name and numbers: a selection not in Edge mode or empty, edges on more than one group edge,
    // position outside (0, 1), or the inserter's own failure (with the problem group edge count).
    [[nodiscard]] Common::ResultStr<RegionOutcome>
    InsertEdgeLoop( const DynamicMesh3& before, const ElementSelection& selection, float position );

    // Weld Edges: MergeCoincidentMeshEdges with UE's defaults (MergeCoincidentMeshEdges.h) and the split
    // attributes welded along merged edges, the settings of test WeldClosesACubeCutAlongEverySeam. Mesh-wide;
    // leaves an empty selection in @p mode. Refused when there is no boundary edge to weld.
    [[nodiscard]] Common::ResultStr<RegionOutcome> WeldEdges( const DynamicMesh3& before, ElementMode mode );

    // The tangent space of @p mesh rebuilt from its normals and UV layer 0 (MeshTangents, per triangle), as UE
    // treats tangents as derived data after an edit (see TANGENTS above). A mesh without a tangent space is left
    // as it is. Refused, naming @p name, when the mesh has tangents but no UV layer 0, or the tangents cannot be
    // written.
    [[nodiscard]] Common::BoolResultStr RecomputeTangentSpace( DynamicMesh3& mesh, const char* name );
    // A mesh with an attribute set but no tangent space gets one, built as RecomputeTangentSpace builds it
    // (UE's FStaticMeshBuilder computes tangents for a source without them); one that has tangents is left as
    // it is. Refused as RecomputeTangentSpace refuses, and on a mesh with no attribute set.
    [[nodiscard]] Common::BoolResultStr EnsureTangentSpace( DynamicMesh3& mesh, const char* name );

    // The Subdivide tool's settings, UE's USubdividePolyToolProperties (SubdividePolyTool.h:44-62) with its
    // defaults. Not ported: bAddExtraCorners / ExtraCornerAngleThresholdDegrees (GroupTopology here has no
    // extra-corner hook yet), bRenderGroups / bRenderCage (preview drawing).
    struct SubdivideSettings
    {
        int                       Level         = 3;
        SubdivisionScheme         Scheme        = SubdivisionScheme::CatmullClark;
        SubdivisionBoundaryScheme Boundary      = SubdivisionBoundaryScheme::SmoothCorners;
        SubdivisionOutputNormals  Normals       = SubdivisionOutputNormals::Generated;
        bool                      NewPolyGroups = false;
    };

    // UE caps a subdivision at this many faces (SubdividePolyTool.cpp:150, from UDisplaceMeshTool).
    inline constexpr int kMaxSubdividedFaces = 3000000;
    // MaxSubdivisionLevel( 1 ): the highest level any cage reaches under kMaxSubdividedFaces - the editor's
    // clamp for the level field (the operation still refuses a level too high for the mesh it runs on).
    inline constexpr int kMaxSubdivisionLevel = 10;

    // The highest level `faces` cage faces may be refined to under kMaxSubdividedFaces
    // (UE: USubdividePolyTool::CapSubdivisionLevel, SubdividePolyTool.cpp:147-175).
    [[nodiscard]] int MaxSubdivisionLevel( int faces );

    // Subdivide, as UE's SubdivPostProcessor runs SubdividePoly on the whole mesh (SubdividePolyTool.cpp:55-82).
    // Mesh-wide; leaves an empty selection in @p mode. A tangent space on @p before is rebuilt on the result.
    // Refused, by name and numbers, on an empty mesh, a level below 1 or above MaxSubdivisionLevel (UE clamps it
    // with a warning), a group topology Bilinear / Catmull-Clark cannot use (UE switches to Loop with a warning,
    // SubdividePolyTool.cpp:230-235; here the user picks Loop), or SubdividePoly's own failure.
    [[nodiscard]] Common::ResultStr<RegionOutcome>
    SubdivideMesh( const DynamicMesh3& before, const SubdivideSettings& settings, ElementMode mode );

    // UE USimplifyMeshTool / FSimplifyMeshOp with the QEM simplifier (SimplifyMeshOp.cpp:33-143, 261), the
    // Modeling MeshOps palette's Simplify (a preview until Accept, one undo step). USimplifyMeshToolProperties:
    //   Target Mode - Percentage keeps max(4, Percentage% of the triangles) (SimplifyMeshOp.cpp:119-123);
    //   Triangle Count / Vertex Count stop at Target Count; Edge Length collapses every edge shorter than
    //   Target Edge Length (world cm, as Remesh measures it; UE SimplifyToEdgeLength);
    //   Preserve Boundaries - UE MeshBoundaryConstraint Fixed (open borders stay exactly) vs Free (they collapse
    //   along themselves only);
    //   Preserve PolyGroups - UE GroupBoundaryConstraint Fixed vs Ignore. UE's own default is Ignore; the editor
    //   keeps them, because a PolyEdit mesh without its group borders loses its faces;
    //   Preserve Sharp Edges (UE bPreserveSharpEdges, default off) - on, a seam (hard edge / UV split) never
    //   collapses and its vertices never move; off, a seam collapses along its own line;
    //   Discard Attributes (UE bDiscardAttributes) - the UV / normal / colour overlays are dropped first (no seam
    //   holds the collapse) and per-vertex normals rebuilt on the result.
    //   Simplifier Type (UE SimplifierType) - Standard (FQEMSimplification: position quadric only; the kept
    //   normal overlay is rebuilt from the collapsed surface) or Attribute-Aware (FAttrMeshSimplification: each
    //   face quadric also carries the linear field of its corner normals, so a collapse pays for bending them, the
    //   collapse point keeps the volume, and a collapsed vertex takes the normal the quadric predicts - the overlay
    //   is NOT rebuilt). One simplifier (QemSimplificationT) over the two quadric types, as in UE.
    enum class SimplifierType : uint8_t
    {
        Standard,
        AttributeAware,
    };
    [[nodiscard]] const char* ToString( SimplifierType type );
    inline constexpr SimplifierType kSimplifierTypes[] = { SimplifierType::Standard, SimplifierType::AttributeAware };

    enum class SimplifyTarget : uint8_t
    {
        Percentage,
        TriangleCount,
        VertexCount,
        EdgeLength,
    };
    [[nodiscard]] const char* ToString( SimplifyTarget target );
    inline constexpr SimplifyTarget kSimplifyTargets[] = {
         SimplifyTarget::Percentage, SimplifyTarget::TriangleCount, SimplifyTarget::VertexCount,
         SimplifyTarget::EdgeLength };

    struct SimplifySettings
    {
        SimplifierType Simplifier              = SimplifierType::Standard; // UE SimplifierType default QEM
        SimplifyTarget Target                  = SimplifyTarget::Percentage;
        float          Percentage              = 50.0f; // UE TargetPercentage default
        int32_t        TargetCount             = 1000;  // UE TargetCount default (triangles or vertices)
        float          TargetEdgeLength        = 5.0f;  // UE TargetEdgeLength default, cm
        bool           PreserveBoundaries      = false; // UE MeshBoundaryConstraint default Free
        bool           PreserveGroupBoundaries = true;
        bool           PreserveSharpEdges      = false;
        bool           DiscardAttributes       = false;

        bool operator==( const SimplifySettings& ) const = default;
    };

    // The input simplified into a copy (DeformOutcome::Mesh, in the input's object space); @p localToWorld only
    // measures Edge Length. Normals recomputed (per-vertex when the attributes were discarded), tangents
    // rebuilt; DeformOutcome::Report reads "Simplify: N -> M triangles (K collapses)". Refused, by name and
    // numbers: an empty mesh, a target out of range, a target the mesh already meets, Preserve PolyGroups on a
    // mesh with no polygroups, or no edge that could collapse under the constraints.
    [[nodiscard]] Common::ResultStr<DeformOutcome> SimplifyMeshOperation( const DynamicMesh3&     input,
                                                                          const glm::dmat4&       localToWorld,
                                                                          const SimplifySettings& settings );
} // namespace Desert::Geometry
