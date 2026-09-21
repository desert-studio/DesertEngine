#include "Retargeter.hpp"

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/Solvers/TwoBoneIK.hpp>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <utility>

namespace Desert::Animation::Retarget
{
    namespace
    {
        using Desert::Animation::Solvers::SolveTwoBoneIK;
        using Desert::Animation::Solvers::TwoBoneIKChain;
        using Desert::Animation::Solvers::TwoBoneIKGoal;

        /// 1 world unit = 1 cm, and +Y is up: every bone in the corpus rigs is offset along +Y from its
        /// parent and the roots sit at y=150 (IKProbe) and y=100 (TwoBoneProbe). "Pelvis height" in
        /// `PelvisMotionOp.cpp:223-224` is the pelvis's height above the ground in the rest pose, so it is
        /// this component of its model-space translation.
        constexpr int UP_AXIS = 1;

        /// Below this a length carries no direction and a height carries no scale. 1e-4 cm is a tenth of a
        /// micrometre — far under anything a rig expresses and far over float noise on a 150 cm rig.
        constexpr float MIN_LENGTH_CM = 1.0e-4F;

        /// The rotation that takes `from` to `to`, identity when either is too short to have a direction.
        /// The same helper, for the same reason, as `TwoBoneIKControl`'s: a DELTA premultiplied onto an
        /// existing rotation, so whatever twist about the limb's own axis the FK stage produced survives
        /// the IK correction.
        glm::quat RotationBetween( const glm::vec3& from, const glm::vec3& to )
        {
            if ( glm::length( from ) < MIN_LENGTH_CM || glm::length( to ) < MIN_LENGTH_CM )
            {
                return { 1.0F, 0.0F, 0.0F, 0.0F };
            }
            return glm::rotation( glm::normalize( from ), glm::normalize( to ) );
        }

        /// The run of bones from `start` down to `end`, inclusive, or empty when `end` is not a descendant
        /// of `start`. Walks UP from the end, which is the only direction a `Skeleton` can be walked: it
        /// keeps parents, not children.
        std::vector<uint32_t> BoneRun( const Skeleton& skeleton, uint32_t start, uint32_t end )
        {
            std::vector<uint32_t> run;
            uint32_t              cursor = end;
            for ( size_t guard = 0; guard <= skeleton.GetBones().size(); ++guard )
            {
                run.push_back( cursor );
                if ( cursor == start )
                {
                    std::reverse( run.begin(), run.end() );
                    return run;
                }
                cursor = skeleton.ResolveParent( cursor );
                if ( cursor == Skeleton::NO_PARENT )
                {
                    break;
                }
            }
            return {};
        }

        /// Normalised distance along the run to each bone's origin, plus the run's total rest length.
        /// The parameterisation is by LENGTH rather than by bone count, because that is what makes a
        /// 3-bone source arm and a 5-bone target arm describe the same physical positions along the limb;
        /// counting bones would put the target's elbow at 0.5 wherever the rig happened to put it.
        float ParameteriseRun( const ModelPose& restModel, const std::vector<uint32_t>& run,
                               std::vector<float>& outParams )
        {
            outParams.assign( run.size(), 0.0F );
            float total = 0.0F;
            for ( size_t i = 1; i < run.size(); ++i )
            {
                total += glm::length( restModel[run[i]].Translation - restModel[run[i - 1]].Translation );
                outParams[i] = total;
            }
            if ( total < MIN_LENGTH_CM )
            {
                return 0.0F;
            }
            for ( float& p : outParams )
            {
                p /= total;
            }
            return total;
        }
    } // namespace

