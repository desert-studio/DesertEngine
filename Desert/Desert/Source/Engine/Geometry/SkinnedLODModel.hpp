#pragma once

#include <Engine/Geometry/MeshTypes.hpp>

#include <cstdint>
#include <vector>

namespace Desert
{
    // ONE SKELETAL LOD'S SKINNING WORK (UE FSkeletalMeshLODRenderData: each LOD its own vertices and its
    // RequiredBones / ActiveBoneIndices). Level L's submesh ranges (Submesh::LODs[L]) index ONLY vertices in
    // [FirstVertex, FirstVertex + VertexCount) - its own copies, appended after LOD 0's at load, their
    // influences reduced to MaxInfluences (UE FSkeletalMeshOptimizationSettings::MaxBonesPerVertex) - so the
    // skin pass of a record drawing level L skins exactly that range and reads RequiredBones bones.
    struct SkinnedLOD
    {
        uint32_t FirstVertex   = 0;
        uint32_t VertexCount   = 0;
        uint32_t RequiredBones = 0; // distinct bones the range's influences name
        uint32_t MaxInfluences = 0;
        // Those bones, ascending (UE FSkeletalMeshLODRenderData::ActiveBoneIndices): what the animation of a
        // record drawing this level must evaluate (Animator::SetRequiredBones closes it over the parents).
        std::vector<uint32_t> Bones;
    };

    /**
     * @brief THE SKELETAL LOD CHAIN ON THE CPU (UE FSkeletalMeshLODModel: CPU data the derivations read, render
     *        resources built from it later). No device: the SkinnedMesh constructor uploads exactly this, and the
     *        package cook (PackageCook CookVertexAnimations) derives the far crowd's vertex animation from it
     *        without one (WP-FAR-12B).
     *
     * The static chain's index levels (Geometry::BuildLODIndexBuffer - the cooked BakedLODs, else meshopt at the
     * unbaked chain), then each level >= 1 re-pointed at its OWN vertex copies appended after the authored ones,
     * level after level, with its influences reduced. A submesh whose chain is shorter repeats its last level at
     * every later one, so level L of every submesh lies in level L's vertex range and skinning that range alone is
     * complete for a draw of level L. Streams and morph deltas follow their source vertex.
     */
    struct SkinnedLODModel
    {
        std::vector<SkinnedVertex>     Vertices;
        std::vector<MeshVertexStreams> Streams;
        std::vector<MorphTarget>       Morphs;
        std::vector<Index>             GpuIndices;
        std::vector<Submesh>           Submeshes; // BakedLODs consumed (emptied), LODs filled
        std::vector<SkinnedLOD>        LODs;      // one per level, [0] = the authored vertices
    };

    [[nodiscard]] SkinnedLODModel BuildSkinnedLODModel( const std::vector<SkinnedVertex>&     vertices,
                                                        const std::vector<Index>&             indices,
                                                        const std::vector<Submesh>&           submeshes,
                                                        const std::vector<MeshVertexStreams>& streams,
                                                        const std::vector<MorphTarget>&       morphTargets );
} // namespace Desert
