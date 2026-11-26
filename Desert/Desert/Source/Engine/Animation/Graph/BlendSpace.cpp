#include "BlendSpace.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <numeric>

// The blend space's refusal, its derived triangulation, the weights at an axis point, the per-axis input filter,
// the weight smoothing and the synced phase: the whole of UE's UBlendSpace evaluation that is not sampling
// (Animator.cpp samples the clips). One body for the 1D and the 2D space.
namespace Desert::Animation::Graph
{
    namespace
    {
        using BlendPoint2 = std::array<double, 2>;

        /// Below this a normalized triangle has no area and a point is on a circumcircle rather than inside it.
        constexpr double kBlendPlaneEpsilon = 1e-9;

        /// Twice the signed area of (o, a, b): positive when counter-clockwise.
        double BlendCross2D( const BlendPoint2& o, const BlendPoint2& a, const BlendPoint2& b )
        {
            return ( a[0] - o[0] ) * ( b[1] - o[1] ) - ( a[1] - o[1] ) * ( b[0] - o[0] );
        }

        /// Whether `p` lies strictly inside the circumcircle of the counter-clockwise triangle (a, b, c).
        bool InBlendCircumcircle( const BlendPoint2& a, const BlendPoint2& b, const BlendPoint2& c,
                                  const BlendPoint2& p )
        {
            const double ax  = a[0] - p[0];
            const double ay  = a[1] - p[1];
            const double bx  = b[0] - p[0];
            const double by  = b[1] - p[1];
            const double cx  = c[0] - p[0];
            const double cy  = c[1] - p[1];
            const double det = ( ax * ax + ay * ay ) * ( bx * cy - cx * by ) -
                               ( bx * bx + by * by ) * ( ax * cy - cx * ay ) +
                               ( cx * cx + cy * cy ) * ( ax * by - bx * ay );
            return det > kBlendPlaneEpsilon;
        }

        BlendPoint2 BlendPointOf( const std::array<float, 2>& point )
        {
            return BlendPoint2{ static_cast<double>( point[0] ), static_cast<double>( point[1] ) };
        }

        /// `value` normalized into the axis's unit range (UE SetGridBox).
        double NormalizedOn( const BlendSpaceAxis& axis, const double value )
        {
            return ( value - static_cast<double>( axis.Min ) ) /
                   ( static_cast<double>( axis.Max ) - static_cast<double>( axis.Min ) );
        }

        /// The difference `to - from` taken the short way round a wrapping axis (the plain one otherwise).
        float AxisDelta( const BlendSpaceAxis& axis, const float from, const float to )
        {
            const float delta = to - from;
            if ( !axis.Wrap )
                return delta;
            const float range = axis.Max - axis.Min;
            return delta - range * std::round( delta / range );
        }

        /// The weights of a 1D row at `x` (already clamped): the two samples around it linearly, from the
        /// row's ascending segments (TriangulateBlendSpace's Hull). The arithmetic is on the raw sample X so
        /// the weights of a row are those it always had.
        void RowWeights( const BlendSpaceSetup& setup, const BlendSpaceTriangulation& triangulation, const float x,
                         std::vector<float>& out )
        {
            const size_t count = setup.Samples.size();
            if ( triangulation.Hull.empty() )
            {
                if ( count > 0 )
                    out[0] = 1.0F; // a row of one sample
                return;
            }
            const uint32_t first = triangulation.Hull.front()[0];
            const uint32_t last  = triangulation.Hull.back()[1];
            if ( first >= count || last >= count )
                return;
            if ( !( x > setup.Samples[first].X ) ) // also a NaN axis: the row's start
            {
                out[first] = 1.0F;
                return;
            }
            if ( x >= setup.Samples[last].X )
            {
                out[last] = 1.0F;
                return;
            }
            for ( const std::array<uint32_t, 2>& segment : triangulation.Hull )
            {
                if ( segment[0] >= count || segment[1] >= count )
                    continue;
                const float upper = setup.Samples[segment[1]].X;
                if ( x > upper )
                    continue;
                const float lower = setup.Samples[segment[0]].X;
                const float alpha = ( x - lower ) / ( upper - lower );
                out[segment[0]]   = 1.0F - alpha;
                out[segment[1]]   = alpha;
                return;
            }
        }

