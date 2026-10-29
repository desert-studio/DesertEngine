#include "Msdf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

namespace Desert::Text::Msdf
{
    namespace
    {
        constexpr double kEpsilon = 1e-14;

        // Channel masks, as values rather than as literals scattered through the generator: a texel's
        // red channel is fed by every edge whose colour carries this bit.
        constexpr unsigned char kRedBit   = 1U;
        constexpr unsigned char kGreenBit = 2U;
        constexpr unsigned char kBlueBit  = 4U;
        constexpr unsigned char kAllBits  = 7U;

        constexpr int kLinePoints      = 2;
        constexpr int kQuadraticPoints = 3;

        // The field is encoded with the outline at 0.5, so half a unit is "on the edge".
        constexpr double kFieldCentre = 0.5;

        // The small integer factors of Bezier calculus and of Cardano's reduction. They are named for
        // the role they play in the formula rather than for their value, so the algebra below still
        // reads as the algebra it is.
        constexpr double kQuadraticDerivativeFactor = 2.0; // d/dt of a quadratic carries a 2
        constexpr double kCubicDerivativeFactor     = 3.0; // …and of a cubic, a 3
        constexpr double kCubicSecondDerivative     = 6.0; // d2/dt2 of a cubic carries a 6
        constexpr double kQuarticDiscriminant       = 4.0; // b^2 - 4ac
        constexpr double kCardanoP                  = 9.0;
        constexpr double kCardanoQ                  = 27.0;
        constexpr double kCardanoR                  = 54.0;
        constexpr double kThirdSplit                = 12.0; // de Casteljau at t = 1/3 and 2/3
        constexpr double kEighthSplit               = 8.0;

        // The error-correction pass needs the same reconstruction the shader performs; it is not exposed,
        // because the authoritative copy is Common/SdfText.glslh and a second public one would drift.
        float Median( float first, float second, float third )
        {
            return std::max( std::min( first, second ), std::min( std::max( first, second ), third ) );
        }

        Vec2 operator+( Vec2 lhs, Vec2 rhs )
        {
            return { lhs.X + rhs.X, lhs.Y + rhs.Y };
        }
        Vec2 operator-( Vec2 lhs, Vec2 rhs )
        {
            return { lhs.X - rhs.X, lhs.Y - rhs.Y };
        }
        Vec2 operator*( double scalar, Vec2 vec )
        {
            return { scalar * vec.X, scalar * vec.Y };
        }
        double Dot( Vec2 lhs, Vec2 rhs )
        {
            return ( lhs.X * rhs.X ) + ( lhs.Y * rhs.Y );
        }
        double Cross( Vec2 lhs, Vec2 rhs )
        {
            return ( lhs.X * rhs.Y ) - ( lhs.Y * rhs.X );
        }
        double Length( Vec2 vec )
        {
            return std::sqrt( Dot( vec, vec ) );
        }
        Vec2 Normalize( Vec2 vec )
        {
            const double len = Length( vec );
            if ( len <= 0.0 )
            {
                return { 0.0, 0.0 };
            }
            return { vec.X / len, vec.Y / len };
        }
        Vec2 Lerp( Vec2 from, Vec2 dest, double amount )
        {
            return from + ( amount * ( dest - from ) );
        }

        // Zero has no sign, and a zero here means "the query point is exactly ON the edge". Calling that
        // outside (the C sign convention) puts a black texel in the middle of a stroke; calling it inside
        // is the choice the field's own 0.5 encoding already makes.
        double NonZeroSign( double value )
        {
            return value >= 0.0 ? 1.0 : -1.0;
        }

        // A candidate distance plus the tie-break that decides WHICH edge a point belongs to when two
        // edges are equidistant (at a corner, always). The tie-break is |cos| between the edge's end
        // direction and the direction to the point: the more orthogonal edge wins, which is the one whose
        // pseudo-distance extension is meaningful there. Without it the winner is whichever edge the loop
        // happened to visit first, and the field flickers along every corner bisector.
        struct SignedDistance
        {
            double Distance = -std::numeric_limits<double>::max();
            double Cosine   = 1.0;

            bool operator<( const SignedDistance& other ) const
            {
                const double mine  = std::fabs( Distance );
                const double yours = std::fabs( other.Distance );
                return mine < yours || ( mine == yours && Cosine < other.Cosine );
            }
        };

