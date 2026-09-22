#include "PoseEditTransaction.hpp"

#include <Common/Core/Logger.hpp>

#include <Engine/Animation/Animator.hpp>

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
                                      size_t trackCountAfter )
         : m_Animator( animator ), m_Clip( clip ), m_Bones( std::move( bones ) ), m_Tracks( std::move( tracks ) ),
           m_PoseSizeBefore( poseSizeBefore ), m_PoseSizeAfter( poseSizeAfter ),
           m_TrackCountBefore( trackCountBefore ), m_TrackCountAfter( trackCountAfter )
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
        return m_Tracks.empty() ? "Pose bone" : "Pose + key";
    }

    bool ClipPoseCommand::Apply( bool undo )
    {
        if ( m_Animator == nullptr || m_Clip == nullptr )
        {
            return false;
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
        m_PoseBefore   = animator->GetAuthoringPose();
        m_TracksBefore = clip->Tracks;
        return Common::MakeSuccess( true );
    }

    void PoseEditTransaction::Cancel()
    {
        m_Open     = false;
        m_Animator = nullptr;
        m_Clip     = nullptr;
        m_TracksBefore.clear();
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

        const size_t poseSizeBefore   = m_PoseBefore.Size();
        const size_t poseSizeAfter    = poseAfter.Size();
        const size_t trackCountBefore = m_TracksBefore.size();
        const size_t trackCountAfter  = tracksAfter.size();

        Animation::Animator*      animator = m_Animator;
        Animation::AnimationClip* clip     = m_Clip;
        Cancel();

        if ( bones.empty() && tracks.empty() && poseSizeBefore == poseSizeAfter &&
             trackCountBefore == trackCountAfter )
        {
            // AN INTERACTION THAT CHANGED NOTHING IS NOT AN UNDO STEP. Pushing one anyway would make
            // Ctrl+Z spend a press doing nothing, which reads as undo being broken — and it is exactly
            // what a click on the gizmo that misses would produce, every time.
            return Common::MakeSuccess( 0U );
        }

        CommandHistory::Get().PushCommand( std::make_unique<ClipPoseCommand>(
             animator, clip, std::move( bones ), std::move( tracks ), poseSizeBefore, poseSizeAfter,
             trackCountBefore, trackCountAfter ) );
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
} // namespace Desert::Editor
