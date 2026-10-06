#include "PoseEditTransaction.hpp"

#include <Common/Core/Logger.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Rig/ControlManipulator.hpp>
#include <Engine/Animation/TrackEditing.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace Desert::Editor
{

    // ── ControlPoseCommand ───────────────────────────────────────────────────────────────────────────

    ControlPoseCommand::ControlPoseCommand( Animation::ControlHierarchy* hierarchy, uint32_t control,
                                            Animation::BoneTransform before, Animation::BoneTransform after )
         : m_Hierarchy( hierarchy ), m_Control( control ), m_Before( std::move( before ) ),
           m_After( std::move( after ) )
    {
    }

    bool ControlPoseCommand::Undo()
    {
        return Apply( m_Before );
    }

    bool ControlPoseCommand::Redo()
    {
        return Apply( m_After );
    }

    bool ControlPoseCommand::Apply( const Animation::BoneTransform& pose )
    {
        if ( m_Hierarchy == nullptr || m_Control >= m_Hierarchy->Size() )
        {
            // Reported rather than asserted: CommandHistory::Undo discards an entry that answers false
            // and keeps walking down, which is the right answer for an entry whose rig was replaced.
            LOG_ERROR( "[PoseUndo] the control rig this entry was recorded against is gone; dropped" );
            return false;
        }
        if ( const auto written = m_Hierarchy->SetPose( m_Control, pose ); !written.IsSuccess() )
        {
            LOG_ERROR( "[PoseUndo] {}", written.GetError() );
            return false;
        }
        return true;
    }

    std::string ControlPoseCommand::GetLabel() const
    {
        return "Control drag";
    }

    namespace
    {
        ControlEdit& LastControlEditSlot()
        {
            static ControlEdit s_Last;
            return s_Last;
        }
    } // namespace

    Common::ResultStr<uint32_t> RecordControlDrag( Animation::ControlHierarchy* hierarchy, uint32_t control,
                                                   const Animation::BoneTransform& before )
    {
        if ( hierarchy == nullptr || control >= hierarchy->Size() )
        {
            return Common::MakeFormattedError<uint32_t>(
                 "a control drag cannot be recorded against control {} of a rig with {} of them", control,
                 hierarchy != nullptr ? hierarchy->Size() : 0U );
        }

        const Animation::BoneTransform& after = hierarchy->Get( control ).Pose;
        if ( SameStoredValue( before, after ) )
        {
            return Common::MakeSuccess( 0U );
        }

        CommandHistory::Get().PushCommand(
             std::make_unique<ControlPoseCommand>( hierarchy, control, before, after ) );
        ControlEdit& last = LastControlEditSlot();
        last = ControlEdit{ last.Generation + 1, hierarchy, control, CommandHistory::Get().Revision() };
        return Common::MakeSuccess( 1U );
    }

    ControlEdit LastControlEdit()
    {
        return LastControlEditSlot();
    }

    Common::ResultStr<uint32_t> RotateControlRecorded( Animation::ControlHierarchy* hierarchy, uint32_t control,
                                                       int axis, float degrees )
    {
        if ( hierarchy == nullptr || control >= hierarchy->Size() )
        {
            return Common::MakeFormattedError<uint32_t>( "cannot rotate control {} of a rig with {} of them",
                                                         control, hierarchy != nullptr ? hierarchy->Size() : 0U );
        }
        const Animation::BoneTransform before = hierarchy->Get( control ).Pose;
        if ( auto turned = Animation::RotateControlLocal( *hierarchy, control, axis, degrees ); !turned )
        {
            return Common::MakeError<uint32_t>( turned.GetError() );
        }
        return RecordControlDrag( hierarchy, control, before );
    }

    Common::ResultStr<uint32_t> ControlGizmoGesture::Step( Animation::ControlHierarchy* hierarchy,
                                                           uint32_t control, bool held )
    {
        if ( held && !m_Active )
        {
            if ( hierarchy == nullptr || control >= hierarchy->Size() )
            {
                return Common::MakeFormattedError<uint32_t>(
                     "a gizmo gesture cannot start on control {} of a rig with {} of them", control,
                     hierarchy != nullptr ? hierarchy->Size() : 0U );
            }
            m_Active    = true;
            m_Hierarchy = hierarchy;
            m_Control   = control;
            m_Before    = hierarchy->Get( control ).Pose;
            return Common::MakeSuccess( 0U );
        }
        if ( held || !m_Active )
        {
            return Common::MakeSuccess( 0U );
        }

        m_Active = false;
        if ( hierarchy != m_Hierarchy || control != m_Control )
        {
            return Common::MakeFormattedError<uint32_t>(
                 "the gizmo gesture on control {} was abandoned: the rig or the selection changed before the "
                 "release (now control {})",
                 m_Control, control );
        }
        return RecordControlDrag( hierarchy, control, m_Before );
    }

    // ── BoneGizmoGesture ─────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<uint32_t> BoneGizmoGesture::Step( SequenceEditTransaction& transaction, const bool held,
                                                        const std::function<Animation::Animator*()>& startPose,
                                                        Animation::AnimationClip*                    clip )
    {
        if ( held && !m_Active )
        {
            Animation::Animator* animator = startPose();
            if ( animator == nullptr || clip == nullptr )
            {
                return Common::MakeFormattedError<uint32_t>( "a bone gizmo gesture cannot start without {}",
                                                             animator == nullptr ? "an animator" : "a clip" );
            }
            if ( auto begun = transaction.Begin( OwnerOf( clip ), animator ); !begun.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( begun.GetError() );
            }
            m_Active = true;
            return Common::MakeSuccess( 0U );
        }
        if ( held || !m_Active )
        {
            return Common::MakeSuccess( 0U );
        }
        m_Active = false;
        return transaction.End();
    }

    // ── Sequencer control keying ─────────────────────────────────────────────────────────────────────

    Common::ResultStr<uint32_t> KeyControlsRecorded( SequenceEditTransaction& transaction,
                                                     Animation::Animator* animator, Animation::ControlKeyer& keyer,
                                                     const Animation::ControlKeyTarget& target,
                                                     std::span<const uint32_t>          controls )
    {
        if ( target.Hierarchy == nullptr || target.Clip == nullptr )
        {
            return Common::MakeError<uint32_t>( "keying controls needs a control rig and an open clip" );
        }
        if ( controls.empty() )
        {
            return Common::MakeError<uint32_t>( "no control is selected; select one in the tree or the viewport" );
        }
        if ( auto begun = transaction.Begin( OwnerOf( target.Clip ), animator ); !begun.IsSuccess() )
        {
            return Common::MakeError<uint32_t>( begun.GetError() );
        }
        uint32_t keyed = 0;
        for ( const uint32_t control : controls )
        {
            if ( control >= target.Hierarchy->Size() )
            {
                transaction.Cancel();
                return Common::MakeFormattedError<uint32_t>( "control {} is not in a rig of {} controls", control,
                                                             target.Hierarchy->Size() );
            }
            const Animation::BoneTransform pose = target.Hierarchy->Get( control ).Pose;
            auto written = keyer.Write( target, control, pose, Animation::ControlWriteSource::Authored );
            if ( !written.IsSuccess() )
            {
                transaction.Cancel();
                return Common::MakeError<uint32_t>( written.GetError() );
            }
            keyed += written.GetValue();
        }
        if ( auto ended = transaction.End(); !ended.IsSuccess() )
        {
            return Common::MakeError<uint32_t>( ended.GetError() );
        }
        return Common::MakeSuccess( keyed );
    }

    Common::ResultStr<uint32_t> ControlAutoKey::Step( SequenceEditTransaction&           transaction,
                                                      Animation::Animator*               animator,
                                                      Animation::ControlKeyer&           keyer,
                                                      const Animation::ControlKeyTarget& target, uint32_t control,
                                                      bool held )
    {
        const ControlEdit edit  = LastControlEdit();
        const bool        fresh = m_SeenEdit.has_value() && edit.Generation != *m_SeenEdit;
        m_SeenEdit              = edit.Generation;
        if ( target.Hierarchy == nullptr || target.Clip == nullptr || control >= target.Hierarchy->Size() )
        {
            // Nothing to observe: forget the gesture rather than carry its edge into another rig.
            if ( keyer.Interacting() )
            {
                keyer.CancelInteraction();
            }
            m_Held    = false;
            m_Control = Animation::ControlHierarchy::INVALID;
            m_JoinRevision.reset();
            return Common::MakeSuccess( 0U );
        }

        // An edit recorded against THIS control holds the gesture for the frame it is first seen, so the
        // command's key comes out of the same edge as the mouse's.
        const bool edited = fresh && edit.Hierarchy == target.Hierarchy && edit.Control == control;
        if ( edited )
        {
            m_JoinRevision = edit.Revision;
        }
        const bool gesture = held || edited;

        const Animation::BoneTransform pose    = target.Hierarchy->Get( control ).Pose;
        const bool                     changed = control == m_Control && !SameStoredValue( pose, m_Last );
        // Only a gesture moves a control as far as keying goes: the playhead writing the clip back onto
        // the controls changes the pose too, and keying THAT would key every scrub. An edit counts as a
        // move by itself — the recorder pushed an entry only because the pose differed, and a Details drag
        // changed it on frames that were no gesture yet.
        const bool moved = gesture && ( changed || edited );
        m_Control        = control;
        m_Last           = pose;

        // The release frame is the only one on which `Observe` writes the clip, so it is the only one that
        // needs the transaction. Opened here, not on the rising edge: during the gesture the CONTROL moves
        // and the clip does not, and the control's own undo entry is `ControlGizmoGesture`'s.
        const bool release = m_Held && !gesture;
        m_Held             = gesture;
        const bool joinable =
             release && m_JoinRevision.has_value() && *m_JoinRevision == CommandHistory::Get().Revision();
        if ( release )
        {
            m_JoinRevision.reset();
        }
        bool opened = false;
        if ( release && !transaction.Open() )
        {
            if ( auto begun = transaction.Begin( OwnerOf( target.Clip ), animator ); !begun.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( begun.GetError() );
            }
            opened = true;
        }

        auto observed = keyer.Observe(
             target, Animation::KeySubject{ Animation::KeySubjectKind::Control, control }, gesture, moved );
        if ( !observed.IsSuccess() )
        {
            if ( opened )
            {
                transaction.Cancel();
            }
            return Common::MakeError<uint32_t>( observed.GetError() );
        }
        if ( !opened )
        {
            return Common::MakeSuccess( 0U );
        }
        auto ended = transaction.End();
        if ( ended.IsSuccess() && ended.GetValue() == 1U && joinable )
        {
            // The control's pose entry and this key are one user action: one Ctrl+Z takes both back.
            CommandHistory::Get().JoinLastTwo();
        }
        return ended;
    }

    Common::ResultStr<uint32_t> KeyBonePose( SequenceEditTransaction& transaction, Animation::Animator* animator,
                                             Animation::AnimationClip* clip, uint32_t bone,
                                             Animation::FrameNumber tick )
    {
        if ( animator == nullptr || clip == nullptr )
        {
            return Common::MakeError<uint32_t>( "Key Bone: no animator or no clip to key into" );
        }
        const auto& bones = animator->GetSkeleton().GetBones();
        if ( bone >= bones.size() || bone >= animator->GetAuthoringPose().Size() )
        {
            return Common::MakeError<uint32_t>(
                 std::format( "Key Bone: bone {} is outside the skeleton ({} bones)", bone, bones.size() ) );
        }
        if ( transaction.Open() )
        {
            return Common::MakeError<uint32_t>( "Key Bone: a pose edit is already open; release it first" );
        }
        if ( const auto began = transaction.Begin( OwnerOf( clip ), animator ); !began.IsSuccess() )
        {
            return Common::MakeError<uint32_t>( began.GetError() );
        }
        // Binding, track and section are created as needed; Cancel pushes nothing, so a refused key must
        // leave nothing behind — SetBoneKey refuses a non-finite pose BEFORE it creates anything.
        const std::string& name = bones[bone].Name;
        if ( const auto keyed =
                  Animation::SetBoneKey( clip->Sequence, name, tick, animator->GetAuthoringPose()[bone] );
             !keyed.IsSuccess() )
        {
            transaction.Cancel();
            return Common::MakeError<uint32_t>( std::format( "Key Bone: '{}': {}", name, keyed.GetError() ) );
        }
        return transaction.End();
    }

} // namespace Desert::Editor