        // Roots of `quadratic*x^2 + linear*x + constant`. -1 means "every x is a root" (the equation
        // vanished entirely), which the callers treat as no candidate, exactly as they treat 0.
        int SolveQuadratic( std::array<double, 2>& roots, double quadratic, double linear, double constant )
        {
            if ( std::fabs( quadratic ) < kEpsilon )
            {
                if ( std::fabs( linear ) < kEpsilon )
                {
                    return std::fabs( constant ) < kEpsilon ? -1 : 0;
                }
                roots[0] = -constant / linear;
                return 1;
            }
            double discriminant = ( linear * linear ) - ( kQuarticDiscriminant * quadratic * constant );
            if ( discriminant > 0.0 )
            {
                discriminant = std::sqrt( discriminant );
                roots[0]     = ( -linear + discriminant ) / ( kQuadraticDerivativeFactor * quadratic );
                roots[1]     = ( -linear - discriminant ) / ( kQuadraticDerivativeFactor * quadratic );
                return 2;
            }
            if ( discriminant == 0.0 )
            {
                roots[0] = -linear / ( kQuadraticDerivativeFactor * quadratic );
                return 1;
            }
            return 0;
        }

        // Cardano on the monic cubic x^3 + coefA x^2 + coefB x + coefC.
        int SolveMonicCubic( std::array<double, 3>& roots, double coefA, double coefB, double coefC )
        {
            const double squareA = coefA * coefA;
            const double third   = coefA / kCubicDerivativeFactor;
            double       reduced = ( squareA - ( kCubicDerivativeFactor * coefB ) ) / kCardanoP;
            const double shifted =
                 ( ( coefA * ( ( kQuadraticDerivativeFactor * squareA ) - ( kCardanoP * coefB ) ) ) +
                   ( kCardanoQ * coefC ) ) /
                 kCardanoR;
            const double shiftedSq = shifted * shifted;
            const double reducedCu = reduced * reduced * reduced;

            if ( shiftedSq < reducedCu )
            {
                // Three real roots: the trigonometric form, which avoids the complex arithmetic the
                // algebraic one would otherwise need here.
                double angle      = std::clamp( shifted / std::sqrt( reducedCu ), -1.0, 1.0 );
                angle             = std::acos( angle );
                reduced           = -kQuadraticDerivativeFactor * std::sqrt( reduced );
                const double turn = kQuadraticDerivativeFactor * std::numbers::pi;
                roots[0]          = ( reduced * std::cos( angle / kCubicDerivativeFactor ) ) - third;
                roots[1]          = ( reduced * std::cos( ( angle + turn ) / kCubicDerivativeFactor ) ) - third;
                roots[2]          = ( reduced * std::cos( ( angle - turn ) / kCubicDerivativeFactor ) ) - third;
                return 3;
            }

            double cubeRoot = -std::pow( std::fabs( shifted ) + std::sqrt( shiftedSq - reducedCu ),
                                         1.0 / kCubicDerivativeFactor );
            if ( shifted < 0.0 )
            {
                cubeRoot = -cubeRoot;
            }
            const double partner = cubeRoot == 0.0 ? 0.0 : reduced / cubeRoot;
            roots[0]             = ( cubeRoot + partner ) - third;
            roots[1]             = ( -kFieldCentre * ( cubeRoot + partner ) ) - third;
            roots[2]             = kFieldCentre * std::numbers::sqrt3 * ( cubeRoot - partner );
            return std::fabs( roots[2] ) < kEpsilon ? 2 : 1;
        }

        int SolveCubic( std::array<double, 3>& roots, double cubic, double quadratic, double linear,
                        double constant )
        {
            if ( std::fabs( cubic ) < kEpsilon )
            {
                std::array<double, 2> lower{};
                const int             found = SolveQuadratic( lower, quadratic, linear, constant );
                for ( int idx = 0; idx < found && idx < 2; ++idx )
                {
                    roots.at( static_cast<size_t>( idx ) ) = lower.at( static_cast<size_t>( idx ) );
                }
                return found;
            }
            return SolveMonicCubic( roots, quadratic / cubic, linear / cubic, constant / cubic );
        }

        SignedDistance LineSignedDistance( const EdgeSegment& edge, Vec2 origin, double& param )
        {
            const Vec2   toOrigin = origin - edge.Points[0];
            const Vec2   along    = edge.Points[1] - edge.Points[0];
            const double lengthSq = Dot( along, along );
            param                 = lengthSq > 0.0 ? Dot( toOrigin, along ) / lengthSq : 0.0;

            const Vec2   toNearerEnd = ( param > kFieldCentre ? edge.Points[1] : edge.Points[0] ) - origin;
            const double endDistance = Length( toNearerEnd );
            if ( param > 0.0 && param < 1.0 )
            {
                // Orthogonal distance to the line, signed by which side of it the point is on.
                const Vec2   normal        = Normalize( { along.Y, -along.X } );
                const double orthoDistance = Dot( normal, toOrigin );
                if ( std::fabs( orthoDistance ) < endDistance )
                {
                    return { orthoDistance, 0.0 };
                }
            }
            return { NonZeroSign( Cross( toOrigin, along ) ) * endDistance,
                     std::fabs( Dot( Normalize( along ), Normalize( toNearerEnd ) ) ) };
        }

