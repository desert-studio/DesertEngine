#pragma once

// A RAGDOLL (UE: USkeletalMeshComponent::PhysicsAssetOverride + its bodies' simulate flag). On Play the entity's
// skinned character gets one physics body per body of its `.dephysasset` and a swing-twist joint per constraint
// (ECS/System/RagdollLifetime.hpp). Kinematic: the bodies follow the animated pose and push what they touch.
// Simulated: gravity, contacts and the joints move them and the bones follow the bodies. The mode is switched
// at any time — from Details, or from Lua (`entity.Ragdoll.Mode`), which is how a hit turns a character limp.

#include <Engine/Assets/Common.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>

#include <cstdint>

namespace Desert::ECS
{
    struct RagdollData
    {
        REFLECT()

        PROPERTY( DisplayName( "Physics Asset" ), Category( "Ragdoll" ), Summary, Asset<PhysicsAsset>,
                  Tooltip( "The bodies and joints of this character (UE Physics Asset) — drag a .dephysasset "
                           "authored on the mesh's skeleton. An empty slot, or one authored on another "
                           "skeleton, is refused at Play by name, not simulated as nothing." ) )
        Assets::AssetHandle PhysicsAsset;

        PROPERTY( DisplayName( "Mode" ), Category( "Ragdoll" ), Summary,
                  Tooltip( "Kinematic: the bodies follow the animation. Simulated: the bodies fall and the "
                           "bones follow them. Switching keeps the pose: a simulated ragdoll starts from the "
                           "animated pose and its motion, a kinematic one snaps back to the animation." ) )
        Physics::RagdollMotion Mode = Physics::RagdollMotion::Kinematic;
    };

    struct RagdollComponent
    {
        RagdollData Data;

        // Transient: the ragdoll in the scene's Physics::PhysicsWorld (Physics::RagdollHandle; created on Play,
        // gone on Stop or when the entity / the component goes). Not reflected/serialized.
        uint32_t RuntimeRagdoll = 0xFFFFFFFFu;
    };
} // namespace Desert::ECS
