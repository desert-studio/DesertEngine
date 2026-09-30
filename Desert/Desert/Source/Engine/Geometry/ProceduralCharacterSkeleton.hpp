#pragma once

#include <filesystem>
#include <memory>
#include <span>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Geometry
{
    // THE BUILT-IN HUMANOID'S RIG — ONE HOME, THE ASSET. Its bones are the ones Humanoid.skeleton states
    // (engine content, Resources/Engine/Meshes/Skinned/), read here; nothing in code restates a joint's name,
    // parent or bind position. The body ProceduralCharacterFactory wraps around it names bones BY NAME
    // (the tables below), so a rig that lacks one is refused by name instead of skinning to a wrong index.
    // CPU only — the factory's mesh registration lives in ProceduralCharacterFactory.cpp.

    // Where the built-in humanoid's skeleton asset is. Its header GUID IS the rig's identity, as every
    // skeleton's is (contract Engine/Animation/SkeletonReference.hpp); ProceduralCharacterAnimations::
    // RegisterClips reads that GUID from the content registry's row of this file, never from a constant.
    std::filesystem::path HumanoidSkeletonFile();

    // The rig read from HumanoidSkeletonFile(), bones and bind exactly as the file states them. A file that
    // is missing or does not read is a LOG_ERROR naming it and nullptr — there is no second rig to fall to.
    std::unique_ptr<Animation::Skeleton> LoadHumanoidSkeleton();

    // A tapered capsule between two bones' bind positions, rigid-skinned to SkinBone. Radii in metres.
    struct HumanoidSegment
    {
        const char* BoneA;
        const char* BoneB;
        float       RadiusA;
        float       RadiusB;
        const char* SkinBone;
    };

    // A sphere rounding off a joint, skinned to that bone. Radius in metres.
    struct HumanoidSphere
    {
        const char* Bone;
        float       Radius;
    };

    std::span<const HumanoidSegment> HumanoidSegments();
    std::span<const HumanoidSphere>  HumanoidSpheres();
    // The ankles a short forward foot capsule hangs off.
    std::span<const char* const> HumanoidFeet();
} // namespace Desert::Geometry
