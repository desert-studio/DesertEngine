#include "EditMeshModelOperations.hpp"

#include "EditMeshNormals.hpp"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <utility>
#include <vector>

namespace Desert::Geometry
{
    namespace
    {
        using Outcome = Common::ResultStr<MeshEditOutcome>;

        // Two attribute values closer than this are the same value: a reflected seam element that lands on
        // the original shares it instead of starting a seam.
        constexpr float kSameValue = 1e-6f;

        template <typename Range>
        std::vector<int> Snapshot( const Range& ids )
        {
            std::vector<int> out;
            for ( const int id : ids )
                out.push_back( id );
            return out;
        }

        void EnableLayersLike( const EditMeshAttributes& from, EditMeshAttributes& to )
        {
            if ( from.Normals() != nullptr )
                to.EnableNormals();
            if ( from.Tangents() != nullptr )
                to.EnableTangents();
            if ( from.Colors() != nullptr )
                to.EnableColors();
        }

        // ── Mirror ───────────────────────────────────────────────────────────────────────────────────

        template <typename T, typename Reflect>
        Common::BoolResultStr MirrorLayer( EditMesh& mesh, EditMeshOverlay<T>* layer,
                                           const std::vector<std::pair<int, int>>& pairs,
                                           const std::vector<int>& mirrorOf, Reflect reflect, const char* name )
        {
            if ( layer == nullptr )
                return Common::MakeSuccess( true );
            std::vector<int> copyOf( static_cast<size_t>( layer->MaxElementId() ), InvalidId );
            // The reflected triangle is wound (c0, c2, c1): its corners take the elements in that order.
            constexpr std::array<int, 3> order{ 0, 2, 1 };
            for ( const auto& [original, mirrored] : pairs )
            {
                if ( !layer->IsSetTriangle( original ) )
                    continue;
                const std::array<int, 3> elements = layer->GetTriangle( original );
                const std::array<int, 3> corners  = mesh.GetTriangle( original );
                std::array<int, 3>       out{};
                for ( int k = 0; k < 3; ++k )
                {
                    const int e     = elements[order[k]];
                    const int v     = corners[order[k]];
                    const T   value = reflect( layer->GetElement( e ) );
                    if ( mirrorOf[v] == v && glm::length( value - layer->GetElement( e ) ) <= kSameValue )
                        out[k] = e;
                    else
                    {
                        if ( copyOf[e] == InvalidId )
                            copyOf[e] = layer->AppendElement( value );
                        out[k] = copyOf[e];
                    }
                }
                if ( const EditResult r = layer->SetTriangle( mesh, mirrored, out ); r != EditResult::Ok )
                    return Common::MakeFormattedError<bool>(
                         "Mirror: the {} of triangle {} could not be reflected onto triangle {} ({})", name,
                         original, mirrored, ToString( r ) );
            }
            return Common::MakeSuccess( true );
        }
    } // namespace

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

