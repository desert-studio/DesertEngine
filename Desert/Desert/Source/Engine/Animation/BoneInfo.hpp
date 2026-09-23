#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <optional>

namespace Desert::Animation
{
    // A BONE HAS NO INDEX FIELD, AND THAT IS THE POINT. It used to carry `uint32_t BoneIndex`, which every
    // producer set to the bone's own position in the array and no consumer could disagree with: ParentBoneID
    // indexes this array, Skeleton::FindBoneIndex returns the position, Animator walks positions. A second
    // copy of a value that must equal the first is a desync waiting to happen — and this one was declared
    // WITHOUT an initialiser, so a BoneInfo that skipped one assignment shipped an indeterminate index into
    // the .skeleton file. The position IS the index; there is nothing left to leave unset.
    struct BoneInfo
    {
        std::string Name;

        // Inverse bind pose (mesh space > bone space). INITIALISED, because glm leaves its matrices
        // indeterminate by default and this struct is written straight to the .skeleton file by reflect-cpp:
        // a producer that forgot one assignment used to serialise heap noise. Identity is the honest default
        // (a bone at rest), and unlike an index it cannot be mistaken for authored data.
        glm::mat4 OffsetMatrix = glm::mat4( 1.0f );

        // Local transform in bind pose (Assimp). Same reason as OffsetMatrix.
        glm::mat4 LocalBindTransform = glm::mat4( 1.0f );

        std::optional<uint32_t> ParentBoneID;

        [[nodiscard]] bool IsRoot() const
        {
            return !ParentBoneID.has_value();
        }

        [[nodiscard]] uint32_t GetParentID() const
        {
            return ParentBoneID.value_or( UINT32_MAX );
        }
    };
} // namespace Desert::Animation
