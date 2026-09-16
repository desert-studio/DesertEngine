#include "ControlRigStage.hpp"

#include <Engine/Animation/Skeleton.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>

namespace Desert::Animation
{
    Common::BoolResultStr ControlRigStage::SetDrives( const Skeleton&               skeleton,
                                                      std::vector<ControlBoneDrive> drives )
    {
        if ( drives.empty() )
        {
            return Common::MakeFormattedError<bool>(
                 "a control rig with no drives is not a pose operator: it would join the pipeline as a "
                 "stage that cannot change the pose, and nothing downstream could tell it from a rig that "
                 "is broken. Declare at least one control -> bone drive, or do not attach the rig." );
        }

        const size_t bones    = skeleton.GetBones().size();
        const size_t controls = m_Hierarchy.Size();

        for ( const ControlBoneDrive& drive : drives )
        {
            if ( drive.Control >= controls )
            {
                return Common::MakeFormattedError<bool>(
                     "drive names control {}, and this rig has {}. An index that far out is a drive list "
                     "written against a different rig, not a clamp to apply.",
                     drive.Control, controls );
            }
            if ( drive.Bone >= bones )
            {
                return Common::MakeFormattedError<bool>(
                     "control '{}' is declared to drive bone {}, and this skeleton has {} — the drive list "
                     "was written against a different skeleton.",
                     m_Hierarchy.Get( drive.Control ).Name, drive.Bone, bones );
            }
        }

        // ORDERED BY RESOLVE RANK, PARENTS FIRST. `ApplyBoneOverrides` requires it and refuses otherwise;
        // sorting once here is what keeps that refusal unreachable for a rig whose author simply listed the
        // hand before the shoulder. Stable so two drives that somehow compared equal keep the authored
        // order rather than a std::sort implementation's opinion — the duplicate check below then rejects
        // the only way that can happen.
        std::stable_sort( drives.begin(), drives.end(),
                          [&skeleton]( const ControlBoneDrive& lhs, const ControlBoneDrive& rhs )
                          { return skeleton.GetResolveRank( lhs.Bone ) < skeleton.GetResolveRank( rhs.Bone ); } );

        for ( size_t i = 1; i < drives.size(); ++i )
        {
            if ( drives[i].Bone == drives[i - 1].Bone )
            {
                return Common::MakeFormattedError<bool>(
                     "controls '{}' and '{}' both drive bone {} ('{}'). Which of the two the bone ends up "
                     "at has no answer that is not invented here, and the one it would silently get is "
                     "whichever the sort happened to put last.",
                     m_Hierarchy.Get( drives[i - 1].Control ).Name, m_Hierarchy.Get( drives[i].Control ).Name,
                     drives[i].Bone, skeleton.GetBones()[drives[i].Bone].Name );
            }
        }

        m_Drives = std::move( drives );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ControlRigStage::Evaluate( const Skeleton& skeleton, LocalPose& pose,
                                                     ComponentPose& component )
    {
        m_LastError.clear();

        // ---- the input hop: pose -> rig hierarchy (report 05 §641, `UpdateInput`) -----------------------
        //
        // The stages before this one have just rewritten every bone of `pose`, so the component view's
        // cached matrices are the previous stage's — or the previous FRAME's. Dropping the flags here is
        // what makes the bone spaces the rig reads below this frame's.
        component.Invalidate();
        m_Hierarchy.Evaluate( skeleton, component );

        if ( !m_Hierarchy.GetStructureError().empty() )
        {
            // The rig could not even read the skeleton it is being run over. Refusing here leaves the pose
            // exactly as the previous stage produced it, which is the only honest answer: a rig built
            // against another skeleton has no transform to contribute, and identity is a wrong one.
            m_LastError = m_Hierarchy.GetStructureError();
            ReportErrorOnChange();
            return Common::MakeError<bool>( m_LastError );
        }

        // ---- the solve would be here (report 05 §642) ---------------------------------------------------
        //
        // T5.5. Today a control reaches its bone through identity — `m_Global` composed by T5.1 IS the
        // bone's new transform — which is a rig whose forwards event is one "set transform" per control.
        // The seam does not change when the graph arrives; the body of the loop below does.

        // ---- the output hop: rig hierarchy -> pose (report 05 §643, `UpdateOutput`) ---------------------
        m_Overrides.clear();
        m_Overrides.reserve( m_Drives.size() );

        for ( const ControlBoneDrive& drive : m_Drives )
        {
            const glm::mat4 global = m_Hierarchy.GetGlobalTransform( drive.Control );

            auto decomposed = BoneTransform::FromMatrix( global );
            if ( !decomposed.IsSuccess() )
            {
                // Named on BOTH sides. "A control produced a basis that will not decompose" is unactionable;
                // "control 'Hand_CTRL', driving bone 'Hand', is mirrored" names the thing the rigger edits.
                m_LastError =
                     fmt::format( "control '{}' driving bone {} ('{}'): {}", m_Hierarchy.Get( drive.Control ).Name,
                                  drive.Bone, skeleton.GetBones()[drive.Bone].Name, decomposed.GetError() );
                ReportErrorOnChange();
                return Common::MakeError<bool>( m_LastError );
            }

            m_Overrides.push_back( BoneOverride{ drive.Bone, decomposed.GetValue() } );
        }

        // 1.0F AND NOT A MEMBER. See the file note: the rig is the authored override, nothing today would
        // set a weight, and a knob that nothing moves is a knob that hides which of two pipelines ran.
        auto applied = ApplyBoneOverrides( skeleton, pose, component, m_Overrides, 1.0F, m_Scratch );
        if ( !applied.IsSuccess() )
        {
            m_LastError = applied.GetError();
            ReportErrorOnChange();
            return Common::MakeError<bool>( m_LastError );
        }

        ReportErrorOnChange(); // reports the END of a standing condition, too
        return Common::MakeSuccess( true );
    }

    void ControlRigStage::ReportErrorOnChange()
    {
        if ( m_LastError == m_ReportedError )
        {
            return;
        }
        if ( m_LastError.empty() )
        {
            LOG_INFO( "[ControlRig] the rig is contributing to the pose again." );
        }
        else
        {
            LOG_ERROR( "[ControlRig] {}", m_LastError );
        }
        m_ReportedError = m_LastError;
    }
} // namespace Desert::Animation