        // The cosine tie-break for a point that fell off one END of a curve: which end, and how squarely
        // the curve leaves it. Shared by the quadratic and the cubic, whose only difference here is which
        // control point is the last one.
        SignedDistance CurveResult( const EdgeSegment& edge, Vec2 origin, double minDistance, double param )
        {
            if ( param >= 0.0 && param <= 1.0 )
            {
                return { minDistance, 0.0 };
            }
            if ( param < kFieldCentre )
            {
                return { minDistance, std::fabs( Dot( Normalize( edge.Direction( 0.0 ) ),
                                                      Normalize( edge.Points[0] - origin ) ) ) };
            }
            const Vec2 endPoint = edge.PointCount == kQuadraticPoints ? edge.Points[2] : edge.Points[3];
            return { minDistance,
                     std::fabs( Dot( Normalize( edge.Direction( 1.0 ) ), Normalize( endPoint - origin ) ) ) };
        }

        SignedDistance QuadraticSignedDistance( const EdgeSegment& edge, Vec2 origin, double& param )
        {
            const Vec2 fromStart = edge.Points[0] - origin;
            const Vec2 firstLeg  = edge.Points[1] - edge.Points[0];
            const Vec2 bend      = ( edge.Points[2] - edge.Points[1] ) - firstLeg;

            // d/dt |B(t) - origin|^2 = 0 is a cubic in t; its interior roots are the candidates.
            std::array<double, 3> roots{};
            const int             found =
                 SolveCubic( roots, Dot( bend, bend ), kCubicDerivativeFactor * Dot( firstLeg, bend ),
                             ( kQuadraticDerivativeFactor * Dot( firstLeg, firstLeg ) ) + Dot( fromStart, bend ),
                             Dot( fromStart, firstLeg ) );

            Vec2   endDirection = edge.Direction( 0.0 );
            double minDistance  = NonZeroSign( Cross( endDirection, fromStart ) ) * Length( fromStart );
            param               = -Dot( fromStart, endDirection ) / Dot( endDirection, endDirection );

            endDirection             = edge.Direction( 1.0 );
            const Vec2   fromEnd     = edge.Points[2] - origin;
            const double endDistance = Length( fromEnd );
            if ( endDistance < std::fabs( minDistance ) )
            {
                minDistance = NonZeroSign( Cross( endDirection, fromEnd ) ) * endDistance;
                param       = Dot( origin - edge.Points[1], endDirection ) / Dot( endDirection, endDirection );
            }

            for ( int idx = 0; idx < found && idx < 3; ++idx )
            {
                const double root = roots.at( static_cast<size_t>( idx ) );
                if ( root <= 0.0 || root >= 1.0 )
                {
                    continue;
                }
                const Vec2 toPoint =
                     fromStart + ( ( kQuadraticDerivativeFactor * root ) * firstLeg ) + ( ( root * root ) * bend );
                const double distance = Length( toPoint );
                if ( distance <= std::fabs( minDistance ) )
                {
                    minDistance = NonZeroSign( Cross( firstLeg + ( root * bend ), toPoint ) ) * distance;
                    param       = root;
                }
            }
            return CurveResult( edge, origin, minDistance, param );
        }

