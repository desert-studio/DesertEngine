#pragma once

#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/BoneInfo.hpp>

#include <Common/Core/ResultStr.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Animation
{
    /**
     * @brief The rig: an array of bones, plus the three derived facts every consumer of it kept
     *        recomputing for itself.
     *
     * A SKELETON'S STRUCTURE IS FIXED FOR ITS LIFETIME, and that is now a property of the type rather than
     * a habit. `GetBonesMutable()` used to hand out the whole vector; both of its callers only ever wrote
     * `LocalBindTransform` (rest-pose editing), but nothing said so, and the name -> index map and the
     * resolve order below would both be silently stale after a rename or a reparent. The narrow
     * `SetLocalBindTransform` expresses what actually happens and cannot invalidate either.
     */
    class Skeleton
    {
    public:
        /// Sentinel returned by ResolveParent for a bone that has no usable parent — a genuine root, or one
        /// whose ParentBoneID is out of range or part of a cycle (see GetStructureError).
        static constexpr uint32_t NO_PARENT = UINT32_MAX;

        explicit Skeleton( std::vector<BoneInfo>&& bones );

        [[nodiscard]] const std::vector<BoneInfo>& GetBones() const
        {
            return m_Bones;
        }

        /**
         * @brief Bone index by name — a hash lookup, and it used to be a linear scan.
         *
         * `for (i) if (m_Bones[i].Name == name)` with no cache, on a path every socket, every layer mask
         * and every future IK goal pays. `SetLayerMaskByNames` alone was O(names x bones) with a
         * std::string compare at the bottom of it. The map is built once, in the constructor, and the
         * structure that would invalidate it cannot change afterwards.
         */
        [[nodiscard]] std::optional<uint32_t> FindBoneIndex( const std::string& name ) const
        {
            const auto it = m_NameToIndex.find( name );
            return it == m_NameToIndex.end() ? std::nullopt : std::optional<uint32_t>( it->second );
        }

        /// The parent to resolve this bone against, or NO_PARENT. Not the same question as
        /// `BoneInfo::ParentBoneID`: this one has already rejected out-of-range and cyclic parents, so the
        /// answer is usable without re-checking it at every call site — which is what the eight hand-written
        /// chain walks each did, in four slightly different ways.
        [[nodiscard]] uint32_t ResolveParent( uint32_t bone ) const
        {
            return bone < m_Parents.size() ? m_Parents[bone] : NO_PARENT;
        }

        /// Bone indices in parent-before-child order. Computed once; it is what turns the chain resolve from
        /// a memoised recursion (a heap-allocated std::function and two scratch vectors per call) into a
        /// flat loop. The bone ARRAY order is deliberately left alone — bone indices are on disk in every
        /// `.skmesh`'s BoneIDs, so reordering the array is a cook-time remap and a separate task.
        [[nodiscard]] const std::vector<uint32_t>& GetResolveOrder() const
        {
            return m_ResolveOrder;
        }

        /**
         * @brief THE ONE PARENT-CHAIN WALK. `localOf(i)` supplies bone i's parent-relative matrix.
         *
         * Eight copies of this existed — `Skeleton::RecomputeOffsetMatrices`, three inside `Animator`,
         * `Scene::BindSkinningMatrices`, the render path's bind branch in `MeshECSSystem`, the bone overlay
         * and the bone gizmo — every one a memoised `std::function` recursion, byte-for-byte the same but
         * for which local transform it read. A ninth, `Animator::CalculateBoneTransform`, walked top-down
         * and found each bone's children by SCANNING THE WHOLE BONE ARRAY: 10 000 comparisons per frame on
         * a 100-bone rig, for a question the resolve order answers once.
         *
         * A template rather than a `std::function` parameter so the provider inlines; the whole point was
         * to stop paying for an indirect call per bone.
         */
        template <class LocalOf>
        void ResolveComponentSpace( LocalOf&& localOf, std::vector<glm::mat4>& out ) const
        {
            if ( out.size() != m_Bones.size() )
                out.assign( m_Bones.size(), glm::mat4( 1.0F ) );
            for ( const uint32_t i : m_ResolveOrder )
            {
                const glm::mat4 local  = localOf( i );
                const uint32_t  parent = m_Parents[i];
                out[i]                 = parent == NO_PARENT ? local : out[parent] * local;
            }
        }

        /**
         * @brief The rig at rest, as skinning matrices: `chainGlobal(LocalBindTransform)_i * OffsetMatrix_i`.
         *
         * "Draw this rig at rest" had THREE implementations — the picking bounds in `Scene::Raycast`, the
         * render path's bind branch in `MeshECSSystem`, and the bone overlay — and `Scene.cpp` carried a
         * comment admitting it: "identical to the render path's bind branch in MeshECSSystem, so the picked
         * bounds line up with the drawn bind-pose mesh". The correctness of two subsystems rested on two
         * copies of a lambda staying in step by hand. They are gone.
         *
         * Resolves the RAW bind matrices rather than a TRS pose, for the same reason
         * RecomputeOffsetMatrices does: this result multiplies by OffsetMatrix, which was computed as the
         * inverse of exactly this chain, and a TRS round trip would stop the two cancelling.
         */
        void WriteBindSkinningMatrices( std::vector<glm::mat4>& out ) const;

        /// Rest-pose editing (the bone gizmo outside pose mode). Structure is not editable — see the class
        /// comment. Returns false for an out-of-range index rather than writing past the array.
        bool SetLocalBindTransform( uint32_t bone, const glm::mat4& localBind );

        /// Rebuilds every bone's OffsetMatrix (= inverse of its global bind pose) from the current
        /// LocalBindTransform chain, so edited rest-pose bones stay consistent with skinning. Resolves the
        /// RAW bind matrices rather than going through a TRS pose: an OffsetMatrix must invert exactly what
        /// the chain produced, and a TRS round trip is not the identity on a matrix carrying shear.
        void RecomputeOffsetMatrices();

        [[nodiscard]] uint64_t GetSignature() const
        {
            return m_Signature;
        }

        /**
         * @brief Empty when the rig's parent links form a forest; otherwise what is wrong with them.
         *
         * Recorded at construction rather than discovered at use. The tree's old behaviour for a bone whose
         * ParentBoneID was out of range was two different answers from two resolvers — the bind-pose walk
         * treated it as a root, and the playback walk, which only ever descended from genuine roots, never
         * visited it and left the previous frame's matrix in its slot. A parent CYCLE was worse than either:
         * the memoised recursion set its `done` flag only on the way out, so it recursed until the stack
         * ended.
         */
        [[nodiscard]] const std::string& GetStructureError() const
        {
            return m_StructureError;
        }

        /**
         * @brief ONE BONE INDEX SPACE, ASSERTED.
         *
         * UE carries three — skeleton, mesh and compact — plus a per-mesh linkup table joining them, because
         * animation is authored against one and skinning against another. We have one: a `Skeleton` bone
         * index, a `LocalPose`/`SkinningMatrices` index and a `SkinnedVertex::BoneIDs` entry are the same
         * number, so the clip -> skeleton join is a name lookup done once per clip and nothing else is
         * needed. That is by far the largest piece of structural complexity this engine does not have.
         *
         * It was true by construction of the importer and of RigBuilder, and guarded by nothing — the first
         * mesh cooked against a different bone order would have read past the end of the pose array, or
         * skinned a vertex to the wrong bone, with no diagnostic anywhere. This is the assertion that keeps
         * the property a property.
         *
         * @param boneIDs every influence index the mesh carries (weight > 0), in any order.
         * @param sourceName what to name in the refusal — the mesh's path or asset name.
         */
        [[nodiscard]] Common::BoolResultStr ValidateBoneIndexSpace( const std::vector<uint32_t>& boneIDs,
                                                                    const std::string& sourceName ) const;

        static uint64_t ComputeSignature( const std::vector<BoneInfo>& bones );

    private:
        /// Fills m_Parents, m_ResolveOrder and m_StructureError. One pass, Kahn-style, so a cycle is what is
        /// LEFT OVER rather than something that has to be searched for.
        void BuildStructure();

        std::vector<BoneInfo> m_Bones;
        uint64_t              m_Signature = 0;

        std::unordered_map<std::string, uint32_t> m_NameToIndex;
        std::vector<uint32_t>                     m_Parents;      ///< resolvable parent, or NO_PARENT
        std::vector<uint32_t>                     m_ResolveOrder; ///< parent-before-child
        std::string                               m_StructureError;
    };

    /**
     * @brief A bone referred to BY NAME, with its index remembered.
     *
     * Report 03 §784 on `FBoneReference`, and the reason it is worth copying is the second predicate. A
     * reference has two independent failure modes and code that collapses them into one `bool IsValid()`
     * cannot tell an artist which happened:
     *
     *   HasName()      — the field was authored at all. Empty means nobody filled it in.
     *   IsResolved()   — the name was looked up against a rig and found. Unresolved with a name set means
     *                    the field IS authored and names a bone this rig does not have, which is a
     *                    different report to the user and a different fix.
     *
     * Resolution is explicit (`Resolve`) rather than lazy-on-read for the reason report 03 gives: cache
     * bone indices on bone-container CHANGE, so that a solver never pays a lookup inside its own loop.
     */
    class BoneRef
    {
    public:
        BoneRef() = default;
        explicit BoneRef( std::string name ) : m_Name( std::move( name ) )
        {
        }

        [[nodiscard]] const std::string& GetName() const
        {
            return m_Name;
        }

        void SetName( std::string name )
        {
            m_Name  = std::move( name );
            m_Index = NOT_RESOLVED; // a new name is not the old name's index
        }

        [[nodiscard]] bool HasName() const
        {
            return !m_Name.empty();
        }

        [[nodiscard]] bool IsResolved() const
        {
            return m_Index != NOT_RESOLVED;
        }

        /// The cached index. Only meaningful when IsResolved().
        [[nodiscard]] uint32_t GetIndex() const
        {
            return m_Index;
        }

        /// Looks the name up in `skeleton` and caches the result. Returns IsResolved().
        bool Resolve( const Skeleton& skeleton )
        {
            m_Index = NOT_RESOLVED;
            if ( m_Name.empty() )
            {
                return false;
            }
            if ( const auto found = skeleton.FindBoneIndex( m_Name ) )
                m_Index = *found;
            return IsResolved();
        }

    private:
        static constexpr uint32_t NOT_RESOLVED = UINT32_MAX;

        std::string m_Name;
        uint32_t    m_Index = NOT_RESOLVED;
    };
} // namespace Desert::Animation
