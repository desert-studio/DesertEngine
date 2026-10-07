#include "ClipGeneration3.hpp"
#include "ClipInterpShift.hpp"

#include <Engine/Animation/Timeline/Binding.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

// `.anim` generation 3 → the clip's Sequence (Hosts.hpp). Its own file and not Hosts.cpp: Hosts.cpp holds the
// UI table and includes the ECS components, and the clip lift must build where the animation core builds —
// the TimelineContract suite and the migrator — without dragging the ECS in.

namespace Desert::Animation::Timeline
{
    using Migration::ClipGen3::AnimationClip;
    using Migration::ClipGen3::AnimationCurve;
    using Migration::ClipGen3::AnimationNotify;
    using Migration::ClipGen3::BoneTrack;
    using Migration::ClipGen3::ClipSection;
    using Migration::ClipGen3::PositionKeyFrame;
    using Migration::ClipGen3::RotationKeyFrame;
    using Migration::ClipGen3::ScaleKeyFrame;

    namespace
    {
        /// Sorted, one key per tick: the channel invariant. BoneTrack tolerated two keys on one tick (the later
        /// won at that tick, the earlier after it); folding them would change a sampled value, so it is refused.
        template <typename TKey>
        [[nodiscard]] std::string CheckKeys( const std::vector<TKey>& keys, const std::string& bone,
                                             const char* what )
        {
            for ( size_t i = 1; i < keys.size(); ++i )
            {
                if ( !( keys[i - 1].Tick < keys[i].Tick ) )
                {
                    return std::format(
                         "LiftClip: track '{}' {} keys {} and {} are on ticks {} and {} (unsorted or two on one "
                         "tick)",
                         bone, what, i - 1, i, keys[i - 1].Tick.Value, keys[i].Tick.Value );
                }
            }
            return {};
        }

        /// A vec3 key's component @p c as a ScalarKey: the same tick, interp, mode, tangents and weights.
        template <typename TKey>
        [[nodiscard]] ScalarKey Component( const TKey& key, const glm::vec3& value, const int c )
        {
            ScalarKey out;
            out.Tick          = key.Tick;
            out.Value         = value[c];
            out.ArriveTangent = key.ArriveTangent[c];
            out.LeaveTangent  = key.LeaveTangent[c];
            out.ArriveWeight  = key.ArriveWeight[c];
            out.LeaveWeight   = key.LeaveWeight[c];
            out.Interp        = key.Interp;
            out.Mode          = key.Mode;
            return out;
        }

        [[nodiscard]] TransformChannel ChannelOf( const BoneTrack& track )
        {
            TransformChannel out;
            for ( const PositionKeyFrame& key : track.PositionKeys )
            {
                out.Translation.X.Keys.push_back( Component( key, key.Position, 0 ) );
                out.Translation.Y.Keys.push_back( Component( key, key.Position, 1 ) );
                out.Translation.Z.Keys.push_back( Component( key, key.Position, 2 ) );
            }
            // The quaternion's four components, keyed together with the key's interp (Constant or Linear —
            // RotationKeyFrame has no tangents, and the channel samples it by slerp exactly as it did).
            for ( const RotationKeyFrame& key : track.RotationKeys )
            {
                const float   parts[4] = { key.Rotation.x, key.Rotation.y, key.Rotation.z, key.Rotation.w };
                FloatChannel* comps[4] = { &out.Rotation.X, &out.Rotation.Y, &out.Rotation.Z, &out.Rotation.W };
                for ( int c = 0; c < 4; ++c )
                {
                    ScalarKey scalar;
                    scalar.Tick   = key.Tick;
                    scalar.Value  = parts[c];
                    scalar.Interp = key.Interp;
                    comps[c]->Keys.push_back( scalar );
                }
            }
            for ( const ScaleKeyFrame& key : track.ScaleKeys )
            {
                out.Scale.X.Keys.push_back( Component( key, key.Scale, 0 ) );
                out.Scale.Y.Keys.push_back( Component( key, key.Scale, 1 ) );
                out.Scale.Z.Keys.push_back( Component( key, key.Scale, 2 ) );
            }
            return out;
        }