        // A cubic's nearest point has no closed form, so it is a Newton refinement from evenly spaced
        // starts. Five starts times eight steps converges to well under a thousandth of a texel on real
        // outlines, and the two endpoints are seeded separately so a start that walks out of [0,1] cannot
        // lose the true minimum.
        SignedDistance CubicSignedDistance( const EdgeSegment& edge, Vec2 origin, double& param )
        {
            constexpr int kSearchStarts = 4;
            constexpr int kSearchSteps  = 8;

            const Vec2 fromStart = edge.Points[0] - origin;
            const Vec2 firstLeg  = edge.Points[1] - edge.Points[0];
            const Vec2 bend      = ( edge.Points[2] - edge.Points[1] ) - firstLeg;
            const Vec2 twist = ( edge.Points[3] - edge.Points[2] ) - ( edge.Points[2] - edge.Points[1] ) - bend;

            const auto pointAt = [&]( double walk )
            {
                return fromStart + ( ( kCubicDerivativeFactor * walk ) * firstLeg ) +
                       ( ( kCubicDerivativeFactor * walk * walk ) * bend ) + ( ( walk * walk * walk ) * twist );
            };

            Vec2   endDirection = edge.Direction( 0.0 );
            double minDistance  = NonZeroSign( Cross( endDirection, fromStart ) ) * Length( fromStart );
            param               = -Dot( fromStart, endDirection ) / Dot( endDirection, endDirection );

            endDirection             = edge.Direction( 1.0 );
            const Vec2   fromEnd     = edge.Points[3] - origin;
            const double endDistance = Length( fromEnd );
            if ( endDistance < std::fabs( minDistance ) )
            {
                minDistance = NonZeroSign( Cross( endDirection, fromEnd ) ) * endDistance;
                param       = Dot( endDirection - fromEnd, endDirection ) / Dot( endDirection, endDirection );
            }

            for ( int start = 0; start <= kSearchStarts; ++start )
            {
                double walk    = static_cast<double>( start ) / kSearchStarts;
                Vec2   toPoint = pointAt( walk );
                for ( int step = 0; step < kSearchSteps; ++step )
                {
                    const Vec2 slope = ( kCubicDerivativeFactor * firstLeg ) +
                                       ( ( kCubicSecondDerivative * walk ) * bend ) +
                                       ( ( kCubicDerivativeFactor * walk * walk ) * twist );
                    const Vec2 curvature =
                         ( kCubicSecondDerivative * bend ) + ( ( kCubicSecondDerivative * walk ) * twist );
                    const double denominator = Dot( slope, slope ) + Dot( toPoint, curvature );
                    if ( std::fabs( denominator ) < kEpsilon )
                    {
                        break;
                    }
                    walk -= Dot( toPoint, slope ) / denominator;
                    if ( walk <= 0.0 || walk >= 1.0 )
                    {
                        break;
                    }
                    toPoint               = pointAt( walk );
                    const double distance = Length( toPoint );
                    if ( distance < std::fabs( minDistance ) )
                    {
                        minDistance = NonZeroSign( Cross( slope, toPoint ) ) * distance;
                        param       = walk;
                    }
                }
            }
            return CurveResult( edge, origin, minDistance, param );
        }

        // Signed distance from `origin` to one segment, plus the curve parameter of the closest point.
        // `param` may leave [0,1]: that is what tells the pseudo-distance step below that the point lies
        // off the end of the segment and must be measured against its infinite extension instead.
        SignedDistance SegmentSignedDistance( const EdgeSegment& edge, Vec2 origin, double& param )
        {
            if ( edge.PointCount == kLinePoints )
            {
                return LineSignedDistance( edge, origin, param );
            }
            if ( edge.PointCount == kQuadraticPoints )
            {
                return QuadraticSignedDistance( edge, origin, param );
            }
            return CubicSignedDistance( edge, origin, param );
        }

        // Past the end of a segment, the plain distance bends around the endpoint and every channel's
        // field acquires a false circular arc there. The PSEUDO-distance extends the segment's end
        // tangent to infinity instead, so a channel that does not own this corner reports a straight
        // half-plane and the median can still find the true intersection. This is the step that makes a
        // corner sharp rather than merely un-rounded.
        void ToPseudoDistance( SignedDistance& distance, const EdgeSegment& edge, Vec2 origin, double param )
        {
            const bool beforeStart = param < 0.0;
            const bool afterEnd    = param > 1.0;
            if ( !beforeStart && !afterEnd )
            {
                return;
            }

            const double endParam  = beforeStart ? 0.0 : 1.0;
            const Vec2   direction = Normalize( edge.Direction( endParam ) );
            const Vec2   fromEnd   = origin - edge.PointAt( endParam );
            const double along     = Dot( fromEnd, direction );
            if ( beforeStart ? along >= 0.0 : along <= 0.0 )
            {
                return; // the foot is back inside the segment, and the plain distance already has it
            }

            const double pseudo = Cross( fromEnd, direction );
            if ( std::fabs( pseudo ) <= std::fabs( distance.Distance ) )
            {
                distance.Distance = pseudo;
                distance.Cosine   = 0.0;
            }
        }

        bool IsCorner( Vec2 incoming, Vec2 outgoing, double crossThreshold )
        {
            return Dot( incoming, outgoing ) <= 0.0 || std::fabs( Cross( incoming, outgoing ) ) > crossThreshold;
        }

