#pragma once

#include "EditMesh.hpp"

#include <Common/Core/ResultStr.hpp>

namespace Desert::Geometry
{
    // GENERATE POLYGROUPS - UE's Modeling Mode "Generate PolyGroups" (Attributes tab), the three modes that
    // do not need a mesh processing library. Each one overwrites every live triangle's polygroup and returns
    // how many groups it made. Groups are numbered 0..N-1 in the order of their lowest triangle ID, so the
    // same mesh always gets the same numbers. Triangles only join across a shared edge: two pieces that touch
    // at a vertex, or two islands of one plane that are not connected, are different groups.

    // Crease detection: two neighbouring triangles join when the angle between their normals is at most
    // maxAngleDegrees. Neighbour-to-neighbour, so a smooth cylinder wall becomes ONE group however many
    // facets it has, and a cube's 90-degree corners separate its six faces at any threshold below 90.
    [[nodiscard]] int GeneratePolyGroupsByAngle( EditMesh& mesh, float maxAngleDegrees );

    // Planar faces: a triangle joins a group when its normal is within angleToleranceDegrees of the group's
    // FIRST triangle and all its corners lie within distanceTolerance (cm) of that triangle's plane. Measured
    // against the seed, not the neighbour, so a finely tessellated curve does NOT drift into one group -
    // the difference from ByAngle. (PolyEditTool's per-click flood compares normals with the hit triangle's,
    // dot > 0.99, and measures no plane distance, so a gentle bend inside that ~8 degree cone merges there.)
    [[nodiscard]] int GeneratePolyGroupsCoplanar( EditMesh& mesh, float angleToleranceDegrees,
                                                  float distanceTolerance );

    // One group per UV island of the given layer: triangles join across every edge that is not a seam of
    // that layer. Refused, naming the layer, when it does not exist.
    [[nodiscard]] Common::ResultStr<int> GeneratePolyGroupsByUVIslands( EditMesh& mesh, int uvLayer );
} // namespace Desert::Geometry
