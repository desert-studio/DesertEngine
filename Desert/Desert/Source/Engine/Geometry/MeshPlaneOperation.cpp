#include "MeshPlaneOperation.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/MeshMirror.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/MeshPlaneCut.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <limits>

namespace Desert::Geometry
{
    const char* ToString( MirrorMode mode )
    {
        switch ( mode )
        {
            case MirrorMode::AddMirroredCopy:
                return "Add Mirrored Copy";
            case MirrorMode::CutAndMirror:
                return "Cut and Mirror";
        }
        return "Unknown";
    }

    const char* ToString( PlaneCutMode mode )
    {
        switch ( mode )
        {
            case PlaneCutMode::DiscardNegativeSide:
                return "Discard Negative Side";
            case PlaneCutMode::KeepBothHalves:
                return "Keep Both Halves";
        }
        return "Unknown";
    }

    namespace
    {
        // FMeshPlaneCut's and FMeshMirror's default PlaneTolerance (MeshPlaneCut.h, MeshMirror.h).
        constexpr double kDefaultPlaneTolerance = static_cast<double>( ZeroTolerance<float> ) * 10.0;

        struct SignedRange
        {
            double Low  = std::numeric_limits<double>::max();
            double High = -std::numeric_limits<double>::max();
        };

        SignedRange SignedDistances( const DynamicMesh3& mesh, const glm::dvec3& origin, const glm::dvec3& unit )
        {
            SignedRange range;
            for ( const int v : mesh.VertexIndicesItr() )
            {
                const double d = glm::dot( mesh.GetVertex( v ) - origin, unit );
                range.Low      = std::min( range.Low, d );
                range.High     = std::max( range.High, d );
            }
            return range;
        }