        /// The weights of a 2D space at the normalized point `p`.
        void PlaneWeights( const BlendSpaceTriangulation& triangulation, const BlendPoint2& p,
                           std::vector<float>& out )
        {
            const size_t count = std::min( out.size(), triangulation.Points.size() );
            const auto   valid = [count]( const uint32_t index ) { return static_cast<size_t>( index ) < count; };

            // Inside a triangle: its three samples by barycentric coordinates (UE FBlendSpaceData::GetSamples2D).
            for ( const std::array<uint32_t, 3>& t : triangulation.Triangles )
            {
                if ( !valid( t[0] ) || !valid( t[1] ) || !valid( t[2] ) )
                    continue;
                const BlendPoint2 a    = BlendPointOf( triangulation.Points[t[0]] );
                const BlendPoint2 b    = BlendPointOf( triangulation.Points[t[1]] );
                const BlendPoint2 c    = BlendPointOf( triangulation.Points[t[2]] );
                const double      area = BlendCross2D( a, b, c );
                if ( area <= kBlendPlaneEpsilon )
                    continue;
                const double wa = BlendCross2D( p, b, c ) / area;
                const double wb = BlendCross2D( a, p, c ) / area;
                const double wc = 1.0 - wa - wb;
                if ( wa < -kBlendPlaneEpsilon || wb < -kBlendPlaneEpsilon || wc < -kBlendPlaneEpsilon )
                    continue;
                const double ca  = std::max( wa, 0.0 );
                const double cb  = std::max( wb, 0.0 );
                const double cc  = std::max( wc, 0.0 );
                const double sum = ca + cb + cc;
                out[t[0]]        = static_cast<float>( ca / sum );
                out[t[1]]        = static_cast<float>( cb / sum );
                out[t[2]]        = static_cast<float>( cc / sum );
                return;
            }

            // Outside the hull: the nearest point of the boundary, between its edge's two samples.
            double   best      = -1.0;
            uint32_t bestA     = 0;
            uint32_t bestB     = 0;
            double   bestAlong = 0.0;
            for ( const std::array<uint32_t, 2>& edge : triangulation.Hull )
            {
                if ( !valid( edge[0] ) || !valid( edge[1] ) )
                    continue;
                const BlendPoint2 a       = BlendPointOf( triangulation.Points[edge[0]] );
                const BlendPoint2 b       = BlendPointOf( triangulation.Points[edge[1]] );
                const double      dx      = b[0] - a[0];
                const double      dy      = b[1] - a[1];
                const double      length2 = dx * dx + dy * dy;
                const double      along =
                     length2 > 0.0
                               ? std::clamp( ( ( p[0] - a[0] ) * dx + ( p[1] - a[1] ) * dy ) / length2, 0.0, 1.0 )
                               : 0.0;
                const double ex       = a[0] + dx * along - p[0];
                const double ey       = a[1] + dy * along - p[1];
                const double distance = ex * ex + ey * ey;
                if ( best < 0.0 || distance < best )
                {
                    best      = distance;
                    bestA     = edge[0];
                    bestB     = edge[1];
                    bestAlong = along;
                }
            }
            if ( best < 0.0 )
                return;
            out[bestA] += static_cast<float>( 1.0 - bestAlong );
            out[bestB] += static_cast<float>( bestAlong );
        }

        /// The FIR coefficient of an entry `age` seconds old in a window of `window` seconds (UE
        /// FFIRFilterTimeBased). OUR CHOICE, NOT PROVEN against UE's GetFilteredData (the file is absent from the
        /// local UE checkout): with r = 1 - age / window, Average 1, Linear r, Cubic r^3, EaseInOut r^2 (3 - 2r).
        float FirCoefficient( const BlendAxisSmoothing shape, const float age, const float window )
        {
            const float r = std::clamp( 1.0F - age / window, 0.0F, 1.0F );
            switch ( shape )
            {
                case BlendAxisSmoothing::Average:
                    return 1.0F;
                case BlendAxisSmoothing::Linear:
                    return r;
                case BlendAxisSmoothing::Cubic:
                    return r * r * r;
                case BlendAxisSmoothing::EaseInOut:
                    return r * r * ( 3.0F - 2.0F * r );
                case BlendAxisSmoothing::ExponentialDecay:
                case BlendAxisSmoothing::SpringDamper:
                    break;
            }
            return 1.0F;
        }

