#include "Channel.hpp"

#include <Engine/Animation/Timeline/Section.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>
#include <type_traits>

namespace Desert::Animation::Timeline
{
    static_assert( std::is_same_v<std::variant_alternative_t<static_cast<size_t>( ChannelKind::Float ), Channel>,
                                  FloatChannel> );
    static_assert( std::is_same_v<std::variant_alternative_t<static_cast<size_t>( ChannelKind::Vector ), Channel>,
                                  VectorChannel> );
    static_assert(
         std::is_same_v<std::variant_alternative_t<static_cast<size_t>( ChannelKind::Rotation ), Channel>,
                        RotationChannel> );
    static_assert(
         std::is_same_v<std::variant_alternative_t<static_cast<size_t>( ChannelKind::Transform ), Channel>,
                        TransformChannel> );
    static_assert( std::is_same_v<std::variant_alternative_t<static_cast<size_t>( ChannelKind::Bool ), Channel>,
                                  BoolChannel> );
    static_assert( std::is_same_v<std::variant_alternative_t<static_cast<size_t>( ChannelKind::Event ), Channel>,
                                  EventChannel> );

    namespace
    {
        /**
         * The segment a sample falls in: the keys either side, and how far through it the sample sits.
         *
         * THE SEGMENT IS (prev, next] — the rule `BoneTrack` samples by today, kept so that lifting every
         * clip in the corpus is bit-exact (a key tick is the END of the segment arriving at it, which is
         * also what `KeyInterp` on the later key means). `next` is the first key at or after the sample as
         * a REAL tick count, not as the whole tick: a sample a sub-tick past a key is inside the segment
         * that key starts, never a factor above 1 in the one before it.
         */
        struct Bracket
        {
            enum class Where : uint8_t
            {
                Before, ///< at or before the first key: hold `keys.front()`
                After,  ///< past the last key: hold `keys.back()`
                Inside,
            };
            Where  Place       = Where::Before;
            size_t Next        = 0;
            float  Factor      = 0.0F;
            double SpanSeconds = 0.0;
        };

        /// @pre `keys` is not empty.
        Bracket FindBracket( const std::vector<ScalarKey>& keys, const FrameTime at, const FrameRate tickRate )
        {
            const double t = at.AsTicks();
            const auto   next =
                 std::lower_bound( keys.begin(), keys.end(), t, []( const ScalarKey& key, double tick )
                                   { return static_cast<double>( key.Tick.Value ) < tick; } );
            Bracket out;
            if ( next == keys.begin() )
            {
                out.Place = Bracket::Where::Before;
                return out;
            }
            if ( next == keys.end() )
            {
                out.Place = Bracket::Where::After;
                return out;
            }
            const auto prev = next - 1;
            const auto span = static_cast<double>( next->Tick.Value - prev->Tick.Value );
            out.Place       = Bracket::Where::Inside;
            out.Next        = static_cast<size_t>( next - keys.begin() );
            // `span` > 0 by the channel invariant (sorted, one key per tick); the factor is computed exactly
            // as BoneTrack computes it, which is what makes the lift bit-exact.
            out.Factor = static_cast<float>( ( t - static_cast<double>( prev->Tick.Value ) ) / span );
            out.SpanSeconds =
                 span * static_cast<double>( tickRate.Denominator ) / static_cast<double>( tickRate.Numerator );
            return out;
        }

        float SampleKeys( const std::vector<ScalarKey>& keys, const Bracket& bracket )
        {
            switch ( bracket.Place )
            {
                case Bracket::Where::Before:
                    return keys.front().Value;
                case Bracket::Where::After:
                    return keys.back().Value;
                case Bracket::Where::Inside:
                    break;
            }
            const ScalarKey& prev = keys[bracket.Next - 1];
            const ScalarKey& next = keys[bracket.Next];
            return EvaluateSegment( prev.Value, prev.LeaveTangent, next.Value, next.ArriveTangent, next.Interp,
                                    bracket.SpanSeconds, bracket.Factor );
        }

        glm::quat QuatAt( const RotationChannel& channel, const size_t index )
        {
            return glm::quat( channel.W.Keys[index].Value, channel.X.Keys[index].Value,
                              channel.Y.Keys[index].Value, channel.Z.Keys[index].Value );
        }

        // ── Events ────────────────────────────────────────────────────────────────────────────────────

        /// One edge of one key, before ordering.
        struct Edge
        {
            double    At   = 0.0;
            size_t    Key  = 0;
            EventEdge Kind = EventEdge::Instant;
        };

