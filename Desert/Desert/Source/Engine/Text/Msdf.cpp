#include "Msdf.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Desert::Text::Msdf
{
    namespace
    {
        constexpr double kEpsilon = 1e-14;

        Vec2 operator+( Vec2 a, Vec2 b )
        {
            return { a.X + b.X, a.Y + b.Y };
        }
        Vec2 operator-( Vec2 a, Vec2 b )
        {
            return { a.X - b.X, a.Y - b.Y };
        }
        Vec2 operator*( double s, Vec2 a )
        {
            return { s * a.X, s * a.Y };
        }
        double Dot( Vec2 a, Vec2 b )
        {
            return a.X * b.X + a.Y * b.Y;
        }
        double Cross( Vec2 a, Vec2 b )
        {
            return a.X * b.Y - a.Y * b.X;
        }
        double Length( Vec2 a )
        {
            return std::sqrt( Dot( a, a ) );
        }
        Vec2 Normalize( Vec2 a )
        {
            const double len = Length( a );
            return len > 0.0 ? Vec2{ a.X / len, a.Y / len } : Vec2{ 0.0, 0.0 };
        }
        // Zero has no sign, and a zero here means "the query point is exactly ON the edge". Calling that
        // outside (the C sign convention) puts a black texel in the middle of a stroke; calling it inside
        // is the choice the field's own 0.5 encoding already makes.
        double NonZeroSign( double v )
        {
            return v >= 0.0 ? 1.0 : -1.0;
        }

        // A candidate distance plus the tie-break that decides WHICH edge a point belongs to when two
        // edges are equidistant (at a corner, always). The tie-break is |cos| between the edge's end
        // direction and the direction to the point: the more orthogonal edge wins, which is the one whose
        // pseudo-distance extension is meaningful there. Without it the winner is whichever edge the loop
        // happened to visit first, and the field flickers along every corner bisector.
        struct SignedDistance
        {
            double Distance = -std::numeric_limits<double>::max();
            double Dot      = 1.0;

            bool operator<( const SignedDistance& o ) const
            {
                return std::fabs( Distance ) < std::fabs( o.Distance ) ||
                       ( std::fabs( Distance ) == std::fabs( o.Distance ) && Dot < o.Dot );
            }
        };

        int SolveQuadratic( double ( &x )[2], double a, double b, double c )
        {
            if ( std::fabs( a ) < kEpsilon )
            {
                if ( std::fabs( b ) < kEpsilon )
                    return std::fabs( c ) < kEpsilon ? -1 : 0;
                x[0] = -c / b;
                return 1;
            }
            double disc = b * b - 4.0 * a * c;
            if ( disc > 0.0 )
            {
                disc = std::sqrt( disc );
                x[0] = ( -b + disc ) / ( 2.0 * a );
                x[1] = ( -b - disc ) / ( 2.0 * a );
                return 2;
            }
            if ( disc == 0.0 )
            {
                x[0] = -b / ( 2.0 * a );
                return 1;
            }
            return 0;
        }

        // Cardano on the depressed cubic. Roots of t^3 + a t^2 + b t + c.
        int SolveCubicNormed( double ( &x )[3], double a, double b, double c )
        {
            const double a2 = a * a;
            double       q  = ( a2 - 3.0 * b ) / 9.0;
            const double r  = ( a * ( 2.0 * a2 - 9.0 * b ) + 27.0 * c ) / 54.0;
            const double r2 = r * r;
            const double q3 = q * q * q;
            if ( r2 < q3 )
            {
                double t = r / std::sqrt( q3 );
                t        = std::clamp( t, -1.0, 1.0 );
                t        = std::acos( t );
                q        = -2.0 * std::sqrt( q );
                x[0]     = q * std::cos( t / 3.0 ) - a / 3.0;
                x[1]     = q * std::cos( ( t + 2.0 * 3.14159265358979323846 ) / 3.0 ) - a / 3.0;
                x[2]     = q * std::cos( ( t - 2.0 * 3.14159265358979323846 ) / 3.0 ) - a / 3.0;
                return 3;
            }
            double A = -std::pow( std::fabs( r ) + std::sqrt( r2 - q3 ), 1.0 / 3.0 );
            if ( r < 0.0 )
                A = -A;
            const double B = A == 0.0 ? 0.0 : q / A;
            x[0]           = ( A + B ) - a / 3.0;
            x[1]           = -0.5 * ( A + B ) - a / 3.0;
            x[2]           = 0.5 * std::sqrt( 3.0 ) * ( A - B );
            return std::fabs( x[2] ) < kEpsilon ? 2 : 1;
        }

        int SolveCubic( double ( &x )[3], double a, double b, double c, double d )
        {
            if ( std::fabs( a ) < kEpsilon )
            {
                double    xx[2];
                const int n = SolveQuadratic( xx, b, c, d );
                for ( int i = 0; i < n && i < 2; ++i )
                    x[i] = xx[i];
                return n;
            }
            return SolveCubicNormed( x, b / a, c / a, d / a );
        }

        // Signed distance from `origin` to one segment, plus the curve parameter of the closest point.
        // `param` may leave [0,1]: that is what tells the pseudo-distance step below that the point lies
        // off the end of the segment and must be measured against its infinite extension instead.
        SignedDistance SegmentSignedDistance( const EdgeSegment& e, Vec2 origin, double& param )
        {
            if ( e.PointCount == 2 )
            {
                const Vec2   aq               = origin - e.P[0];
                const Vec2   ab               = e.P[1] - e.P[0];
                const double d2               = Dot( ab, ab );
                param                         = d2 > 0.0 ? Dot( aq, ab ) / d2 : 0.0;
                const Vec2   eq               = e.P[param > 0.5 ? 1 : 0] - origin;
                const double endpointDistance = Length( eq );
                if ( param > 0.0 && param < 1.0 )
                {
                    // Orthogonal distance to the line, signed by which side of it the point is on.
                    const Vec2   ortho{ ab.Y / Length( ab ), -ab.X / Length( ab ) };
                    const double orthoDistance = Dot( ortho, aq );
                    if ( std::fabs( orthoDistance ) < endpointDistance )
                        return { orthoDistance, 0.0 };
                }
                return { NonZeroSign( Cross( aq, ab ) ) * endpointDistance,
                         std::fabs( Dot( Normalize( ab ), Normalize( eq ) ) ) };
            }

            if ( e.PointCount == 3 )
            {
                const Vec2 qa = e.P[0] - origin;
                const Vec2 ab = e.P[1] - e.P[0];
                const Vec2 br = ( e.P[2] - e.P[1] ) - ab;

                // d/dt |B(t) - origin|^2 = 0 is a cubic in t; its roots are the interior candidates.
                const double a = Dot( br, br );
                const double b = 3.0 * Dot( ab, br );
                const double c = 2.0 * Dot( ab, ab ) + Dot( qa, br );
                const double d = Dot( qa, ab );
                double       t[3]{};
                const int    solutions = SolveCubic( t, a, b, c, d );

                Vec2   epDir       = e.Direction( 0.0 );
                double minDistance = NonZeroSign( Cross( epDir, qa ) ) * Length( qa );
                param              = -Dot( qa, epDir ) / Dot( epDir, epDir );
                {
                    epDir                 = e.Direction( 1.0 );
                    const Vec2   bq       = e.P[2] - origin;
                    const double distance = Length( bq );
                    if ( distance < std::fabs( minDistance ) )
                    {
                        minDistance = NonZeroSign( Cross( epDir, bq ) ) * distance;
                        param       = Dot( origin - e.P[1], epDir ) / Dot( epDir, epDir );
                    }
                }
                for ( int i = 0; i < solutions; ++i )
                {
                    if ( t[i] > 0.0 && t[i] < 1.0 )
                    {
                        const Vec2   qe       = qa + ( 2.0 * t[i] ) * ab + ( t[i] * t[i] ) * br;
                        const double distance = Length( qe );
                        if ( distance <= std::fabs( minDistance ) )
                        {
                            minDistance = NonZeroSign( Cross( ab + t[i] * br, qe ) ) * distance;
                            param       = t[i];
                        }
                    }
                }

                if ( param >= 0.0 && param <= 1.0 )
                    return { minDistance, 0.0 };
                if ( param < 0.5 )
                    return { minDistance, std::fabs( Dot( Normalize( e.Direction( 0.0 ) ), Normalize( qa ) ) ) };
                return { minDistance,
                         std::fabs( Dot( Normalize( e.Direction( 1.0 ) ), Normalize( e.P[2] - origin ) ) ) };
            }

            // Cubic: the same minimisation has no closed form, so it is a Newton refinement from a set of
            // evenly spaced starts. Four starts x eight steps converges to well under a thousandth of a
            // texel on real glyph outlines, and the endpoints are seeded separately so a start that walks
            // out of [0,1] cannot lose the true minimum.
            const Vec2 qa = e.P[0] - origin;
            const Vec2 ab = e.P[1] - e.P[0];
            const Vec2 br = ( e.P[2] - e.P[1] ) - ab;
            const Vec2 as = ( e.P[3] - e.P[2] ) - ( e.P[2] - e.P[1] ) - br;

            Vec2   epDir       = e.Direction( 0.0 );
            double minDistance = NonZeroSign( Cross( epDir, qa ) ) * Length( qa );
            param              = -Dot( qa, epDir ) / Dot( epDir, epDir );
            {
                epDir                 = e.Direction( 1.0 );
                const Vec2   bq       = e.P[3] - origin;
                const double distance = Length( bq );
                if ( distance < std::fabs( minDistance ) )
                {
                    minDistance = NonZeroSign( Cross( epDir, bq ) ) * distance;
                    param       = Dot( epDir - bq, epDir ) / Dot( epDir, epDir );
                }
            }

            constexpr int kSearchStarts = 4;
            constexpr int kSearchSteps  = 8;
            for ( int i = 0; i <= kSearchStarts; ++i )
            {
                double t  = static_cast<double>( i ) / kSearchStarts;
                Vec2   qe = qa + ( 3.0 * t ) * ab + ( 3.0 * t * t ) * br + ( t * t * t ) * as;
                for ( int step = 0; step < kSearchSteps; ++step )
                {
                    const Vec2   d1    = ( 3.0 * ab ) + ( 6.0 * t ) * br + ( 3.0 * t * t ) * as;
                    const Vec2   d2    = ( 6.0 * br ) + ( 6.0 * t ) * as;
                    const double denom = Dot( d1, d1 ) + Dot( qe, d2 );
                    if ( std::fabs( denom ) < kEpsilon )
                        break;
                    t -= Dot( qe, d1 ) / denom;
                    if ( t <= 0.0 || t >= 1.0 )
                        break;
                    qe                    = qa + ( 3.0 * t ) * ab + ( 3.0 * t * t ) * br + ( t * t * t ) * as;
                    const double distance = Length( qe );
                    if ( distance < std::fabs( minDistance ) )
                    {
                        minDistance = NonZeroSign( Cross( d1, qe ) ) * distance;
                        param       = t;
                    }
                }
            }

            if ( param >= 0.0 && param <= 1.0 )
                return { minDistance, 0.0 };
            if ( param < 0.5 )
                return { minDistance, std::fabs( Dot( Normalize( e.Direction( 0.0 ) ), Normalize( qa ) ) ) };
            return { minDistance,
                     std::fabs( Dot( Normalize( e.Direction( 1.0 ) ), Normalize( e.P[3] - origin ) ) ) };
        }

        // Past the end of a segment, the plain distance bends around the endpoint and every channel's
        // field acquires a false circular arc there. The PSEUDO-distance extends the segment's end
        // tangent to infinity instead, so a channel that does not own this corner reports a straight
        // half-plane and the median can still find the true intersection. This is the step that makes a
        // corner sharp rather than merely un-rounded.
        void ToPseudoDistance( SignedDistance& distance, const EdgeSegment& e, Vec2 origin, double param )
        {
            if ( param < 0.0 )
            {
                const Vec2   dir = Normalize( e.Direction( 0.0 ) );
                const Vec2   aq  = origin - e.Point( 0.0 );
                const double ts  = Dot( aq, dir );
                if ( ts < 0.0 )
                {
                    const double pseudo = Cross( aq, dir );
                    if ( std::fabs( pseudo ) <= std::fabs( distance.Distance ) )
                    {
                        distance.Distance = pseudo;
                        distance.Dot      = 0.0;
                    }
                }
            }
            else if ( param > 1.0 )
            {
                const Vec2   dir = Normalize( e.Direction( 1.0 ) );
                const Vec2   bq  = origin - e.Point( 1.0 );
                const double ts  = Dot( bq, dir );
                if ( ts > 0.0 )
                {
                    const double pseudo = Cross( bq, dir );
                    if ( std::fabs( pseudo ) <= std::fabs( distance.Distance ) )
                    {
                        distance.Distance = pseudo;
                        distance.Dot      = 0.0;
                    }
                }
            }
        }

        bool IsCorner( Vec2 aDir, Vec2 bDir, double crossThreshold )
        {
            return Dot( aDir, bDir ) <= 0.0 || std::fabs( Cross( aDir, bDir ) ) > crossThreshold;
        }

        // Step to the next colour for a new spline. `banned` is the colour the new spline must not share
        // with (used to stop the last spline of a closed contour from matching the first, which would
        // silently un-sharpen that one corner).
        void SwitchColor( EdgeColor& color, uint64_t& seed, EdgeColor banned = EdgeColor::Black )
        {
            const auto          c        = static_cast<unsigned char>( color );
            const auto          b        = static_cast<unsigned char>( banned );
            const unsigned char combined = static_cast<unsigned char>( c & b );
            if ( combined == 1 || combined == 2 || combined == 4 )
            {
                color = static_cast<EdgeColor>( combined ^ 7 );
                return;
            }
            if ( color == EdgeColor::Black || color == EdgeColor::White )
            {
                static const EdgeColor kStart[3] = { EdgeColor::Cyan, EdgeColor::Magenta, EdgeColor::Yellow };
                color                            = kStart[seed % 3];
                seed /= 3;
                return;
            }
            const int shifted = c << ( 1 + ( seed & 1 ) );
            color             = static_cast<EdgeColor>( ( shifted | shifted >> 3 ) & 7 );
            seed >>= 1;
        }

        // Split one segment into three at t = 1/3, 2/3. Only needed so that a contour with a single
        // corner and fewer than three edges still has three splines to colour; without it a two-edge
        // teardrop (which real fonts do contain) would get two colours and lose its one corner.
        void SplitInThirds( const EdgeSegment& e, EdgeSegment ( &parts )[3] )
        {
            auto lerp = []( Vec2 a, Vec2 b, double t ) { return a + t * ( b - a ); };
            for ( int i = 0; i < 3; ++i )
            {
                parts[i].PointCount = e.PointCount;
                parts[i].Color      = e.Color;
            }
            const double t0 = 1.0 / 3.0, t1 = 2.0 / 3.0;
            if ( e.PointCount == 2 )
            {
                parts[0].P[0] = e.P[0];
                parts[0].P[1] = e.Point( t0 );
                parts[1].P[0] = e.Point( t0 );
                parts[1].P[1] = e.Point( t1 );
                parts[2].P[0] = e.Point( t1 );
                parts[2].P[1] = e.P[1];
                return;
            }
            if ( e.PointCount == 3 )
            {
                parts[0].P[0] = e.P[0];
                parts[0].P[1] = lerp( e.P[0], e.P[1], t0 );
                parts[0].P[2] = e.Point( t0 );
                parts[1].P[0] = e.Point( t0 );
                parts[1].P[1] = lerp( lerp( e.P[0], e.P[1], t1 ), lerp( e.P[1], e.P[2], t0 ), 0.5 );
                parts[1].P[2] = e.Point( t1 );
                parts[2].P[0] = e.Point( t1 );
                parts[2].P[1] = lerp( e.P[1], e.P[2], t1 );
                parts[2].P[2] = e.P[2];
                return;
            }
            // Cubic: de Casteljau at each split point.
            auto cubicAt = [&]( double a, double b, double c, double d )
            {
                return Vec2{ a * e.P[0].X + b * e.P[1].X + c * e.P[2].X + d * e.P[3].X,
                             a * e.P[0].Y + b * e.P[1].Y + c * e.P[2].Y + d * e.P[3].Y };
            };
            parts[0].P[0] = e.P[0];
            parts[0].P[1] = cubicAt( 2.0 / 3.0, 1.0 / 3.0, 0.0, 0.0 );
            parts[0].P[2] = cubicAt( 4.0 / 9.0, 4.0 / 9.0, 1.0 / 9.0, 0.0 );
            parts[0].P[3] = e.Point( t0 );
            parts[1].P[0] = e.Point( t0 );
            parts[1].P[1] = cubicAt( 8.0 / 27.0, 12.0 / 27.0, 6.0 / 27.0, 1.0 / 27.0 );
            parts[1].P[2] = cubicAt( 1.0 / 27.0, 6.0 / 27.0, 12.0 / 27.0, 8.0 / 27.0 );
            parts[1].P[3] = e.Point( t1 );
            parts[2].P[0] = e.Point( t1 );
            parts[2].P[1] = cubicAt( 0.0, 1.0 / 9.0, 4.0 / 9.0, 4.0 / 9.0 );
            parts[2].P[2] = cubicAt( 0.0, 0.0, 1.0 / 3.0, 2.0 / 3.0 );
            parts[2].P[3] = e.P[3];
        }

        struct EdgePoint
        {
            SignedDistance     MinDistance;
            const EdgeSegment* NearEdge  = nullptr;
            double             NearParam = 0.0;
        };

        bool HasChannel( EdgeColor c, unsigned char mask )
        {
            return ( static_cast<unsigned char>( c ) & mask ) != 0;
        }

        // A texel where the three channels disagree ACROSS a neighbour is a "clash": the median flips
        // there and paints an isolated dot or notch that no outline accounts for. It is the one artefact
        // MSDF adds over a plain SDF, it happens where a feature is thinner than the sampling grid, and
        // the cure is to make that texel single-channel again (median in all three), which degrades it to
        // the ordinary distance field exactly where the extra channels could not be trusted anyway.
        bool DetectClash( const float* a, const float* b, double threshold )
        {
            float a0 = a[0], a1 = a[1], a2 = a[2];
            float b0 = b[0], b1 = b[1], b2 = b[2];
            if ( std::fabs( b0 - a0 ) < std::fabs( b1 - a1 ) )
            {
                std::swap( a0, a1 );
                std::swap( b0, b1 );
            }
            if ( std::fabs( b1 - a1 ) < std::fabs( b2 - a2 ) )
            {
                std::swap( a1, a2 );
                std::swap( b1, b2 );
                if ( std::fabs( b0 - a0 ) < std::fabs( b1 - a1 ) )
                {
                    std::swap( a0, a1 );
                    std::swap( b0, b1 );
                }
            }
            return std::fabs( b1 - a1 ) >= threshold && !( b0 == b1 && b0 == b2 ) &&
                   std::fabs( a2 - 0.5f ) >= std::fabs( b2 - 0.5f );
        }

        void CorrectErrors( std::vector<float>& rgb, int w, int h, double threshold )
        {
            std::vector<int> clashes;
            for ( int y = 0; y < h; ++y )
            {
                for ( int x = 0; x < w; ++x )
                {
                    const float* p = &rgb[( static_cast<size_t>( y ) * w + x ) * 3];
                    const bool   clash =
                         ( x > 0 && DetectClash( p, p - 3, threshold ) ) ||
                         ( x < w - 1 && DetectClash( p, p + 3, threshold ) ) ||
                         ( y > 0 && DetectClash( p, p - static_cast<size_t>( w ) * 3, threshold ) ) ||
                         ( y < h - 1 && DetectClash( p, p + static_cast<size_t>( w ) * 3, threshold ) );
                    if ( clash )
                        clashes.push_back( y * w + x );
                }
            }
            for ( int idx : clashes )
            {
                float*      p   = &rgb[static_cast<size_t>( idx ) * 3];
                const float med = Median( p[0], p[1], p[2] );
                p[0] = p[1] = p[2] = med;
            }
        }
    } // namespace

    Vec2 EdgeSegment::Point( double t ) const
    {
        if ( PointCount == 2 )
            return P[0] + t * ( P[1] - P[0] );
        if ( PointCount == 3 )
        {
            const Vec2 a = P[0] + t * ( P[1] - P[0] );
            const Vec2 b = P[1] + t * ( P[2] - P[1] );
            return a + t * ( b - a );
        }
        const Vec2 a  = P[0] + t * ( P[1] - P[0] );
        const Vec2 b  = P[1] + t * ( P[2] - P[1] );
        const Vec2 c  = P[2] + t * ( P[3] - P[2] );
        const Vec2 ab = a + t * ( b - a );
        const Vec2 bc = b + t * ( c - b );
        return ab + t * ( bc - ab );
    }

    Vec2 EdgeSegment::Direction( double t ) const
    {
        if ( PointCount == 2 )
            return P[1] - P[0];
        if ( PointCount == 3 )
        {
            const Vec2 d = ( P[1] - P[0] ) + t * ( ( P[2] - P[1] ) - ( P[1] - P[0] ) );
            // A degenerate control point makes the derivative vanish at an end; the chord is the only
            // direction left, and returning zero instead would make every angle test at that join lie.
            if ( std::fabs( d.X ) < kEpsilon && std::fabs( d.Y ) < kEpsilon )
                return P[2] - P[0];
            return d;
        }
        const Vec2 a  = P[1] - P[0];
        const Vec2 b  = P[2] - P[1];
        const Vec2 c  = P[3] - P[2];
        const Vec2 ab = a + t * ( b - a );
        const Vec2 bc = b + t * ( c - b );
        const Vec2 d  = ab + t * ( bc - ab );
        if ( std::fabs( d.X ) < kEpsilon && std::fabs( d.Y ) < kEpsilon )
        {
            if ( t == 0.0 )
                return P[2] - P[0];
            if ( t == 1.0 )
                return P[3] - P[1];
        }
        return d;
    }

    float Median( float a, float b, float c )
    {
        return std::max( std::min( a, b ), std::min( std::max( a, b ), c ) );
    }

    void ColorEdges( Shape& shape, double angleThresholdRad, uint64_t seed )
    {
        const double     crossThreshold = std::sin( angleThresholdRad );
        EdgeColor        color          = EdgeColor::White;
        std::vector<int> corners;

        for ( Contour& contour : shape.Contours )
        {
            if ( contour.Edges.empty() )
                continue;

            corners.clear();
            {
                Vec2 prevDirection = contour.Edges.back().Direction( 1.0 );
                int  index         = 0;
                for ( const EdgeSegment& e : contour.Edges )
                {
                    if ( IsCorner( Normalize( prevDirection ), Normalize( e.Direction( 0.0 ) ), crossThreshold ) )
                        corners.push_back( index );
                    prevDirection = e.Direction( 1.0 );
                    ++index;
                }
            }

            if ( corners.empty() )
            {
                // A smooth closed contour ('O', 'o', a bowl) has no corner to preserve, so all three
                // channels carry the same field and the median degrades to the ordinary distance.
                for ( EdgeSegment& e : contour.Edges )
                    e.Color = EdgeColor::White;
                continue;
            }

            if ( corners.size() == 1 )
            {
                // Teardrop: one corner, so the contour needs three splines around it to keep the two
                // incident edges in different channels.
                EdgeColor colors[3]{};
                SwitchColor( color, seed );
                colors[0] = color;
                colors[1] = EdgeColor::White;
                SwitchColor( color, seed );
                colors[2] = color;

                int corner = corners[0];
                int m      = static_cast<int>( contour.Edges.size() );
                if ( m < 3 )
                {
                    // Fewer edges than splines: subdivide, rotating the corner to index 0 as we go, so
                    // the band assignment below needs no second case.
                    std::vector<EdgeSegment> split;
                    split.reserve( static_cast<size_t>( m ) * 3 );
                    for ( int i = 0; i < m; ++i )
                    {
                        EdgeSegment parts[3];
                        SplitInThirds( contour.Edges[( corner + i ) % m], parts );
                        split.push_back( parts[0] );
                        split.push_back( parts[1] );
                        split.push_back( parts[2] );
                    }
                    contour.Edges = std::move( split );
                    m             = static_cast<int>( contour.Edges.size() );
                    corner        = 0;
                }
                // Three equal bands around the contour starting AT the corner: the first and last bands
                // are the two edges that meet there, and they are in different channels by construction.
                for ( int i = 0; i < m; ++i )
                {
                    const int band                          = std::clamp( static_cast<int>( 3.0 * i / m ), 0, 2 );
                    contour.Edges[( corner + i ) % m].Color = colors[band];
                }
                continue;
            }

            const int cornerCount = static_cast<int>( corners.size() );
            const int m           = static_cast<int>( contour.Edges.size() );
            int       spline      = 0;
            const int start       = corners[0];
            SwitchColor( color, seed );
            const EdgeColor initialColor = color;
            for ( int i = 0; i < m; ++i )
            {
                const int index = ( start + i ) % m;
                if ( spline + 1 < cornerCount && corners[spline + 1] == index )
                {
                    ++spline;
                    SwitchColor( color, seed, spline == cornerCount - 1 ? initialColor : EdgeColor::Black );
                }
                contour.Edges[index].Color = color;
            }
        }
    }

    void GenerateMSDF( std::vector<float>& outRGB, int w, int h, const Shape& shape, double rangeTexels )
    {
        outRGB.assign( static_cast<size_t>( w ) * h * 3, 0.0f );
        if ( w <= 0 || h <= 0 || rangeTexels <= 0.0 )
            return;

        for ( int y = 0; y < h; ++y )
        {
            for ( int x = 0; x < w; ++x )
            {
                const Vec2 p{ x + 0.5, y + 0.5 };
                EdgePoint  r, g, b;

                for ( const Contour& contour : shape.Contours )
                {
                    for ( const EdgeSegment& e : contour.Edges )
                    {
                        double               param    = 0.0;
                        const SignedDistance distance = SegmentSignedDistance( e, p, param );
                        if ( HasChannel( e.Color, 1 ) && distance < r.MinDistance )
                            r = { distance, &e, param };
                        if ( HasChannel( e.Color, 2 ) && distance < g.MinDistance )
                            g = { distance, &e, param };
                        if ( HasChannel( e.Color, 4 ) && distance < b.MinDistance )
                            b = { distance, &e, param };
                    }
                }

                if ( r.NearEdge )
                    ToPseudoDistance( r.MinDistance, *r.NearEdge, p, r.NearParam );
                if ( g.NearEdge )
                    ToPseudoDistance( g.MinDistance, *g.NearEdge, p, g.NearParam );
                if ( b.NearEdge )
                    ToPseudoDistance( b.MinDistance, *b.NearEdge, p, b.NearParam );

                float* out = &outRGB[( static_cast<size_t>( y ) * w + x ) * 3];
                out[0]     = static_cast<float>( r.MinDistance.Distance / rangeTexels + 0.5 );
                out[1]     = static_cast<float>( g.MinDistance.Distance / rangeTexels + 0.5 );
                out[2]     = static_cast<float>( b.MinDistance.Distance / rangeTexels + 0.5 );
            }
        }

        // 1.001 rather than 1: a clash is a disagreement of more than one texel of distance between
        // neighbours, and the slack keeps an exactly-one-texel step (which is the legitimate slope of a
        // distance field) from being flagged on every edge in the glyph.
        CorrectErrors( outRGB, w, h, 1.001 / rangeTexels );
    }

    void GenerateSDF( std::vector<float>& outR, int w, int h, const Shape& shape, double rangeTexels )
    {
        outR.assign( static_cast<size_t>( w ) * h, 0.0f );
        if ( w <= 0 || h <= 0 || rangeTexels <= 0.0 )
            return;

        for ( int y = 0; y < h; ++y )
        {
            for ( int x = 0; x < w; ++x )
            {
                const Vec2     p{ x + 0.5, y + 0.5 };
                SignedDistance best;
                for ( const Contour& contour : shape.Contours )
                    for ( const EdgeSegment& e : contour.Edges )
                    {
                        double               param    = 0.0;
                        const SignedDistance distance = SegmentSignedDistance( e, p, param );
                        if ( distance < best )
                            best = distance;
                    }
                outR[static_cast<size_t>( y ) * w + x] = static_cast<float>( best.Distance / rangeTexels + 0.5 );
            }
        }
    }
} // namespace Desert::Text::Msdf
