#include "UIPathGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace Desert::UI
{
    namespace
    {
        // Centripetal knot step (alpha = 0.5). The floor keeps coincident points from dividing by zero.
        float KnotStep( const glm::vec2& a, const glm::vec2& b )
        {
            return std::max( std::sqrt( glm::length( b - a ) ), 1e-4f );
        }

        // Barry-Goldman evaluation of the centripetal Catmull-Rom span p1 -> p2 at u in [0, 1].
        glm::vec2 CatmullRom( const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec2& p3,
                              float u )
        {
            const float t0 = 0.0f;
            const float t1 = t0 + KnotStep( p0, p1 );
            const float t2 = t1 + KnotStep( p1, p2 );
            const float t3 = t2 + KnotStep( p2, p3 );
            const float t  = t1 + ( t2 - t1 ) * u;

            const glm::vec2 a1 = ( t1 - t ) / ( t1 - t0 ) * p0 + ( t - t0 ) / ( t1 - t0 ) * p1;
            const glm::vec2 a2 = ( t2 - t ) / ( t2 - t1 ) * p1 + ( t - t1 ) / ( t2 - t1 ) * p2;
            const glm::vec2 a3 = ( t3 - t ) / ( t3 - t2 ) * p2 + ( t - t2 ) / ( t3 - t2 ) * p3;
            const glm::vec2 b1 = ( t2 - t ) / ( t2 - t0 ) * a1 + ( t - t0 ) / ( t2 - t0 ) * a2;
            const glm::vec2 b2 = ( t3 - t ) / ( t3 - t1 ) * a2 + ( t - t1 ) / ( t3 - t1 ) * a3;
            return ( t2 - t ) / ( t2 - t1 ) * b1 + ( t - t1 ) / ( t2 - t1 ) * b2;
        }
    } // namespace

    float PolylineLength( std::span<const glm::vec2> points )
    {
        float length = 0.0f;
        for ( size_t i = 1; i < points.size(); ++i )
            length += glm::length( points[i] - points[i - 1] );
        return length;
    }

    UIPathPolyline TessellateUIPath( std::span<const glm::vec2> control, bool smooth, int segmentsPerSpan )
    {
        UIPathPolyline out;
        if ( control.size() < 2 )
            return out;

        if ( !smooth || control.size() == 2 )
            out.Points.assign( control.begin(), control.end() );
        else
        {
            const int    steps = std::max( 1, segmentsPerSpan );
            const size_t n     = control.size();
            out.Points.reserve( ( n - 1 ) * static_cast<size_t>( steps ) + 1 );
            out.Points.push_back( control[0] );
            for ( size_t i = 0; i + 1 < n; ++i )
            {
                // The phantom neighbours past each end are reflections, so the curve leaves the first point
                // and arrives at the last one heading along the end segment rather than curling.
                const glm::vec2 p1 = control[i];
                const glm::vec2 p2 = control[i + 1];
                const glm::vec2 p0 = i > 0 ? control[i - 1] : p1 * 2.0f - p2;
                const glm::vec2 p3 = i + 2 < n ? control[i + 2] : p2 * 2.0f - p1;
                for ( int s = 1; s <= steps; ++s )
                    out.Points.push_back(
                         s == steps ? p2 : CatmullRom( p0, p1, p2, p3, static_cast<float>( s ) / steps ) );
            }
        }

        out.Distance.resize( out.Points.size(), 0.0f );
        for ( size_t i = 1; i < out.Points.size(); ++i )
            out.Distance[i] = out.Distance[i - 1] + glm::length( out.Points[i] - out.Points[i - 1] );
        out.Length = out.Distance.back();
        return out;
    }

    std::vector<glm::vec2> RevealUIPath( const UIPathPolyline& path, float reveal )
    {
        std::vector<glm::vec2> out;
        const float            r = std::clamp( reveal, 0.0f, 1.0f );
        if ( r <= 0.0f || path.Points.size() < 2 || path.Length <= 0.0f )
            return out;

        const float target = r * path.Length;
        out.push_back( path.Points[0] );
        for ( size_t i = 1; i < path.Points.size(); ++i )
        {
            if ( path.Distance[i] < target )
            {
                out.push_back( path.Points[i] );
                continue;
            }
            const float span = path.Distance[i] - path.Distance[i - 1];
            const float u    = span > 0.0f ? ( target - path.Distance[i - 1] ) / span : 1.0f;
            out.push_back( glm::mix( path.Points[i - 1], path.Points[i], u ) );
            break;
        }
        return out;
    }
} // namespace Desert::UI
