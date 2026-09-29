#include "PoseEditTransaction.hpp"

#include <Common/Core/Logger.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Rig/ControlManipulator.hpp>
#include <Engine/Animation/TrackEditing.hpp>

#include <algorithm>
#include <utility>

namespace Desert::Editor
{
    bool SameStoredValue( const Animation::BoneTransform& a, const Animation::BoneTransform& b )
    {
        const auto& [aT, aR, aS] = a;
        const auto& [bT, bR, bS] = b;
        return aT == bT && aR == bR && aS == bS;
    }

    bool SameStoredValue( const Animation::PositionKeyFrame& a, const Animation::PositionKeyFrame& b )
    {
        const auto& [aTick, aValue, aInterp, aMode, aArrive, aLeave, aArriveW, aLeaveW] = a;
        const auto& [bTick, bValue, bInterp, bMode, bArrive, bLeave, bArriveW, bLeaveW] = b;
        return aTick == bTick && aValue == bValue && aInterp == bInterp && aMode == bMode && aArrive == bArrive &&
               aLeave == bLeave && aArriveW == bArriveW && aLeaveW == bLeaveW;
    }

    bool SameStoredValue( const Animation::RotationKeyFrame& a, const Animation::RotationKeyFrame& b )
    {
        const auto& [aTick, aValue, aInterp] = a;
        const auto& [bTick, bValue, bInterp] = b;
        return aTick == bTick && aValue == bValue && aInterp == bInterp;
    }

    bool SameStoredValue( const Animation::ScaleKeyFrame& a, const Animation::ScaleKeyFrame& b )
    {
        const auto& [aTick, aValue, aInterp, aMode, aArrive, aLeave, aArriveW, aLeaveW] = a;
        const auto& [bTick, bValue, bInterp, bMode, bArrive, bLeave, bArriveW, bLeaveW] = b;
        return aTick == bTick && aValue == bValue && aInterp == bInterp && aMode == bMode && aArrive == bArrive &&
               aLeave == bLeave && aArriveW == bArriveW && aLeaveW == bLeaveW;
    }