        /// The spring's offset and velocity after `dt` from offset `x0`, velocity `v0` (target at 0): the
        /// analytic damped harmonic oscillator x'' = -2 zeta omega x' - omega^2 x.
        void SpringStep( const double omega, const double zeta, const double dt, double& x, double& v )
        {
            const double x0 = x;
            const double v0 = v;
            if ( std::abs( zeta - 1.0 ) < 1e-4 )
            {
                const double j1 = v0 + omega * x0;
                const double e  = std::exp( -omega * dt );
                x               = ( x0 + j1 * dt ) * e;
                v               = ( j1 - omega * ( x0 + j1 * dt ) ) * e;
                return;
            }
            if ( zeta < 1.0 )
            {
                const double a  = zeta * omega;
                const double wd = omega * std::sqrt( 1.0 - zeta * zeta );
                const double b  = ( v0 + a * x0 ) / wd;
                const double e  = std::exp( -a * dt );
                const double c  = std::cos( wd * dt );
                const double s  = std::sin( wd * dt );
                x               = e * ( x0 * c + b * s );
                v               = e * ( v0 * c + ( -a * b - x0 * wd ) * s );
                return;
            }
            const double root = std::sqrt( zeta * zeta - 1.0 );
            const double r1   = -omega * ( zeta - root );
            const double r2   = -omega * ( zeta + root );
            const double c2   = ( v0 - r1 * x0 ) / ( r2 - r1 );
            const double c1   = x0 - c2;
            const double e1   = std::exp( r1 * dt );
            const double e2   = std::exp( r2 * dt );
            x                 = c1 * e1 + c2 * e2;
            v                 = r1 * c1 * e1 + r2 * c2 * e2;
        }
    } // namespace

    std::string BlendSpaceError( const BlendSpaceSetup& setup, const size_t axisCount )
    {
        if ( setup.Axes.size() != axisCount )
            return std::format( "it has {} axes; a Blend Space {}D has {}", setup.Axes.size(), axisCount,
                                axisCount );
        static constexpr std::array<const char*, 2> kAxisNames{ "X", "Y" };
        for ( size_t a = 0; a < setup.Axes.size(); ++a )
        {
            const BlendSpaceAxis& axis = setup.Axes[a];
            const char*           name = a < kAxisNames.size() ? kAxisNames[a] : "?";
            if ( !std::isfinite( axis.Min ) || !std::isfinite( axis.Max ) || !( axis.Max > axis.Min ) )
                return std::format( "axis {} runs from {} to {}; its Max must be a number above its Min", name,
                                    axis.Min, axis.Max );
            if ( !std::isfinite( axis.SmoothingTime ) || axis.SmoothingTime < 0.0F )
                return std::format( "axis {} has smoothing time {}; it must be 0 (none) or more seconds", name,
                                    axis.SmoothingTime );
            if ( !std::isfinite( axis.DampingRatio ) || !( axis.DampingRatio > 0.0F ) )
                return std::format( "axis {} has damping ratio {}; it must be a number above 0", name,
                                    axis.DampingRatio );
            if ( !std::isfinite( axis.MaxSpeed ) || axis.MaxSpeed < 0.0F )
                return std::format( "axis {} has max speed {}; it must be 0 (unlimited) or more", name,
                                    axis.MaxSpeed );
        }
        const size_t minimum = axisCount == 2 ? 3 : 1;
        if ( setup.Samples.size() < minimum )
            return axisCount == 2
                        ? std::format( "a Blend Space 2D needs at least three samples spanning an area; it has {}",
                                       setup.Samples.size() )
                        : std::string( "it has no sample, so it plays nothing" );
        for ( size_t i = 0; i < setup.Samples.size(); ++i )
        {
            const BlendSample& sample = setup.Samples[i];
            if ( sample.Clip.empty() )
                return std::format( "sample {} names no clip", i );
            if ( !std::isfinite( sample.X ) || !std::isfinite( sample.Y ) )
                return std::format( "sample {} ('{}') is at a position that is not a number", i, sample.Clip );
            if ( !( std::isfinite( sample.RateScale ) && sample.RateScale > 0.0F ) )
                return std::format( "sample {} ('{}') has RateScale {}; it must be a number above 0", i,
                                    sample.Clip, sample.RateScale );
            const float position[2] = { sample.X, sample.Y };
            for ( size_t a = 0; a < axisCount; ++a )
                if ( position[a] < setup.Axes[a].Min || position[a] > setup.Axes[a].Max )
                    return std::format( "sample {} ('{}') sits at {} {} outside the axis range {}..{}", i,
                                        sample.Clip, kAxisNames[a], position[a], setup.Axes[a].Min,
                                        setup.Axes[a].Max );
            for ( size_t j = 0; j < i; ++j )
            {
                const BlendSample& other = setup.Samples[j];
                if ( other.X == sample.X && ( axisCount == 1 || other.Y == sample.Y ) )
                    return std::format( "samples {} ('{}') and {} ('{}') are at one position: one point plays one "
                                        "sample",
                                        j, other.Clip, i, sample.Clip );
            }
        }
        if ( !std::isfinite( setup.WeightSpeed ) || setup.WeightSpeed < 0.0F )
            return std::format( "its weight speed {} is not a speed (0 = no smoothing, else weight per second)",
                                setup.WeightSpeed );
        if ( setup.ScaleAxis < -1 || setup.ScaleAxis >= static_cast<int32_t>( axisCount ) )
            return std::format( "its scale axis {} is no axis (-1 = none, else 0..{})", setup.ScaleAxis,
                                axisCount - 1 );
        if ( axisCount == 2 && TriangulateBlendSpace( setup ).Triangles.empty() )
            return "every sample lies on one line, which spans no area: that is a Blend Space 1D";
        return {};
    }

