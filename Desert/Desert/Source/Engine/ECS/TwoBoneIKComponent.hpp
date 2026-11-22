#pragma once

#include <entt/entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::ECS
{
    /**
     * @brief TWO-BONE IK ON ONE LIMB OF THIS ENTITY'S RIG. The first skeletal control to reach a scene.
     *
     * REFLECTED, unlike AnimationComponent next door — which is hand-serialised because it owns an
     * `Animator`, a graph and a notify queue, none of which are values an artist types. This one is four
     * values an artist types, so it is a `Data` block and gets its Details page, its undo, its duplicate,
     * its prefab and its Lua binding from the same table as every other reflected component.
     *
     * THE COMPONENT IS THE AUTHORED DATA; THE ANIMATOR OWNS THE SOLVER. `AnimationECSSystem` copies these
     * four values into the entity's `TwoBoneIKControl` every frame and creates or drops that control as
     * this component appears or goes. The alternative — storing the control here — would put a live,
     * rig-resolved object into a struct that is copied by duplicate, written by undo and rebuilt by the
     * prefab path, and every one of those would carry bone indices resolved against a different rig.
     *
     * It follows `AnimationComponent::Playing`, because that flag gates the whole pose pipeline for the
     * entity: with playback stopped the Animator is not updated at all and no stage runs, this one
     * included.
     */
    struct TwoBoneIKData
    {
        REFLECT()

        // THE ONLY AUTHORED BONE. The chain is this bone, its parent and its grandparent — UE authors it
        // the same way and has no root-bone pin at all. Naming both ends would let an artist name two
        // bones that are not related, and there is nothing to be done about that except detect it; this
        // way the invalid case cannot be typed.
        PROPERTY( DisplayName( "End Bone" ), Category( "Two-Bone IK" ),
                  Tooltip( "The hand/foot bone. Its parent and grandparent become the two limbs" ) )
        std::string EndBone;

        // COMPONENT (MESH-LOCAL) SPACE, CENTIMETRES — the space the pose itself is resolved in, so no
        // conversion stands between what is typed here and what the solver reads. A world-space goal would
        // need the entity's transform, which is a second source of truth for where the goal is whenever
        // the entity moves.
        PROPERTY( DisplayName( "Goal" ), Category( "Two-Bone IK" ),
                  Tooltip( "Where the end bone should land, in mesh-local centimetres" ) )
        glm::vec3 Goal = glm::vec3( 0.0f );

        // A POINT the joint bends towards, not a direction (report 03 §785) — so it can become a bone or a
        // socket later without this field changing meaning. On the root->goal line it names no plane, and
        // the solver then keeps the pose's own bend rather than inventing an axis.
        PROPERTY( DisplayName( "Pole Target" ), Category( "Two-Bone IK" ),
                  Tooltip( "Point the elbow/knee bends towards, in mesh-local centimetres" ) )
        glm::vec3 PoleTarget = glm::vec3( 0.0f );

        // 0 = the animation's own pose, 1 = the solve. Blended in LOCAL space by the control base, so half
        // is a valid pose and not a sheared chain. There is deliberately no separate "Enabled": alpha 0 is
        // off, and two knobs for one fact is a disagreement waiting to be authored.
        PROPERTY( DisplayName( "Alpha" ), Category( "Two-Bone IK" ), Range( 0.0f, 1.0f ),
                  Tooltip( "How far from the animated pose towards the solved one" ) )
        float Alpha = 1.0f;
    };

    struct TwoBoneIKComponent
    {
        COMPONENT( Key( "TwoBoneIK" ), Block( Data ), Run( ActorsAndUI ) )
        TwoBoneIKData Data;
    };
} // namespace Desert::ECS