    namespace
    {
        template <typename Key>
        [[nodiscard]] bool SameChannel( const std::vector<Key>& a, const std::vector<Key>& b )
        {
            if ( a.size() != b.size() )
            {
                return false;
            }
            for ( size_t i = 0; i < a.size(); ++i )
            {
                if ( !SameStoredValue( a[i], b[i] ) )
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    bool SameStoredValue( const Animation::BoneTrack& a, const Animation::BoneTrack& b )
    {
        const auto& [aName, aPos, aRot, aScale] = a;
        const auto& [bName, bPos, bRot, bScale] = b;
        return aName == bName && SameChannel( aPos, bPos ) && SameChannel( aRot, bRot ) &&
               SameChannel( aScale, bScale );
    }

    bool SameStoredValue( const Animation::ScalarKey& a, const Animation::ScalarKey& b )
    {
        const auto& [aTick, aValue, aArrive, aLeave, aArriveW, aLeaveW, aInterp, aMode] = a;
        const auto& [bTick, bValue, bArrive, bLeave, bArriveW, bLeaveW, bInterp, bMode] = b;
        return aTick == bTick && aValue == bValue && aArrive == bArrive && aLeave == bLeave &&
               aArriveW == bArriveW && aLeaveW == bLeaveW && aInterp == bInterp && aMode == bMode;
    }

    bool SameStoredValue( const Animation::ClipSection& a, const Animation::ClipSection& b )
    {
        const auto& [aName, aStart, aEnd, aBlend, aTracks, aWeight] = a;
        const auto& [bName, bStart, bEnd, bBlend, bTracks, bWeight] = b;
        // `Tracks` is compared as the LIST IT IS and not as a set: the empty spelling and the
        // every-name spelling mean the same thing to a sampler (ClipSection.hpp), and an undo that
        // "restored" one as the other would silently rewrite what the file says. Collapsing the two is
        // `SetSectionSpeaksFor`'s job, at the moment of the edit, where the animator can see it.
        return aName == bName && aStart == bStart && aEnd == bEnd && aBlend == bBlend && aTracks == bTracks &&
               SameChannel( aWeight, bWeight );
    }

    bool SameStoredValue( const Animation::LocalPose& a, const Animation::LocalPose& b )
    {
        if ( a.Size() != b.Size() )
        {
            return false;
        }
        for ( size_t i = 0; i < a.Size(); ++i )
        {
            if ( !SameStoredValue( a[i], b[i] ) )
            {
                return false;
            }
        }
        return true;
    }

    // ── ClipPoseCommand ──────────────────────────────────────────────────────────────────────────────

    ClipPoseCommand::ClipPoseCommand( Animation::Animator* animator, Animation::AnimationClip* clip,
                                      std::vector<BoneDelta> bones, std::vector<TrackDelta> tracks,
                                      size_t poseSizeBefore, size_t poseSizeAfter, size_t trackCountBefore,
                                      size_t trackCountAfter, SectionEdit sections )
         : m_Animator( animator ), m_Clip( clip ), m_Bones( std::move( bones ) ), m_Tracks( std::move( tracks ) ),
           m_PoseSizeBefore( poseSizeBefore ), m_PoseSizeAfter( poseSizeAfter ),
           m_TrackCountBefore( trackCountBefore ), m_TrackCountAfter( trackCountAfter ),
           m_Sections( std::move( sections ) )
    {
    }

    bool ClipPoseCommand::Undo()
    {
        return Apply( true );
    }

    bool ClipPoseCommand::Redo()
    {
        return Apply( false );
    }

    std::string ClipPoseCommand::GetLabel() const
    {
        // Two labels because the History panel is read to find WHERE to stop, and "Pose" and "Pose + key"
        // are the two things the animator did. It is not a third bit: both are derived from what the
        // entry is carrying, so a label cannot disagree with the entry.
        if ( m_Bones.empty() && m_Tracks.empty() && m_Sections.Changed )
        {
            // A SECTION EDIT ON ITS OWN, which is most of them: pressing "Additive" moves no bone and
            // writes no key. Naming it "Pose bone" would send somebody reading the History panel for a
            // place to stop past the very edit they were looking for.
            return "Edit section";
        }
        return m_Tracks.empty() ? "Pose bone" : "Pose + key";
    }

    bool ClipPoseCommand::Apply( bool undo )
    {
        if ( m_Animator == nullptr || m_Clip == nullptr )
        {
            return false;
        }

        // THE SECTIONS GO BACK WHOLE, and before the tracks: `ClipSection::Tracks` names tracks, so a
        // section list restored against the WRONG track list would be readable for one instant -- and the
        // order matters only because the next lines resize `m_Clip->Tracks`, which a section list holding
        // a name is not indexed by. Written first so the clip is never observed half-restored by a
        // sampler on another thread reading through `SectionFor`.
        if ( m_Sections.Changed )
        {
            m_Clip->Sections = undo ? m_Sections.Before : m_Sections.After;
        }

        // THE TRACK LIST IS RESIZED FIRST AND THE DELTAS WRITTEN SECOND, so a track the interaction
        // created is erased by the resize and a track it removed is re-created by it before its contents
        // arrive. Doing it per-delta instead would need every delta to know whether the vector had already
        // grown, which is the same fact stored twice.
        const size_t wanted = undo ? m_TrackCountBefore : m_TrackCountAfter;
        m_Clip->Tracks.resize( wanted );
        for ( const TrackDelta& delta : m_Tracks )
        {
            const bool has = undo ? delta.HasBefore : delta.HasAfter;
            if ( !has || delta.Index >= m_Clip->Tracks.size() )
            {
                continue;
            }
            m_Clip->Tracks[delta.Index] = undo ? delta.Before : delta.After;
        }

        // THE POSE IS READ BACK, PATCHED, AND INSTALLED WHOLE. Patching in place through a mutable
        // reference is not available (`GetAuthoringPose` is const, deliberately), and installing only the
        // changed bones one at a time would go through `SetBoneLocalPose`'s matrix round trip — which is
        // not the identity, so the "before" the animator gets back would not be the "before" that was
        // captured. `Animator::SetAuthoringPose` says the same thing at its declaration.
        const size_t         poseWanted = undo ? m_PoseSizeBefore : m_PoseSizeAfter;
        Animation::LocalPose pose       = m_Animator->GetAuthoringPose();
        if ( pose.Size() != poseWanted )
        {
            // The rig under this entry is not the rig it was recorded against (a skeleton reload, a
            // different mesh). Reporting failure is what `CommandHistory::Undo` wants: it discards the
            // entry and keeps walking down, rather than writing bones into somebody else's pose.
            LOG_ERROR( "[PoseUndo] discarding an entry recorded against {} bone(s); the rig now has {}",
                       poseWanted, pose.Size() );
            return false;
        }
        for ( const BoneDelta& delta : m_Bones )
        {
            if ( delta.Bone >= pose.Size() )
            {
                continue;
            }
            pose[delta.Bone] = undo ? delta.Before : delta.After;
        }
        if ( const auto installed = m_Animator->SetAuthoringPose( pose ); !installed.IsSuccess() )
        {
            LOG_ERROR( "[PoseUndo] {}", installed.GetError() );
            return false;
        }
        // Render what was just restored. Without this the skinning matrices keep the pose the undo just
        // took away, and the viewport disagrees with both the buffer and the clip.
        m_Animator->ApplyLocalPose();
        return true;
    }

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

    // ── PoseEditTransaction ──────────────────────────────────────────────────────────────────────────

    Common::BoolResultStr PoseEditTransaction::Begin( Animation::Animator*      animator,
                                                      Animation::AnimationClip* clip )
    {
        if ( m_Open )
        {
            return Common::MakeFormattedError<bool>(
                 "a pose-edit transaction is already open ({} track(s) captured); they do not nest, "
                 "because there is no answer to which end commits the undo entry",
                 m_TracksBefore.size() );
        }
        if ( animator == nullptr || clip == nullptr )
        {
            return Common::MakeFormattedError<bool>(
                 "a pose-edit transaction needs both an animator ({}) and a clip ({}): an entry that can "
                 "put back only one of the two restores a pose the clip contradicts",
                 animator != nullptr ? "present" : "null", clip != nullptr ? "present" : "null" );
        }

        m_Animator = animator;
        m_Clip     = clip;
        m_Open     = true;
        m_Driver   = Driver::Explicit;
        // An explicit transaction is opened BEFORE the edit it brackets, so the live buffer is the true
        // before. The edge-driven one cannot say that, which is what the baseline is for.
        m_PoseBefore     = animator->GetAuthoringPose();
        m_TracksBefore   = clip->Tracks;
        m_SectionsBefore = clip->Sections;
        return Common::MakeSuccess( true );
    }

    void PoseEditTransaction::Cancel()
    {
        m_Open     = false;
        m_Animator = nullptr;
        m_Clip     = nullptr;
        m_TracksBefore.clear();
        m_SectionsBefore.clear();
        m_PoseBefore = Animation::LocalPose{};
    }

    Common::ResultStr<uint32_t> PoseEditTransaction::End()
    {
        if ( !m_Open )
        {
            return Common::MakeFormattedError<uint32_t>(
                 "no pose-edit transaction is open; an undo entry closed by an end that never began would "
                 "hold whatever the last interaction left behind" );
        }
        if ( m_Animator == nullptr || m_Clip == nullptr )
        {
            Cancel();
            return Common::MakeFormattedError<uint32_t>( "the transaction's animator or clip went away "
                                                         "while it was open; nothing was recorded" );
        }

        const Animation::LocalPose&              poseAfter   = m_Animator->GetAuthoringPose();
        const std::vector<Animation::BoneTrack>& tracksAfter = m_Clip->Tracks;

        std::vector<ClipPoseCommand::BoneDelta> bones;
        const size_t                            common = std::min( m_PoseBefore.Size(), poseAfter.Size() );
        for ( size_t i = 0; i < common; ++i )
        {
            if ( !SameStoredValue( m_PoseBefore[i], poseAfter[i] ) )
            {
                bones.push_back(
                     ClipPoseCommand::BoneDelta{ static_cast<uint32_t>( i ), m_PoseBefore[i], poseAfter[i] } );
            }
        }

        std::vector<ClipPoseCommand::TrackDelta> tracks;
        const size_t                             maxTracks = std::max( m_TracksBefore.size(), tracksAfter.size() );
        for ( size_t i = 0; i < maxTracks; ++i )
        {
            const bool hasBefore = i < m_TracksBefore.size();
            const bool hasAfter  = i < tracksAfter.size();
            if ( hasBefore && hasAfter && SameStoredValue( m_TracksBefore[i], tracksAfter[i] ) )
            {
                continue;
            }
            ClipPoseCommand::TrackDelta delta;
            delta.Index     = i;
            delta.HasBefore = hasBefore;
            delta.HasAfter  = hasAfter;
            if ( hasBefore )
            {
                delta.Before = m_TracksBefore[i];
            }
            if ( hasAfter )
            {
                delta.After = tracksAfter[i];
            }
            tracks.push_back( std::move( delta ) );
        }

        ClipPoseCommand::SectionEdit               sections;
        const std::vector<Animation::ClipSection>& sectionsAfter = m_Clip->Sections;
        if ( sectionsAfter.size() != m_SectionsBefore.size() )
        {
            sections.Changed = true;
        }
        else
        {
            for ( size_t i = 0; i < sectionsAfter.size(); ++i )
            {
                if ( !SameStoredValue( m_SectionsBefore[i], sectionsAfter[i] ) )
                {
                    sections.Changed = true;
                    break;
                }
            }
        }
        if ( sections.Changed )
        {
            sections.Before = m_SectionsBefore;
            sections.After  = sectionsAfter;
        }

        const size_t poseSizeBefore   = m_PoseBefore.Size();
        const size_t poseSizeAfter    = poseAfter.Size();
        const size_t trackCountBefore = m_TracksBefore.size();
        const size_t trackCountAfter  = tracksAfter.size();

        Animation::Animator*      animator = m_Animator;
        Animation::AnimationClip* clip     = m_Clip;
        Cancel();

        if ( bones.empty() && tracks.empty() && !sections.Changed && poseSizeBefore == poseSizeAfter &&
             trackCountBefore == trackCountAfter )
        {
            // AN INTERACTION THAT CHANGED NOTHING IS NOT AN UNDO STEP. Pushing one anyway would make
            // Ctrl+Z spend a press doing nothing, which reads as undo being broken — and it is exactly
            // what a click on the gizmo that misses would produce, every time.
            return Common::MakeSuccess( 0U );
        }

        CommandHistory::Get().PushCommand( std::make_unique<ClipPoseCommand>(
             animator, clip, std::move( bones ), std::move( tracks ), poseSizeBefore, poseSizeAfter,
             trackCountBefore, trackCountAfter, std::move( sections ) ) );
        return Common::MakeSuccess( 1U );
    }

    Common::ResultStr<uint32_t> PoseEditTransaction::Observe( Animation::Animator*      animator,
                                                              Animation::AnimationClip* clip, bool held )
    {
        const bool rose = held && !m_Held;
        const bool fell = !held && m_Held;
        m_Held          = held;

        uint32_t pushed = 0;
        if ( fell && m_Open && m_Driver == Driver::Edge )
        {
            const auto ended = End();
            if ( !ended.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( ended.GetError() );
            }
            pushed += ended.GetValue();
        }

        if ( rose && !m_Open && animator != nullptr && clip != nullptr )
        {
            if ( const auto began = Begin( animator, clip ); !began.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( began.GetError() );
            }
            m_Driver = Driver::Edge; // `Begin` marks it Explicit; this is the one caller that owns the edge
            // THE RISING EDGE TAKES LAST FRAME'S POSE, NOT THIS FRAME'S. See the baseline note in the
            // header: the manipulator writes the pose and raises the bit in the same function, and the
            // panel that reads the bit runs after it, so "the buffer right now" has already moved.
            if ( m_BaselineValid && m_Baseline.Size() == animator->GetAuthoringPose().Size() )
            {
                m_PoseBefore = m_Baseline;
            }
        }

        if ( !m_Open )
        {
            if ( animator != nullptr )
            {
                m_Baseline      = animator->GetAuthoringPose();
                m_BaselineValid = true;
            }
            else
            {
                m_BaselineValid = false;
            }
        }
        return Common::MakeSuccess( pushed );
    }

    // ── ScopedPoseEdit ───────────────────────────────────────────────────────────────────────────────

    ScopedPoseEdit::ScopedPoseEdit( PoseEditTransaction& transaction, Animation::Animator* animator,
                                    Animation::AnimationClip* clip )
         : m_Transaction( transaction )
    {
        const auto began = transaction.Begin( animator, clip );
        m_Opened         = began.IsSuccess();
        if ( !m_Opened )
        {
            // Not silent: the edit inside the scope is about to happen anyway, and an animator who cannot
            // undo it deserves to know which of the two it was.
            LOG_ERROR( "[PoseUndo] this edit will not be undoable: {}", began.GetError() );
        }
    }

    ScopedPoseEdit::~ScopedPoseEdit()
    {
        if ( !m_Opened )
        {
            return;
        }
        if ( const auto ended = m_Transaction.End(); !ended.IsSuccess() )
        {
            LOG_ERROR( "[PoseUndo] {}", ended.GetError() );
        }
    }
    // ── Sequencer control keying ─────────────────────────────────────────────────────────────────────

    Common::ResultStr<uint32_t> KeyControlsRecorded( PoseEditTransaction& transaction,
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
        if ( auto begun = transaction.Begin( animator, target.Clip ); !begun.IsSuccess() )
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

    Common::ResultStr<uint32_t> ControlAutoKey::Step( PoseEditTransaction&               transaction,
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
            if ( auto begun = transaction.Begin( animator, target.Clip ); !begun.IsSuccess() )
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

    namespace
    {
        template <typename Keys>
        [[nodiscard]] bool HasKeyAt( const Keys& keys, Animation::FrameNumber tick )
        {
            return std::any_of( keys.begin(), keys.end(), [tick]( const auto& key ) { return key.Tick == tick; } );
        }

        template <typename Keys>
        uint32_t RetimeKeys( Keys& keys, Animation::FrameNumber from, Animation::FrameNumber to )
        {
            uint32_t moved = 0;
            for ( auto& key : keys )
            {
                if ( key.Tick == from )
                {
                    key.Tick = to;
                    ++moved;
                }
            }
            std::sort( keys.begin(), keys.end() );
            return moved;
        }

        template <typename Keys>
        uint32_t EraseKeys( Keys& keys, Animation::FrameNumber at )
        {
            const auto before = keys.size();
            std::erase_if( keys, [at]( const auto& key ) { return key.Tick == at; } );
            return static_cast<uint32_t>( before - keys.size() );
        }
    } // namespace

    Common::ResultStr<uint32_t> MoveKeysAtTick( Animation::BoneTrack& track, Animation::FrameNumber from,
                                                Animation::FrameNumber to, Animation::FrameRate tickRate )
    {
        if ( from == to )
        {
            return Common::MakeSuccess( 0U );
        }
        const bool collides = ( HasKeyAt( track.PositionKeys, from ) && HasKeyAt( track.PositionKeys, to ) ) ||
                              ( HasKeyAt( track.RotationKeys, from ) && HasKeyAt( track.RotationKeys, to ) ) ||
                              ( HasKeyAt( track.ScaleKeys, from ) && HasKeyAt( track.ScaleKeys, to ) );
        if ( collides )
        {
            return Common::MakeFormattedError<uint32_t>(
                 "'{}' already has a key at tick {}; moving the key from tick {} onto it would lose one of them",
                 track.BoneName, to.Value, from.Value );
        }
        const uint32_t moved = RetimeKeys( track.PositionKeys, from, to ) +
                               RetimeKeys( track.RotationKeys, from, to ) +
                               RetimeKeys( track.ScaleKeys, from, to );
        if ( moved > 0 )
        {
            Animation::RefreshTangents( track, tickRate );
        }
        return Common::MakeSuccess( moved );
    }

    uint32_t DeleteKeysAtTick( Animation::BoneTrack& track, Animation::FrameNumber at,
                               Animation::FrameRate tickRate )
    {
        const uint32_t removed = EraseKeys( track.PositionKeys, at ) + EraseKeys( track.RotationKeys, at ) +
                                 EraseKeys( track.ScaleKeys, at );
        if ( removed > 0 )
        {
            Animation::RefreshTangents( track, tickRate );
        }
        return removed;
    }
} // namespace Desert::Editor