        // Step to the next colour for a new spline. `banned` is the colour the new spline must not share
        // with (used to stop the last spline of a closed contour from matching the first, which would
        // silently un-sharpen that one corner).
        void SwitchColor( EdgeColor& color, uint64_t& seed, EdgeColor banned = EdgeColor::Black )
        {
            const auto current   = static_cast<unsigned char>( color );
            const auto forbidden = static_cast<unsigned char>( banned );
            const auto shared    = static_cast<unsigned char>( current & forbidden );
            if ( shared == kRedBit || shared == kGreenBit || shared == kBlueBit )
            {
                color = static_cast<EdgeColor>( shared ^ kAllBits );
                return;
            }
            if ( color == EdgeColor::Black || color == EdgeColor::White )
            {
                constexpr std::array<EdgeColor, 3> kStart = { EdgeColor::Cyan, EdgeColor::Magenta,
                                                              EdgeColor::Yellow };
                color                                     = kStart.at( seed % kStart.size() );
                seed /= kStart.size();
                return;
            }
            const unsigned shifted = static_cast<unsigned>( current ) << ( 1U + ( seed & 1U ) );
            color                  = static_cast<EdgeColor>( ( shifted | ( shifted >> 3U ) ) & kAllBits );
            seed >>= 1U;
        }

        // Split one segment into three at t = 1/3, 2/3. Only needed so that a contour with a single
        // corner and fewer than three edges still has three splines to colour; without it a two-edge
        // teardrop (which real fonts do contain) would get two colours and lose its one corner.
        void SplitInThirds( const EdgeSegment& edge, std::array<EdgeSegment, 3>& parts )
        {
            constexpr double kFirstCut  = 1.0 / 3.0;
            constexpr double kSecondCut = 2.0 / 3.0;

            for ( EdgeSegment& part : parts )
            {
                part.PointCount = edge.PointCount;
                part.Color      = edge.Color;
            }

            if ( edge.PointCount == kLinePoints )
            {
                parts[0].Points[0] = edge.Points[0];
                parts[0].Points[1] = edge.PointAt( kFirstCut );
                parts[1].Points[0] = edge.PointAt( kFirstCut );
                parts[1].Points[1] = edge.PointAt( kSecondCut );
                parts[2].Points[0] = edge.PointAt( kSecondCut );
                parts[2].Points[1] = edge.Points[1];
                return;
            }
            if ( edge.PointCount == kQuadraticPoints )
            {
                parts[0].Points[0] = edge.Points[0];
                parts[0].Points[1] = Lerp( edge.Points[0], edge.Points[1], kFirstCut );
                parts[0].Points[2] = edge.PointAt( kFirstCut );
                parts[1].Points[0] = edge.PointAt( kFirstCut );
                parts[1].Points[1] = Lerp( Lerp( edge.Points[0], edge.Points[1], kSecondCut ),
                                           Lerp( edge.Points[1], edge.Points[2], kFirstCut ), kFieldCentre );
                parts[1].Points[2] = edge.PointAt( kSecondCut );
                parts[2].Points[0] = edge.PointAt( kSecondCut );
                parts[2].Points[1] = Lerp( edge.Points[1], edge.Points[2], kSecondCut );
                parts[2].Points[2] = edge.Points[2];
                return;
            }

            // Cubic: the Bernstein weights of de Casteljau at each cut, written out.
            const auto blend = [&edge]( double wgt0, double wgt1, double wgt2, double wgt3 ) -> Vec2
            {
                return { ( wgt0 * edge.Points[0].X ) + ( wgt1 * edge.Points[1].X ) + ( wgt2 * edge.Points[2].X ) +
                              ( wgt3 * edge.Points[3].X ),
                         ( wgt0 * edge.Points[0].Y ) + ( wgt1 * edge.Points[1].Y ) + ( wgt2 * edge.Points[2].Y ) +
                              ( wgt3 * edge.Points[3].Y ) };
            };
            constexpr double kNinth         = 1.0 / kCardanoP;
            constexpr double kFourNinths    = kQuarticDiscriminant / kCardanoP;
            constexpr double kTwentySeventh = 1.0 / kCardanoQ;

            parts[0].Points[0] = edge.Points[0];
            parts[0].Points[1] = blend( kSecondCut, kFirstCut, 0.0, 0.0 );
            parts[0].Points[2] = blend( kFourNinths, kFourNinths, kNinth, 0.0 );
            parts[0].Points[3] = edge.PointAt( kFirstCut );
            parts[1].Points[0] = edge.PointAt( kFirstCut );
            parts[1].Points[1] = blend( kEighthSplit * kTwentySeventh, kThirdSplit * kTwentySeventh,
                                        kCubicSecondDerivative * kTwentySeventh, kTwentySeventh );
            parts[1].Points[2] = blend( kTwentySeventh, kCubicSecondDerivative * kTwentySeventh,
                                        kThirdSplit * kTwentySeventh, kEighthSplit * kTwentySeventh );
            parts[1].Points[3] = edge.PointAt( kSecondCut );
            parts[2].Points[0] = edge.PointAt( kSecondCut );
            parts[2].Points[1] = blend( 0.0, kNinth, kFourNinths, kFourNinths );
            parts[2].Points[2] = blend( 0.0, 0.0, kFirstCut, kSecondCut );
            parts[2].Points[3] = edge.Points[3];
        }

