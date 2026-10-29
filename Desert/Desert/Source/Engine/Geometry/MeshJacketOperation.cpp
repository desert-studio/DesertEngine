// UE 5.8 MeshModelingTools RemoveOccludedTrianglesTool.cpp + DynamicMesh/Operations/RemoveOccludedTriangles.h
// (TRemoveOccludedTriangles::Apply) on our DynamicMesh3, the mesh occluding only itself (bOnlySelfOcclude): the
// samples of every triangle are pushed a tolerance off the surface and tested - by the generalized winding
// number against the Winding Iso Value (FastWindingTree), or by casting the six axis rays plus Add Random Rays
// sphere directions against the mesh's AABB tree, a sample being hidden when no ray escapes. A triangle with all
// samples hidden is removed. UE offsets a vertex sample along the VERTEX normal; here every sample of a triangle
// is offset along that TRIANGLE's normal, so a sample is tested on the side its own triangle faces (the same
// answer for closed shells, and no averaged normal at a sharp corner pointing into a neighbouring face).
// Not ported: UE's ShrinkRemoval / MinAreaIsland / MinTriCountIsland clean-up passes, its "Set New PolyGroup"
// action (the removal only), and occlusion by the other meshes of the selection.
#include "MeshJacketOperation.hpp"

#include "Engine/Geometry/MeshCore/DynamicMeshEditor.hpp"
#include "Engine/Geometry/MeshCore/Spatial/FastWinding.hpp"
#include "Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <random>
#include <vector>

namespace Desert::Geometry
{
    const char* ToString( JacketOcclusionMode mode )
    {
        switch ( mode )
        {
            case JacketOcclusionMode::WindingNumber:
                return "Winding Number";
            case JacketOcclusionMode::RaycastOcclusionSamples:
                return "Raycast Occlusion Samples";
        }
        return "Unknown";
    }

    const char* ToString( JacketTriangleSampling sampling )
    {
        switch ( sampling )
        {
            case JacketTriangleSampling::Vertices:
                return "Vertices";
            case JacketTriangleSampling::VerticesAndCentroids:
                return "Vertices and Centroids";
        }
        return "Unknown";
    }

    Common::ResultStr<DeformOutcome> Jacket( const DynamicMesh3& input, const JacketSettings& settings )
    {
        if ( settings.WindingIsoValue < 0.0f || settings.WindingIsoValue > 1.0f || settings.AddRandomRays < 0 ||
             settings.AddRandomRays > 1000 || settings.AddTriangleSamples < 0 ||
             settings.AddTriangleSamples > 1000 )
            return Common::MakeFormattedError<DeformOutcome>( "Jacket: Winding Iso Value must be in [0, 1] (is "
                                                              "{}), Add Random Rays and Add Triangle Samples in "
                                                              "[0, 1000] (are {} and {})",
                                                              settings.WindingIsoValue, settings.AddRandomRays,
                                                              settings.AddTriangleSamples );
        if ( input.TriangleCount() == 0 )
            return Common::MakeError<DeformOutcome>( "Jacket: the mesh has no triangle" );

        DynamicMeshAABBTree3 tree( &input );
        FastWindingTree      winding( &tree, settings.OcclusionMode == JacketOcclusionMode::WindingNumber );

        // UE: the offset is a small tolerance; relative to the mesh so a 1 cm part and a 100 m hull both work.
        const glm::dvec3 extents = input.GetBounds().Extents();
        const double     size    = std::max( { extents.x, extents.y, extents.z, 1e-6 } );
        const double     offset  = size * 1e-4;

        // UE: a fixed-seed stream, so the same settings remove the same triangles every time.
        std::mt19937                     rng( 31337u );
        std::normal_distribution<double> gauss( 0.0, 1.0 );
        std::vector<glm::dvec3>          rays = { { 1, 0, 0 },  { -1, 0, 0 }, { 0, 1, 0 },
                                                  { 0, -1, 0 }, { 0, 0, 1 },  { 0, 0, -1 } };
        for ( int i = 0; i < settings.AddRandomRays; ++i )
        {
            glm::dvec3   d( gauss( rng ), gauss( rng ), gauss( rng ) );
            const double len = glm::length( d );
            if ( len > 1e-9 )
                rays.push_back( d / len );
        }

        const auto hidden = [&]( const glm::dvec3& point )
        {
            if ( settings.OcclusionMode == JacketOcclusionMode::WindingNumber )
                return winding.FastWindingNumber( point ) > static_cast<double>( settings.WindingIsoValue );
            for ( const glm::dvec3& dir : rays )
            {
                double t = 0.0;
                if ( tree.FindNearestHitTriangle( point, dir, t ) < 0 )
                    return false; // this ray escapes: the point is seen from outside
            }
            return true;
        };

        std::uniform_real_distribution<double> unit( 0.0, 1.0 );
        std::vector<int>                       removed;
        for ( const int tid : input.TriangleIndicesItr() )
        {
            glm::dvec3 a{}, b{}, c{};
            input.GetTriVertices( tid, a, b, c );
            const glm::dvec3 n     = input.GetTriNormal( tid );
            const auto       test  = [&]( const glm::dvec3& p ) { return hidden( p + n * offset ); };
            bool             isHid = test( a ) && test( b ) && test( c );
            if ( isHid && settings.TriangleSampling == JacketTriangleSampling::VerticesAndCentroids )
                isHid = test( ( a + b + c ) / 3.0 );
            for ( int s = 0; isHid && s < settings.AddTriangleSamples; ++s )
            {
                double u = unit( rng ), v = unit( rng );
                if ( u + v > 1.0 )
                {
                    u = 1.0 - u;
                    v = 1.0 - v;
                }
                isHid = test( a + ( b - a ) * u + ( c - a ) * v );
            }
            if ( isHid )
                removed.push_back( tid );
        }

        DeformOutcome out{ input };
        if ( !removed.empty() )
        {
            const DynamicMeshEditor editor( &out.Mesh );
            if ( !editor.RemoveTriangles( removed, true ) )
                return Common::MakeFormattedError<DeformOutcome>(
                     "Jacket: removing the {} hidden triangles failed (non-manifold result)", removed.size() );
        }
        out.MovedVertices = static_cast<int32_t>( removed.size() );
        out.Report =
             std::format( "Jacket ({}, {}): {} of {} triangles removed", ToString( settings.OcclusionMode ),
                          ToString( settings.TriangleSampling ), removed.size(), input.TriangleCount() );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry
