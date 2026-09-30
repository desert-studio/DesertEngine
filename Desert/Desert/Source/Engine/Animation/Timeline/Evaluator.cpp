#include "Evaluator.hpp"

#include <Engine/Animation/Skeleton.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace Desert::Animation::Timeline
{
    namespace
    {
        // ── The fold (Section.hpp): acc starts at the channel's Default, sections apply in (Row, list) order ──

        [[nodiscard]] glm::vec3 DefaultOf( const VectorChannel& channel )
        {
            return { channel.X.Default, channel.Y.Default, channel.Z.Default };
        }
        [[nodiscard]] glm::quat DefaultOf( const RotationChannel& channel )
        {
            return { channel.W.Default, channel.X.Default, channel.Y.Default, channel.Z.Default };
        }
        [[nodiscard]] float DefaultOf( const FloatChannel& channel )
        {
            return channel.Default;
        }
        [[nodiscard]] BoneTransform DefaultOf( const TransformChannel& channel )
        {
            return { DefaultOf( channel.Translation ), DefaultOf( channel.Rotation ), DefaultOf( channel.Scale ) };
        }
        [[nodiscard]] bool DefaultOf( const BoolChannel& channel )
        {
            // The same reading Evaluate( BoolChannel ) gives an empty channel.
            return channel.Bits.Default != 0.0F;
        }

        const glm::quat kIdentity( 1.0F, 0.0F, 0.0F, 0.0F );

        // Absolute at w == 1 is the key value itself: mix( a, b, 1 ) is not b bit for bit (Section.hpp).
        void Absolute( float& acc, const float v, const float w )
        {
            acc = w == 1.0F ? v : glm::mix( acc, v, w );
        }
        void Absolute( glm::vec3& acc, const glm::vec3& v, const float w )
        {
            acc = w == 1.0F ? v : glm::mix( acc, v, w );
        }
        void Absolute( glm::quat& acc, const glm::quat& v, const float w )
        {
            acc = w == 1.0F ? v : glm::slerp( acc, v, w );
        }
        // A BONE is spelled exactly as ApplySection (ClipSection.cpp) spells it — `acc + w * ( v - acc )`, not
        // glm::mix's `acc * ( 1 - w ) + v * w` — so a lifted generation-3 section at a partial weight lands on
        // the same bits the clip played before the lift; weight 0 leaves the pose underneath untouched.
        void Absolute( BoneTransform& acc, const BoneTransform& v, const float w )
        {
            if ( w == 1.0F )
            {
                acc = v;
                return;
            }
            if ( w == 0.0F )
            {
                return;
            }
            acc.Translation = acc.Translation + w * ( v.Translation - acc.Translation );
            acc.Rotation    = glm::slerp( acc.Rotation, v.Rotation, w );
            acc.Scale       = acc.Scale + w * ( v.Scale - acc.Scale );
        }
        // A bool has no in-between: a section states its value from half weight on.
        void Absolute( bool& acc, const bool v, const float w )
        {
            acc = w >= 0.5F ? v : acc;
        }

        void Additive( float& acc, const float v, const float w )
        {
            acc = acc + w * v;
        }
        void Additive( glm::vec3& acc, const glm::vec3& v, const float w )
        {
            acc = acc + w * v;
        }
        void Additive( glm::quat& acc, const glm::quat& v, const float w )
        {
            acc = acc * glm::slerp( kIdentity, v, w );
        }
        // ApplySection's spelling again (see Absolute above): the full-weight rotation offset is the key itself.
        void Additive( BoneTransform& acc, const BoneTransform& v, const float w )
        {
            if ( w == 0.0F )
            {
                return;
            }
            acc.Translation = acc.Translation + w * v.Translation;
            acc.Rotation    = acc.Rotation * ( w == 1.0F ? v.Rotation : glm::slerp( kIdentity, v.Rotation, w ) );
            acc.Scale       = acc.Scale * ( glm::vec3( 1.0F ) + w * ( v.Scale - glm::vec3( 1.0F ) ) );
        }
        /// [Start, End] inclusive on the REAL tick: a subframe past End is outside.
        [[nodiscard]] bool CoversTime( const Section& section, const FrameTime at )
        {
            if ( at.Frame < section.Start || section.End < at.Frame )
            {
                return false;
            }
            return at.Frame != section.End || at.Subframe <= 0.0F;
        }

        /**
         * @brief Visit the sections covering @p at in fold order: ascending Row, list order within a row.
         * No scratch list — rows are few, and Evaluate allocates nothing once warm.
         */
        template <typename Fn>
        void ForEachCovering( const Track& track, const FrameTime at, Fn&& fn )
        {
            int64_t previous = std::numeric_limits<int64_t>::min();
            for ( ;; )
            {
                int64_t row = std::numeric_limits<int64_t>::max();
                for ( const Section& section : track.Sections )
                {
                    if ( section.Row > previous && section.Row < row && CoversTime( section, at ) )
                    {
                        row = section.Row;
                    }
                }
                if ( row == std::numeric_limits<int64_t>::max() )
                {
                    return;
                }
                for ( const Section& section : track.Sections )
                {
                    if ( section.Row == row && CoversTime( section, at ) )
                    {
                        fn( section );
                    }
                }
                previous = row;
            }
        }

        template <typename TChannel>
        [[nodiscard]] bool FoldChannel( const Track& track, const FrameTime at, const FrameRate tickRate,
                                        EvaluatedValue& out )
        {
            using Value = std::remove_cvref_t<decltype( DefaultOf( std::declval<const TChannel&>() ) )>;
            bool  any   = false;
            Value acc{};
            ForEachCovering( track, at,
                             [&]( const Section& section )
                             {
                                 const auto* channel = std::get_if<Channel>( &section.Content );
                                 const auto* typed =
                                      channel != nullptr ? std::get_if<TChannel>( channel ) : nullptr;
                                 if ( typed == nullptr )
                                 {
                                     return; // Validate refuses a section of another kind; a sequence that skipped
                                             // it gets nothing
                                 }
                                 if ( !any )
                                 {
                                     acc = DefaultOf( *typed );
                                     any = true;
                                 }
                                 const Value v = Evaluate( *typed, at, tickRate );
                                 const float w = WeightAt( section, at, tickRate );
                                 if constexpr ( std::is_same_v<Value, bool> )
                                 {
                                     // A flag has no additive: Validate refuses an Additive section on a
                                     // Bool track, and a sequence that skipped it gets nothing from one.
                                     if ( section.Blend == SectionBlendType::Additive )
                                     {
                                         return;
                                     }
                                     Absolute( acc, v, w );
                                 }
                                 else if ( section.Blend == SectionBlendType::Additive )
                                 {
                                     Additive( acc, v, w );
                                 }
                                 else
                                 {
                                     Absolute( acc, v, w );
                                 }
                             } );
            if ( any )
            {
                out = acc;
            }
            return any;
        }

        [[nodiscard]] bool FoldValue( const Track& track, const FrameTime at, const FrameRate tickRate,
                                      EvaluatedValue& out )
        {
            switch ( track.Kind )
            {
                case TrackKind::Float:
                    return FoldChannel<FloatChannel>( track, at, tickRate, out );
                case TrackKind::Vector:
                    return FoldChannel<VectorChannel>( track, at, tickRate, out );
                case TrackKind::Rotation:
                    return FoldChannel<RotationChannel>( track, at, tickRate, out );
                case TrackKind::Transform:
                    return FoldChannel<TransformChannel>( track, at, tickRate, out );
                case TrackKind::Bool:
                    return FoldChannel<BoolChannel>( track, at, tickRate, out );
                case TrackKind::Event:
                case TrackKind::Animation:
                case TrackKind::CameraCut:
                    return false;
            }
            return false;
        }

        [[nodiscard]] FrameTime FromTicks( const double ticks )
        {
            const double whole = std::floor( ticks );
            return FrameTime{ FrameNumber{ static_cast<int32_t>( whole ) }, static_cast<float>( ticks - whole ) };
        }

        // ── Events: the step as one or two unwrapped legs ─────────────────────────────────────────────

        struct Leg
        {
            FrameTime From;
            FrameTime To;
            bool      Wrapped = false; ///< a FORWARD wrap, which CollectCrossed takes in one call
            bool      Forward = true;
        };

        /**
         * The step's path: its ends, its flags and its DIRECTION (the Player's, UE's evaluation-range
         * direction), so a wrap or a turn goes the way the playhead actually went.
         *
         *   plain       one leg, From → To (CollectCrossed orders backwards itself)
         *   Wrapped     forward: one wrapped leg; backward: [Start, From) then [To, End] — I1 left the
         *               backward wrap to two unwrapped calls
         *   Reversed    PingPong turned at End (From → End, End → To) or at Start (From → Start, Start → To)
         */
        [[nodiscard]] size_t LegsOf( const TimeStep& step, const FrameNumber start, const FrameNumber end,
                                     Leg ( &legs )[2] )
        {
            const double a       = step.From.AsTicks();
            const double b       = step.To.AsTicks();
            const bool   forward = step.Direction == PlayDirection::Forward;
            const auto   at      = []( const FrameNumber tick ) { return FrameTime{ tick, 0.0F }; };
            if ( step.Wrapped )
            {
                if ( forward )
                {
                    legs[0] = Leg{ step.From, step.To, true, true };
                    return 1;
                }
                // Backward legs are [to, from): the second leg starts half a tick past End so that End —
                // arrived at by the wrap, as Start is on a forward wrap — is inside it. Keys sit on ticks.
                legs[0] = Leg{ step.From, at( start ), false, false };
                legs[1] = Leg{ FrameTime{ end, 0.5F }, step.To, false, false };
                return 2;
            }
            if ( step.Reversed )
            {
                if ( forward )
                {
                    legs[0] = Leg{ step.From, at( end ), false, true };
                    legs[1] = Leg{ at( end ), step.To, false, false };
                }
                else
                {
                    legs[0] = Leg{ step.From, at( start ), false, false };
                    legs[1] = Leg{ at( start ), step.To, false, true };
                }
                return 2;
            }
            legs[0] = Leg{ step.From, step.To, false, !( b < a ) };
            return 1;
        }

        [[nodiscard]] FrameNumber TickOf( const CrossedEvent& event )
        {
            return event.Edge == EventEdge::End ? event.Key->Tick + event.Key->Duration : event.Key->Tick;
        }

        /// Distance along the leg — the firing order. Stable insertion sort keeps (track, CollectCrossed's
        /// own order) for events on one tick: Ends before Begins, lower tracks first.
        [[nodiscard]] double Along( const Leg& leg, const FiredEvent& event, const FrameNumber start,
                                    const FrameNumber end )
        {
            const double a = leg.From.AsTicks();
            const double t = TickOf( event.Event ).Value;
            if ( !leg.Forward )
            {
                return a - t;
            }
            if ( leg.Wrapped && t <= a )
            {
                return ( end.Value - a ) + ( t - start.Value ) + 1.0;
            }
            return t - a;
        }

        void SortLeg( std::vector<FiredEvent>& events, const size_t first, const Leg& leg, const FrameNumber start,
                      const FrameNumber end )
        {
            for ( size_t i = first + 1; i < events.size(); ++i )
            {
                const FiredEvent moving = events[i];
                const double key    = Along( leg, moving, start, end );
                size_t       j      = i;
                while ( j > first && Along( leg, events[j - 1], start, end ) > key )
                {
                    events[j] = events[j - 1];
                    --j;
                }
                events[j] = moving;
            }
        }
    } // namespace

    Evaluator::Evaluator( const Sequence& sequence ) : m_Sequence( &sequence )
    {
    }

    void Evaluator::Evaluate( const TimeStep& step, EvaluatedFrame& out ) const
    {
        out.Values.clear();
        out.Events.clear();
        out.Animations.clear();
        out.ActiveCamera.reset();

        const Sequence& sequence = *m_Sequence;
        const FrameTime at       = step.To;
        const FrameRate rate     = sequence.TickRate;

        for ( uint32_t ti = 0; ti < sequence.Tracks.size(); ++ti )
        {
            const Track& track = sequence.Tracks[ti];
            if ( track.Muted )
            {
                continue;
            }
            switch ( track.Kind )
            {
                case TrackKind::Event:
                    break; // below, leg by leg, so the firing order spans tracks
                case TrackKind::CameraCut:
                    // The last section in fold order wins — on one row cuts never overlap (Validate).
                    ForEachCovering( track, at,
                                     [&]( const Section& section )
                                     {
                                         if ( const auto* cut =
                                                   std::get_if<CameraCutSectionContent>( &section.Content ) )
                                         {
                                             out.ActiveCamera = cut->Camera;
                                         }
                                     } );
                    break;
                case TrackKind::Animation:
                    ForEachCovering(
                         track, at,
                         [&]( const Section& section )
                         {
                             const auto* anim = std::get_if<AnimationSectionContent>( &section.Content );
                             if ( anim == nullptr )
                             {
                                 return;
                             }
                             const double into = ( at.AsTicks() - section.Start.Value ) * anim->PlayRate;
                             out.Animations.push_back(
                                  AnimationSample{ ti, anim->Clip, FromTicks( anim->StartOffset.Value + into ),
                                                   section.Blend, WeightAt( section, at, rate ), anim->Loop } );
                         } );
                    break;
                default:
                {
                    EvaluatedValue value;
                    if ( FoldValue( track, at, rate, value ) )
                    {
                        out.Values.push_back( EvaluatedTrack{ ti, value } );
                    }
                    break;
                }
            }
        }

        CollectFired( sequence, step, out.Events );
    }

    void CollectFired( const Sequence& sequence, const TimeStep& step, std::vector<FiredEvent>& out )
    {
        Leg          legs[2];
        const size_t legCount = LegsOf( step, sequence.Start, sequence.End, legs );
        const bool   moved    = step.From.AsTicks() != step.To.AsTicks() || step.Wrapped || step.Reversed;

        std::vector<CrossedEvent> crossed;
        if ( !moved )
        {
            return; // a jump (From == To) crosses nothing
        }
        for ( size_t li = 0; li < legCount; ++li )
        {
            const Leg&   leg   = legs[li];
            const size_t first = out.size();
            for ( uint32_t ti = 0; ti < sequence.Tracks.size(); ++ti )
            {
                const Track& track = sequence.Tracks[ti];
                if ( track.Muted || track.Kind != TrackKind::Event )
                {
                    continue;
                }
                for ( const Section& section : track.Sections )
                {
                    const auto* channel = std::get_if<Channel>( &section.Content );
                    const auto* events  = channel != nullptr ? std::get_if<EventChannel>( channel ) : nullptr;
                    if ( events == nullptr )
                    {
                        continue;
                    }
                    crossed.clear();
                    CollectCrossed( *events, leg.From, leg.To, leg.Wrapped, sequence.Start, sequence.End,
                                    crossed );
                    for ( const CrossedEvent& event : crossed )
                    {
                        // A section contributes nothing outside [Start, End] — its keys may lie beyond it.
                        if ( section.Covers( TickOf( event ) ) )
                        {
                            out.push_back( FiredEvent{ ti, event } );
                        }
                    }
                }
            }
            SortLeg( out, first, leg, sequence.Start, sequence.End );
        }
    }

    bool EvaluateTrack( const Track& track, const FrameTime at, const FrameRate tickRate, EvaluatedValue& out )
    {
        return !track.Muted && FoldValue( track, at, tickRate, out );
    }

    ApplyReport Evaluator::Apply( const EvaluatedFrame& frame, ITimelineHost& host )
    {
        const Sequence& sequence = *m_Sequence;
        if ( m_ResolvedRevision != sequence.Revision || m_Resolved.size() != sequence.Bindings.size() )
        {
            m_Resolved.assign( sequence.Bindings.size(), std::nullopt );
            for ( size_t i = 0; i < sequence.Bindings.size(); ++i )
            {
                // A Sequence binding names no object: its tracks (Event, Camera Cut, clip curves) need none.
                if ( sequence.Bindings[i].Kind != BindingKind::Sequence )
                {
                    m_Resolved[i] = host.Resolve( sequence.Bindings[i] );
                }
            }
            m_ResolvedRevision = sequence.Revision;
        }

        ApplyReport report;
        for ( size_t i = 0; i < sequence.Bindings.size(); ++i )
        {
            if ( sequence.Bindings[i].Kind != BindingKind::Sequence && !m_Resolved[i] )
            {
                report.Unresolved.push_back( sequence.Bindings[i].Label );
            }
        }

        const auto resolvedOf = [&]( const BindingGuid& guid ) -> const std::optional<ResolvedBinding>*
        {
            const Binding* binding = FindBinding( sequence, guid );
            if ( binding == nullptr )
            {
                return nullptr;
            }
            return &m_Resolved[static_cast<size_t>( binding - sequence.Bindings.data() )];
        };

        for ( const EvaluatedTrack& value : frame.Values )
        {
            const Track& track = sequence.Tracks[value.TrackIndex];
            if ( const auto* target = resolvedOf( track.Binding ); target != nullptr && target->has_value() )
            {
                host.Apply( **target, track.Property, value.Value );
            }
        }
        for ( const AnimationSample& sample : frame.Animations )
        {
            const Track& track = sequence.Tracks[sample.TrackIndex];
            if ( const auto* target = resolvedOf( track.Binding ); target != nullptr && target->has_value() )
            {
                host.PlayAnimation( **target, sample );
            }
        }
        for ( const FiredEvent& event : frame.Events )
        {
            host.Fire( event );
        }

        std::optional<ResolvedBinding> camera;
        if ( frame.ActiveCamera )
        {
            if ( const auto* target = resolvedOf( *frame.ActiveCamera ); target != nullptr )
            {
                camera = *target;
            }
        }
        host.SetCamera( camera );
        return report;
    }

    BoneBindingTable BindBones( const Sequence& sequence, const Skeleton& skeleton )
    {
        BoneBindingTable table;
        table.Revision = sequence.Revision;
        table.BoneOfTrack.assign( sequence.Tracks.size(), UINT32_MAX );
        for ( size_t ti = 0; ti < sequence.Tracks.size(); ++ti )
        {
            const Track& track = sequence.Tracks[ti];
            if ( track.Kind != TrackKind::Transform )
            {
                continue;
            }
            const Binding* binding = FindBinding( sequence, track.Binding );
            if ( binding == nullptr || binding->Kind != BindingKind::Bone )
            {
                continue;
            }
            // By locator = bone name, Animator::ResolveTrack's rule: the clip and the rig come from different
            // files with different bone orders, and the name is the one thing they share.
            if ( const std::optional<uint32_t> bone = skeleton.FindBoneIndex( binding->Locator ) )
            {
                table.BoneOfTrack[ti] = *bone;
            }
            else
            {
                ++table.Missing;
            }
        }
        return table;
    }

    Common::BoolResultStr EvaluatePose( const Sequence& sequence, const BoneBindingTable& table,
                                        const FrameTime at, LocalPose& pose )
    {
        if ( table.Revision != sequence.Revision || table.BoneOfTrack.size() != sequence.Tracks.size() )
        {
            return Common::MakeFormattedError<bool>(
                 "EvaluatePose: the bone table was bound at revision {} for {} tracks and the sequence is at "
                 "revision {} with {} tracks; bind again (BindBones)",
                 table.Revision, table.BoneOfTrack.size(), sequence.Revision, sequence.Tracks.size() );
        }
        for ( size_t ti = 0; ti < sequence.Tracks.size(); ++ti )
        {
            const uint32_t bone  = table.BoneOfTrack[ti];
            const Track&   track = sequence.Tracks[ti];
            if ( bone == UINT32_MAX || track.Muted )
            {
                continue; // not a bone track, a bone this rig lacks, or muted: the pose underneath shows
            }
            if ( bone >= pose.Size() )
            {
                return Common::MakeFormattedError<bool>(
                     "EvaluatePose: track {} drives bone {} and the pose holds {} bones (sized for another rig)",
                     ti, bone, pose.Size() );
            }
            // THE FOLD STARTS FROM THE POSE, not from the channel's Default: for a bone the rest value the host
            // states IS what the caller seeded (the bind or retarget pose — ApplySection's `reference`), so a
            // partial Absolute section blends from it exactly as the generation-3 clip did.
            BoneTransform& acc = pose[bone];
            ForEachCovering( track, at,
                             [&]( const Section& section )
                             {
                                 const auto* channel = std::get_if<Channel>( &section.Content );
                                 const auto* typed =
                                      channel != nullptr ? std::get_if<TransformChannel>( channel ) : nullptr;
                                 if ( typed == nullptr )
                                 {
                                     return; // Validate refuses a section of another kind
                                 }
                                 const BoneTransform v = Evaluate( *typed, at, sequence.TickRate );
                                 const float         w = WeightAt( section, at, sequence.TickRate );
                                 if ( section.Blend == SectionBlendType::Additive )
                                 {
                                     Additive( acc, v, w );
                                 }
                                 else
                                 {
                                     Absolute( acc, v, w );
                                 }
                             } );
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Animation::Timeline