        // FMeshPlaneCut::Cut on a copy of @p before, keeping the side @p keep points to; @p fillHole caps the cut.
        Common::ResultStr<RegionOutcome> KeepSide( const DynamicMesh3& before, const glm::dvec3& origin,
                                                   const glm::dvec3& keep, bool fillHole, double tolerance,
                                                   float uvScale, const char* name )
        {
            auto         mesh = std::make_shared<DynamicMesh3>( before );
            MeshPlaneCut cut( mesh.get(), origin, -keep );
            cut.m_PlaneTolerance = tolerance;
            cut.m_UVScaleFactor  = uvScale;
            if ( !cut.Cut() )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh {}: the cut's outline could not be walked into loops (the plane meets the mesh along a "
                     "non-manifold border)",
                     name );
            if ( mesh->TriangleCount() == 0 )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: nothing is left on the kept side",
                                                                  name );
            ElementSelection cap( ElementMode::PolyGroup );
            if ( fillHole )
            {
                const MeshPlaneCut::OpenBoundary& boundary = cut.m_OpenBoundaries.front();
                if ( boundary.FoundOpenSpans )
                    return Common::MakeFormattedError<RegionOutcome>(
                         "Mesh {}: the cut leaves {} open outline(s) - the mesh's own open border crosses the "
                         "plane, so there is no closed loop to cap; cut without Fill",
                         name, boundary.CutSpans.size() );
                if ( !cut.HoleFill( false ) )
                    return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: the cap could not be filled - {}",
                                                                      name, cut.m_FailureReason );
                const std::vector<int>& capTriangles = cut.m_HoleFillTriangles.front();
                if ( !capTriangles.empty() )
                    if ( auto added = cap.Add( *mesh, mesh->GetTriangleGroup( capTriangles.front() ) );
                         !added.IsSuccess() )
                        return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: {}", name, added.GetError() );
            }
            if ( auto tangents = RecomputeTangentSpace( *mesh, name ); !tangents.IsSuccess() )
                return Common::MakeError<RegionOutcome>( tangents.GetError() );
            return Common::MakeSuccess( RegionOutcome{ std::move( mesh ), std::move( cap ) } );
        }

        Common::ResultStr<glm::dvec3> UnitNormal( const MeshPlane& plane, const char* name )
        {
            const double length = glm::length( plane.Normal );
            if ( !( length > 0.0 ) )
                return Common::MakeFormattedError<glm::dvec3>( "Mesh {}: the plane's normal is zero", name );
            return Common::MakeSuccess( plane.Normal / length );
        }
    } // namespace

    Common::ResultStr<RegionOutcome> MirrorMesh( const DynamicMesh3& before, const MeshPlane& plane,
                                                 MirrorMode mode, float weldTolerance, ElementMode selectionMode )
    {
        auto unit = UnitNormal( plane, "Mirror" );
        if ( !unit.IsSuccess() )
            return Common::MakeError<RegionOutcome>( unit.GetError() );
        if ( !( weldTolerance >= 0.0f ) )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Mirror: the weld tolerance must be >= 0 cm, not {}", weldTolerance );
        if ( before.TriangleCount() == 0 )
            return Common::MakeError<RegionOutcome>( "Mesh Mirror: the mesh has no triangles" );
        const double tolerance = std::max( kDefaultPlaneTolerance, static_cast<double>( weldTolerance ) );

        std::shared_ptr<DynamicMesh3> mesh;
        if ( mode == MirrorMode::CutAndMirror )
        {
            const SignedRange range = SignedDistances( before, plane.Origin, unit.GetValue() );
            if ( !( range.High > tolerance ) )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Mirror: nothing lies on the kept side of the plane (signed distances {:.4f} .. {:.4f} "
                     "cm) - flip the side or move the plane",
                     range.Low, range.High );
            // UE's crop never fills: the open half is what the mirror closes.
            auto kept = KeepSide( before, plane.Origin, unit.GetValue(), false, tolerance, 1.0f, "Mirror" );
            if ( !kept.IsSuccess() )
                return Common::MakeError<RegionOutcome>( kept.GetError() );
            mesh = std::make_shared<DynamicMesh3>( *kept.GetValue().Mesh );
        }
        else
            mesh = std::make_shared<DynamicMesh3>( before );

        MeshMirror mirror( mesh.get(), plane.Origin, unit.GetValue() );
        mirror.m_PlaneTolerance  = tolerance;
        mirror.m_bWeldAlongPlane = true;
        if ( !mirror.MirrorAndAppend() )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Mirror: the reflection of triangle {} cannot join the mesh (error {}) - an edge in the "
                 "plane "
                 "already has a triangle on each side (a fin); cut the mesh first (Cut and Mirror)",
                 mirror.m_FailedTriangle, mirror.m_FailureCode );
        if ( auto tangents = RecomputeTangentSpace( *mesh, "Mirror" ); !tangents.IsSuccess() )
            return Common::MakeError<RegionOutcome>( tangents.GetError() );
        return Common::MakeSuccess( RegionOutcome{ std::move( mesh ), ElementSelection( selectionMode ) } );
    }

    Common::ResultStr<PlaneCutOutcome> PlaneCutMesh( const DynamicMesh3& before, const MeshPlane& plane,
                                                     PlaneCutMode mode, bool fillHole )
    {
        auto unit = UnitNormal( plane, "Plane Cut" );
        if ( !unit.IsSuccess() )
            return Common::MakeError<PlaneCutOutcome>( unit.GetError() );
        const SignedRange range = SignedDistances( before, plane.Origin, unit.GetValue() );
        if ( !( range.High > kDefaultPlaneTolerance ) || !( range.Low < -kDefaultPlaneTolerance ) )
            return Common::MakeFormattedError<PlaneCutOutcome>(
                 "Mesh Plane Cut: the plane does not cross the mesh (signed distances {:.4f} .. {:.4f} cm) - one "
                 "half would be empty",
                 range.Low, range.High );
        // UPlaneCutTool's UV scale: one unit of UV across the mesh's largest dimension.
        const AxisAlignedBox3d bounds  = before.GetBounds();
        const glm::dvec3       size    = bounds.Max - bounds.Min;
        const double           maxDim  = std::max( { size.x, size.y, size.z } );
        const float            uvScale = maxDim > 0.0 ? static_cast<float>( 1.0 / maxDim ) : 1.0f;

        auto positive = KeepSide( before, plane.Origin, unit.GetValue(), fillHole, kDefaultPlaneTolerance, uvScale,
                                  "Plane Cut" );
        if ( !positive.IsSuccess() )
            return Common::MakeError<PlaneCutOutcome>( positive.GetError() );
        PlaneCutOutcome outcome{ positive.ExtractValue(), nullptr };
        if ( mode == PlaneCutMode::KeepBothHalves )
        {
            auto negative = KeepSide( before, plane.Origin, -unit.GetValue(), fillHole, kDefaultPlaneTolerance,
                                      uvScale, "Plane Cut, other half" );
            if ( !negative.IsSuccess() )
                return Common::MakeError<PlaneCutOutcome>( negative.GetError() );
            outcome.OtherHalf = negative.GetValue().Mesh;
        }
        return Common::MakeSuccess( std::move( outcome ) );
    }
} // namespace Desert::Geometry
