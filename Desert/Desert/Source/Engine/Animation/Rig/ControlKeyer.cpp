#include "ControlKeyer.hpp"

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/TrackEditing.hpp>

#include <algorithm>

namespace Desert::Animation
{
    namespace
    {
        /**
         * @brief Everything that has to be true before a control can be keyed at all.
         *
         * ONE FUNCTION FOR THREE CALLERS, and that is the point rather than tidiness: `Write`,
         * `EndInteraction` and `ApplyClipToControls` all have to make the same refusals, and three copies
         * of a refusal list is how one of them ends up missing the case the other two catch. The tick is
         * checked here too, even for a write that will defer, so that a bad target is reported at the
         * moment the caller can still do something about it.
         */
        [[nodiscard]] Common::BoolResultStr Check( const ControlKeyTarget& target, uint32_t control )
        {
            if ( target.Hierarchy == nullptr || target.Skeleton == nullptr || target.Clip == nullptr )
            {
                return Common::MakeFormattedError<bool>(
                     "a control key needs a hierarchy, a skeleton and a clip; this target has {}/{}/{}",
                     target.Hierarchy != nullptr, target.Skeleton != nullptr, target.Clip != nullptr );
            }
            if ( control >= target.Hierarchy->Size() )
            {
                return Common::MakeFormattedError<bool>( "no control {} in a rig of {}", control,
                                                         target.Hierarchy->Size() );
            }
            if ( target.Tick.Value < 0 || target.Tick > target.Clip->DurationTicks )
            {
                // A KEY OUTSIDE THE CLIP IS A KEY NOTHING SAMPLES. Extending the clip instead would be the
                // helpful-looking answer and it is the wrong one: the length is what the Sequencer's ruler,
                // its key fields and the AnimGraph's exit-time fraction are all measured against, so
                // growing it as a side effect of a nudge moves four things the animator did not touch.
                // Lengthening a clip is a clip edit; keying is an animation one.
                return Common::MakeFormattedError<bool>(
                     "tick {} is outside clip '{}', which is {} ticks long — a key there would never be "
                     "sampled, and lengthening the clip is a separate edit",
                     target.Tick.Value, target.Clip->AnimationName, target.Clip->DurationTicks.Value );
            }

            const std::string& name = target.Hierarchy->Get( control ).Name;
            if ( const auto bone = target.Skeleton->FindBoneIndex( name ) )
            {
                // See the file note: a track name is the only binding key there is, so this control's keys
                // would be bound onto that bone by `Animator::ResolveTrack` and drive it with a value that
                // means "offset from this control's parent space".
                return Common::MakeFormattedError<bool>(
                     "control '{}' has the name of bone {}, and a track name is the only thing playback "
                     "binds on — this control's keys would drive that bone directly. Rename the control.",
                     name, *bone );
            }
            return Common::MakeSuccess( true );
        }

        /// The clip's track for this control, created empty if the clip has none yet.
        [[nodiscard]] BoneTrack& TrackFor( AnimationClip& clip, const std::string& name )
        {
            for ( BoneTrack& track : clip.Tracks )
            {
                if ( track.BoneName == name )
                {
                    return track;
                }
            }
            // NO `TrackRevision` BUMP, and that is checked rather than assumed: `Animator::ResolveTrack`
            // rebuilds its binding when EITHER the revision or `Tracks.size()` changed (Animator.cpp:433),
            // and an append changes the size. The revision exists for the case the size cannot see — a
            // whole list replaced by one of equal length — and it has exactly one writer, `AnimationAsset`,
            // which says so at its declaration. A second writer here would be a second answer.
            BoneTrack track;
            track.BoneName = name;
            clip.Tracks.push_back( std::move( track ) );
            return clip.Tracks.back();
        }

