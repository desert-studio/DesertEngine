#pragma once

#include <Engine/Assets/Common.hpp> // AssetHandle

#include <Common/Content/AssetEnvelope.hpp> // AssetGuid

#include <cstdint>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Geometry
{
    // Builds a BLOCKY humanoid (head, torso, 2 arms, 2 legs) as a fully rigged + skinned mesh, entirely in
    // code — a greybox "mannequin" placeholder character (think the default UE/Unity rig), NOT a sculpted
    // art asset. Each body segment is one box rigid-skinned (weight 1.0) to its joint, so rotating a joint
    // swings the whole segment. The skeleton (~17 joints, A-pose with arms hanging) is authored here so the
    // accompanying procedural idle/walk/run clips ([[ProceduralCharacterAnimations]]) can drive it.
    //
    // The mesh is generated + GPU-registered ONCE and cached process-wide (handle reused). Use it as a
    // SkinnedMeshComponent.MeshHandle; add an AnimationComponent and the AnimationECSSystem auto-plays any
    // clip whose skeleton reference is GetHumanoidSkeletonGuid() (ClipPlaysOnMesh, SkeletonReference.hpp).
    class ProceduralCharacterFactory
    {
    public:
        // Cooked-equivalent handle for the humanoid skinned mesh (built + registered on first call).
        static Assets::AssetHandle GetHumanoidMesh();

        // THE IDENTITY of the built-in humanoid skeleton: engine content with no .skeleton file, so its GUID is a
        // compiled-in constant (minted once, never regenerated), as a cooked skeleton's is its header GUID. The
        // procedural clips reference it; ProceduralCharacterAnimations::RegisterClips states it for the mesh.
        static Common::Content::AssetGuid GetHumanoidSkeletonGuid();

        // The humanoid skeleton (built on first use). Used by the procedural animation generator to read each
        // bone's bind-local translation + index by name. Owned by the factory (process lifetime).
        static const Animation::Skeleton* GetHumanoidSkeleton();
    };
} // namespace Desert::Geometry