        struct EdgePoint
        {
            SignedDistance     MinDistance;
            const EdgeSegment* NearEdge  = nullptr;
            double             NearParam = 0.0;
        };

        bool HasChannel( EdgeColor color, unsigned char mask )
        {
            return ( static_cast<unsigned char>( color ) & mask ) != 0;
        }

        // The three channels' winning edges for one texel, pseudo-distances already applied. Each
        // channel sees only the edges whose colour carries its bit — that subset is the whole trick.
        std::array<EdgePoint, 3> NearestPerChannel( const Shape& shape, Vec2 pixel )
        {
            std::array<EdgePoint, 3>               nearest{};
            constexpr std::array<unsigned char, 3> kBits = { kRedBit, kGreenBit, kBlueBit };

            for ( const Contour& contour : shape.Contours )
            {
                for ( const EdgeSegment& edge : contour.Edges )
                {
                    double               param    = 0.0;
                    const SignedDistance distance = SegmentSignedDistance( edge, pixel, param );
                    for ( size_t idx = 0; idx < kBits.size(); ++idx )
                    {
                        if ( HasChannel( edge.Color, kBits.at( idx ) ) &&
                             distance < nearest.at( idx ).MinDistance )
                        {
                            nearest.at( idx ) = { distance, &edge, param };
                        }
                    }
                }
            }

            for ( EdgePoint& channel : nearest )
            {
                if ( channel.NearEdge != nullptr )
                {
                    ToPseudoDistance( channel.MinDistance, *channel.NearEdge, pixel, channel.NearParam );
                }
            }
            return nearest;
        }

        std::array<float, 3> ReadTexel( const std::vector<float>& field, size_t texel )
        {
            return { field[( texel * 3 ) + 0], field[( texel * 3 ) + 1], field[( texel * 3 ) + 2] };
        }

        // A texel where the three channels disagree ACROSS a neighbour is a "clash": the median flips
        // there and paints an isolated dot or notch that no outline accounts for. It is the one artefact
        // MSDF adds over a plain SDF, it happens where a feature is thinner than the sampling grid, and
        // the cure is to make that texel single-channel again (median in all three), which degrades it to
        // the ordinary distance field exactly where the extra channels could not be trusted anyway.
        bool DetectClash( const std::array<float, 3>& here, const std::array<float, 3>& there, double threshold )
        {
            std::array<float, 3> mine  = here;
            std::array<float, 3> yours = there;
            const auto           gap   = [&mine, &yours]( size_t idx )
            { return std::fabs( yours.at( idx ) - mine.at( idx ) ); };
            const auto swap = [&mine, &yours]( size_t lhs, size_t rhs )
            {
                std::swap( mine.at( lhs ), mine.at( rhs ) );
                std::swap( yours.at( lhs ), yours.at( rhs ) );
            };

            // Order the three channels by how much they disagree with the neighbour, largest first.
            if ( gap( 0 ) < gap( 1 ) )
            {
                swap( 0, 1 );
            }
            if ( gap( 1 ) < gap( 2 ) )
            {
                swap( 1, 2 );
                if ( gap( 0 ) < gap( 1 ) )
                {
                    swap( 0, 1 );
                }
            }

            const auto centre          = static_cast<float>( kFieldCentre );
            const bool middleDisagrees = gap( 1 ) >= threshold;
            const bool neighbourVaries = yours[0] != yours[1] || yours[0] != yours[2];
            // Of the pair, only flag the texel FARTHER from an edge: the nearer one is where the outline
            // actually is, and flattening that one would blunt a real feature.
            const bool weAreTheFarOne = std::fabs( mine[2] - centre ) >= std::fabs( yours[2] - centre );
            return middleDisagrees && neighbourVaries && weAreTheFarOne;
        }

        // Does this texel clash with any of its four neighbours? Split out of the sweep below so that
        // neither half is long enough to hide a case, and so the edge conditions are in one place.
        bool ClashesWithNeighbour( const std::vector<float>& rgb, int width, int height, int row, int col,
                                   double threshold )
        {
            const size_t here = ( static_cast<size_t>( row ) * width ) + col;
            const auto   mine = ReadTexel( rgb, here );

            const bool hasLeft  = col > 0;
            const bool hasRight = col < width - 1;
            const bool hasAbove = row > 0;
            const bool hasBelow = row < height - 1;

            return ( hasLeft && DetectClash( mine, ReadTexel( rgb, here - 1 ), threshold ) ) ||
                   ( hasRight && DetectClash( mine, ReadTexel( rgb, here + 1 ), threshold ) ) ||
                   ( hasAbove && DetectClash( mine, ReadTexel( rgb, here - width ), threshold ) ) ||
                   ( hasBelow && DetectClash( mine, ReadTexel( rgb, here + width ), threshold ) );
        }