    BlendSpaceTriangulation TriangulateBlendSpace( const BlendSpaceSetup& setup )
    {
        BlendSpaceTriangulation out;
        if ( setup.Samples.empty() || setup.Axes.empty() )
            return out;

        // Each sample in the unit box the AXIS ranges map to (UE SetGridBox): a sample added at the edge does not
        // re-shape the others' triangles, and cm/s against 0..1 does not decide the triangles' shape.
        const bool plane = setup.Axes.size() >= 2;
        out.Points.reserve( setup.Samples.size() );
        for ( const BlendSample& sample : setup.Samples )
            out.Points.push_back(
                 { static_cast<float>( NormalizedOn( setup.Axes[0], sample.X ) ),
                   plane ? static_cast<float>( NormalizedOn( setup.Axes[1], sample.Y ) ) : 0.0F } );

        const auto count = static_cast<uint32_t>( out.Points.size() );
        if ( !plane )
        {
            // The row: its samples in ascending X, each neighbouring pair a segment.
            std::vector<uint32_t> order( count );
            std::iota( order.begin(), order.end(), 0U );
            std::stable_sort( order.begin(), order.end(), [&setup]( const uint32_t a, const uint32_t b )
                              { return setup.Samples[a].X < setup.Samples[b].X; } );
            for ( size_t i = 1; i < order.size(); ++i )
                out.Hull.push_back( { order[i - 1], order[i] } );
            return out;
        }

        // Bowyer-Watson: start from one counter-clockwise triangle far around [0, 1]^2, insert each sample by
        // re-triangulating the cavity of the triangles whose circumcircle holds it, then drop every triangle that
        // still touches the outer one.
        std::vector<BlendPoint2> points;
        points.reserve( out.Points.size() + 3 );
        for ( const std::array<float, 2>& point : out.Points )
            points.push_back( BlendPointOf( point ) );
        points.push_back( BlendPoint2{ -1000.0, -1000.0 } );
        points.push_back( BlendPoint2{ 1000.0, -1000.0 } );
        points.push_back( BlendPoint2{ 0.0, 1000.0 } );

        std::vector<std::array<uint32_t, 3>> triangles{ { count, count + 1, count + 2 } };
        std::vector<std::array<uint32_t, 3>> kept;
        std::vector<std::array<uint32_t, 2>> cavity;
        for ( uint32_t i = 0; i < count; ++i )
        {
            kept.clear();
            cavity.clear();
            for ( const std::array<uint32_t, 3>& t : triangles )
            {
                if ( InBlendCircumcircle( points[t[0]], points[t[1]], points[t[2]], points[i] ) )
                {
                    cavity.push_back( { t[0], t[1] } );
                    cavity.push_back( { t[1], t[2] } );
                    cavity.push_back( { t[2], t[0] } );
                }
                else
                    kept.push_back( t );
            }
            // The cavity's boundary is every edge exactly one removed triangle has; each, closed by the new
            // sample, keeps its triangle's counter-clockwise winding.
            for ( const std::array<uint32_t, 2>& edge : cavity )
            {
                const auto shared = std::count_if( cavity.begin(), cavity.end(),
                                                   [&edge]( const std::array<uint32_t, 2>& other ) {
                                                       return ( other[0] == edge[0] && other[1] == edge[1] ) ||
                                                              ( other[0] == edge[1] && other[1] == edge[0] );
                                                   } );
                if ( shared == 1 )
                    kept.push_back( { edge[0], edge[1], i } );
            }
            std::swap( triangles, kept );
        }

        for ( const std::array<uint32_t, 3>& t : triangles )
            if ( t[0] < count && t[1] < count && t[2] < count &&
                 BlendCross2D( points[t[0]], points[t[1]], points[t[2]] ) > kBlendPlaneEpsilon )
                out.Triangles.push_back( t );

        for ( const std::array<uint32_t, 3>& t : out.Triangles )
            for ( size_t e = 0; e < 3; ++e )
            {
                const uint32_t a     = t[e];
                const uint32_t b     = t[( e + 1 ) % 3];
                const bool     inner = std::any_of( out.Triangles.begin(), out.Triangles.end(),
                                                    [a, b]( const std::array<uint32_t, 3>& other )
                                                    {
                                                    for ( size_t f = 0; f < 3; ++f )
                                                        if ( other[f] == b && other[( f + 1 ) % 3] == a )
                                                            return true;
                                                    return false;
                                                } );
                if ( !inner )
                    out.Hull.push_back( { a, b } );
            }
        return out;
    }