        /// Ends before Begins and instants on the same tick (a state handing over to the next one is
        /// finished before its successor starts — `StepNotifyStates`' order), then list order.
        int Rank( const EventEdge kind )
        {
            return kind == EventEdge::End ? 0 : 1;
        }

        /**
         * Forward sweep over (lo, hi], or [lo, hi] when `loInclusive` (the part after a loop wrap starts ON
         * the range start). An instant fires on its tick; a state Begins on its tick and Ends on
         * Tick + Duration — the first tick it is no longer active on, since the span is [Tick, Tick + Duration).
         */
        void SweepForward( const EventChannel& channel, const double lo, const double hi, const bool loInclusive,
                           std::vector<Edge>& edges )
        {
            const auto inside = [&]( const double x ) { return ( loInclusive ? x >= lo : x > lo ) && x <= hi; };
            for ( size_t i = 0; i < channel.Keys.size(); ++i )
            {
                const EventKey& key   = channel.Keys[i];
                const auto      begin = static_cast<double>( key.Tick.Value );
                if ( key.Duration.Value <= 0 )
                {
                    if ( inside( begin ) )
                    {
                        edges.push_back( Edge{ begin, i, EventEdge::Instant } );
                    }
                    continue;
                }
                const double end = begin + static_cast<double>( key.Duration.Value );
                if ( inside( begin ) )
                {
                    edges.push_back( Edge{ begin, i, EventEdge::Begin } );
                }
                if ( inside( end ) )
                {
                    edges.push_back( Edge{ end, i, EventEdge::End } );
                }
            }
            std::stable_sort( edges.begin(), edges.end(), []( const Edge& a, const Edge& b )
                              { return a.At != b.At ? a.At < b.At : Rank( a.Kind ) < Rank( b.Kind ); } );
        }

        /**
         * Backward sweep from `hi` down to `lo`. An instant fires on [lo, hi) — the mirror of (from, to], so
         * an event a forward step landed on does not fire again when playback turns round on it. A state
         * Begins when playback enters it from above, through Tick + Duration, and Ends when it leaves it
         * downwards through Tick: both on (lo, hi], where the activity [Tick, Tick + Duration) changes.
         */
        void SweepBackward( const EventChannel& channel, const double lo, const double hi,
                            std::vector<Edge>& edges )
        {
            for ( size_t i = 0; i < channel.Keys.size(); ++i )
            {
                const EventKey& key   = channel.Keys[i];
                const auto      begin = static_cast<double>( key.Tick.Value );
                if ( key.Duration.Value <= 0 )
                {
                    if ( begin >= lo && begin < hi )
                    {
                        edges.push_back( Edge{ begin, i, EventEdge::Instant } );
                    }
                    continue;
                }
                const double end = begin + static_cast<double>( key.Duration.Value );
                if ( end > lo && end <= hi )
                {
                    edges.push_back( Edge{ end, i, EventEdge::Begin } );
                }
                if ( begin > lo && begin <= hi )
                {
                    edges.push_back( Edge{ begin, i, EventEdge::End } );
                }
            }
            std::stable_sort( edges.begin(), edges.end(), []( const Edge& a, const Edge& b )
                              { return a.At != b.At ? a.At > b.At : Rank( a.Kind ) < Rank( b.Kind ); } );
        }

        void Emit( const EventChannel& channel, const std::vector<Edge>& edges, std::vector<CrossedEvent>& out )
        {
            for ( const Edge& edge : edges )
            {
                out.push_back( CrossedEvent{ &channel.Keys[edge.Key], edge.Kind } );
            }
        }
    } // namespace

    const char* ToString( const ChannelKind kind )
    {
        switch ( kind )
        {
            case ChannelKind::Float:
                return "Float";
            case ChannelKind::Vector:
                return "Vector";
            case ChannelKind::Rotation:
                return "Rotation";
            case ChannelKind::Transform:
                return "Transform";
            case ChannelKind::Bool:
                return "Bool";
            case ChannelKind::Event:
                return "Event";
        }
        // A kind outside the enum is refused where it is READ; this names it for the log that says so.
        return "Unknown";
    }

    ChannelKind KindOf( const Channel& channel )
    {
        // The variant's alternatives are in ChannelKind's order — pinned by the static_asserts above.
        return static_cast<ChannelKind>( channel.index() );
    }