        /**
         * @brief The track's sections: the ClipSection answering at each tick of [0, duration], as runs.
         *
         * THE LIFT IS BY WINNER, NOT BY SECTION. Generation 3 says "the later section speaking for the track
         * wins, and no section means the authored value"; the fold (Section.hpp) composes every covering
         * section instead. Copying each ClipSection over verbatim would therefore change the value wherever
         * two overlapped at a partial weight, and leave the authored value un-stated wherever none covered.
         * So the range is cut where the winner changes, each run becomes ONE section holding the winner's
         * blend, weight and name (a gap: Absolute, empty weight), and no two lifted sections of a track
         * overlap — the fold then applies exactly the one section generation 3 applied.
         */
        void LiftSections( const AnimationClip& clip, const BoneTrack& track, Track& out )
        {
            const int32_t        last = clip.DurationTicks.Value;
            std::vector<int32_t> cuts{ 0, last + 1 };
            for ( const ClipSection& section : clip.Sections )
            {
                if ( !section.Speaks( track.BoneName ) )
                {
                    continue;
                }
                cuts.push_back( std::clamp( section.Start.Value, 0, last + 1 ) );
                cuts.push_back( std::clamp( section.End.Value + 1, 0, last + 1 ) );
            }
            std::sort( cuts.begin(), cuts.end() );
            cuts.erase( std::unique( cuts.begin(), cuts.end() ), cuts.end() );

            const TransformChannel keys = ChannelOf( track );
            const ClipSection*     open = nullptr;
            for ( size_t i = 0; i + 1 < cuts.size(); ++i )
            {
                const FrameNumber  from{ cuts[i] };
                const ClipSection* winner = clip.SectionFor( track.BoneName, from );
                if ( !out.Sections.empty() && winner == open )
                {
                    out.Sections.back().End = FrameNumber{ cuts[i + 1] - 1 }; // same winner: one run
                    continue;
                }
                Section lifted;
                lifted.Start   = from;
                lifted.End     = FrameNumber{ cuts[i + 1] - 1 };
                lifted.Content = Channel{ keys };
                if ( winner != nullptr )
                {
                    lifted.Blend  = winner->Blend;
                    lifted.Weight = winner->Weight;
                    lifted.Name   = winner->Name;
                }
                out.Sections.push_back( std::move( lifted ) );
                open = winner;
            }
        }
    } // namespace

