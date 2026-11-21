#pragma once

#include <span>

/**
 * THE POSE IS THE PIPELINE'S CURRENCY, NOT ITS RESULT.
 *
 * This file used to be twelve lines: `struct Pose { std::vector<glm::mat4> BoneMatrices; };`, and every
 * entry in it was already `chainGlobal_i * OffsetMatrix_i` — a SKINNING MATRIX, which is what comes out of
 * an animation pipeline, sitting where the thing flowing through it belongs. Three consequences were
 * measurable in the tree:
 *
 *   - the crossfade blended skinning matrices. `Decompose` was applied to `global * OffsetMatrix` and the
 *     rotation OF THAT PRODUCT was slerped; the inverse bind is baked into it, so that is not the bone's
 *     rotation and its slerp is not a rotation blend. Invisible on a rig whose OffsetMatrix is near
 *     identity — which our one probe is, exactly — and wrong on the first imported character;
 *   - recovering where a bone actually IS cost a 4x4 inverse per call (`skin * inverse(OffsetMatrix)`),
 *     because the only thing stored was the space nobody wanted to ask about;
 *   - "resolve the parent chain" had no name, so it was written out by hand eight times, every one of them
 *     a memoised `std::function` recursion allocating a closure and two scratch vectors per call.
 *
 * The three types below separate what was one:
 *
 *   LocalPose         parent-relative TRS per bone. What clips sample into, what a blend blends, what the
 *                     gizmo writes, what the Sequencer keys, and what an IK solver will one day modify.
 *   ComponentPose     the same pose resolved through the parent chain, LAZILY and per bone. Three callers
 *                     want exactly one bone out of a hundred (a socket, the bone gizmo, the Details
 *                     readout) and one wants all of them (skinning).
 *   SkinningMatrices  the OUTPUT. `component_i * OffsetMatrix_i`, the only thing that reaches the GPU.
 */

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <cstdint>
#include <vector>

#include <Engine/Animation/BoneTransform.hpp>

namespace Desert::Animation
{
    class Skeleton;


    /**
     * @brief PARENT-RELATIVE TRS, index-for-index with `Skeleton::GetBones()`.
     *
     * Index-for-index is not a convenience, it is the invariant that keeps UE's largest structural
     * complexity out of this engine: UE has three bone index spaces (skeleton, mesh, compact) and a linkup
     * table joining them because animation is authored against one and skinning against another. Here a
     * skeleton's bone index, a pose's index and a skinned vertex's `BoneIDs` entry are the same number.
     * `Skeleton::ValidateBoneIndexSpace` is what stops that being true only by accident.
     */
    class LocalPose
    {
    public:
        LocalPose() = default;
        explicit LocalPose( size_t boneCount ) : m_Bones( boneCount )
        {
        }

        [[nodiscard]] size_t Size() const
        {
            return m_Bones.size();
        }
        [[nodiscard]] bool Empty() const
        {
            return m_Bones.empty();
        }
        void Resize( size_t boneCount )
        {
            m_Bones.resize( boneCount );
        }

        [[nodiscard]] const BoneTransform& operator[]( size_t bone ) const
        {
            return m_Bones[bone];
        }
        [[nodiscard]] BoneTransform& operator[]( size_t bone )
        {
            return m_Bones[bone];
        }

        [[nodiscard]] auto begin() const
        {
            return m_Bones.begin();
        }
        [[nodiscard]] auto end() const
        {
            return m_Bones.end();
        }

        /**
         * @brief The rig at rest, as an ORDINARY POSE.
         *
         * This is the single strongest argument for the type. Before it, "draw the bind pose" was a
         * separate route written out three times — in `Scene::Raycast`'s picking bounds, in the render
         * path's bind branch, and in the bone overlay — each walking the chain itself from
         * `LocalBindTransform`. Bind is not a special case; it is the pose you get when nothing has
         * animated yet, and those three routes are gone.
         *
         * Refuses, naming the bone, when a bind transform cannot be decomposed (see FromMatrix).
         */
        [[nodiscard]] static Common::ResultStr<LocalPose> FromBindPose( const Skeleton& skeleton );

