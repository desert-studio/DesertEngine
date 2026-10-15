#pragma once

#include "Mesh.hpp"

#include <Engine/Geometry/PosedBounds.hpp>

#include <memory>

namespace Desert::Animation
{
    class Skeleton;
    struct BoneInfo;
} // namespace Desert::Animation

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert
{

    class SkinnedMesh : public Mesh
    {
    public:
        // @p morphTargets are the mesh asset's (index-aligned with @p vertices); empty for a mesh without any.
        SkinnedMesh( const std::vector<SkinnedVertex>& vertices, const std::vector<Index>& indices,
                     const std::vector<Submesh>& submeshes, const Animation::Skeleton* skeleton,
                     const std::vector<MeshVertexStreams>& streams, const std::vector<MorphTarget>& morphTargets );

        // THE MORPHED TWIN OF ONE COMPONENT (UE FSkeletalMeshObjectGPUSkin's morph vertex buffer bound in place of
        // the base positions). Shares everything with @p base - index buffer, streams, submeshes, skeleton, morph
        // data - except the vertices, which are @p morphedVertices: a GpuWritten buffer of base.GetVertexCount()
        // SkinnedVertex that the morph update (MeshRendererMorph.cpp) writes before any pass skins from it. Every
        // pass that draws a skinned mesh then reads the morphed vertices without knowing morphs exist.
        static std::shared_ptr<SkinnedMesh>
        MakeMorphedTwin( const SkinnedMesh& base, std::shared_ptr<Graphic::VertexBuffer> morphedVertices );

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

        // Morph targets on the GPU (UE FMorphTargetVertexInfoBuffers), uploaded once with the mesh: target k's
        // delta of vertex i at [k * VertexCount + i] as two vec4 (position.xyz, 0) (normal.xyz, 0) - the layout
        // Programs/Compute/MorphTargets.shader reads. Null when the mesh has no morph targets.
        [[nodiscard]] uint32_t GetMorphTargetCount() const
        {
            return m_MorphTargetCount;
        }
        [[nodiscard]] uint32_t GetVertexCount() const
        {
            return m_VertexCount;
        }
        [[nodiscard]] const std::shared_ptr<ShaderResources::StorageBuffer>& GetMorphDeltas() const
        {
            return m_MorphDeltas;
        }

        // Each bone's box of the vertices it moves and of their morph deltas (Geometry::SkinnedBoneBounds): what
        // the renderer carries by the pose to cull the mesh in every view. Shared with the morphed twin.
        [[nodiscard]] const Geometry::SkinnedBoneBounds& GetBoneBounds() const
        {
            return *m_BoneBounds;
        }

    private:
        SkinnedMesh() = default; // MakeMorphedTwin

        // `std::unordered_map<std::string, uint32_t> m_BoneNameToIndex;` used to sit here. It was never
        // populated and never read — dead state, and on the wrong object besides: a rig is SHARED between
        // meshes, so the name -> index map belongs to the Skeleton (which now has one) and not to each mesh
        // that references it.
        const Animation::Skeleton* m_Skeleton = nullptr;
        std::vector<SkinnedVertex> m_Vertices; // empty on a morphed twin: picking reads the base mesh

        // Built in the constructor; never null.
        std::shared_ptr<const Geometry::SkinnedBoneBounds> m_BoneBounds;

        uint32_t                                        m_VertexCount      = 0;
        uint32_t                                        m_MorphTargetCount = 0;
        std::shared_ptr<ShaderResources::StorageBuffer> m_MorphDeltas;
    };
} // namespace Desert
