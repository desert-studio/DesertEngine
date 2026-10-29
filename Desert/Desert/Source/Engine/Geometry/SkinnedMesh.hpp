#pragma once

#include "Mesh.hpp"

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

        // THE DISPLACED TWIN OF A SKINNED MESH (CONTRACT-DISPLACE §9.8, System::DisplacementCache): the rest pose of
        // the tessellated surface - @p vertices are @p vertexCount SkinnedVertex (rest displaced positions, the
        // nearest corner's bones), @p indices the tessellation, @p submeshes its ranges (bounds already grown by the
        // largest displacement) - skinned with @p base's skeleton. The frame's pose of it is written by
        // "Displacement: Skinned" into the skin cache's displaced streams; this twin's vertices feed only the
        // vertex stage's fallback, its UVs and the BLAS segments. Bone bounds are @p base's grown by
        // @p boundsGrowthCm, so posed bounds still enclose the displaced surface. No morph data: morphs apply to the
        // base before skinning, and the displacement moves that result.
        static std::shared_ptr<SkinnedMesh>
        MakeDisplacedTwin( const SkinnedMesh& base, std::shared_ptr<Graphic::VertexBuffer> vertices,
                           std::shared_ptr<Graphic::VertexBuffer> streams,
                           std::shared_ptr<Graphic::IndexBuffer>  indices,
                           std::vector<Submesh> submeshes, uint32_t vertexCount, float boundsGrowthCm );

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

        // One bone's reach inside one submesh: the bind-space box of the submesh's vertices the bone influences
        // (weight > 0). A skinned vertex is the weighted blend of its bones' transforms of it, a convex
        // combination of points each inside its bone's transformed box, so the union of every box under its
        // bone's pose matrix bounds the posed mesh (the virtual shadow map's caster bounds, UE: the posed bounds
        // of a skeletal mesh's bodies). Built once at load; a morphed twin shares its base's.
        struct BoneBounds
        {
            uint32_t           Submesh = 0;
            uint32_t           Bone    = 0;
            Common::Math::AABB Box;
        };
        [[nodiscard]] const std::vector<BoneBounds>& GetBoneBounds() const
        {
            return m_BoneBounds;
        }
        // How far the vertices this mesh draws may lie from their bind positions before skinning, cm: 0 on a base
        // mesh (it draws unmorphed), on a morphed twin the sum over targets of the target's longest position delta
        // (a blend of weights in [0, 1], MorphComponent's range).
        [[nodiscard]] float GetMorphReach() const
        {
            return m_MorphReach;
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

    private:
        SkinnedMesh() = default; // MakeMorphedTwin, MakeDisplacedTwin

        // `std::unordered_map<std::string, uint32_t> m_BoneNameToIndex;` used to sit here. It was never
        // populated and never read — dead state, and on the wrong object besides: a rig is SHARED between
        // meshes, so the name -> index map belongs to the Skeleton (which now has one) and not to each mesh
        // that references it.
        const Animation::Skeleton* m_Skeleton = nullptr;
        std::vector<SkinnedVertex> m_Vertices; // empty on a morphed twin: picking reads the base mesh
        std::vector<BoneBounds>    m_BoneBounds;
        float                      m_MorphReach       = 0.0f; // twin only (GetMorphReach)
        float                      m_MorphTargetReach = 0.0f; // the base's sum, handed to its twins

        uint32_t                                        m_VertexCount      = 0;
        uint32_t                                        m_MorphTargetCount = 0;
        std::shared_ptr<ShaderResources::StorageBuffer> m_MorphDeltas;
    };
} // namespace Desert
