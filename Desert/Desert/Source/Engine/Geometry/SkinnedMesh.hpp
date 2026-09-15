#pragma once

#include "Mesh.hpp"

namespace Desert::Animation
{
    class Skeleton;
    struct BoneInfo;
} // namespace Desert::Animation

namespace Desert
{

    class SkinnedMesh : public Mesh
    {
    public:
        SkinnedMesh( const std::vector<SkinnedVertex>& vertices, const std::vector<Index>& indices,
                     const std::vector<Submesh>& submeshes, const Animation::Skeleton* skeleton );

        MeshType GetType() const override
        {
            return MeshType::Skinned;
        }

        const Animation::Skeleton& GetSkeleton() const
        {
            return *m_Skeleton;
        }

        // Mutable skeleton for the in-editor skeleton editor. The underlying Skeleton is owned (non-const) by
        // its SkeletonAsset and merely referenced here as const*, so the cast is safe. Edits affect every
        // skinned mesh sharing this rig (correct — a rig is shared).
        Animation::Skeleton* GetSkeletonMutable() const
        {
            return const_cast<Animation::Skeleton*>( m_Skeleton );
        }

        [[nodiscard]] virtual Common::BoolResultWithCodes<MeshError> Invalidate() override;

        // CPU-side skinned vertices, retained for viewport picking (posed-AABB) — mirrors the static mesh's
        // m_TriangleCache. Bone IDs/weights let a picker deform them by the current pose off the render path.
        const std::vector<SkinnedVertex>& GetVertices() const
        {
            return m_Vertices;
        }

    private:
        // `std::unordered_map<std::string, uint32_t> m_BoneNameToIndex;` used to sit here. It was never
        // populated and never read — dead state, and on the wrong object besides: a rig is SHARED between
        // meshes, so the name -> index map belongs to the Skeleton (which now has one) and not to each mesh
        // that references it.
        const Animation::Skeleton* m_Skeleton;
        std::vector<SkinnedVertex> m_Vertices;
    };
} // namespace Desert