        /// Write the control's CURRENT pose as one key. The value is read here rather than remembered; see
        /// `EndInteraction`'s note on why that is the whole of "resolve before you commit".
        [[nodiscard]] Common::BoolResultStr KeyNow( const ControlKeyTarget& target, uint32_t control )
        {
            const ControlElement& element = target.Hierarchy->Get( control );
            BoneTrack&            track   = TrackFor( *target.Clip, element.Name );
            if ( !SetTransformKey( track, target.Tick, element.Pose, target.Clip->TickRate ) )
            {
                return Common::MakeFormattedError<bool>(
                     "control '{}': its pose could not be written as a key at tick {}", element.Name,
                     target.Tick.Value );
            }
            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::BoolResultStr ControlKeyer::BeginInteraction()
    {
        if ( m_Interacting )
        {
            return Common::MakeFormattedError<bool>(
                 "an interaction is already open with {} control(s) pending; interactions do not nest, "
                 "because there is no answer to which end commits the key",
                 m_Pending.size() );
        }
        m_Interacting = true;
        m_Pending.clear();
        return Common::MakeSuccess( true );
    }

    void ControlKeyer::CancelInteraction()
    {
        m_Interacting = false;
        m_Pending.clear();
    }

    Common::ResultStr<uint32_t> ControlKeyer::Write( const ControlKeyTarget& target, uint32_t control,
                                                     const BoneTransform& pose, ControlWriteSource source )
    {
        if ( const auto checked = Check( target, control ); !checked.IsSuccess() )
        {
            return Common::MakeError<uint32_t>( checked.GetError() );
        }
        // THE POSE IS STORED BEFORE ANY DECISION ABOUT KEYING, and only after the refusals above: a pose
        // stored under a control that cannot be keyed is animation the animator loses without being told.
        if ( const auto written = target.Hierarchy->SetPose( control, pose ); !written.IsSuccess() )
        {
            return Common::MakeError<uint32_t>( written.GetError() );
        }

        if ( source == ControlWriteSource::Playback )
        {
            // REPORT 01 §823. Not "defer" and not "key with a flag set" — it does not become pending
            // either, because a pending control is keyed by whatever `EndInteraction` runs next, and an
            // animator holding a control while the clip plays is exactly when both are true at once.
            return Common::MakeSuccess( 0U );
        }

        if ( m_Interacting )
        {
            // REPORT 05 §971. The clip is not touched at all yet; what is remembered is WHICH control
            // moved, never the value it moved to.
            if ( std::find( m_Pending.begin(), m_Pending.end(), control ) == m_Pending.end() )
            {
                m_Pending.push_back( control );
            }
            return Common::MakeSuccess( 0U );
        }

        if ( const auto keyed = KeyNow( target, control ); !keyed.IsSuccess() )
        {
            return Common::MakeError<uint32_t>( keyed.GetError() );
        }
        return Common::MakeSuccess( 1U );
    }

    Common::ResultStr<uint32_t> ControlKeyer::EndInteraction( const ControlKeyTarget& target )
    {
        if ( !m_Interacting )
        {
            return Common::MakeFormattedError<uint32_t>(
                 "no interaction is open; a key at the end of one that never began would be a key nobody "
                 "asked for, at whatever tick the playhead happens to be on" );
        }

        // REFUSE BEFORE WRITING ANYTHING, over every pending control. A loop that keyed three controls and
        // then refused the fourth would leave the clip holding a third of an interaction, and the undo the
        // caller pushes covers the whole of one — that is §971's other half.
        for ( const uint32_t control : m_Pending )
        {
            if ( const auto checked = Check( target, control ); !checked.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( checked.GetError() );
            }
        }

        uint32_t keyed = 0;
        for ( const uint32_t control : m_Pending )
        {
            if ( const auto wrote = KeyNow( target, control ); !wrote.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( wrote.GetError() );
            }
            ++keyed;
        }

        m_Interacting = false;
        m_Pending.clear();
        return Common::MakeSuccess( keyed );
    }

    Common::ResultStr<uint32_t> ApplyClipToControls( const ControlKeyTarget& target, ControlKeyer& keyer )
    {
        if ( target.Hierarchy == nullptr || target.Skeleton == nullptr || target.Clip == nullptr )
        {
            return Common::MakeFormattedError<uint32_t>(
                 "applying a clip to controls needs a hierarchy, a skeleton and a clip; this target has "
                 "{}/{}/{}",
                 target.Hierarchy != nullptr, target.Skeleton != nullptr, target.Clip != nullptr );
        }

        uint32_t moved = 0;
        for ( uint32_t control = 0; control < static_cast<uint32_t>( target.Hierarchy->Size() ); ++control )
        {
            const std::string& name  = target.Hierarchy->Get( control ).Name;
            const BoneTrack*   found = nullptr;
            for ( const BoneTrack& track : target.Clip->Tracks )
            {
                if ( track.BoneName == name )
                {
                    found = &track;
                    break;
                }
            }
            if ( found == nullptr || !found->HasKeys() )
            {
                // LEFT WHERE IT IS. An unkeyed control has no animation, and "no animation" is not "at the
                // origin" — sampling an empty track would hand back a zero translation and an identity
                // rotation and park the control there, which is §936's defect in a different costume.
                continue;
            }

            const BoneTransform sampled =
                 found->Sample( FrameTime{ target.Tick, 0.0F }, target.Clip->TickRate );
            const auto written = keyer.Write( target, control, sampled, ControlWriteSource::Playback );
            if ( !written.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( written.GetError() );
            }
            ++moved;
        }
        return Common::MakeSuccess( moved );
    }
} // namespace Desert::Animation
