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

#include <Engine/Assets/Common.hpp>

namespace Desert::ECS
{
    /**
     * @brief THE RIG PAIR THIS ENTITY PLAYS ITS CLIPS THROUGH. What makes tier T6.2 reachable from a scene.
     *
     * T6.2 shipped a three-stage retargeting pipeline measured against `JPH::SkeletonMapper` on the same
     * rigs and clips — worst limb-length error 0.000024 % against 7.934 %, a pelvis that rises by the ratio
     * of the two rigs' heights instead of by 1.00 — and NOT ONE SCENE COULD USE IT: `Retargeter::Initialize`
     * takes a `RetargetSetup` somebody has to fill in in C++, and nobody did. This component is the
     * "somebody", exactly as `ControlRigData` next door is for T5.
     *
     * ONE AUTHORED VALUE, AND THE PAIR IS NOT IN IT. A retarget is a statement about two rigs; the TARGET
     * rig is this entity's own mesh, and the SOURCE rig is named by the `.retarget` file, by signature
     * (see Engine/Assets/Serialization/Retarget.hpp). Authoring the source rig here as a second handle
     * would let the same file be pointed at a rig it was not authored against — accepted whenever the bone
     * names happened to resolve, and wrong by whatever the proportions differ by. One value, no way to
     * write a disagreement.
     *
     * THE COMPONENT IS THE AUTHORED DATA; THE ANIMATOR OWNS THE RETARGETER, for `ControlRigData`'s reason
     * and one more of its own: a `Retargeter` caches bone indices for BOTH rigs and a copy of the source
     * skeleton, and this struct is copied by duplicate, rewritten by undo and rebuilt by the prefab path.
     *
     * THERE IS DELIBERATELY NO ALPHA AND NO "SOURCE CLIP" FIELD. An empty handle is "no retarget", which
     * is the off switch and the same one bit the Animator's membership test reads; and which clip plays is
     * already `AnimationComponent::CurrentClip` — a second name for it here would be two statements about
     * one fact.
     *
     * It follows `AnimationComponent::Playing` for `TwoBoneIKData`'s reason: with playback stopped the
     * Animator is not updated at all and no source stage runs, this one included.
     */
    struct RetargetData
    {
        REFLECT()

        // THE ONLY AUTHORED VALUE. Empty = this entity's clips are on its own rig, which is how the
        // retarget is turned off without a second flag that could disagree with it.
        PROPERTY( DisplayName( "Retarget" ), Category( "Retarget" ), Asset<RetargetAsset>,
                  Tooltip( "The .retarget whose rig pair this entity's clips are played through" ) )
        Assets::AssetHandle Retarget;
    };

    struct RetargetComponent
    {
        COMPONENT( Key( "Retarget" ), Block( Data ), Run( ActorsAndUI ) )
        RetargetData Data;
    };
} // namespace Desert::ECS
