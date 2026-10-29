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
        // UE FFlattenBrushOp / FPinchBrushOp move a share of the gap per stamp (to the plane, to the stroke
        // line) rather than a share of the radius: at full power and full falloff, this share.
        constexpr double kGapRate = 0.25;
        // Pinch's sink along the brush normal, as a share of the Standard step (UE's pinch digs the crease it
        // draws together; without it the surface only bunches up sideways).
        constexpr double kPinchDepth = 0.25;

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
            case SculptBrushType::Move:
                return "Move";
            case SculptBrushType::Inflate:
                return "Inflate";
            case SculptBrushType::Flatten:
                return "Flatten";
            case SculptBrushType::Pinch:
                return "Pinch";
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
        // Move reaches the surface that was under the brush (the previous stamp) and carries it to this one.
        const bool isMove = settings.Brush == SculptBrushType::Move;
        if ( radius <= 0.0 || ( !isMove && settings.Strength <= 0.0f ) )
            return moved;
        const glm::dvec3 centre = isMove ? stamp.PrevCenter : stamp.Center;
        if ( isMove && glm::dot( stamp.Center - stamp.PrevCenter, stamp.Center - stamp.PrevCenter ) <= 0.0 )
            return moved;
        // Dynamic sculpt: the stamp's region gains (or sheds) resolution before it moves.
        std::vector<int> remeshed;
        if ( topology )
            remeshed = RemeshRegion( mesh, localToWorld, centre, radius, *topology );
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
            const double     f = SurfaceSculptFalloff( glm::length( w - centre ), radius, settings.Falloff );
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
        const double            sign          = settings.Invert ? -1.0 : 1.0;
        const glm::dmat3        normalToWorld = glm::transpose( toLocal );
        // A vertex's world normal (the mesh's own, through the inverse-transpose), zero where it has none.
        const auto worldNormal = [&]( int v )
        {
            const glm::dvec3 n   = normalToWorld * MeshNormals::ComputeVertexNormal( mesh, v );
            const double     len = glm::length( n );
            return len > 1e-12 ? n / len : glm::dvec3( 0.0 );
        };
        // The region's normal: the falloff-weighted mean of the ROI's vertex normals (UE's sculpt tool averages
        // the ROI normals; a single facet would tilt every stamp on a low-poly surface).
        const auto regionNormal = [&]
        {
            glm::dvec3 n( 0.0 );
            for ( const Roi& r : roi )
                n += r.Weight * worldNormal( r.Vertex );
            const double len = glm::length( n );
            return len > 1e-12 ? n / len : stamp.Normal;
        };
        switch ( settings.Brush )
        {
            case SculptBrushType::Standard:
            {
                const double     speed     = sign * radius * power * kStandardSpeed;
                const glm::dvec3 moveLocal = toLocal * ( regionNormal() * speed );
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
            case SculptBrushType::Inflate:
            {
                // UE FInflateBrushOp: NewPos = OrigPos + Falloff * Speed * VertexNormal - each vertex along its
                // own normal, so the region swells outward instead of rising as one slab.
                const double speed = sign * radius * power * kStandardSpeed;
                for ( size_t i = 0; i < roi.size(); ++i )
                    next[i] = mesh.GetVertex( roi[i].Vertex ) +
                              toLocal * ( ( roi[i].Weight * speed ) * worldNormal( roi[i].Vertex ) );
                break;
            }
            case SculptBrushType::Flatten:
            {
                // UE FFlattenBrushOp: the stamp's region plane runs through the falloff-weighted ROI centroid,
                // across the region normal; each vertex closes a share of its gap to it.
                glm::dvec3 planePoint( 0.0 );
                double     weightSum = 0.0;
                for ( const Roi& r : roi )
                {
                    planePoint += r.Weight * r.World;
                    weightSum += r.Weight;
                }
                planePoint /= weightSum;
                const glm::dvec3 planeNormal = regionNormal();
                for ( size_t i = 0; i < roi.size(); ++i )
                {
                    const double     t     = std::clamp( roi[i].Weight * power * kGapRate, 0.0, 1.0 );
                    const glm::dvec3 toPln = -glm::dot( roi[i].World - planePoint, planeNormal ) * planeNormal;
                    next[i]                = mesh.GetVertex( roi[i].Vertex ) + toLocal * ( t * toPln );
                }
                break;
            }
            case SculptBrushType::Pinch:
            {
                // UE FPinchBrushOp: each vertex pulled toward the brush across the surface (the pull's part along
                // the brush normal removed) and, once the stroke has a direction, only across it - the surface
                // gathers onto the stroke line - plus a sink along the brush normal that digs the crease.
                const glm::dvec3 brushNormal = regionNormal();
                glm::dvec3       strokeDir   = stamp.Center - stamp.PrevCenter;
                strokeDir -= glm::dot( strokeDir, brushNormal ) * brushNormal;
                const double strokeLen = glm::length( strokeDir );
                strokeDir              = strokeLen > 1e-9 ? strokeDir / strokeLen : glm::dvec3( 0.0 );
                const double depth     = -sign * radius * power * kStandardSpeed * kPinchDepth;
                for ( size_t i = 0; i < roi.size(); ++i )
                {
                    glm::dvec3 pull = stamp.Center - roi[i].World;
                    pull -= glm::dot( pull, brushNormal ) * brushNormal;
                    pull -= glm::dot( pull, strokeDir ) * strokeDir;
                    const double     w     = roi[i].Weight;
                    const double     t     = std::clamp( w * power * kGapRate, 0.0, 1.0 );
                    const glm::dvec3 world = sign * t * pull + ( w * depth ) * brushNormal;
                    next[i]                = mesh.GetVertex( roi[i].Vertex ) + toLocal * world;
                }
                break;
            }
            case SculptBrushType::Move:
            {
                // UE FMoveBrushOp: NewPos = OrigPos + Falloff * (Stamp - PrevStamp); the brush power is not
                // applied, the vertices under the brush centre stay under it.
                const glm::dvec3 moveLocal = toLocal * ( stamp.Center - stamp.PrevCenter );
                for ( size_t i = 0; i < roi.size(); ++i )
                    next[i] = mesh.GetVertex( roi[i].Vertex ) + roi[i].Weight * moveLocal;
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