    Common::BoolResultStr Retargeter::Initialize( const Skeleton& source, const Skeleton& target,
                                                  RetargetSetup setup )
    {
        m_Initialized      = false;
        m_Setup            = std::move( setup );
        m_SourceSignature  = source.GetSignature();
        m_TargetSignature  = target.GetSignature();
        m_SourceBoneCount  = source.GetBones().size();
        m_TargetBoneCount  = target.GetBones().size();

        const auto sourcePelvis = source.FindBoneIndex( m_Setup.SourcePelvisBone );
        if ( !sourcePelvis.has_value() )
        {
            return Common::MakeFormattedError<bool>(
                 "the source rig has no pelvis bone named '{}'; retargeting has no root to scale against.",
                 m_Setup.SourcePelvisBone );
        }
        const auto targetPelvis = target.FindBoneIndex( m_Setup.TargetPelvisBone );
        if ( !targetPelvis.has_value() )
        {
            return Common::MakeFormattedError<bool>(
                 "the target rig has no pelvis bone named '{}'; retargeting has no root to scale against.",
                 m_Setup.TargetPelvisBone );
        }
        m_SourcePelvis = *sourcePelvis;
        m_TargetPelvis = *targetPelvis;

        auto sourceInitial = m_Setup.SourceRetargetPose.Apply( source, m_SourcePelvis );
        if ( !sourceInitial.IsSuccess() )
        {
            return Common::MakeFormattedError<bool>( "the source retarget pose: {}", sourceInitial.GetError() );
        }
        m_SourceInitialLocal = sourceInitial.ExtractValue();

        auto targetInitial = m_Setup.TargetRetargetPose.Apply( target, m_TargetPelvis );
        if ( !targetInitial.IsSuccess() )
        {
            return Common::MakeFormattedError<bool>( "the target retarget pose: {}", targetInitial.GetError() );
        }
        m_TargetInitialLocal = targetInitial.ExtractValue();

        auto sourceModel = ModelPose::FromLocal( source, m_SourceInitialLocal );
        if ( !sourceModel.IsSuccess() )
        {
            return Common::MakeFormattedError<bool>( "the source rig's rest pose: {}", sourceModel.GetError() );
        }
        m_SourceInitialModel = sourceModel.ExtractValue();

        auto targetModel = ModelPose::FromLocal( target, m_TargetInitialLocal );
        if ( !targetModel.IsSuccess() )
        {
            return Common::MakeFormattedError<bool>( "the target rig's rest pose: {}", targetModel.GetError() );
        }
        m_TargetInitialModel = targetModel.ExtractValue();

        // THE PELVIS HEIGHT SCALE. `08_retarget_measurement.md` §2.2 measured what its absence costs:
        // "the source root rises 100 cm from its rest pose; the root of a target 1.5 times taller rises
        // 100 cm. A ratio of 1.00 where a retargeter with pelvis height scaling would give 1.5" — a step
        // over a 50 cm kerb stays a 50 cm step on a character half again as tall.
        const float sourceHeight = m_SourceInitialModel[m_SourcePelvis].Translation[UP_AXIS];
        const float targetHeight = m_TargetInitialModel[m_TargetPelvis].Translation[UP_AXIS];
        if ( glm::abs( sourceHeight ) < MIN_LENGTH_CM || glm::abs( targetHeight ) < MIN_LENGTH_CM )
        {
            return Common::MakeFormattedError<bool>(
                 "pelvis height scaling is undefined: '{}' rests {:g} cm above the ground and '{}' rests "
                 "{:g} cm. A pelvis at zero height gives no ratio, and inventing 1.0 here would hide a rig "
                 "whose root was never placed.",
                 m_Setup.SourcePelvisBone, sourceHeight, m_Setup.TargetPelvisBone, targetHeight );
        }
        m_PelvisHeightScale = targetHeight / sourceHeight;

        m_ChainOfTarget.assign( target.GetBones().size(), NO_CHAIN );
        m_ParamOfTarget.assign( target.GetBones().size(), 0.0F );
        m_RunIndexOfTarget.assign( target.GetBones().size(), 0 );
        m_SourceOfTarget.assign( target.GetBones().size(), NO_SOURCE );

        auto chains = ResolveChains( source, target );
        if ( !chains.IsSuccess() )
        {
            return chains;
        }
        auto pairings = BuildPairings( source, target );
        if ( !pairings.IsSuccess() )
        {
            return pairings;
        }

        m_WorkLocal = m_TargetInitialLocal;
        m_WorkModel = m_TargetInitialModel;

        m_Initialized = true;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr Retargeter::ResolveChains( const Skeleton& source, const Skeleton& target )
    {
        m_Chains.clear();
        m_Chains.reserve( m_Setup.Chains.size() );

        for ( const RetargetChain& authored : m_Setup.Chains )
        {
            const auto sourceStart = source.FindBoneIndex( authored.SourceStartBone );
            const auto sourceEnd   = source.FindBoneIndex( authored.SourceEndBone );
            const auto targetStart = target.FindBoneIndex( authored.TargetStartBone );
            const auto targetEnd   = target.FindBoneIndex( authored.TargetEndBone );
            if ( !sourceStart || !sourceEnd || !targetStart || !targetEnd )
            {
                return Common::MakeFormattedError<bool>(
                     "chain '{}' names bones that do not all exist: source '{}'->'{}' ({}, {}), target "
                     "'{}'->'{}' ({}, {}).",
                     authored.Name, authored.SourceStartBone, authored.SourceEndBone,
                     sourceStart ? "found" : "MISSING", sourceEnd ? "found" : "MISSING",
                     authored.TargetStartBone, authored.TargetEndBone, targetStart ? "found" : "MISSING",
                     targetEnd ? "found" : "MISSING" );
            }

            ResolvedChain resolved;
            resolved.Name        = authored.Name;
            resolved.DriveWithIK = authored.DriveWithIK;
            resolved.SourceRun   = BoneRun( source, *sourceStart, *sourceEnd );
            resolved.TargetRun   = BoneRun( target, *targetStart, *targetEnd );

            if ( resolved.SourceRun.empty() )
            {
                return Common::MakeFormattedError<bool>(
                     "chain '{}': source bone '{}' is not a descendant of '{}', so there is no run between "
                     "them.",
                     authored.Name, authored.SourceEndBone, authored.SourceStartBone );
            }
            if ( resolved.TargetRun.empty() )
            {
                return Common::MakeFormattedError<bool>(
                     "chain '{}': target bone '{}' is not a descendant of '{}', so there is no run between "
                     "them.",
                     authored.Name, authored.TargetEndBone, authored.TargetStartBone );
            }

            resolved.SourceRestLength =
                 ParameteriseRun( m_SourceInitialModel, resolved.SourceRun, resolved.SourceParams );
            resolved.TargetRestLength =
                 ParameteriseRun( m_TargetInitialModel, resolved.TargetRun, resolved.TargetParams );

            // TIER 2 SHIPS A TWO-BONE SOLVER, AND A THREE-BONE RUN IS WHAT IT SOLVES. Silently solving the
            // first three bones of a five-bone run would leave the last two behind the goal while every
            // assertion about the chain still passed -- the shape of defect this project keeps closing.
            // An N-bone limb solver is Tier 2's remainder, not something to fake here.
            if ( resolved.DriveWithIK && resolved.TargetRun.size() != 3 )
            {
                return Common::MakeFormattedError<bool>(
                     "chain '{}' is driven by IK and its target run is {} bones. The solver from Tier 2 is "
                     "TWO-BONE: it needs exactly root, joint and end.",
                     authored.Name, resolved.TargetRun.size() );
            }
            if ( resolved.DriveWithIK &&
                 ( resolved.SourceRestLength < MIN_LENGTH_CM || resolved.TargetRestLength < MIN_LENGTH_CM ) )
            {
                return Common::MakeFormattedError<bool>(
                     "chain '{}' is driven by IK and rests at {:g} cm on the source and {:g} cm on the "
                     "target. A normalised extension needs a rest length to normalise against.",
                     authored.Name, resolved.SourceRestLength, resolved.TargetRestLength );
            }

            const int32_t chainIndex = static_cast<int32_t>( m_Chains.size() );
            for ( size_t i = 0; i < resolved.TargetRun.size(); ++i )
            {
                const uint32_t bone = resolved.TargetRun[i];
                if ( m_ChainOfTarget[bone] != NO_CHAIN )
                {
                    return Common::MakeFormattedError<bool>(
                         "chains '{}' and '{}' both claim target bone '{}'; which of them writes it has no "
                         "answer that is not invented here.",
                         m_Chains[static_cast<size_t>( m_ChainOfTarget[bone] )].Name, authored.Name,
                         target.GetBones()[bone].Name );
                }
                m_ChainOfTarget[bone]    = chainIndex;
                m_ParamOfTarget[bone]    = resolved.TargetParams[i];
                m_RunIndexOfTarget[bone] = static_cast<uint32_t>( i );
            }

            m_Chains.push_back( std::move( resolved ) );
        }
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr Retargeter::BuildPairings( const Skeleton& source, const Skeleton& target )
    {
        m_Pairings.clear();

        // The pelvis is paired unconditionally: its correspondence is the one the setup states outright,
        // and leaving it to the name match would make a renamed pelvis silently lose its rotation while
        // keeping its retargeted translation -- a root that turns one way and travels another.
        const auto pairBone = [this]( uint32_t targetBone, uint32_t sourceBone )
        {
            m_SourceOfTarget[targetBone] = sourceBone;
            m_Pairings.push_back( BonePairing{ targetBone, sourceBone } );
        };

        for ( const uint32_t targetBone : target.GetResolveOrder() )
        {
            if ( m_ChainOfTarget[targetBone] != NO_CHAIN )
            {
                // CHAINS WIN over the name match, and that is the whole reason chains exist: the bones a
                // chain covers are exactly the ones whose correspondence is a run, not a name.
                continue;
            }

            const std::string& targetName = target.GetBones()[targetBone].Name;
            const auto         renamed    = m_Setup.BoneRenames.find( targetName );
            const std::string& sourceName = renamed == m_Setup.BoneRenames.end() ? targetName : renamed->second;

            if ( renamed != m_Setup.BoneRenames.end() && !source.FindBoneIndex( sourceName ).has_value() )
            {
                return Common::MakeFormattedError<bool>(
                     "the setup maps target bone '{}' onto source bone '{}', and the source rig of {} bones "
                     "has no such bone.",
                     targetName, sourceName, source.GetBones().size() );
            }

            if ( const auto sourceBone = source.FindBoneIndex( sourceName ) )
            {
                pairBone( targetBone, *sourceBone );
            }
        }

        if ( m_SourceOfTarget[m_TargetPelvis] == NO_SOURCE && m_ChainOfTarget[m_TargetPelvis] == NO_CHAIN )
        {
            pairBone( m_TargetPelvis, m_SourcePelvis );
            // Pairings are read in the target's resolve order by stage 2; the pelvis appended last would
            // still be correct (the pass is over the resolve order, not over this list) but a caller
            // reading GetBonePairings() for a measurement deserves the order it was promised.
            std::sort( m_Pairings.begin(), m_Pairings.end(),
                       [&target]( const BonePairing& a, const BonePairing& b )
                       {
                           return target.GetResolveRank( a.TargetBone ) <
                                  target.GetResolveRank( b.TargetBone );
                       } );
        }

        // A RETARGETER THAT MAPS NOTHING EMITS THE TARGET'S REST POSE, SUCCESSFULLY, FOREVER. T5.4's
        // header names this exact failure: a stage that provably cannot change the pose passes every
        // assertion a working one passes. One pelvis pairing on its own is that stage -- it can only ever
        // move the root -- so the count that matters is "something other than the pelvis".
        const bool movesMoreThanTheRoot = m_Pairings.size() > 1 || !m_Chains.empty();
        if ( !movesMoreThanTheRoot )
        {
            return Common::MakeFormattedError<bool>(
                 "nothing but the pelvis is mapped between a source rig of {} bones and a target rig of {}: "
                 "no bone names match, no rename was authored and no chain was declared. This retargeter "
                 "could only ever emit the target's rest pose.",
                 source.GetBones().size(), target.GetBones().size() );
        }
        return Common::MakeSuccess( true );
    }

    glm::vec3 Retargeter::StagePelvisMotion( const ModelPose& sourceModel ) const
    {
        // THE DELTA FROM THE SOURCE'S OWN RETARGET POSE, SCALED, ONTO THE TARGET'S. Not the absolute
        // position scaled: a source rig whose rest pelvis is not at the origin would then be retargeted to
        // somewhere it never stood, and a clip that never moves the root would move the target's.
        const glm::vec3 sourceRise =
             sourceModel[m_SourcePelvis].Translation - m_SourceInitialModel[m_SourcePelvis].Translation;
        return m_TargetInitialModel[m_TargetPelvis].Translation + ( sourceRise * m_PelvisHeightScale );
    }

    glm::quat Retargeter::SourceChainDeltaAt( const ResolvedChain& chain, size_t runIndex, float param,
                                              const ModelPose& sourceModel ) const
    {
        const auto deltaOf = [&]( uint32_t bone )
        {
            return glm::normalize( sourceModel[bone].Rotation *
                                   glm::inverse( m_SourceInitialModel[bone].Rotation ) );
        };

        if ( chain.SourceRun.size() == chain.TargetRun.size() )
        {
            return deltaOf( chain.SourceRun[runIndex] );
        }
        if ( chain.SourceRun.size() == 1 )
        {
            return deltaOf( chain.SourceRun.front() );
        }

        size_t segment = 0;
        while ( segment + 2 < chain.SourceRun.size() && chain.SourceParams[segment + 1] < param )
        {
            ++segment;
        }

        const float lower = chain.SourceParams[segment];
        const float upper = chain.SourceParams[segment + 1];
        const float alpha = upper > lower ? glm::clamp( ( param - lower ) / ( upper - lower ), 0.0F, 1.0F ) : 0.0F;

        return glm::normalize(
             glm::slerp( deltaOf( chain.SourceRun[segment] ), deltaOf( chain.SourceRun[segment + 1] ), alpha ) );
    }

    Common::BoolResultStr Retargeter::StageFKChains( const Skeleton& target, const ModelPose& sourceModel,
                                                     const glm::vec3& pelvisModelTranslation )
    {
        for ( const uint32_t bone : target.GetResolveOrder() )
        {
            const uint32_t       parent = target.ResolveParent( bone );
            const BoneTransform& initial = m_TargetInitialLocal[bone];

            // THE DELTA GOES ON THE ROTATION AND NOWHERE ELSE. Translation and scale below are the
            // target's own, so its bone lengths are preserved by construction rather than by arithmetic.
            glm::quat modelRotation;
            if ( const int32_t chain = m_ChainOfTarget[bone]; chain != NO_CHAIN )
            {
                modelRotation = SourceChainDeltaAt( m_Chains[static_cast<size_t>( chain )],
                                                    m_RunIndexOfTarget[bone], m_ParamOfTarget[bone],
                                                    sourceModel ) *
                                m_TargetInitialModel[bone].Rotation;
            }
            else if ( const uint32_t sourceBone = m_SourceOfTarget[bone]; sourceBone != NO_SOURCE )
            {
                modelRotation = sourceModel[sourceBone].Rotation *
                                glm::inverse( m_SourceInitialModel[sourceBone].Rotation ) *
                                m_TargetInitialModel[bone].Rotation;
            }
            else
            {
                // An unmapped bone keeps its rest orientation RELATIVE TO ITS PARENT, so a target bone the
                // source rig does not have (a twist bone, an accessory) rides the limb it hangs off
                // instead of staying behind in model space.
                modelRotation = parent == Skeleton::NO_PARENT ? initial.Rotation
                                                              : m_WorkModel[parent].Rotation * initial.Rotation;
            }
            modelRotation = glm::normalize( modelRotation );

            BoneTransform& model = m_WorkModel[bone];
            model.Rotation       = modelRotation;
            if ( parent == Skeleton::NO_PARENT )
            {
                model.Scale       = initial.Scale;
                model.Translation = initial.Translation;
            }
            else
            {
                const BoneTransform& parentModel = m_WorkModel[parent];
                model.Scale                      = parentModel.Scale * initial.Scale;
                model.Translation =
                     parentModel.Translation + ( parentModel.Rotation * ( parentModel.Scale * initial.Translation ) );
            }

            m_WorkLocal[bone].Scale = initial.Scale;
            m_WorkLocal[bone].Rotation =
                 parent == Skeleton::NO_PARENT ? modelRotation
                                               : glm::normalize( glm::inverse( m_WorkModel[parent].Rotation ) *
                                                                 modelRotation );

            if ( bone != m_TargetPelvis )
            {
                m_WorkLocal[bone].Translation = initial.Translation;
                continue;
            }

            // The pelvis is the one bone whose translation stage 1 decided.
            model.Translation = pelvisModelTranslation;
            if ( parent == Skeleton::NO_PARENT )
            {
                m_WorkLocal[bone].Translation = pelvisModelTranslation;
                continue;
            }
            auto relative = Relative( m_WorkModel[parent], model );
            if ( !relative.IsSuccess() )
            {
                return Common::MakeFormattedError<bool>( "placing pelvis '{}' under '{}': {}",
                                                         target.GetBones()[bone].Name,
                                                         target.GetBones()[parent].Name,
                                                         relative.GetError() );
            }
            m_WorkLocal[bone].Translation = relative.ExtractValue().Translation;
        }
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr Retargeter::StageIKChains( const Skeleton& target, const ModelPose& sourceModel )
    {
        for ( const ResolvedChain& chain : m_Chains )
        {
            if ( !chain.DriveWithIK )
            {
                continue;
            }

            const uint32_t root  = chain.TargetRun[0];
            const uint32_t joint = chain.TargetRun[1];
            const uint32_t tip   = chain.TargetRun[2];

            // THE NORMALISED LIMB EXTENSION (§4.4, `IKChainsOp.cpp:121-135`): the goal is the source's
            // DIRECTION at the source's FRACTION of its own reach, laid off the TARGET's own rest chain
            // length. A source limb 70 % extended puts the target's goal at 70 % of what the target can
            // reach -- which is the whole difference between "the same angles" and "the same gesture".
            const glm::vec3 sourceSpan = sourceModel[chain.SourceRun.back()].Translation -
                                         sourceModel[chain.SourceRun.front()].Translation;
            const float     sourceReach = glm::length( sourceSpan );
            const float     extension   = sourceReach / chain.SourceRestLength;
            const glm::vec3 direction =
                 sourceReach < MIN_LENGTH_CM ? glm::vec3( 0.0F ) : sourceSpan / sourceReach;

            const TwoBoneIKChain solveChain{ m_WorkModel[root].Translation, m_WorkModel[joint].Translation,
                                             m_WorkModel[tip].Translation };
            const TwoBoneIKGoal  goal{ solveChain.Root + ( direction * extension * chain.TargetRestLength ),
                                      // THE POLE IS THE FK RESULT'S OWN JOINT. Stage 2 has already put the
                                      // elbow where the source's bend says it goes; the solver only has to
                                      // fix how far the limb reaches, and handing it the FK joint keeps the
                                      // authored bend plane instead of inventing an axis.
                                      solveChain.Joint };

            const auto solution = SolveTwoBoneIK( solveChain, goal );

            const glm::quat rootDelta =
                 RotationBetween( solveChain.Joint - solveChain.Root, solution.Joint - solveChain.Root );
            const glm::quat rootModel = glm::normalize( rootDelta * m_WorkModel[root].Rotation );

            // Where the tip ends up once the root bone has turned -- the joint bone has not moved in its
            // own parent's frame yet, so it carries the root's delta with it.
            const glm::vec3 tipAfterRoot = solution.Joint + ( rootDelta * ( solveChain.End - solveChain.Joint ) );
            const glm::quat jointDelta =
                 RotationBetween( tipAfterRoot - solution.Joint, solution.End - solution.Joint );
            const glm::quat jointModel =
                 glm::normalize( jointDelta * rootDelta * m_WorkModel[joint].Rotation );

            const uint32_t rootParent = target.ResolveParent( root );
            m_WorkLocal[root].Rotation =
                 rootParent == Skeleton::NO_PARENT
                      ? rootModel
                      : glm::normalize( glm::inverse( m_WorkModel[rootParent].Rotation ) * rootModel );
            m_WorkLocal[joint].Rotation = glm::normalize( glm::inverse( rootModel ) * jointModel );

            // Re-resolve the root bone and everything under it. Descendants of the tip -- fingers on a
            // hand, a foot's toes -- are exactly what a chain solved in isolation would leave behind.
            auto propagated =
                 m_WorkModel.PropagateFrom( target, m_WorkLocal, target.GetResolveRank( root ) );
            if ( !propagated.IsSuccess() )
            {
                return Common::MakeFormattedError<bool>( "chain '{}' after its IK solve: {}", chain.Name,
                                                         propagated.GetError() );
            }
        }
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr Retargeter::RefuseAForeignRig( const Skeleton& rig, uint64_t signature,
                                                         size_t boneCount, const char* which ) const
    {
        // THE CACHE IS A PILE OF BONE INDICES RESOLVED AGAINST ONE PARTICULAR RIG. Handed a different
        // one, every index means a different bone and the result is a pose, not an error -- which is the
        // failure a downstream test cannot see. The signature is FNV-1a over names and parents, so it
        // catches a rig of a different STRUCTURE; it is deliberately blind to proportions (T6.1 measured
        // a rig scaled x3 carrying the same signature), and it does not need to see them: a rig with the
        // same names and the same parents is one this cache's indices are correct for, which is the only
        // thing being claimed here.
        if ( rig.GetSignature() == signature && rig.GetBones().size() == boneCount )
        {
            return Common::MakeSuccess( true );
        }
        return Common::MakeFormattedError<bool>(
             "this retargeter was built for a {} rig of {} bones with signature {}, and was handed one of "
             "{} bones with signature {}. Every cached bone index would name a different bone.",
             which, boneCount, signature, rig.GetBones().size(), rig.GetSignature() );
    }

    Common::BoolResultStr Retargeter::Retarget( const Skeleton& source, const Skeleton& target,
                                                const LocalPose& sourceLocal, LocalPose& targetPose )
    {
        if ( !m_Initialized )
        {
            return Common::MakeError<bool>(
                 "this retargeter was never initialised, or its last Initialize() was refused." );
        }

        auto sameSource = RefuseAForeignRig( source, m_SourceSignature, m_SourceBoneCount, "source" );
        if ( !sameSource.IsSuccess() )
        {
            return sameSource;
        }
        auto sameTarget = RefuseAForeignRig( target, m_TargetSignature, m_TargetBoneCount, "target" );
        if ( !sameTarget.IsSuccess() )
        {
            return sameTarget;
        }

        auto sourceModel = ModelPose::FromLocal( source, sourceLocal );
        if ( !sourceModel.IsSuccess() )
        {
            return Common::MakeFormattedError<bool>( "the source pose: {}", sourceModel.GetError() );
        }
        const ModelPose model = sourceModel.ExtractValue();

        // THE PIPELINE. Three stages, one order, no dispatch (R8).
        const glm::vec3 pelvis = StagePelvisMotion( model );

        auto fk = StageFKChains( target, model, pelvis );
        if ( !fk.IsSuccess() )
        {
            return fk;
        }

        auto ik = StageIKChains( target, model );
        if ( !ik.IsSuccess() )
        {
            return ik;
        }

        if ( targetPose.Size() != m_WorkLocal.Size() )
        {
            targetPose.Resize( m_WorkLocal.Size() );
        }
        for ( size_t bone = 0; bone < m_WorkLocal.Size(); ++bone )
        {
            targetPose[bone] = m_WorkLocal[bone];
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Animation::Retarget