    Outcome MirrorMesh( const EditMesh& mesh, const CutPlane& plane, MirrorMode mode, float weldTolerance )
    {
        const float normalLength = glm::length( plane.Normal );
        if ( !( normalLength > 0.0f ) )
            return Common::MakeError<MeshEditOutcome>( "Mirror: the plane has a zero normal" );
        if ( !( weldTolerance >= 0.0f ) )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Mirror: the weld tolerance must be >= 0 cm, not {}", weldTolerance );
        const glm::vec3 normal         = plane.Normal / normalLength;
        const auto      signedDistance = [&]( const glm::vec3& p ) { return glm::dot( p - plane.Point, normal ); };

        MeshEditOutcome out;
        EditMesh&       work = out.Mesh;
        work                 = mesh;
        int splitEdges       = 0;
        int cutTriangles     = 0;
        int inPlaneTriangles = 0;
        int farVertices      = 0;

        if ( mode == MirrorMode::CutAndMirror )
            for ( const int e : Snapshot( work.EdgeIds() ) )
            {
                const auto& ends = work.GetEdgeVertices( e );
                const float da   = signedDistance( work.GetPosition( ends[0] ) );
                const float db   = signedDistance( work.GetPosition( ends[1] ) );
                if ( ( da <= weldTolerance || db >= -weldTolerance ) &&
                     ( da >= -weldTolerance || db <= weldTolerance ) )
                    continue;
                SplitEdgeInfo info;
                if ( const EditResult r = work.SplitEdge( e, da / ( da - db ), info ); r != EditResult::Ok )
                    return Common::MakeFormattedError<MeshEditOutcome>(
                         "Mirror: edge {} crossing the plane could not be split ({})", e, ToString( r ) );
                ++splitEdges;
            }

        std::vector<char> onPlane( static_cast<size_t>( work.MaxVertexId() ), 0 );
        for ( const int v : work.VertexIds() )
        {
            const glm::vec3 p = work.GetPosition( v );
            const float     d = signedDistance( p );
            if ( std::abs( d ) <= weldTolerance )
            {
                work.SetPosition( v, p - d * normal );
                onPlane[v] = 1;
            }
            else if ( d < 0.0f )
                ++farVertices;
        }
        for ( const int t : Snapshot( work.TriangleIds() ) )
        {
            const auto& c      = work.GetTriangle( t );
            bool        inside = true;
            bool        beyond = false;
            for ( const int v : c )
            {
                inside = inside && onPlane[v] != 0;
                beyond = beyond || ( onPlane[v] == 0 && signedDistance( work.GetPosition( v ) ) < 0.0f );
            }
            const bool drop = inside || ( mode == MirrorMode::CutAndMirror && beyond );
            if ( !drop )
                continue;
            if ( const EditResult r = work.RemoveTriangle( t, true ); r != EditResult::Ok )
                return Common::MakeFormattedError<MeshEditOutcome>(
                     "Mirror: triangle {} could not be removed ({})", t, ToString( r ) );
            ++( inside ? inPlaneTriangles : cutTriangles );
        }
        if ( work.TriangleCount() == 0 )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Mirror ({}): nothing is left to mirror - {} triangles lie in the plane, {} on its far side",
                 ToString( mode ), inPlaneTriangles, cutTriangles );

        std::vector<int> mirrorOf( static_cast<size_t>( work.MaxVertexId() ), InvalidId );
        std::vector<int> seam;
        for ( const int v : Snapshot( work.VertexIds() ) )
        {
            if ( work.GetVertexEdges( v ).empty() )
                continue;
            if ( onPlane[v] != 0 )
            {
                mirrorOf[v] = v;
                seam.push_back( v );
                continue;
            }
            const glm::vec3 p = work.GetPosition( v );
            mirrorOf[v]       = work.AppendVertex( p - 2.0f * signedDistance( p ) * normal );
        }
        std::vector<std::pair<int, int>> pairs;
        for ( const int t : Snapshot( work.TriangleIds() ) )
        {
            const std::array<int, 3> c = work.GetTriangle( t );
            int                      m = InvalidId;
            if ( const EditResult r = work.AppendTriangle( mirrorOf[c[0]], mirrorOf[c[2]], mirrorOf[c[1]], m );
                 r != EditResult::Ok )
                return Common::MakeFormattedError<MeshEditOutcome>(
                     "Mirror: the reflection of triangle {} cannot join the surface ({}) - an edge of it in the "
                     "plane "
                     "already has a triangle on each side",
                     t, ToString( r ) );
            EditMeshAttributes& attributes = work.Attributes();
            attributes.SetPolyGroup( m, attributes.GetPolyGroup( t ) );
            attributes.SetMaterialId( m, attributes.GetMaterialId( t ) );
            pairs.emplace_back( t, m );
        }