    Channel MakeChannel( const ChannelKind kind )
    {
        // Each struct's default member initialisers ARE the rest values (W = 1, Scale = 1), so a
        // default-constructed alternative is the channel a new section starts as.
        switch ( kind )
        {
            case ChannelKind::Float:
                return FloatChannel{};
            case ChannelKind::Vector:
                return VectorChannel{};
            case ChannelKind::Rotation:
                return RotationChannel{};
            case ChannelKind::Transform:
                return TransformChannel{};
            case ChannelKind::Bool:
                return BoolChannel{};
            case ChannelKind::Event:
                return EventChannel{};
        }
        // Unreachable for a valid kind: a kind outside the enum is refused where it is READ (the TMLN
        // reader), so no caller can ask for one — the assert says so rather than a quiet Float channel.
        assert( false && "MakeChannel: ChannelKind outside the enum" );
        return FloatChannel{};
    }

    float Evaluate( const FloatChannel& channel, const FrameTime at, const FrameRate tickRate )
    {
        if ( channel.Keys.empty() )
        {
            return channel.Default;
        }
        return SampleKeys( channel.Keys, FindBracket( channel.Keys, at, tickRate ) );
    }

    glm::vec3 Evaluate( const VectorChannel& channel, const FrameTime at, const FrameRate tickRate )
    {
        return glm::vec3( Evaluate( channel.X, at, tickRate ), Evaluate( channel.Y, at, tickRate ),
                          Evaluate( channel.Z, at, tickRate ) );
    }

    glm::quat Evaluate( const RotationChannel& channel, const FrameTime at, const FrameRate tickRate )
    {
        const std::vector<ScalarKey>& keys = channel.X.Keys;
        // The four components are keyed together (the channel's invariant, refused by Validate otherwise):
        // one bracket from X serves all four, and the quaternion is rebuilt whole on each side.
        assert( channel.Y.Keys.size() == keys.size() && channel.Z.Keys.size() == keys.size() &&
                channel.W.Keys.size() == keys.size() );
        if ( keys.empty() )
        {
            return glm::quat( channel.W.Default, channel.X.Default, channel.Y.Default, channel.Z.Default );
        }
        const Bracket bracket = FindBracket( keys, at, tickRate );
        switch ( bracket.Place )
        {
            case Bracket::Where::Before:
                return QuatAt( channel, 0 );
            case Bracket::Where::After:
                return QuatAt( channel, keys.size() - 1 );
            case Bracket::Where::Inside:
                break;
        }
        const glm::quat prev = QuatAt( channel, bracket.Next - 1 );
        // CONSTANT OR SLERP — RotationKeyFrame's two shapes; `Cubic` is refused by Validate until squad.
        if ( keys[bracket.Next].Interp == KeyInterp::Constant )
        {
            // Held until the later key, and that key's own tick reads it (as `EvaluateSegment` does).
            return bracket.Factor < 1.0F ? prev : QuatAt( channel, bracket.Next );
        }
        return glm::slerp( prev, QuatAt( channel, bracket.Next ), bracket.Factor );
    }

    BoneTransform Evaluate( const TransformChannel& channel, const FrameTime at, const FrameRate tickRate )
    {
        BoneTransform out;
        out.Translation = Evaluate( channel.Translation, at, tickRate );
        out.Rotation    = Evaluate( channel.Rotation, at, tickRate );
        out.Scale       = Evaluate( channel.Scale, at, tickRate );
        return out;
    }

    bool Evaluate( const BoolChannel& channel, const FrameTime at, const FrameRate tickRate )
    {
        // Keys are Constant and exactly 0 or 1 (the invariant), so the sampled value is one of the two.
        return Evaluate( channel.Bits, at, tickRate ) != 0.0F;
    }

    void CollectCrossed( const EventChannel& channel, const FrameTime from, const FrameTime to, const bool wrapped,
                         const FrameNumber rangeStart, const FrameNumber rangeEnd, std::vector<CrossedEvent>& out )
    {
        const double      a = from.AsTicks();
        const double      b = to.AsTicks();
        std::vector<Edge> edges;
        if ( wrapped )
        {
            // A FORWARD wrap: (from, end] then [start, to]. The player never skips more than one loop per
            // step, so these two sweeps are the whole interval. A backward wrap is two backward steps, and
            // the evaluator hands them over as two unwrapped calls (to Start, then from End).
            SweepForward( channel, a, static_cast<double>( rangeEnd.Value ), false, edges );
            Emit( channel, edges, out );
            edges.clear();
            SweepForward( channel, static_cast<double>( rangeStart.Value ), b, true, edges );
            Emit( channel, edges, out );
            return;
        }
        if ( b >= a )
        {
            SweepForward( channel, a, b, false, edges );
        }
        else
        {
            SweepBackward( channel, b, a, edges );
        }
        Emit( channel, edges, out );
    }
    // ── Section weight ────────────────────────────────────────────────────────────────────────────────