        void CorrectErrors( std::vector<float>& rgb, int width, int height, double threshold )
        {
            std::vector<size_t> clashes;
            for ( int row = 0; row < height; ++row )
            {
                for ( int col = 0; col < width; ++col )
                {
                    if ( ClashesWithNeighbour( rgb, width, height, row, col, threshold ) )
                    {
                        clashes.push_back( ( static_cast<size_t>( row ) * width ) + col );
                    }
                }
            }

            for ( const size_t texel : clashes )
            {
                const auto  mine       = ReadTexel( rgb, texel );
                const float med        = Median( mine[0], mine[1], mine[2] );
                rgb[( texel * 3 ) + 0] = med;
                rgb[( texel * 3 ) + 1] = med;
                rgb[( texel * 3 ) + 2] = med;
            }
        }

        // Every edge index around one contour, starting at `start` and wrapping — so a spline that
        // straddles the contour's own seam is one run rather than two.
        size_t WrapIndex( size_t start, size_t offset, size_t count )
        {
            return ( start + offset ) % count;
        }

        std::vector<size_t> FindCorners( const Contour& contour, double crossThreshold )
        {
            std::vector<size_t> corners;
            Vec2                previous = contour.Edges.back().Direction( 1.0 );
            size_t              index    = 0;
            for ( const EdgeSegment& edge : contour.Edges )
            {
                if ( IsCorner( Normalize( previous ), Normalize( edge.Direction( 0.0 ) ), crossThreshold ) )
                {
                    corners.push_back( index );
                }
                previous = edge.Direction( 1.0 );
                ++index;
            }
            return corners;
        }

        // One corner: the contour needs three splines around it so the two incident edges land in
        // different channels. Fewer than three edges are subdivided first.
        void ColorTeardrop( Contour& contour, size_t corner, EdgeColor& color, uint64_t& seed )
        {
            constexpr size_t         kSplines = 3;
            std::array<EdgeColor, 3> colors{};
            SwitchColor( color, seed );
            colors[0] = color;
            colors[1] = EdgeColor::White;
            SwitchColor( color, seed );
            colors[2] = color;

            size_t start = corner;
            size_t count = contour.Edges.size();
            if ( count < kSplines )
            {
                std::vector<EdgeSegment> split;
                split.reserve( count * kSplines );
                for ( size_t offset = 0; offset < count; ++offset )
                {
                    std::array<EdgeSegment, 3> parts{};
                    SplitInThirds( contour.Edges[WrapIndex( start, offset, count )], parts );
                    for ( const EdgeSegment& part : parts )
                    {
                        split.push_back( part );
                    }
                }
                contour.Edges = std::move( split );
                count         = contour.Edges.size();
                start         = 0;
            }

            for ( size_t offset = 0; offset < count; ++offset )
            {
                const size_t band = std::min( ( kSplines * offset ) / count, kSplines - 1 );
                contour.Edges[WrapIndex( start, offset, count )].Color = colors.at( band );
            }
        }

        void ColorSplines( Contour& contour, const std::vector<size_t>& corners, EdgeColor& color, uint64_t& seed )
        {
            const size_t cornerCount = corners.size();
            const size_t count       = contour.Edges.size();
            const size_t start       = corners.front();
            size_t       spline      = 0;

            SwitchColor( color, seed );
            const EdgeColor initialColor = color;
            for ( size_t offset = 0; offset < count; ++offset )
            {
                const size_t index = WrapIndex( start, offset, count );
                if ( spline + 1 < cornerCount && corners[spline + 1] == index )
                {
                    ++spline;
                    const bool last = ( spline == cornerCount - 1 );
                    SwitchColor( color, seed, last ? initialColor : EdgeColor::Black );
                }
                contour.Edges[index].Color = color;
            }
        }
    } // namespace

    Vec2 EdgeSegment::PointAt( double param ) const
    {
        if ( PointCount == kLinePoints )
        {
            return Lerp( Points[0], Points[1], param );
        }
        if ( PointCount == kQuadraticPoints )
        {
            return Lerp( Lerp( Points[0], Points[1], param ), Lerp( Points[1], Points[2], param ), param );
        }
        const Vec2 firstPair  = Lerp( Points[0], Points[1], param );
        const Vec2 middlePair = Lerp( Points[1], Points[2], param );
        const Vec2 lastPair   = Lerp( Points[2], Points[3], param );
        return Lerp( Lerp( firstPair, middlePair, param ), Lerp( middlePair, lastPair, param ), param );
    }

