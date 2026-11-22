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
     * @brief THE CONTROL RIG THIS ENTITY IS POSED BY. The thing that makes tier T5 reachable from a scene.
     *
     * T5 shipped a control hierarchy, a manipulator, keying and a pipeline stage, all proven by suites, and
     * NOT ONE SCENE COULD HAVE A RIG: `Animator::AttachRig` takes a `ControlRigStage` somebody has to build
     * in C++, and nobody did. This component is the "somebody" — one authored value, an asset handle, from
     * which `AnimationECSSystem` builds the stage against this entity's own skeleton every time the handle
     * or the file behind it changes.
     *
     * THE COMPONENT IS THE AUTHORED DATA; THE ANIMATOR OWNS THE STAGE, exactly as `TwoBoneIKData` next door
     * splits them and for the identical reason: a `ControlRigStage` holds bone indices resolved against one
     * skeleton, and this struct is copied by duplicate, rewritten by undo and rebuilt by the prefab path.
     * A duplicated entity with a different mesh would inherit indices into a skeleton it does not have.
     *
     * THERE IS DELIBERATELY NO ALPHA. `ControlRigStage::Evaluate` applies its overrides at 1.0 and says
     * why: the rig IS the authored override, and a weight nothing sets is a knob for a knob's sake. An
     * empty handle is "no rig", which is the off switch, and it is the same one bit the stage membership
     * test reads.
     *
     * It follows `AnimationComponent::Playing` for `TwoBoneIKData`'s reason: with playback stopped the
     * Animator is not updated at all and no stage runs, this one included.
     */
    struct ControlRigData
    {
        REFLECT()

        // THE ONLY AUTHORED VALUE. Empty = this entity has no rig, which is how the stage is turned off
        // without a second flag that could disagree with it.
        PROPERTY( DisplayName( "Rig" ), Category( "Control Rig" ), Asset<ControlRigAsset>,
                  Tooltip( "The .derig whose controls pose this entity's skeleton" ) )
        Assets::AssetHandle Rig;
    };

    struct ControlRigComponent
    {
        COMPONENT( Key( "ControlRig" ), Block( Data ), Run( ActorsAndUI ) )
        ControlRigData Data;
    };
} // namespace Desert::ECS