    // Declared in Section.hpp, DEFINED HERE because a weight is a bare key list sampled by the channel
    // sampler above: wrapping it in a FloatChannel to call `Evaluate` would copy the keys on every sample,
    // and a second bracket search would be a second sampling rule.
    float WeightAt( const Section& section, const FrameTime at, const FrameRate tickRate )
    {
        if ( section.Weight.empty() )
        {
            return 1.0F;
        }
        return SampleKeys( section.Weight, FindBracket( section.Weight, at, tickRate ) );
    }

    // ── Easing presets ────────────────────────────────────────────────────────────────────────────────

    namespace
    {
        /**
         * The easing formulas, normalised: f(0) = 0, f(1) = 1, u in [0, 1]. They are the UI tween's
         * (UICanvasRenderer2D `Ease`, deleted with `UIEasing` by the UI lift) term for term, so a lifted
         * UI clip plays the curve it played before. Double precision: they feed key values and the
         * deviation report, not playback.
         */
        constexpr double kBackC1    = 1.70158;
        constexpr double kBackC3    = kBackC1 + 1.0;
        constexpr double kElasticC4 = 2.0 * std::numbers::pi / 3.0;
        constexpr double kBounceN1  = 7.5625;
        constexpr double kBounceD1  = 2.75;

        /// Bounce's four parabolas: the offset each is centred on and the height it rests at.
        struct BouncePiece
        {
            double Centre = 0.0;
            double Floor  = 0.0;
        };
        BouncePiece BouncePieceAt( const double u )
        {
            if ( u < 1.0 / kBounceD1 )
            {
                return { 0.0, 0.0 };
            }
            if ( u < 2.0 / kBounceD1 )
            {
                return { 1.5 / kBounceD1, 0.75 };
            }
            if ( u < 2.5 / kBounceD1 )
            {
                return { 2.25 / kBounceD1, 0.9375 };
            }
            return { 2.625 / kBounceD1, 0.984375 };
        }

        double EaseValue( const EasingPreset preset, const double u )
        {
            switch ( preset )
            {
                case EasingPreset::Linear:
                    return u;
                case EasingPreset::QuadIn:
                    return u * u;
                case EasingPreset::QuadOut:
                    return 1.0 - ( 1.0 - u ) * ( 1.0 - u );
                case EasingPreset::QuadInOut:
                    return u < 0.5 ? 2.0 * u * u : 1.0 - 2.0 * ( 1.0 - u ) * ( 1.0 - u );
                case EasingPreset::CubicIn:
                    return u * u * u;
                case EasingPreset::CubicOut:
                    return 1.0 - std::pow( 1.0 - u, 3.0 );
                case EasingPreset::CubicInOut:
                    return u < 0.5 ? 4.0 * u * u * u : 1.0 - std::pow( -2.0 * u + 2.0, 3.0 ) * 0.5;
                case EasingPreset::BackOut:
                    return 1.0 + kBackC3 * std::pow( u - 1.0, 3.0 ) + kBackC1 * std::pow( u - 1.0, 2.0 );
                case EasingPreset::ElasticOut:
                    if ( u <= 0.0 || u >= 1.0 )
                    {
                        return u;
                    }
                    return std::pow( 2.0, -10.0 * u ) * std::sin( ( u * 10.0 - 0.75 ) * kElasticC4 ) + 1.0;
                case EasingPreset::BounceOut:
                {
                    const BouncePiece piece = BouncePieceAt( u );
                    const double      d     = u - piece.Centre;
                    return kBounceN1 * d * d + piece.Floor;
                }
            }
            return u;
        }