        EditMeshAttributes& attributes = work.Attributes();
        const auto          same       = []( const auto& value ) { return value; };
        const auto reflectNormal = [&]( const glm::vec3& n ) { return n - 2.0f * glm::dot( n, normal ) * normal; };
        const auto reflectTangent = [&]( const glm::vec4& t )
        { return glm::vec4( reflectNormal( glm::vec3( t ) ), -t.w ); };
        Common::BoolResultStr mirrored =
             MirrorLayer( work, attributes.Normals(), pairs, mirrorOf, reflectNormal, "normals" );
        if ( mirrored.IsSuccess() )
            mirrored = MirrorLayer( work, attributes.Tangents(), pairs, mirrorOf, reflectTangent, "tangents" );
        if ( mirrored.IsSuccess() )
            mirrored = MirrorLayer( work, attributes.Colors(), pairs, mirrorOf, same, "colours" );
        for ( int layer = 0; layer < attributes.UVLayerCount() && mirrored.IsSuccess(); ++layer )
            mirrored = MirrorLayer( work, attributes.UV( layer ), pairs, mirrorOf, same, "UVs" );
        if ( mirrored.IsSuccess() && !seam.empty() )
            mirrored = RebuildNormalsByPolyGroupAt( work, seam );
        if ( mirrored.IsSuccess() && !seam.empty() && ( attributes.Tangents() != nullptr ) )
        {
            std::vector<int> around;
            for ( const int v : seam )
                for ( const int t : work.GetVertexTriangles( v ) )
                    around.push_back( t );
            std::sort( around.begin(), around.end() );
            around.erase( std::unique( around.begin(), around.end() ), around.end() );
            mirrored = ComputeTangentsAt( work, around );
        }
        if ( !mirrored.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( mirrored.GetError() );
        work.Compact();

        out.Report = fmt::format( "{}: {} triangles reflected, {} seam vertices shared", ToString( mode ),
                                  pairs.size(), seam.size() );
        if ( mode == MirrorMode::CutAndMirror )
            out.Report += fmt::format( ", {} edges split and {} triangles cut away", splitEdges, cutTriangles );
        if ( inPlaneTriangles > 0 )
            out.Report += fmt::format( ", {} triangles lying in the plane dropped", inPlaneTriangles );
        if ( mode == MirrorMode::AddMirroredCopy && farVertices > 0 )
            out.Report += fmt::format( ", {} vertices were on the far side: the copy overlaps the original there",
                                       farVertices );
        return Common::MakeSuccess( std::move( out ) );
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

    const char* ToString( TrimSide side )
    {
        switch ( side )
        {
            case TrimSide::RemoveInside:
                return "Remove Inside";
            case TrimSide::RemoveOutside:
                return "Remove Outside";
        }
        return "Unknown";
    }

    namespace
    {
        struct CutHalf
        {
            EditMesh    Mesh;
            PlaneCutCap Cap;
        };

        // The half of `mesh` on the side `keep` points to, capped when asked, compacted.
        Common::ResultStr<CutHalf> KeepSide( const EditMesh& mesh, const CutPlane& keep, bool fillHole )
        {
            CutHalf           half{ mesh, {} };
            std::vector<char> inSet( static_cast<size_t>( half.Mesh.MaxTriangleId() ), 1 );
            auto cut = CutAwayPositiveSide( half.Mesh, inSet, CutPlane{ keep.Point, -keep.Normal }, fillHole,
                                            "Plane Cut" );
            if ( !cut.IsSuccess() )
                return Common::MakeError<CutHalf>( cut.GetError() );
            half.Cap = cut.GetValue();
            half.Mesh.Compact();
            return Common::MakeSuccess( std::move( half ) );
        }
    } // namespace

    Common::ResultStr<PlaneCutOutcome> PlaneCutMesh( const EditMesh& mesh, const CutPlane& plane,
                                                     PlaneCutMode mode, bool fillHole )
    {
        if ( !( glm::length( plane.Normal ) > 0.0f ) )
            return Common::MakeError<PlaneCutOutcome>( "Plane Cut: the plane has a zero normal" );
        if ( mesh.TriangleCount() == 0 )
            return Common::MakeError<PlaneCutOutcome>( "Plane Cut: the mesh has no triangle" );
        auto positive = KeepSide( mesh, plane, fillHole );
        if ( !positive.IsSuccess() )
            return Common::MakeError<PlaneCutOutcome>( positive.GetError() );
        CutHalf kept = positive.ExtractValue();
        if ( kept.Cap.RemovedTriangles == 0 || kept.Mesh.TriangleCount() == kept.Cap.CapTriangles )
            return Common::MakeFormattedError<PlaneCutOutcome>(
                 "Plane Cut: the plane does not cross the mesh - {} of its {} triangles are on the negative side",
                 kept.Cap.RemovedTriangles, mesh.TriangleCount() );

        PlaneCutOutcome out;
        out.Kept.Mesh      = std::move( kept.Mesh );
        out.Kept.Selection = ElementSelection( ElementMode::PolyGroup );
        if ( kept.Cap.Group != InvalidId )
            if ( auto r = out.Kept.Selection.Add( out.Kept.Mesh, kept.Cap.Group ); !r.IsSuccess() )
                return Common::MakeFormattedError<PlaneCutOutcome>( "Plane Cut: selecting the cap {}: {}",
                                                                    kept.Cap.Group, r.GetError() );
        out.Kept.Report =
             fmt::format( "{}: {} triangles cut away, the cap {} triangles in {} outline(s)", ToString( mode ),
                          kept.Cap.RemovedTriangles, kept.Cap.CapTriangles, kept.Cap.CapLoops );
        if ( mode == PlaneCutMode::KeepBothHalves )
        {
            auto negative = KeepSide( mesh, CutPlane{ plane.Point, -plane.Normal }, fillHole );
            if ( !negative.IsSuccess() )
                return Common::MakeError<PlaneCutOutcome>( negative.GetError() );
            out.OtherHalf = std::move( negative.ExtractValue().Mesh );
            out.Kept.Report += fmt::format( "; the other half {} triangles", out.OtherHalf->TriangleCount() );
        }
        for ( const EditMesh* half : { &out.Kept.Mesh, out.OtherHalf ? &*out.OtherHalf : nullptr } )
            if ( half != nullptr )
                if ( auto valid = half->CheckValidity(); !valid.IsSuccess() )
                    return Common::MakeFormattedError<PlaneCutOutcome>(
                         "Plane Cut: a half is not a valid mesh: {}", valid.GetError() );
        return Common::MakeSuccess( std::move( out ) );
    }

    Outcome TrimMesh( const EditMesh& mesh, const EditMesh& cutter, const glm::mat4& cutterToMesh, TrimSide side )
    {
        if ( mesh.TriangleCount() == 0 )
            return Common::MakeError<MeshEditOutcome>( "Trim: the mesh has no triangle" );
        if ( cutter.TriangleCount() == 0 )
            return Common::MakeError<MeshEditOutcome>( "Trim: the cutter has no triangle" );
        for ( const int e : cutter.EdgeIds() )
            if ( cutter.IsBoundaryEdge( e ) )
                return Common::MakeFormattedError<MeshEditOutcome>(
                     "Trim: the cutter is open at its edge {} - it must be a closed convex solid (a Boolean, "
                     "which would take any closed cutter, is not in this wave)",
                     e );

        std::vector<glm::vec3> corners( static_cast<size_t>( cutter.MaxVertexId() ) );
        float                  extent = 0.0f;
        for ( const int v : cutter.VertexIds() )
        {
            corners[v] = glm::vec3( cutterToMesh * glm::vec4( cutter.GetPosition( v ), 1.0f ) );
            extent     = std::max(
                 { extent, std::abs( corners[v].x ), std::abs( corners[v].y ), std::abs( corners[v].z ) } );
        }
        // Float noise of a transformed corner grows with its distance from the origin.
        const float tolerance = 1e-3f + 1e-6f * extent;
        // A mirroring transform turns the cutter inside out; its faces' outward normals flip with it.
        const float handed = glm::determinant( glm::mat3( cutterToMesh ) ) < 0.0f ? -1.0f : 1.0f;

        std::vector<CutPlane> planes;
        for ( const int t : cutter.TriangleIds() )
        {
            const auto&     c = cutter.GetTriangle( t );
            const glm::vec3 face =
                 handed * glm::cross( corners[c[1]] - corners[c[0]], corners[c[2]] - corners[c[0]] );
            const float area = glm::length( face );
            if ( !( area > tolerance * tolerance ) )
                continue;
            const CutPlane plane{ corners[c[0]], face / area };
            const bool     known =
                 std::any_of( planes.begin(), planes.end(),
                              [&]( const CutPlane& p )
                              {
                                  return glm::dot( p.Normal, plane.Normal ) > 1.0f - 1e-6f &&
                                         std::abs( glm::dot( plane.Point - p.Point, p.Normal ) ) <= tolerance;
                              } );
            if ( known )
                continue;
            for ( const int v : cutter.VertexIds() )
                if ( const float d = glm::dot( corners[v] - plane.Point, plane.Normal ); d > tolerance )
                    return Common::MakeFormattedError<MeshEditOutcome>(
                         "Trim: the cutter is not convex - its vertex {} is {:.3f} cm in front of the plane of "
                         "its "
                         "triangle {}; without a Boolean only a convex cutter can trim",
                         v, d, t );
            planes.push_back( plane );
        }
        if ( planes.size() < 4 )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Trim: the cutter has {} distinct face planes - a closed solid needs at least 4", planes.size() );

        MeshEditOutcome out{ mesh, ElementSelection( ElementMode::Triangle ), {} };
        EditMesh&       work = out.Mesh;
        const int       from = work.TriangleCount();
        for ( const CutPlane& plane : planes )
        {
            std::vector<char> inSet( static_cast<size_t>( work.MaxTriangleId() ), 1 );
            if ( auto split = SplitMeshAlongPlane( work, inSet, plane, "Trim" ); !split.IsSuccess() )
                return Common::MakeError<MeshEditOutcome>( split.GetError() );
        }
        std::vector<int> inside;
        std::vector<int> outside;
        for ( const int t : work.TriangleIds() )
        {
            const auto&     c = work.GetTriangle( t );
            const glm::vec3 center =
                 ( work.GetPosition( c[0] ) + work.GetPosition( c[1] ) + work.GetPosition( c[2] ) ) / 3.0f;
            const bool in = std::all_of( planes.begin(), planes.end(), [&]( const CutPlane& p )
                                         { return glm::dot( center - p.Point, p.Normal ) < -1e-4f; } );
            ( in ? inside : outside ).push_back( t );
        }
        const std::vector<int>& removed = side == TrimSide::RemoveInside ? inside : outside;
        if ( inside.empty() )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Trim: the cutter does not reach the mesh - no triangle of it is inside the cutter's {} planes",
                 planes.size() );
        if ( removed.size() == static_cast<size_t>( work.TriangleCount() ) )
            return Common::MakeFormattedError<MeshEditOutcome>( "Trim ({}): all {} triangles would be removed",
                                                                ToString( side ), removed.size() );
        const int split = work.TriangleCount() - from;
        for ( const int t : removed )
            if ( const EditResult r = work.RemoveTriangle( t, true ); r != EditResult::Ok )
                return Common::MakeFormattedError<MeshEditOutcome>( "Trim: removing triangle {} refused: {}", t,
                                                                    ToString( r ) );
        work.Compact();
        if ( auto valid = work.CheckValidity(); !valid.IsSuccess() )
            return Common::MakeFormattedError<MeshEditOutcome>( "Trim: the result is not a valid mesh: {}",
                                                                valid.GetError() );
        out.Report = fmt::format( "{}: {} cutter planes, {} triangles added by the splits, {} removed",
                                  ToString( side ), planes.size(), split, removed.size() );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry
