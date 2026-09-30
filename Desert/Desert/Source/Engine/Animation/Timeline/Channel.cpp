#include "Channel.hpp"

#include <algorithm>
#include <cassert>
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
            return prev;
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
} // namespace Desert::Animation::Timeline