        /// df/du, analytic. At an InOut's middle both halves agree; at a bounce's floor contact the piece
        /// `u` falls in speaks (the contact lands between display frames, never on a key the bake makes).
        double EaseSlope( const EasingPreset preset, const double u )
        {
            switch ( preset )
            {
                case EasingPreset::Linear:
                    return 1.0;
                case EasingPreset::QuadIn:
                    return 2.0 * u;
                case EasingPreset::QuadOut:
                    return 2.0 * ( 1.0 - u );
                case EasingPreset::QuadInOut:
                    return u < 0.5 ? 4.0 * u : 4.0 * ( 1.0 - u );
                case EasingPreset::CubicIn:
                    return 3.0 * u * u;
                case EasingPreset::CubicOut:
                    return 3.0 * ( 1.0 - u ) * ( 1.0 - u );
                case EasingPreset::CubicInOut:
                    return u < 0.5 ? 12.0 * u * u : 12.0 * ( 1.0 - u ) * ( 1.0 - u );
                case EasingPreset::BackOut:
                    return 3.0 * kBackC3 * ( u - 1.0 ) * ( u - 1.0 ) + 2.0 * kBackC1 * ( u - 1.0 );
                case EasingPreset::ElasticOut:
                {
                    const double phase = ( u * 10.0 - 0.75 ) * kElasticC4;
                    return std::pow( 2.0, -10.0 * u ) * ( -10.0 * std::numbers::ln2 * std::sin( phase ) +
                                                          10.0 * kElasticC4 * std::cos( phase ) );
                }
                case EasingPreset::BounceOut:
                    return 2.0 * kBounceN1 * ( u - BouncePieceAt( u ).Centre );
            }
            return 1.0;
        }

        enum class EasingShape : uint8_t
        {
            OneCubic,  ///< the formula IS a cubic: the two keys' tangents reproduce it exactly
            TwoCubics, ///< two cubics joined at u = 0.5: one key at the middle tick
            Baked,     ///< not a polynomial: keys on the display grid, deviation measured
        };

        EasingShape ShapeOf( const EasingPreset preset )
        {
            switch ( preset )
            {
                case EasingPreset::QuadInOut:
                case EasingPreset::CubicInOut:
                    return EasingShape::TwoCubics;
                case EasingPreset::ElasticOut:
                case EasingPreset::BounceOut:
                    return EasingShape::Baked;
                default:
                    return EasingShape::OneCubic;
            }
        }

        /// The largest |curve - formula| over the segment [first, last] of @p keys, in normalised units.
        float MeasureDeviation( const std::vector<ScalarKey>& keys, const size_t first, const size_t last,
                                const EasingPreset preset, const FrameRate tickRate )
        {
            constexpr int kSamplesPerSegment = 16;
            const double  startTick          = static_cast<double>( keys[first].Tick.Value );
            const double  span               = static_cast<double>( keys[last].Tick.Value ) - startTick;
            const double  delta =
                 static_cast<double>( keys[last].Value ) - static_cast<double>( keys[first].Value );
            double worst = 0.0;
            for ( size_t segment = first + 1; segment <= last; ++segment )
            {
                const double a = static_cast<double>( keys[segment - 1].Tick.Value );
                const double b = static_cast<double>( keys[segment].Tick.Value );
                for ( int i = 1; i < kSamplesPerSegment; ++i )
                {
                    const double tick = a + ( b - a ) * i / kSamplesPerSegment;
                    FrameTime    at;
                    at.Frame.Value     = static_cast<int32_t>( std::floor( tick ) );
                    at.Subframe        = static_cast<float>( tick - std::floor( tick ) );
                    const double value = SampleKeys( keys, FindBracket( keys, at, tickRate ) );
                    const double u     = ( at.AsTicks() - startTick ) / span;
                    // A flat segment has no shape to miss: every preset of it is the constant.
                    const double error =
                         delta == 0.0 ? 0.0
                                      : std::abs( ( value - keys[first].Value ) / delta - EaseValue( preset, u ) );
                    worst = std::max( worst, error );
                }
            }
            return static_cast<float>( worst );
        }
    } // namespace