    float ClampBlendSpaceAxis( const BlendSpaceAxis& axis, const float raw )
    {
        if ( !std::isfinite( raw ) )
            return axis.Min;
        if ( axis.Wrap )
        {
            const float range   = axis.Max - axis.Min;
            const float wrapped = axis.Min + ( raw - axis.Min ) - range * std::floor( ( raw - axis.Min ) / range );
            return wrapped < axis.Max ? wrapped : axis.Min;
        }
        return std::clamp( raw, axis.Min, axis.Max );
    }

    void BlendSpaceTargetWeights( const BlendSpaceSetup& setup, const BlendSpaceTriangulation& triangulation,
                                  const std::span<const float> axisValues, std::vector<float>& out )
    {
        out.assign( setup.Samples.size(), 0.0F );
        if ( setup.Samples.empty() || setup.Axes.empty() )
            return;
        const auto valueOn = [&]( const size_t a )
        { return ClampBlendSpaceAxis( setup.Axes[a], a < axisValues.size() ? axisValues[a] : 0.0F ); };
        if ( setup.Axes.size() == 1 )
        {
            RowWeights( setup, triangulation, valueOn( 0 ), out );
            return;
        }
        const BlendPoint2 p{ NormalizedOn( setup.Axes[0], valueOn( 0 ) ),
                             NormalizedOn( setup.Axes[1], valueOn( 1 ) ) };
        PlaneWeights( triangulation, p, out );
    }

