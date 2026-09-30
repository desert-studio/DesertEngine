#pragma once

#include <Engine/Assets/Common.hpp> // AssetHandle

#include <cstdint>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Geometry
{
    // Builds the built-in humanoid as a rigged + skinned mesh: tapered capsules and joint spheres, each
    // rigid-skinned (weight 1.0) to one bone, laid over the bind pose of the rig Humanoid.skeleton states — a
    // greybox "mannequin" placeholder (think the default UE/Unity rig), NOT a sculpted art asset. The rig is
    // the ASSET's (ProceduralCharacterSkeleton.hpp: HumanoidSkeletonFile, LoadHumanoidSkeleton); no joint is
    // restated in code, so the procedural idle/walk/run clips ([[ProceduralCharacterAnimations]]) drive the
    // same bones the file names.
    //
    // The mesh is generated + GPU-registered ONCE and cached process-wide (handle reused). Use it as a
    // SkinnedMeshComponent.MeshHandle; add an AnimationComponent and the AnimationECSSystem auto-plays any
    // clip whose skeleton reference is Humanoid.skeleton's GUID (ClipPlaysOnMesh).
    class ProceduralCharacterFactory
    {
    public:
        // Cooked-equivalent handle for the humanoid skinned mesh (built + registered on first call). Handle 0
        // when the rig does not read or lacks a bone the body names (both logged by name).
        static Assets::AssetHandle GetHumanoidMesh();

        // The humanoid skeleton, read from Humanoid.skeleton on first use. Used by the procedural animation
        // generator to read each bone's bind-local translation + index by name. Owned by the factory (process
        // lifetime); nullptr when the rig does not read or lacks a bone the body names.
        static const Animation::Skeleton* GetHumanoidSkeleton();
    };
} // namespace Desert::Geometry
