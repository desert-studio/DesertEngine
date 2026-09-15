#include "BoneControl.hpp"

#include <Engine/Animation/Skeleton.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Desert::Animation
{
    const char* ToString( BoneControlKind kind )
    {
        switch ( kind )
        {
            case BoneControlKind::TwoBoneIK:
                return "TwoBoneIK";
        }
        return "?";
    }

    Common::BoolResultStr ApplyBoneOverrides( const Skeleton& skeleton, LocalPose& pose, ComponentPose& component,
                                              const std::vector<BoneOverride>& overrides, float alpha,
                                              std::vector<BoneTransform>& scratch )
    {
        if ( alpha <= 0.0F )
        {
            // Not a fast path — a SPECIFIED one. A control at alpha 0 must leave the pose bit-for-bit as it
            // found it, and running the blend at 0 would not: `Blend` slerps, and a slerp at alpha 0 is not
            // bit-identical to its input for every quaternion. A1 short-circuited `BlendedBaseLocal` for the
            // same reason, and it is what lets a frame at alpha 0 be diffed against a frame with no control
            // at all and be required to differ in ZERO pixels.
            return Common::MakeSuccess( true );
        }

        // ---- validate the WHOLE list before touching the pose ------------------------------------------
        // A refusal has to leave the pose exactly as it was, and the write loop below mutates as it goes;
        // checking first is what makes "refused" and "half applied" different states.
        const size_t boneCount = pose.Size();
        uint32_t     lastRank  = 0;
        bool         haveLast  = false;
        for ( const BoneOverride& entry : overrides )
        {
            if ( entry.Bone >= boneCount )
            {
                return Common::MakeFormattedError<bool>(
                     "a control produced an override for bone {}, but the pose has {} bones. An index this "
                     "far out is a control resolved against a different rig, not a clamp to apply.",
                     entry.Bone, boneCount );
            }

            const uint32_t rank = skeleton.GetResolveRank( entry.Bone );
            if ( haveLast && rank <= lastRank )
            {
                return Common::MakeFormattedError<bool>(
                     "a control produced overrides out of order: bone {} ('{}') has resolve rank {}, which "
                     "is not after the previous override's rank {}. The list must be sorted parents before "
                     "children, because a child's component-space override is converted to a local "
                     "transform against its parent's SOLVED component transform — and out of order, the "
                     "parent has not been solved yet.",
                     entry.Bone, skeleton.GetBones()[entry.Bone].Name, rank, lastRank );
            }
            lastRank = rank;
            haveLast = true;
        }

        // ---- pass one: apply at FULL strength, remembering what was there ------------------------------
        //
        // THE TARGET LOCALS ARE BUILT AGAINST THE SOLVED CHAIN, NOT AGAINST A HALF-BLENDED ONE, and the
        // difference is the whole meaning of the alpha. What a caller asks for at 0.5 is "half-way between
        // the pose that came in and the pose the solver wants" — every bone's local half-way, so the result
        // is a valid pose that travels smoothly from one to the other. Converting a child against a parent
        // that had ALREADY been blended (the first shape this had) makes the child's local a function of
        // the parent's alpha as well as its own, so 0.5 is no longer half of anything and the chain reaches
        // the goal along a curve nobody authored. Measured on the eight-bone rig in
        // `Tests/Engine/BoneControlContract`: 5.06 cm of disagreement at the elbow, 4.47 cm at the hand.
        scratch.clear();
        scratch.reserve( overrides.size() );

        for ( const BoneOverride& entry : overrides )
        {
            const uint32_t parent = skeleton.ResolveParent( entry.Bone );
            // By value: `Get` hands back a reference into storage the next `Get` may rewrite.
            const glm::mat4 parentComponent =
                 parent == Skeleton::NO_PARENT ? glm::mat4( 1.0F ) : component.Get( parent );

            const glm::mat4 local      = glm::inverse( parentComponent ) * entry.Transform.ToMatrix();
            auto            decomposed = BoneTransform::FromMatrix( local );
            if ( !decomposed.IsSuccess() )
            {
                // Put back what has already been written, so a refusal in the middle of a chain is still a
                // refusal and not a pose that is half solved.
                for ( size_t done = 0; done < scratch.size(); ++done )
                {
                    pose[overrides[done].Bone] = scratch[done];
                }
                component.Invalidate();
                return Common::MakeFormattedError<bool>( "override for bone {} ('{}'): {}", entry.Bone,
                                                         skeleton.GetBones()[entry.Bone].Name,
                                                         decomposed.GetError() );
            }

            scratch.push_back( pose[entry.Bone] );
            pose[entry.Bone] = decomposed.GetValue();

            // This bone and every descendant now have a stale component transform, and the next override in
            // the list may be one of those descendants — which is why the list has to be ordered.
            component.Invalidate();
        }

        // ---- pass two: THE BLEND, IN LOCAL SPACE, and nowhere else -------------------------------------
        if ( alpha < 1.0F )
        {
            for ( size_t i = 0; i < overrides.size(); ++i )
            {
                pose[overrides[i].Bone] = Blend( scratch[i], pose[overrides[i].Bone], alpha );
            }
            component.Invalidate();
        }

        return Common::MakeSuccess( true );
    }

    float BoneControl::GetAlpha() const
    {
        return glm::clamp( m_Alpha, 0.0F, 1.0F );
    }

    void BoneControl::SetAlpha( float alpha )
    {
        m_Alpha = alpha;
    }

    Common::BoolResultStr BoneControl::Evaluate( const Skeleton& skeleton, LocalPose& pose,
                                                 ComponentPose& component )
    {
        m_LastError.clear();

        const float alpha = GetAlpha();
        if ( alpha <= 0.0F )
        {
            return Common::MakeSuccess( true );
        }

        m_Overrides.clear();
        // `this->` IS DELIBERATE AND IT IS NOT DECORATION. An unqualified call to a virtual on self is a
        // call nobody can find: `PureVirtualCensus` scans the tree for `.Name(` and `->Name(`, and with
        // the implicit receiver it reported `BoneControl::Solve` as a pure virtual that every implementer
        // overrides and NO translation unit ever calls — which is the exact shape of the dead-virtual
        // defect that census exists to catch, and it would have gone into a register as a lie. Spelling
        // the receiver makes the dispatch visible to the reader and to the gate at the same time.
        auto solved = this->Solve( skeleton, component, m_Overrides );
        if ( !solved.IsSuccess() )
        {
            m_LastError = solved.GetError();
            ReportErrorOnChange();
            return solved;
        }

        if ( m_Overrides.empty() )
        {
            // AN EMPTY SUCCESSFUL ANSWER IS A SILENT WRONG ANSWER (contract §1.4). A solve that reports
            // success and writes nothing is indistinguishable, from every caller and every frame, from one
            // that worked — which is precisely the failure mode this engine spent a task removing from the
            // clip lookup.
            m_LastError = fmt::format( "control '{}' reported a successful solve and produced no bone "
                                       "overrides at all.",
                                       ToString( GetKind() ) );
            ReportErrorOnChange();
            return Common::MakeError<bool>( m_LastError );
        }

        auto applied = ApplyBoneOverrides( skeleton, pose, component, m_Overrides, alpha, m_Scratch );
        if ( !applied.IsSuccess() )
        {
            m_LastError = applied.GetError();
        }
        ReportErrorOnChange();
        return applied;
    }

    void BoneControl::ReportErrorOnChange()
    {
        if ( m_LastError == m_ReportedError )
        {
            return;
        }

        if ( m_LastError.empty() )
        {
            LOG_INFO( "[BoneControl] '{}' is solving again.", ToString( GetKind() ) );
        }
        else
        {
            LOG_ERROR( "[BoneControl] '{}': {}", ToString( GetKind() ), m_LastError );
        }
        m_ReportedError = m_LastError;
    }
} // namespace Desert::Animation