    Common::ResultStr<EasingResult> ApplyEasingPreset( std::vector<ScalarKey>& keys, const size_t endKey,
                                                       const EasingPreset preset, const FrameRate tickRate,
                                                       const FrameRate displayRate )
    {
        if ( endKey == 0 || endKey >= keys.size() )
        {
            return Common::MakeFormattedError<EasingResult>(
                 "key {} of {} ends no segment: a preset shapes the segment ending at a key after the first",
                 endKey, keys.size() );
        }
        if ( !tickRate.IsValid() || !displayRate.IsValid() )
        {
            return Common::MakeFormattedError<EasingResult>(
                 "easing key {}: tick rate {}/{} or display rate {}/{} "
                 "is not a rate",
                 endKey, tickRate.Numerator, tickRate.Denominator, displayRate.Numerator,
                 displayRate.Denominator );
        }
        const FrameNumber startTick = keys[endKey - 1].Tick;
        const FrameNumber endTick   = keys[endKey].Tick;
        if ( !( startTick < endTick ) )
        {
            return Common::MakeFormattedError<EasingResult>(
                 "easing key {}: the segment runs from tick {} to tick {} — keys are sorted, one per tick", endKey,
                 startTick.Value, endTick.Value );
        }

        const auto   span        = static_cast<double>( endTick.Value - startTick.Value );
        const double spanSeconds = span / tickRate.AsDouble();
        const float  startValue  = keys[endKey - 1].Value;
        const double delta       = static_cast<double>( keys[endKey].Value ) - static_cast<double>( startValue );
        // A slope of the normalised formula, df/du, as a key tangent in value units per SECOND.
        const auto tangentAt = [&]( const double u )
        { return static_cast<float>( EaseSlope( preset, u ) * delta / spanSeconds ); };
        const auto keyAt = [&]( const FrameNumber tick )
        {
            const double u = static_cast<double>( tick.Value - startTick.Value ) / span;
            ScalarKey    key;
            key.Tick  = tick;
            key.Value = static_cast<float>( static_cast<double>( startValue ) + delta * EaseValue( preset, u ) );
            key.ArriveTangent = tangentAt( u );
            key.LeaveTangent  = key.ArriveTangent;
            key.Interp        = KeyInterp::Cubic;
            key.Mode          = TangentMode::User;
            return key;
        };

        // The keys between the two ends, strictly inside the segment, in tick order.
        std::vector<ScalarKey> inner;
        EasingResult           result;
        switch ( ShapeOf( preset ) )
        {
            case EasingShape::OneCubic:
                break;
            case EasingShape::TwoCubics:
            {
                if ( span < 2.0 )
                {
                    return Common::MakeFormattedError<EasingResult>(
                         "easing key {}: a {}-tick segment has no middle tick for the second cubic of {}", endKey,
                         endTick.Value - startTick.Value,
                         preset == EasingPreset::QuadInOut ? "QuadInOut" : "CubicInOut" );
                }
                inner.push_back(
                     keyAt( FrameNumber{ startTick.Value + ( endTick.Value - startTick.Value ) / 2 } ) );
                break;
            }
            case EasingShape::Baked:
            {
                // Every display frame strictly inside the segment, on the grid `SnapToDisplayRate` defines.
                const double ticksPerFrame = tickRate.AsDouble() / displayRate.AsDouble();
                for ( int64_t frame = DisplayFrameIndex( startTick, tickRate, displayRate );; ++frame )
                {
                    const FrameTime   candidate{ FrameNumber{ static_cast<int32_t>( std::llround(
                                                    static_cast<double>( frame ) * ticksPerFrame ) ) },
                                               0.0F };
                    const FrameNumber tick = SnapToDisplayRate( candidate, tickRate, displayRate );
                    if ( !( tick < endTick ) )
                    {
                        break;
                    }
                    if ( startTick < tick && ( inner.empty() || inner.back().Tick < tick ) )
                    {
                        inner.push_back( keyAt( tick ) );
                    }
                }
                break;
            }
        }

        ScalarKey& start   = keys[endKey - 1];
        ScalarKey& end     = keys[endKey];
        start.LeaveTangent = tangentAt( 0.0 );
        start.Mode         = TangentMode::User;
        end.ArriveTangent  = tangentAt( 1.0 );
        end.Mode           = TangentMode::User;
        // Linear is the one preset that is not a Hermite: its keys say so, which is what the curve
        // editor draws and what a later key edit keeps.
        end.Interp = preset == EasingPreset::Linear ? KeyInterp::Linear : KeyInterp::Cubic;

        result.InsertedKeys = static_cast<uint32_t>( inner.size() );
        keys.insert( keys.begin() + static_cast<std::ptrdiff_t>( endKey ), inner.begin(), inner.end() );

        // Exact shapes report 0 because they ARE the formula; a measurement there would report float noise
        // as a deviation. The two-cubic split is exact only when the middle tick is the segment's middle.
        const bool exact =
             ShapeOf( preset ) == EasingShape::OneCubic ||
             ( ShapeOf( preset ) == EasingShape::TwoCubics && ( endTick.Value - startTick.Value ) % 2 == 0 );
        if ( !exact )
        {
            result.MaxDeviation = MeasureDeviation( keys, endKey - 1, endKey + inner.size(), preset, tickRate );
        }
        return Common::MakeSuccess( result );
    }
} // namespace Desert::Animation::Timeline
