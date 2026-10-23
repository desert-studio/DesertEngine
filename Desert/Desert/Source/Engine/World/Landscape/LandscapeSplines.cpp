// Ported from UE 5.8 Runtime/Landscape: LandscapeSplineControlPoint.cpp (AutoCalcRotation),
// LandscapeSplineSegment.cpp (UpdateSplinePoints) and LandscapeSplineRaster.cpp (RasterizeSegmentPoints).
#include "LandscapeSplines.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace Desert::World::Landscape
{
    namespace
    {
        bool Finite( const glm::vec3& v )
        {
            return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
        }

        bool NonNegative( float value )
        {
            return std::isfinite( value ) && value >= 0.0f;
        }

        /// UE AutoCalcRotation: the point faces along the sum of its segments' directions — out of the point
        /// for a segment that starts there, into it for one that ends there. A lone point faces +X.
        glm::vec3 PointDirection( const LandscapeSplines& splines, uint32_t point )
        {
            glm::vec3 sum( 0.0f );
            for ( const LandscapeSplineSegment& segment : splines.Segments )
            {
                const glm::vec3 a = splines.ControlPoints[segment.Start].Location;
                const glm::vec3 b = splines.ControlPoints[segment.End].Location;
                const glm::vec3 d = b - a;
                const float     l = glm::length( d );
                if ( l <= 0.0f )
                    continue;
                if ( segment.Start == point )
                    sum += d / l;
                if ( segment.End == point )
                    sum += d / l;
            }
            const float length = glm::length( sum );
            return length > 0.0f ? sum / length : glm::vec3( 1.0f, 0.0f, 0.0f );
        }

        size_t Connections( const LandscapeSplines& splines, uint32_t point )
        {
            return static_cast<size_t>( std::count_if( splines.Segments.begin(), splines.Segments.end(),
                                                       [point]( const LandscapeSplineSegment& s )
                                                       { return s.Start == point || s.End == point; } ) );
        }

        /// UE's FMath::SmoothStep(0, 1, x): the falloff curve of the raster's alpha.
        float SmoothStep( float x )
        {
            x = std::clamp( x, 0.0f, 1.0f );
            return x * x * ( 3.0f - 2.0f * x );
        }

        struct BandHit
        {
            float Alpha      = 0.0f;
            float PaintAlpha = 0.0f; // the same falloff measured from the painted half-width (LayerWidth)
            float Height     = 0.0f; // world cm, the curve's centre height at the nearest point
        };

        /// The band of one sampled segment at world (x, z): the nearest point on the centre polyline (XZ),
        /// alpha from the lateral distance and, past an end of the spline, from the overshoot.
        BandHit SegmentBand( std::span<const LandscapeSplineInterpPoint> points, bool startIsEnd,
                             float startFalloff, bool endIsEnd, float endFalloff, glm::vec2 at )
        {
            BandHit best;
            float   bestDistance = std::numeric_limits<float>::max();
            for ( size_t i = 0; i + 1 < points.size(); ++i )
            {
                const glm::vec2 a( points[i].Center.x, points[i].Center.z );
                const glm::vec2 b( points[i + 1].Center.x, points[i + 1].Center.z );
                const glm::vec2 ab        = b - a;
                const float     length    = glm::dot( ab, ab );
                float           t         = length > 0.0f ? glm::dot( at - a, ab ) / length : 0.0f;
                float           overshoot = 0.0f;
                if ( t < 0.0f && i == 0 && startIsEnd )
                    overshoot = -t * std::sqrt( length );
                if ( t > 1.0f && i + 2 == points.size() && endIsEnd )
                    overshoot = ( t - 1.0f ) * std::sqrt( length );
                // Past a spline end the lateral distance is measured across the end's line; inside, or at a
                // joint to another segment, it is the distance to the nearest point of the piece.
                const float     clamped = std::clamp( t, 0.0f, 1.0f );
                const glm::vec2 foot    = overshoot > 0.0f ? a + ab * t : a + ab * clamped;
                const float     lateral = glm::length( at - foot );
                const float     score   = lateral + overshoot;
                if ( score >= bestDistance )
                    continue;
                bestDistance            = score;
                const float width       = glm::mix( points[i].Width, points[i + 1].Width, clamped );
                const float sideFalloff = glm::mix( points[i].SideFalloff, points[i + 1].SideFalloff, clamped );
                const float layerWidth  = glm::mix( points[i].LayerWidth, points[i + 1].LayerWidth, clamped );
                auto        side        = [&]( float half )
                {
                    if ( lateral <= half )
                        return 1.0f;
                    return sideFalloff > 0.0f ? SmoothStep( 1.0f - ( lateral - half ) / sideFalloff ) : 0.0f;
                };
                float alpha = side( width );
                float paint = side( layerWidth );
                if ( overshoot > 0.0f )
                {
                    const float falloff = t < 0.0f ? startFalloff : endFalloff;
                    const float fade    = falloff > 0.0f ? SmoothStep( 1.0f - overshoot / falloff ) : 0.0f;
                    alpha *= fade;
                    paint *= fade;
                }
                best.Alpha      = alpha;
                best.PaintAlpha = paint;
                best.Height = glm::mix( points[i].Center.y, points[i + 1].Center.y, clamped );
            }
            return best;
        }
    } // namespace

    Common::BoolResultStr ValidateLandscapeSplines( const LandscapeSplines& splines )
    {
        for ( size_t i = 0; i < splines.ControlPoints.size(); ++i )
        {
            const LandscapeSplineControlPoint& p = splines.ControlPoints[i];
            if ( !Finite( p.Location ) )
                return Common::MakeFormattedError<bool>( "landscape spline point {}: location is not finite", i );
            if ( !NonNegative( p.Width ) || !NonNegative( p.SideFalloff ) || !NonNegative( p.EndFalloff ) ||
                 !NonNegative( p.LayerWidthRatio ) )
                return Common::MakeFormattedError<bool>( "landscape spline point {}: width {} / side falloff {} / "
                                                         "end falloff {} / layer width ratio {} "
                                                         "must be finite and not negative",
                                                         i, p.Width, p.SideFalloff, p.EndFalloff,
                                                         p.LayerWidthRatio );
        }
        for ( size_t i = 0; i < splines.Segments.size(); ++i )
        {
            const LandscapeSplineSegment& s = splines.Segments[i];
            if ( s.Start >= splines.ControlPoints.size() || s.End >= splines.ControlPoints.size() )
                return Common::MakeFormattedError<bool>(
                     "landscape spline segment {}: joins points {} and {} of {}", i, s.Start, s.End,
                     splines.ControlPoints.size() );
            if ( s.Start == s.End )
                return Common::MakeFormattedError<bool>( "landscape spline segment {}: joins point {} to itself",
                                                         i, s.Start );
        }
        return Common::MakeSuccess( true );
    }

    std::vector<LandscapeSplineInterpPoint> LandscapeSplineSegmentPoints( const LandscapeSplines& splines,
                                                                          size_t segment, float stepCm )
    {
        const LandscapeSplineSegment&      s     = splines.Segments[segment];
        const LandscapeSplineControlPoint& start = splines.ControlPoints[s.Start];
        const LandscapeSplineControlPoint& end   = splines.ControlPoints[s.End];
        const glm::vec3                    p0    = start.Location;
        const glm::vec3                    p1    = end.Location;
        const float                        chord = glm::length( p1 - p0 );
        // UE: both connections' TangentLen default to the chord; a point's tangent points along its rotation.
        const glm::vec3 m0 = PointDirection( splines, s.Start ) * chord;
        const glm::vec3 m1 = PointDirection( splines, s.End ) * chord;
        const size_t    count =
             std::max<size_t>( 2u, static_cast<size_t>( std::ceil( chord / std::max( stepCm, 1.0f ) ) ) + 1u );

        std::vector<LandscapeSplineInterpPoint> points( count );
        for ( size_t i = 0; i < count; ++i )
        {
            const float t   = static_cast<float>( i ) / static_cast<float>( count - 1u );
            const float t2  = t * t;
            const float t3  = t2 * t;
            const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
            const float h10 = t3 - 2.0f * t2 + t;
            const float h01 = -2.0f * t3 + 3.0f * t2;
            const float h11 = t3 - t2;
            // The derivative gives the side direction (UE: the spline's right vector on the ground plane).
            const glm::vec3 tangent = ( 6.0f * t2 - 6.0f * t ) * p0 + ( 3.0f * t2 - 4.0f * t + 1.0f ) * m0 +
                                      ( -6.0f * t2 + 6.0f * t ) * p1 + ( 3.0f * t2 - 2.0f * t ) * m1;
            glm::vec3   right( -tangent.z, 0.0f, tangent.x );
            const float rightLength = glm::length( right );
            right                   = rightLength > 0.0f ? right / rightLength : glm::vec3( 0.0f, 0.0f, 1.0f );

            LandscapeSplineInterpPoint& point = points[i];
            point.Center                      = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
            point.Width                       = glm::mix( start.Width, end.Width, t );
            point.SideFalloff                 = glm::mix( start.SideFalloff, end.SideFalloff, t );
            point.LayerWidth = point.Width * glm::mix( start.LayerWidthRatio, end.LayerWidthRatio, t );
            point.Left                        = point.Center - right * point.Width;
            point.Right                       = point.Center + right * point.Width;
            point.FalloffLeft                 = point.Center - right * ( point.Width + point.SideFalloff );
            point.FalloffRight                = point.Center + right * ( point.Width + point.SideFalloff );
        }
        return points;
    }

    Common::BoolResultStr RasterizeLandscapeSplines( const LandscapeSplines&             splines,
                                                     const LandscapeEditLayerStack&      stack,
                                                     std::span<const LandscapeLayerRule> rules,
                                                     const Common::UUID& layer, const LandscapeFrame& frame,
                                                     LandscapeTileData& tile )
    {
        if ( auto valid = ValidateLandscapeEditLayerStack( stack ); !valid )
            return valid;
        if ( auto valid = ValidateLandscapeSplines( splines ); !valid )
            return valid;
        if ( auto valid = ValidateLandscapeFrame( frame ); !valid )
            return valid;
        const LandscapeEditLayer* splinesLayer = stack.FindSplinesLayer();
        if ( splinesLayer == nullptr ||
             static_cast<uint64_t>( splinesLayer->Guid ) != static_cast<uint64_t>( layer ) )
            return Common::MakeFormattedError<bool>(
                 "landscape splines: edit layer {} is not the stack's Splines layer",
                 static_cast<uint64_t>( layer ) );

        const uint32_t samplesX = tile.SamplesX();
        const uint32_t samplesZ = tile.SamplesZ();
        const float    minX     = frame.OriginX;
        const float    minZ     = frame.OriginZ;
        const float    maxX     = frame.OriginX + static_cast<float>( samplesX - 1u ) * frame.SpacingCm;
        const float    maxZ     = frame.OriginZ + static_cast<float>( samplesZ - 1u ) * frame.SpacingCm;

        // Each segment sampled once, and kept only when its band (with falloff) can reach this tile.
        struct Sampled
        {
            std::vector<LandscapeSplineInterpPoint> Points;
            const LandscapeSplineSegment*           Segment      = nullptr;
            bool                                    StartIsEnd   = false;
            bool                                    EndIsEnd     = false;
            float                                   StartFalloff = 0.0f;
            float                                   EndFalloff   = 0.0f;
        };
        std::vector<Sampled> segments;
        for ( size_t i = 0; i < splines.Segments.size(); ++i )
        {
            const LandscapeSplineSegment& s = splines.Segments[i];
            Sampled   sampled{ LandscapeSplineSegmentPoints( splines, i, frame.SpacingCm * 0.5f ),
                             &s,
                             Connections( splines, s.Start ) == 1u,
                             Connections( splines, s.End ) == 1u,
                             splines.ControlPoints[s.Start].EndFalloff,
                             splines.ControlPoints[s.End].EndFalloff };
            float     reach = std::max( sampled.StartFalloff, sampled.EndFalloff );
            glm::vec2 lo( std::numeric_limits<float>::max() );
            glm::vec2 hi( std::numeric_limits<float>::lowest() );
            for ( const LandscapeSplineInterpPoint& p : sampled.Points )
            {
                reach = std::max( reach, std::max( p.Width, p.LayerWidth ) + p.SideFalloff );
                lo    = glm::min( lo, glm::vec2( p.Center.x, p.Center.z ) );
                hi    = glm::max( hi, glm::vec2( p.Center.x, p.Center.z ) );
            }
            if ( hi.x + reach < minX || lo.x - reach > maxX || hi.y + reach < minZ || lo.y - reach > maxZ )
                continue;
            segments.push_back( std::move( sampled ) );
        }

        // The ground below the Splines layer: the visible layers under it, merged as the merge adds heights.
        std::vector<const LandscapeEditLayerTileData*> below;
        std::vector<float>                             belowAlpha;
        for ( const LandscapeEditLayer& l : stack.Layers )
        {
            if ( l.Kind == LandscapeEditLayerKind::Splines )
                break;
            const LandscapeEditLayerTileData* data = tile.FindEditLayer( l.Guid );
            if ( !l.Visible || data == nullptr || data->Heights.empty() )
                continue;
            below.push_back( data );
            belowAlpha.push_back( l.HeightAlpha );
        }

        // The paint layers the segments name, each with its plane (UE: one weightmap target per LayerName).
        std::vector<LandscapeWeightLayer> paint;
        std::vector<size_t>               paintOf( segments.size(), std::numeric_limits<size_t>::max() );
        for ( size_t i = 0; i < segments.size(); ++i )
        {
            const std::string& name = segments[i].Segment->LayerName;
            if ( name.empty() )
                continue;
            auto found = std::find_if( paint.begin(), paint.end(),
                                       [&]( const LandscapeWeightLayer& w ) { return w.Name == name; } );
            if ( found == paint.end() )
            {
                paint.push_back(
                     { name, std::vector<uint8_t>( static_cast<size_t>( samplesX ) * samplesZ, 0u ) } );
                found = paint.end() - 1;
            }
            paintOf[i] = static_cast<size_t>( found - paint.begin() );
        }

        constexpr float       kMid       = static_cast<float>( kLandscapeMidSample );
        const float           alphaUp    = splinesLayer->HeightAlpha;
        const float           stepsPerCm = 128.0f / frame.ZScale; // kLandscapeStepsPerLocal per local unit
        std::vector<uint16_t> plane( static_cast<size_t>( samplesX ) * samplesZ, kLandscapeMidSample );
        bool                  touched = false;
        for ( uint32_t z = 0; z < samplesZ && !segments.empty(); ++z )
            for ( uint32_t x = 0; x < samplesX; ++x )
            {
                const glm::vec2               at( frame.OriginX + static_cast<float>( x ) * frame.SpacingCm,
                                                  frame.OriginZ + static_cast<float>( z ) * frame.SpacingCm );
                BandHit                       best;
                const LandscapeSplineSegment* winner = nullptr;
                const size_t                  index  = static_cast<size_t>( z ) * samplesX + x;
                for ( size_t i = 0; i < segments.size(); ++i )
                {
                    const Sampled& s = segments[i];
                    const BandHit  hit =
                         SegmentBand( s.Points, s.StartIsEnd, s.StartFalloff, s.EndIsEnd, s.EndFalloff, at );
                    if ( paintOf[i] < paint.size() && hit.PaintAlpha > 0.0f )
                    {
                        uint8_t&      w      = paint[paintOf[i]].Weights[index];
                        const uint8_t weight = static_cast<uint8_t>( std::lround( hit.PaintAlpha * 255.0f ) );
                        w                    = std::max( w, weight );
                    }
                    if ( hit.Alpha > best.Alpha )
                    {
                        best   = hit;
                        winner = s.Segment;
                    }
                }
                if ( winner == nullptr || best.Alpha <= 0.0f || alphaUp == 0.0f )
                    continue;
                float        ground = 0.0f; // relative to mid, exactly as MergeLandscapeEditLayers sums it
                for ( size_t b = 0; b < below.size(); ++b )
                    ground = std::clamp( ground + ( static_cast<float>( below[b]->Heights[index] ) - kMid ) *
                                                       belowAlpha[b],
                                         -kMid, 65535.0f - kMid );
                const float target = ( best.Height - frame.BaseY ) * stepsPerCm;
                if ( ( target > ground && !winner->RaiseTerrain ) || ( target < ground && !winner->LowerTerrain ) )
                    continue;
                const float delta = ( target - ground ) * best.Alpha / alphaUp;
                plane[index]      = static_cast<uint16_t>( std::clamp( std::lround( kMid + delta ), 0L, 65535L ) );
                touched           = touched || plane[index] != kLandscapeMidSample;
            }

        // Only a plane that carries paint here goes on the tile (a tile holds at most eight weight layers).
        std::erase_if(
             paint, []( const LandscapeWeightLayer& w )
             { return std::all_of( w.Weights.begin(), w.Weights.end(), []( uint8_t v ) { return v == 0u; } ); } );
        const bool painted = !paint.empty();

        const LandscapeEditLayerTileData* held = tile.FindEditLayer( layer );
        if ( !touched && !painted && held == nullptr )
            return Common::MakeSuccess( true ); // nothing reaches the tile and nothing was there: no layer data
        LandscapeEditLayerTileData data;
        data.Layer = layer;
        if ( touched )
            data.Heights = std::move( plane );
        data.Weights = std::move( paint );
        if ( auto set = tile.SetEditLayer( std::move( data ) ); !set )
            return set;
        const LandscapeRect whole{ 0u, 0u, samplesX, samplesZ };
        return MergeLandscapeEditLayers( stack, rules, whole, tile );
    }
} // namespace Desert::World::Landscape