    Common::ResultStr<Sequence> LiftClip( const AnimationClip& generation3 )
    {
        const AnimationClip& clip = generation3;
        if ( clip.DurationTicks.Value < 0 )
        {
            return Common::MakeFormattedError<Sequence>( "LiftClip: clip '{}' is {} ticks long",
                                                         clip.AnimationName, clip.DurationTicks.Value );
        }

        Sequence sequence;
        sequence.Host        = SequenceHost::AnimationClip;
        sequence.TickRate    = clip.TickRate;
        sequence.DisplayRate = clip.DisplayRate;
        sequence.Start       = FrameNumber{ 0 };
        sequence.End         = clip.DurationTicks;

        const Section whole = [&]
        {
            Section section;
            section.Start = sequence.Start;
            section.End   = sequence.End;
            return section;
        }();

        // ONE master binding for what belongs to the clip itself: its named curves and its notifies.
        const bool hasMaster = !clip.Curves.empty() || !clip.Notifies.empty();
        Binding    master;
        master.Guid  = BindingGuid::ForObject( BindingKind::Sequence, {} );
        master.Kind  = BindingKind::Sequence;
        master.Label = clip.AnimationName;
        if ( hasMaster )
        {
            sequence.Bindings.push_back( master );
        }

        for ( size_t i = 0; i < clip.Tracks.size(); ++i )
        {
            const BoneTrack& bone = clip.Tracks[i];
            for ( size_t j = 0; j < i; ++j )
            {
                if ( clip.Tracks[j].BoneName == bone.BoneName )
                {
                    return Common::MakeFormattedError<Sequence>(
                         "LiftClip: clip '{}' has two tracks for bone '{}' (tracks {} and {}); a bone has one "
                         "binding",
                         clip.AnimationName, bone.BoneName, j, i );
                }
            }
            for ( const std::string& refusal : { CheckKeys( bone.PositionKeys, bone.BoneName, "position" ),
                                                 CheckKeys( bone.RotationKeys, bone.BoneName, "rotation" ),
                                                 CheckKeys( bone.ScaleKeys, bone.BoneName, "scale" ) } )
            {
                if ( !refusal.empty() )
                {
                    return Common::MakeFormattedError<Sequence>( "{}", refusal );
                }
            }

            Binding binding;
            binding.Guid    = BindingGuid::ForObject( BindingKind::Bone, bone.BoneName );
            binding.Kind    = BindingKind::Bone;
            binding.Locator = bone.BoneName;
            binding.Label   = bone.BoneName;
            sequence.Bindings.push_back( binding );

            Track track;
            track.Binding = binding.Guid;
            track.Kind    = TrackKind::Transform;
            LiftSections( clip, bone, track );
            sequence.Tracks.push_back( std::move( track ) );
        }

        // A ClipSection naming a track the clip lacks says something about nothing: refused, not dropped.
        for ( const ClipSection& section : clip.Sections )
        {
            for ( const std::string& name : section.Tracks )
            {
                const bool known = std::any_of( clip.Tracks.begin(), clip.Tracks.end(),
                                                [&]( const BoneTrack& t ) { return t.BoneName == name; } );
                if ( !known )
                {
                    return Common::MakeFormattedError<Sequence>(
                         "LiftClip: clip '{}' section '{}' names track '{}', which the clip does not have",
                         clip.AnimationName, section.Name, name );
                }
            }
        }

        // Named curves: a Float track on the master binding, one full-range section. Generation 3 never put a
        // curve under a ClipSection (SampleTrack is bones only), so none is cut here.
        for ( const AnimationCurve& curve : clip.Curves )
        {
            Track track;
            track.Binding   = master.Guid;
            track.Property  = curve.Name;
            track.Kind      = TrackKind::Float;
            Section section = whole;
            section.Content = Channel{ FloatChannel{ curve.Keys, 0.0F } }; // AnimationCurve's empty value is 0
            track.Sections.push_back( std::move( section ) );
            sequence.Tracks.push_back( std::move( track ) );
        }

        // Notifies: ONE Event track; the notify's authoring row travels on the key.
        if ( !clip.Notifies.empty() )
        {
            EventChannel events;
            for ( const AnimationNotify& notify : clip.Notifies )
            {
                events.Keys.push_back( EventKey{ notify.Tick, notify.DurationTicks, notify.Name, notify.Track } );
            }
            Track track;
            track.Binding   = master.Guid;
            track.Kind      = TrackKind::Event;
            Section section = whole;
            section.Content = Channel{ std::move( events ) };
            track.Sections.push_back( std::move( section ) );
            sequence.Tracks.push_back( std::move( track ) );
        }

        // Generation 3 stated each key's mode for the segment ARRIVING at it; the sequence follows UE's rule
        // (the segment LEAVING it) — the same shift ANIM v5 -> v6 applies (ClipInterpShift.hpp).
        Migration::ShiftInterpToLeavingKey( sequence );

        if ( const Common::BoolResultStr valid = Validate( sequence ); !valid.IsSuccess() )
        {
            return Common::MakeFormattedError<Sequence>( "LiftClip: clip '{}': {}", clip.AnimationName,
                                                         valid.GetError() );
        }
        return Common::MakeSuccess( std::move( sequence ) );
    }
} // namespace Desert::Animation::Timeline
