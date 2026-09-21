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
        [[nodiscard]] Common::BoolResultStr Check( const ControlKeyTarget& target, KeySubject subject )
        {
            if ( target.Hierarchy == nullptr || target.Skeleton == nullptr || target.Clip == nullptr )
            {
                return Common::MakeFormattedError<bool>(
                     "a control key needs a hierarchy, a skeleton and a clip; this target has {}/{}/{}",
                     target.Hierarchy != nullptr, target.Skeleton != nullptr, target.Clip != nullptr );
            }
            if ( subject.Kind == KeySubjectKind::Control && subject.Index >= target.Hierarchy->Size() )
            {
                return Common::MakeFormattedError<bool>( "no control {} in a rig of {}", subject.Index,
                                                         target.Hierarchy->Size() );
            }
            if ( subject.Kind == KeySubjectKind::Bone )
            {
                if ( subject.Index >= static_cast<uint32_t>( target.Skeleton->GetBones().size() ) )
                {
                    return Common::MakeFormattedError<bool>( "no bone {} in a skeleton of {}", subject.Index,
                                                             target.Skeleton->GetBones().size() );
                }
                if ( target.AuthoredPose == nullptr )
                {
                    // NOT "fall back to the bind pose". A key holding the bind pose is a keyframe that
                    // moves the bone the moment it is written, which is the defect §936 names.
                    return Common::MakeFormattedError<bool>(
                         "bone {} ('{}') cannot be keyed: this target carries no authoring pose to read it "
                         "from, and the bind pose is not the animator's work",
                         subject.Index, target.Skeleton->GetBones()[subject.Index].Name );
                }
                if ( subject.Index >= static_cast<uint32_t>( target.AuthoredPose->Size() ) )
                {
                    return Common::MakeFormattedError<bool>(
                         "bone {} is outside the authoring pose, which holds {} bones — the pose and the "
                         "skeleton disagree about the rig",
                         subject.Index, target.AuthoredPose->Size() );
                }
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

            if ( subject.Kind == KeySubjectKind::Bone )
            {
                // A BONE'S NAME IS ITS OWN. The collision refusal below is about a CONTROL borrowing a
                // bone's name; a bone track landing on that bone is what playback binding is for.
                return Common::MakeSuccess( true );
            }

            const std::string& name = target.Hierarchy->Get( subject.Index ).Name;
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

        /// The clip track name and the pose a subject would be keyed with, in one place. Two callers —
        /// the commit and the `Changed` comparison — and two spellings of "where does a bone's value come
        /// from" is exactly how one of them ends up reading the bind pose.
        [[nodiscard]] const std::string& NameOf( const ControlKeyTarget& target, KeySubject subject )
        {
            return subject.Kind == KeySubjectKind::Bone ? target.Skeleton->GetBones()[subject.Index].Name
                                                        : target.Hierarchy->Get( subject.Index ).Name;
        }

        [[nodiscard]] const BoneTransform& PoseOf( const ControlKeyTarget& target, KeySubject subject )
        {
            return subject.Kind == KeySubjectKind::Bone ? ( *target.AuthoredPose )[subject.Index]
                                                        : target.Hierarchy->Get( subject.Index ).Pose;
        }

        [[nodiscard]] BoneTrack* FindTrack( AnimationClip& clip, const std::string& name )
        {
            for ( BoneTrack& track : clip.Tracks )
            {
                if ( track.BoneName == name )
                {
                    return &track;
                }
            }
            return nullptr;
        }
    } // namespace

    Common::ResultStr<uint32_t> ControlKeyer::Commit( const ControlKeyTarget& target, PendingSubject pending )
    {
        const KeySubject     subject = pending.Subject;
        const std::string&   name    = NameOf( target, subject );
        const BoneTransform& pose    = PoseOf( target, subject );

        // THE AUTOMATIC PATH IS THE ONLY ONE `AutoChangeMode` SPEAKS FOR. A key the animator asked for by
        // pressing a button is not an automatic change, and a mode called "auto-key off" that silenced it
        // would be a button that does nothing with no way to find out why.
        const AutoChangeMode change = pending.Automatic ? m_Modes.AutoChange : AutoChangeMode::All;
        // NO `None` BRANCH HERE, AND ITS ABSENCE IS THE RESULT OF A MUTATION. Deleting the guard that used
        // to stand here changed nothing that any test could see, because `Observe` already refuses to open
        // an interaction or call this function while auto-key is off — the branch was unreachable, which
        // makes it a rule with no reader rather than a safety net. One decider, and it is `Observe`.

        BoneTrack* existing = FindTrack( *target.Clip, name );
        if ( existing == nullptr && change == AutoChangeMode::AutoKey )
        {
            // "Key what is already animated." A subject with no track is not part of this take yet, and
            // giving it one is the decision `AutoTrack` and `All` make and this one does not.
            return Common::MakeSuccess( 0U );
        }

        BoneTrack& track = ( existing != nullptr ) ? *existing : TrackFor( *target.Clip, name );
        if ( change == AutoChangeMode::AutoTrack )
        {
            return Common::MakeSuccess( 0U ); // the track now exists; the key is what this mode withholds
        }

        if ( pending.Automatic && m_Modes.KeyGroup == KeyGroupMode::Changed && track.HasKeys() )
        {
            // AUTOMATIC ONLY, for the same reason `AutoChangeMode` is: a Key button that silently writes
            // nothing because the curve happens to agree is a button with no way to find out why. An
            // animator who presses it at a tick the curve already covers means "pin it here".

            // THE CLIP ALREADY SAYS THIS. Compared against what the track SAMPLES at the tick rather than
            // against a key sitting on it: a subject held still between two keys is agreed with by the
            // curve, and keying it there would pin an interpolated value an animator never authored —
            // which is the one edit that makes a curve stop being editable.
            const BoneTransform sampled = track.Sample( FrameTime{ target.Tick, 0.0F }, target.Clip->TickRate );
            if ( sampled.Translation == pose.Translation && sampled.Rotation == pose.Rotation &&
                 sampled.Scale == pose.Scale )
            {
                return Common::MakeSuccess( 0U );
            }
        }

        if ( !SetTransformKey( track, target.Tick, pose, target.Clip->TickRate ) )
        {
            return Common::MakeFormattedError<uint32_t>(
                 "{} '{}': its pose could not be written as a key at tick {}",
                 subject.Kind == KeySubjectKind::Bone ? "bone" : "control", name, target.Tick.Value );
        }
        return Common::MakeSuccess( 1U );
    }

    void ControlKeyer::Remember( PendingSubject pending )
    {
        for ( PendingSubject& existing : m_Pending )
        {
            if ( existing.Subject == pending.Subject )
            {
                // A SUBJECT THE ANIMATOR ALSO KEYED BY HAND STOPS BEING AUTOMATIC, and never the other way
                // round: an explicit key is the stronger claim, and `AutoChangeMode` must not be able to
                // swallow it because a later frame of the same drag touched the same subject.
                existing.Automatic = existing.Automatic && pending.Automatic;
                return;
            }
        }
        m_Pending.push_back( pending );
    }

    Common::BoolResultStr ControlKeyer::BeginInteraction()
    {
        if ( m_Interacting )
        {
            return Common::MakeFormattedError<bool>(
                 "an interaction is already open with {} subject(s) pending; interactions do not nest, "
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
        const KeySubject subject{ KeySubjectKind::Control, control };
        if ( const auto checked = Check( target, subject ); !checked.IsSuccess() )
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
            Remember( PendingSubject{ subject, false } );
            return Common::MakeSuccess( 0U );
        }

        return Commit( target, PendingSubject{ subject, false } );
    }

    Common::ResultStr<uint32_t> ControlKeyer::WriteBone( const ControlKeyTarget& target, uint32_t bone )
    {
        const KeySubject subject{ KeySubjectKind::Bone, bone };
        if ( const auto checked = Check( target, subject ); !checked.IsSuccess() )
        {
            return Common::MakeError<uint32_t>( checked.GetError() );
        }

        // NO POSE IS STORED HERE. The gizmo already wrote it through `Animator::SetBoneLocalPose`; see the
        // file note for why standing a second funnel in front of that one would only duplicate its
        // refusals.
        if ( m_Interacting )
        {
            Remember( PendingSubject{ subject, false } );
            return Common::MakeSuccess( 0U );
        }

        return Commit( target, PendingSubject{ subject, false } );
    }

    Common::ResultStr<uint32_t> ControlKeyer::Observe( const ControlKeyTarget& target, KeySubject subject,
                                                       bool pointerHeld, bool subjectMoved )
    {
        // AUTO-KEY OFF MEANS THE KEYER IS NOT IN THE FRAME AT ALL — no interaction is opened, so releasing
        // the pointer after switching the mode off mid-drag cannot commit a key from a drag nobody was
        // recording. `m_PointerHeld` is still tracked, or the first frame after switching it back on would
        // read as a rising edge in the middle of a drag already under way.
        const bool armed = m_Modes.AutoChange != AutoChangeMode::None;
        const bool rose  = pointerHeld && !m_PointerHeld;
        const bool fell  = !pointerHeld && m_PointerHeld;
        m_PointerHeld    = pointerHeld;

        if ( !armed )
        {
            if ( fell && m_Interacting )
            {
                CancelInteraction();
            }
            return Common::MakeSuccess( 0U );
        }

        if ( rose && !m_Interacting )
        {
            if ( const auto began = BeginInteraction(); !began.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( began.GetError() );
            }
        }

        uint32_t keyed = 0;
        if ( subjectMoved )
        {
            if ( const auto checked = Check( target, subject ); !checked.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( checked.GetError() );
            }
            if ( m_Interacting )
            {
                Remember( PendingSubject{ subject, true } );
            }
            else
            {
                const auto wrote = Commit( target, PendingSubject{ subject, true } );
                if ( !wrote.IsSuccess() )
                {
                    return Common::MakeError<uint32_t>( wrote.GetError() );
                }
                keyed += wrote.GetValue();
            }
        }

        if ( fell && m_Interacting )
        {
            const auto ended = EndInteraction( target );
            if ( !ended.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( ended.GetError() );
            }
            keyed += ended.GetValue();
        }
        return Common::MakeSuccess( keyed );
    }

    Common::ResultStr<uint32_t> ControlKeyer::EndInteraction( const ControlKeyTarget& target )
    {
        if ( !m_Interacting )
        {
            return Common::MakeFormattedError<uint32_t>(
                 "no interaction is open; a key at the end of one that never began would be a key nobody "
                 "asked for, at whatever tick the playhead happens to be on" );
        }

        // `KeyGroupMode::All` IS EXPANDED HERE AND NOWHERE ELSE, because "everything" is only knowable
        // once the interaction is over: which KINDS it touched is what decides whether "everything" means
        // every control or every bone. Expanding at the write would make the answer depend on the order
        // the animator happened to grab things in.
        std::vector<PendingSubject> commits = m_Pending;
        if ( m_Modes.KeyGroup == KeyGroupMode::All && !m_Pending.empty() )
        {
            bool controls = false;
            bool bones    = false;
            bool automatic = false;
            for ( const PendingSubject& pending : m_Pending )
            {
                controls  = controls || pending.Subject.Kind == KeySubjectKind::Control;
                bones     = bones || pending.Subject.Kind == KeySubjectKind::Bone;
                automatic = automatic || pending.Automatic;
            }
            commits.clear();
            if ( controls && target.Hierarchy != nullptr )
            {
                for ( uint32_t i = 0; i < static_cast<uint32_t>( target.Hierarchy->Size() ); ++i )
                {
                    commits.push_back( PendingSubject{ KeySubject{ KeySubjectKind::Control, i }, automatic } );
                }
            }
            if ( bones && target.Skeleton != nullptr )
            {
                for ( uint32_t i = 0; i < static_cast<uint32_t>( target.Skeleton->GetBones().size() ); ++i )
                {
                    commits.push_back( PendingSubject{ KeySubject{ KeySubjectKind::Bone, i }, automatic } );
                }
            }
        }

        // REFUSE BEFORE WRITING ANYTHING, over every subject. A loop that keyed three and then refused the
        // fourth would leave the clip holding a third of an interaction, and the undo the caller pushes
        // covers the whole of one — that is §971's other half.
        for ( const PendingSubject& pending : commits )
        {
            if ( const auto checked = Check( target, pending.Subject ); !checked.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( checked.GetError() );
            }
        }

        uint32_t keyed = 0;
        for ( const PendingSubject& pending : commits )
        {
            const auto wrote = Commit( target, pending );
            if ( !wrote.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( wrote.GetError() );
            }
            keyed += wrote.GetValue();
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

            const BoneTransform sampled = found->Sample( FrameTime{ target.Tick, 0.0F }, target.Clip->TickRate );
            const auto          written = keyer.Write( target, control, sampled, ControlWriteSource::Playback );
            if ( !written.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( written.GetError() );
            }
            ++moved;
        }
        return Common::MakeSuccess( moved );
    }
} // namespace Desert::Animation
