#pragma once

// AI-STATETREE CONTRACT (header only, owner review pending) - docs/contracts/AI-STATETREE.md.
//
// THE PERCEIVER AND THE PERCEIVED (UE UAIPerceptionComponent AIPerceptionComponent.h:208 and
// UAIPerceptionStimuliSourceComponent AIPerceptionStimuliSourceComponent.h:15).
//
// UE configures senses as an array of UAISenseConfig objects; here the three senses this contract has are
// fixed members (a sense the NPC lacks is Enabled = false), so the scene block is plain reflection and the
// Details panel needs no instanced-object editor. A new sense is a new member, added with its system.
//
// UE registers EVERY pawn as a sight source by default (bAutoRegisterAllPawnsAsSources); here only an entity
// with AIPerceptionStimuliSourceComponent can be perceived - no hidden global rule, and a crowd of props costs
// the sight query nothing.
//
// ON THE AI CONTROLLER (lead decision 10-22 Q4, UE UAIPerceptionComponent on AAIController): this component sits
// on the controller entity (Engine/AI/AIController.hpp), next to StateTreeComponent. The senses are the
// controller's, the body is the pawn's: the sight origin is the possessed pawn's origin + SightConfig::EyeHeight
// and its forward is the pawn's (UE GetActorEyesViewPoint), hearing and damage distances are measured from the
// pawn. A controller with no pawn (Pawn = entt::null) senses nothing and keeps its Known memory ageing.
// AIPerceptionStimuliSourceComponent sits on the PAWN (what is seen is a body, not a brain).

#include <Engine/AI/Perception/PerceptionTypes.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <vector>

namespace Desert::ECS
{
    struct AIPerceptionData
    {
        REFLECT()

        PROPERTY( DisplayName( "Sight" ), Category( "Perception" ) )
        AI::Perception::SightConfig Sight;

        PROPERTY( DisplayName( "Hearing" ), Category( "Perception" ) )
        AI::Perception::HearingConfig Hearing;

        PROPERTY( DisplayName( "Damage" ), Category( "Perception" ) )
        AI::Perception::DamageConfig Damage;

        PROPERTY( DisplayName( "Team" ), Category( "Perception" ), Range( 0, 255 ),
                  Tooltip( "This perceiver's team (UE FGenericTeamId). 0 = neutral." ) )
        int Team = 1;
    };

    struct AIPerceptionComponent
    {
        // GATED until AI-ST-ASSET/AI-ST-EXEC (the header tool skips commented markers; ReflectedComponentBlocks.gen.hpp would include this
        // header and the transient members need the execution's out-of-line destructor): COMPONENT( Key( "AIPerception" ), Block( Data ), Run( ActorsAndUI ) )
        AIPerceptionData Data;

        // ---- transient (not serialized): written by PerceptionSystem only ----
        /// One entry per (source, sense) currently sensed or remembered (UE FActorPerceptionInfo).
        std::vector<AI::Perception::Stimulus> Known;
        /// The sight target picked by PerceptionSystem's round-robin this tick (budgeted traces).
        std::uint32_t SightCursor = 0;
    };

    struct AIPerceptionStimuliSourceData
    {
        REFLECT()

        PROPERTY( DisplayName( "Sight" ), Category( "Stimuli Source" ),
                  Tooltip( "Registered as a sight source (UE RegisterAsSourceForSenses: Sight)." ) )
        bool Sight = true;

        PROPERTY( DisplayName( "Hearing" ), Category( "Stimuli Source" ),
                  Tooltip( "Noises this entity instigates are heard (a noise with no registered instigator "
                           "is still heard; this filters affiliation only)." ) )
        bool Hearing = true;

        PROPERTY( DisplayName( "Team" ), Category( "Stimuli Source" ), Range( 0, 255 ) )
        int Team = 0;

        PROPERTY( DisplayName( "Target Height" ), Category( "Stimuli Source" ), Length,
                  Tooltip( "Height above the origin the sight trace aims at (UE CanBeSeenFrom target point)." ) )
        float TargetHeight = 100.0f;
    };

    struct AIPerceptionStimuliSourceComponent
    {
        // GATED until AI-ST-ASSET/AI-ST-EXEC (the header tool skips commented markers; ReflectedComponentBlocks.gen.hpp would include this
        // header and the transient members need the execution's out-of-line destructor): COMPONENT( Key( "AIPerceptionStimuliSource" ), Block( Data ), Run( ActorsAndUI ) )
        AIPerceptionStimuliSourceData Data;
    };
} // namespace Desert::ECS