    Vec2 EdgeSegment::Direction( double param ) const
    {
        if ( PointCount == kLinePoints )
        {
            return Points[1] - Points[0];
        }

        const Vec2 firstLeg  = Points[1] - Points[0];
        const Vec2 secondLeg = Points[2] - Points[1];
        if ( PointCount == kQuadraticPoints )
        {
            const Vec2 slope = Lerp( firstLeg, secondLeg, param );
            // A degenerate control point makes the derivative vanish at an end; the chord is the only
            // direction left, and returning zero instead would make every angle test at that join lie.
            if ( std::fabs( slope.X ) < kEpsilon && std::fabs( slope.Y ) < kEpsilon )
            {
                return Points[2] - Points[0];
            }
            return slope;
        }

        const Vec2 thirdLeg = Points[3] - Points[2];
        const Vec2 slope = Lerp( Lerp( firstLeg, secondLeg, param ), Lerp( secondLeg, thirdLeg, param ), param );
        if ( std::fabs( slope.X ) < kEpsilon && std::fabs( slope.Y ) < kEpsilon )
        {
            if ( param == 0.0 )
            {
                return Points[2] - Points[0];
            }
            if ( param == 1.0 )
            {
                return Points[3] - Points[1];
            }
        }
        return slope;
    }

    void ColorEdges( Shape& shape, double angleThresholdRad, uint64_t seed )
    {
        const double crossThreshold = std::sin( angleThresholdRad );
        EdgeColor    color          = EdgeColor::White;

        for ( Contour& contour : shape.Contours )
        {
            if ( contour.Edges.empty() )
            {
                continue;
            }

            const std::vector<size_t> corners = FindCorners( contour, crossThreshold );
            if ( corners.empty() )
            {
                // A smooth closed contour ('O', 'o', a bowl) has no corner to preserve, so all three
                // channels carry the same field and the median degrades to the ordinary distance.
                for ( EdgeSegment& edge : contour.Edges )
                {
                    edge.Color = EdgeColor::White;
                }
            }
            else if ( corners.size() == 1 )
            {
                ColorTeardrop( contour, corners.front(), color, seed );
            }
            else
            {
                ColorSplines( contour, corners, color, seed );
            }
        }
    }

    void GenerateMSDF( std::vector<float>& outRGB, int width, int height, const Shape& shape, double rangeTexels )
    {
        outRGB.assign( static_cast<size_t>( width ) * height * 3, 0.0F );
        if ( width <= 0 || height <= 0 || rangeTexels <= 0.0 )
        {
            return;
        }

        for ( int row = 0; row < height; ++row )
        {
            for ( int col = 0; col < width; ++col )
            {
                const Vec2   pixel{ col + kFieldCentre, row + kFieldCentre };
                const auto   channels = NearestPerChannel( shape, pixel );
                const size_t texel    = ( ( static_cast<size_t>( row ) * width ) + col ) * 3;
                for ( size_t idx = 0; idx < channels.size(); ++idx )
                {
                    outRGB[texel + idx] = static_cast<float>(
                         ( channels.at( idx ).MinDistance.Distance / rangeTexels ) + kFieldCentre );
                }
            }
        }

        // 1.001 rather than 1: a clash is a disagreement of more than one texel of distance between
        // neighbours, and the slack keeps an exactly-one-texel step (which is the legitimate slope of a
        // distance field) from being flagged on every edge in the glyph.
        constexpr double kClashSlack = 1.001;
        CorrectErrors( outRGB, width, height, kClashSlack / rangeTexels );
    }

    void GenerateSDF( std::vector<float>& outR, int width, int height, const Shape& shape, double rangeTexels )
    {
        outR.assign( static_cast<size_t>( width ) * height, 0.0F );
        if ( width <= 0 || height <= 0 || rangeTexels <= 0.0 )
        {
            return;
        }

        for ( int row = 0; row < height; ++row )
        {
            for ( int col = 0; col < width; ++col )
            {
                const Vec2     pixel{ col + kFieldCentre, row + kFieldCentre };
                SignedDistance best;
                for ( const Contour& contour : shape.Contours )
                {
                    for ( const EdgeSegment& edge : contour.Edges )
                    {
                        double               param    = 0.0;
                        const SignedDistance distance = SegmentSignedDistance( edge, pixel, param );
                        if ( distance < best )
                        {
                            best = distance;
                        }
                    }
                }
                outR[( static_cast<size_t>( row ) * width ) + col] =
                     static_cast<float>( ( best.Distance / rangeTexels ) + kFieldCentre );
            }
        }
    }
} // namespace Desert::Text::Msdf