    float FilterBlendSpaceAxis( const BlendSpaceAxis& axis, BlendSpaceAxisFilter& filter, const float raw,
                                const float dt )
    {
        const float input = ClampBlendSpaceAxis( axis, raw );
        // The first input is taken as is (UE: a blend space player does not blend in from 0), and so is every
        // input of an unsmoothed axis.
        if ( !filter.Initialised || axis.SmoothingTime <= 0.0F )
        {
            filter.History.clear();
            filter.History.push_back( { input, 0.0F } );
            filter.Value       = input;
            filter.Rate        = 0.0F;
            filter.Initialised = true;
            return input;
        }
        if ( !( dt > 0.0F ) )
            return filter.Value;

        // The target unwrapped next to the current value, so a wrapping axis is smoothed the short way round.
        const float target = filter.Value + AxisDelta( axis, filter.Value, input );
        const float window = axis.SmoothingTime;
        float       next   = filter.Value;
        switch ( axis.Smoothing )
        {
            case BlendAxisSmoothing::Average:
            case BlendAxisSmoothing::Linear:
            case BlendAxisSmoothing::Cubic:
            case BlendAxisSmoothing::EaseInOut:
            {
                // UE FFIRFilterTimeBased: every input of the last `window` seconds, weighted by its age.
                for ( BlendSpaceAxisFilter::Entry& entry : filter.History )
                    entry.Age += dt;
                const float newest =
                     filter.History.empty()
                          ? target
                          : filter.History.back().Value + AxisDelta( axis, filter.History.back().Value, input );
                filter.History.push_back( { newest, 0.0F } );
                const auto expired = std::find_if( filter.History.begin(), filter.History.end(),
                                                   [window]( const BlendSpaceAxisFilter::Entry& entry )
                                                   { return entry.Age < window; } );
                filter.History.erase( filter.History.begin(), expired );
                float sum    = 0.0F;
                float weight = 0.0F;
                for ( const BlendSpaceAxisFilter::Entry& entry : filter.History )
                {
                    const float c = FirCoefficient( axis.Smoothing, entry.Age, window );
                    sum += c * entry.Value;
                    weight += c;
                }
                // The window's average sits beside the history's chain; carried next to the current value.
                const float average = weight > 0.0F ? sum / weight : newest;
                next                = filter.Value + AxisDelta( axis, filter.Value, average );
                break;
            }
            case BlendAxisSmoothing::ExponentialDecay:
                // OUR CHOICE, NOT PROVEN against UE: v = t + (v - t) e^(-dt / T).
                next = target + ( filter.Value - target ) * std::exp( -dt / window );
                break;
            case BlendAxisSmoothing::SpringDamper:
            {
                // OUR CHOICE, NOT PROVEN against UE: the analytic spring with omega = 2 pi / T, zeta =
                // DampingRatio.
                double x = static_cast<double>( filter.Value - target );
                double v = static_cast<double>( filter.Rate );
                SpringStep( 2.0 * std::numbers::pi / static_cast<double>( window ),
                            static_cast<double>( axis.DampingRatio ), static_cast<double>( dt ), x, v );
                next        = target + static_cast<float>( x );
                filter.Rate = static_cast<float>( v );
                break;
            }
        }
        if ( axis.MaxSpeed > 0.0F )
        {
            const float limit = axis.MaxSpeed * dt;
            next              = filter.Value + std::clamp( next - filter.Value, -limit, limit );
        }
        filter.Value = ClampBlendSpaceAxis( axis, next );
        return filter.Value;
    }

    void InterpolateBlendWeights( std::vector<float>& weights, const std::vector<float>& targets,
                                  const float speed, const bool easeInOut, const float dt )
    {
        if ( weights.size() != targets.size() || speed <= 0.0F )
        {
            weights = targets;
            return;
        }
        const float step  = speed * std::max( dt, 0.0F );
        float       total = 0.0F;
        for ( size_t i = 0; i < weights.size(); ++i )
        {
            const float delta = targets[i] - weights[i];
            // Ease-in-out (UE bTargetWeightInterpolationEaseInOut). OUR CHOICE, NOT PROVEN against UE: the step
            // is scaled by 0.5 + 0.5 sin(pi |d|) of the remaining distance d, slow leaving a weight and slow
            // arriving at the target, fastest half way.
            const float scale =
                 easeInOut
                      ? 0.5F + 0.5F * std::sin( std::numbers::pi_v<float> * std::min( std::abs( delta ), 1.0F ) )
                      : 1.0F;
            weights[i] += std::clamp( delta, -step * scale, step * scale );
            total += weights[i];
        }
        if ( total <= 0.0F )
        {
            weights = targets;
            return;
        }
        for ( float& weight : weights )
            weight /= total;
    }

    float AdvanceSyncedPhase( const float phase, const std::span<const float> weights,
                              const std::span<const float> durations, const std::span<const BlendSample> samples,
                              const float rate, const bool loop, const float dt )
    {
        float weighted = 0.0F;
        float total    = 0.0F;
        // An unresolved sample has no length (0): it neither stretches nor shrinks the shared cycle.
        const size_t count = std::min( { weights.size(), durations.size(), samples.size() } );
        for ( size_t i = 0; i < count; ++i )
        {
            const float length = durations[i] / samples[i].RateScale;
            if ( weights[i] > 0.0F && length > 0.0F )
            {
                weighted += weights[i] * length;
                total += weights[i];
            }
        }
        if ( total <= 0.0F )
            return phase;
        const float next = phase + ( dt * rate ) / ( weighted / total );
        if ( !loop )
            return std::clamp( next, 0.0F, 1.0F );
        const float wrapped = next - std::floor( next );
        return wrapped < 1.0F ? wrapped : 0.0F;
    }
} // namespace Desert::Animation::Graph