    private:
        std::vector<BoneTransform> m_Bones;
    };

    /**
     * @brief The same pose resolved through the parent chain — one bone at a time, on demand.
     *
     * LAZY BECAUSE THE CALLERS ARE. Of the four production readers of a resolved pose, three want exactly
     * one bone: `SocketAttachmentComponent` follows "hand_r", the bone gizmo places itself on the selected
     * bone and its parent, and the Details readout shows one row. Only skinning wants all of them.
     * Resolving one bone costs its depth, not the rig's size, and `Converted()` says which is which.
     *
     * A bone whose parent is out of range, or part of a parent cycle, RESOLVES AS A ROOT — and the
     * skeleton records that as a structure error at construction (`Skeleton::GetStructureError`). It is
     * defined behaviour rather than a choice made twice: the old tree had `ComputeBindPose` treat such a
     * bone as a root while `CalculatePose`, which only ever descended from real roots, never visited it at
     * all and left last frame's matrix in place — two resolvers, two answers, no one asserting either.
     */
    class ComponentPose
    {
    public:
        ComponentPose( const Skeleton& skeleton, const LocalPose& local );

        /// The bone's transform in component (mesh-local) space, converting it and any unconverted
        /// ancestors on the way. Identity for an out-of-range index.
        [[nodiscard]] const glm::mat4& Get( uint32_t bone );

        /// Whether `bone` has already been converted. The per-bone flag this class exists for, and the
        /// only way a test can assert that laziness is real rather than claimed.
        [[nodiscard]] bool Converted( uint32_t bone ) const
        {
            return bone < m_Converted.size() && m_Converted[bone] != 0;
        }

        /// Converts every bone, in the skeleton's cached parent-before-child order — a flat loop, no
        /// recursion and no per-call scratch.
        void ConvertAll();

        /// Drops every converted flag. Called when the LocalPose this view is over has been rewritten;
        /// the storage is kept so a per-frame re-evaluation allocates nothing.
        void Invalidate();

        /// Re-sizes to the pose's bone count and drops every flag. For the case the old code guarded with
        /// `if ( m_LocalPose.size() != bones.size() ) InitLocalPose();` scattered across four methods.
        void Reset( size_t boneCount );

        [[nodiscard]] size_t Size() const
        {
            return m_Global.size();
        }

        /// `component_i * OffsetMatrix_i` for every bone. Converts everything first.
        void WriteSkinningMatrices( std::vector<glm::mat4>& out );
        /// Only @p bones (parent-closed, in resolve order; empty = every bone): the others keep what @p out held.
        void WriteSkinningMatrices( std::vector<glm::mat4>& out, std::span<const uint32_t> bones );

    private:
        const Skeleton&        m_Skeleton;
        const LocalPose&       m_Local;
        std::vector<glm::mat4> m_Global;
        std::vector<uint8_t>   m_Converted;

        /// Scratch for Get()'s walk up to the nearest converted ancestor. A member rather than a local so a
        /// per-bone query allocates nothing after the first one.
        std::vector<uint32_t> m_Chain;
    };

    /**
     * @brief THE PIPELINE'S OUTPUT. `component_i * OffsetMatrix_i`, and nothing else reaches the GPU.
     *
     * Kept as a bare matrix array on purpose: every consumer downstream of `Animator::GetPose()` —
     * `DrawSkinnedMeshCommand`, `MeshRenderer`, `Material::UploadSkinnedBones`, `SkinnedMaterialUB` — forwards
     * it unchanged into a storage buffer. Introducing the pose types above changed those call sites not at
     * all; it changed what the Animator keeps INSIDE.
     */
    struct SkinningMatrices
    {
        std::vector<glm::mat4> Matrices;
    };
} // namespace Desert::Animation
