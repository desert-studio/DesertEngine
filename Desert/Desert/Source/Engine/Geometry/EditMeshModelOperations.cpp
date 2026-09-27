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
    } // namespace

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
