#include "DynamicMeshSculpt.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Desert::Geometry
{
    namespace
    {
        // UE MeshSculptBrushOps.h FSurfaceSculptBrushOp: the per-stamp move at full power and full falloff,
        // as a share of the radius (UE scales by Radius * Power * BrushSpeedTuning * DeltaTime; with stamps
        // laid by spacing rather than by time, the time factor folds into this constant).
        constexpr double kStandardSpeed = 0.1;

        glm::dvec3 Transform( const glm::dmat4& m, const glm::dvec3& p )
        {
            return glm::dvec3( m * glm::dvec4( p, 1.0 ) );
        }

        // Every primary-normal element on a triangle touching a moved vertex gets its one-ring average again
        // (UE: the sculpt tool's RecomputeOverlayElementNormals over the stamp's ROI).
        void RefreshNormalsAround( DynamicMesh3& mesh, const std::vector<int>& moved )
        {
            auto* normals = mesh.HasAttributes() ? mesh.Attributes()->PrimaryNormals() : nullptr;
            if ( !normals || moved.empty() )
                return;
            std::vector<int> tris;
            for ( const int v : moved )
                for ( const int t : mesh.VtxTrianglesItr( v ) )
                    tris.push_back( t );
            // A triangle beside a moved one shares its elements' one-rings too: widen by one ring.
            std::vector<int> ring = tris;
            for ( const int t : tris )
            {
                const Index3i tv = mesh.GetTriangle( t );
                for ( int j = 0; j < 3; ++j )
                    for ( const int nt : mesh.VtxTrianglesItr( tv[j] ) )
                        ring.push_back( nt );
            }
            std::vector<int> elems;
            for ( const int t : ring )
                if ( normals->IsSetTriangle( t ) )
                {
                    const Index3i el = normals->GetTriangle( t );
                    elems.insert( elems.end(), { el.A, el.B, el.C } );
                }
            std::sort( elems.begin(), elems.end() );
            elems.erase( std::unique( elems.begin(), elems.end() ), elems.end() );
            std::vector<glm::vec3> values( elems.size() );
            for ( size_t i = 0; i < elems.size(); ++i )
                values[i] = glm::vec3( MeshNormals::ComputeOverlayNormal( mesh, normals, elems[i] ) );
            for ( size_t i = 0; i < elems.size(); ++i )
                normals->SetElement( elems[i], values[i] );
        }
    } // namespace

    const char* ToString( SculptBrushType type )
    {
        switch ( type )
        {
            case SculptBrushType::Standard:
                return "Standard";
            case SculptBrushType::Smooth:
                return "Smooth";
        }
        return "?";
    }

    double SurfaceSculptFalloff( double distance, double radius, double falloff )
    {
        if ( radius <= 0.0 || distance >= radius )
            return 0.0;
        const double inner = std::clamp( 1.0 - falloff, 0.0, 1.0 );
        const double d     = distance / radius;
        if ( d <= inner )
            return 1.0;
        const double t = std::clamp( ( d - inner ) / ( 1.0 - inner ), 0.0, 1.0 );
        const double w = 1.0 - t * t;
        return w * w * w;
    }

    std::vector<int> ApplySculptStamp( DynamicMesh3& mesh, const glm::dmat4& localToWorld,
                                       const SurfaceSculptStamp& stamp, const SculptBrushSettings& settings,
                                       const RemeshSettings* topology )
    {
        std::vector<int> moved;
        const double     radius = settings.Radius;
        if ( radius <= 0.0 || settings.Strength <= 0.0f )
            return moved;
        // Dynamic sculpt: the stamp's region gains (or sheds) resolution before it moves.
        std::vector<int> remeshed;
        if ( topology )
            remeshed = RemeshRegion( mesh, localToWorld, stamp.Center, radius, *topology );
        const glm::dmat3 toLocal = glm::inverse( glm::dmat3( localToWorld ) );

        // The region of interest: every vertex inside the radius, with its world position and falloff.
        struct Roi
        {
            int        Vertex;
            glm::dvec3 World;
            double     Weight;
        };
        std::vector<Roi> roi;
        for ( const int v : mesh.VertexIndicesItr() )
        {
            const glm::dvec3 w = Transform( localToWorld, mesh.GetVertex( v ) );
            const double     f = SurfaceSculptFalloff( glm::length( w - stamp.Center ), radius, settings.Falloff );
            if ( f > 0.0 )
                roi.push_back( { v, w, f } );
        }
        if ( roi.empty() )
        {
            RefreshNormalsAround( mesh, remeshed );
            return moved;
        }

        const double power = std::clamp( static_cast<double>( settings.Strength ), 0.0, 1.0 );
        // Positions are computed from the mesh as the stamp found it and written afterwards, so a vertex's
        // move never feeds a neighbour's centroid within the same stamp (UE writes NewPositions the same way).
        std::vector<glm::dvec3> next( roi.size() );
        switch ( settings.Brush )
        {
            case SculptBrushType::Standard:
            {
                const double sign  = settings.Invert ? -1.0 : 1.0;
                const double speed = sign * radius * power * kStandardSpeed;
                // The brush normal is the region's, not the one triangle's under the cursor (UE's sculpt tool
                // averages the ROI normals): a single facet would tilt every stamp on a low-poly surface.
                const glm::dmat3 normalToWorld = glm::transpose( toLocal );
                glm::dvec3       brushNormal( 0.0 );
                for ( const Roi& r : roi )
                    brushNormal +=
                         r.Weight * ( normalToWorld * MeshNormals::ComputeVertexNormal( mesh, r.Vertex ) );
                const double len           = glm::length( brushNormal );
                brushNormal                = len > 1e-12 ? brushNormal / len : stamp.Normal;
                const glm::dvec3 moveLocal = toLocal * ( brushNormal * speed );
                for ( size_t i = 0; i < roi.size(); ++i )
                    next[i] = mesh.GetVertex( roi[i].Vertex ) + roi[i].Weight * moveLocal;
                break;
            }
            case SculptBrushType::Smooth:
            {
                for ( size_t i = 0; i < roi.size(); ++i )
                {
                    const int        v    = roi[i].Vertex;
                    const glm::dvec3 orig = mesh.GetVertex( v );
                    // An open border stays put: smoothing it would shrink the hole (UE's smooth op keeps it).
                    if ( mesh.IsBoundaryVertex( v ) )
                    {
                        next[i] = orig;
                        continue;
                    }
                    glm::dvec3 centroid( 0.0 );
                    int        count = 0;
                    for ( const int nb : mesh.VtxVerticesItr( v ) )
                    {
                        centroid += mesh.GetVertex( nb );
                        ++count;
                    }
                    if ( count == 0 )
                    {
                        next[i] = orig;
                        continue;
                    }
                    centroid /= static_cast<double>( count );
                    next[i] = glm::mix( orig, centroid, std::clamp( roi[i].Weight * power, 0.0, 1.0 ) );
                }
                break;
            }
        }
        moved.reserve( roi.size() );
        for ( size_t i = 0; i < roi.size(); ++i )
        {
            mesh.SetVertex( roi[i].Vertex, next[i] );
            moved.push_back( roi[i].Vertex );
        }
        std::vector<int> refresh = moved;
        refresh.insert( refresh.end(), remeshed.begin(), remeshed.end() );
        RefreshNormalsAround( mesh, refresh );
        return moved;
    }

    bool RaycastMesh( const DynamicMesh3& mesh, const glm::dmat4& localToWorld, const glm::dvec3& rayOrigin,
                      const glm::dvec3& rayDirection, SurfaceSculptStamp& hitOut )
    {
        // Möller-Trumbore against every triangle, in world space (the brush and the ray both live there).
        double     best = std::numeric_limits<double>::max();
        glm::dvec3 bestNormal( 0.0 );
        for ( const int t : mesh.TriangleIndicesItr() )
        {
            const Index3i    tri = mesh.GetTriangle( t );
            const glm::dvec3 a   = Transform( localToWorld, mesh.GetVertex( tri.A ) );
            const glm::dvec3 b   = Transform( localToWorld, mesh.GetVertex( tri.B ) );
            const glm::dvec3 c   = Transform( localToWorld, mesh.GetVertex( tri.C ) );
            const glm::dvec3 e1 = b - a, e2 = c - a;
            const glm::dvec3 p   = glm::cross( rayDirection, e2 );
            const double     det = glm::dot( e1, p );
            if ( std::abs( det ) < 1e-12 )
                continue;
            const double     inv = 1.0 / det;
            const glm::dvec3 s   = rayOrigin - a;
            const double     u   = glm::dot( s, p ) * inv;
            if ( u < 0.0 || u > 1.0 )
                continue;
            const glm::dvec3 q = glm::cross( s, e1 );
            const double     v = glm::dot( rayDirection, q ) * inv;
            if ( v < 0.0 || u + v > 1.0 )
                continue;
            const double dist = glm::dot( e2, q ) * inv;
            if ( dist > 0.0 && dist < best )
            {
                best       = dist;
                bestNormal = glm::cross( e1, e2 );
            }
        }
        if ( best == std::numeric_limits<double>::max() )
            return false;
        const double len = glm::length( bestNormal );
        hitOut.Center    = rayOrigin + best * rayDirection;
        hitOut.Normal    = len > 0.0 ? bestNormal / len : glm::dvec3( 0.0, 1.0, 0.0 );
        return true;
    }
} // namespace Desert::Geometry
