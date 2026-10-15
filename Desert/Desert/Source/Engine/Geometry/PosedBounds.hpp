#pragma once

#include <Common/Core/Math/AABB.hpp>

#include <Engine/Geometry/MeshBounds.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <vector>

namespace Desert::Geometry
{
    /// Mesh-local box of a skinned mesh AS IT IS DRAWN: every retained CPU vertex deformed by @p skin (linear
    /// blend, the same weighted sum the skinning shader takes). A skinned submesh's stored BoundingBox is in
    /// RAW-vertex space, which matches the drawn mesh only when the bind is identity; an imported rig with a
    /// scaled or offset bind draws somewhere else entirely. Picking (Scene::Raycast) and the pose thumbnail
    /// both frame what is on screen, so both measure here.
    ///
    /// Templated on the vertex range so a test can feed a stub with the same three fields
    /// (StaticVertex.Position, BoneIDs[], BoneWeights[]) without linking mesh code that needs a GPU.
    struct PosedBox
    {
        glm::vec3 Min{ FLT_MAX };
        glm::vec3 Max{ -FLT_MAX };

        /// False for a mesh with no retained vertices: there is nothing to measure, and no box stands in.
        [[nodiscard]] bool Valid() const
        {
            return Max.x >= Min.x;
        }
    };

    template <typename VertexRange>
    [[nodiscard]] PosedBox MeasurePosedVertices( const VertexRange& vertices, const std::vector<glm::mat4>& skin )
    {
        PosedBox box;
        for ( const auto& sv : vertices )
        {
            glm::vec3 pos( 0.0f );
            float     wsum = 0.0f;
            for ( size_t j = 0; j < std::size( sv.BoneWeights ); ++j )
            {
                const float w = sv.BoneWeights[j];
                if ( w <= 0.0f )
                    continue;
                const auto b = static_cast<size_t>( sv.BoneIDs[j] );
                if ( b < skin.size() )
                    pos += w * glm::vec3( skin[b] * glm::vec4( sv.StaticVertex.Position, 1.0f ) );
                wsum += w;
            }
            if ( wsum > 1e-5f )
                pos /= wsum; // weighted average (robust to weights that don't sum to exactly 1)
            else
                pos = sv.StaticVertex.Position;
            box.Min = glm::min( box.Min, pos );
            box.Max = glm::max( box.Max, pos );
        }
        return box;
    }

    /// THE POSED BOX OF A SKINNED MESH WITHOUT A PASS OVER ITS VERTICES PER FRAME, for culling it in every view
    /// (UE: USkinnedMeshComponent::CalcBounds unions the physics asset's bodies carried by the current bone
    /// transforms, times BoundsScale). Here the "body" of a bone is the box of the very vertices it moves, so the
    /// union is exact-conservative for linear blend skinning and needs no hand-made physics asset, and no
    /// BoundsScale to cover what bodies miss: a drawn vertex is a weighted average of its bones' matrices applied
    /// to (position + sum of weight * morph delta), each term lies in its bone's box widened by the weighted
    /// delta boxes and carried by that bone's matrix, and an average of points lies in the box of their union.
    struct SkinnedBoneBounds
    {
        // The slot of the vertices no bone moves: drawn where they are (MeasurePosedVertices' rule).
        static constexpr uint32_t kUnskinned = std::numeric_limits<uint32_t>::max();

        std::vector<uint32_t>           Bones; // per slot: the bone whose vertices it holds, or kUnskinned
        std::vector<Common::Math::AABB> Boxes; // per slot: the box of those vertices in the raw vertex space
        uint32_t                        MorphTargetCount = 0;
        // [slot * MorphTargetCount + k]: the box of morph target k's position deltas over the slot's vertices.
        std::vector<Common::Math::AABB> MorphDeltas;
    };

    namespace PosedBoundsDetail
    {
        [[nodiscard]] inline Common::Math::AABB EmptyBox()
        {
            return { glm::vec3( kNoBoundsSentinel ), glm::vec3( -kNoBoundsSentinel ) };
        }

        inline void Grow( Common::Math::AABB& box, const glm::vec3& point )
        {
            box.Min = glm::min( box.Min, point );
            box.Max = glm::max( box.Max, point );
        }
    } // namespace PosedBoundsDetail

    /// Built once per mesh from its CPU vertices and the asset's morph targets (index-aligned with the vertices;
    /// a target shorter than the mesh leaves the rest unmoved, SkinnedMesh's rule).
    template <typename VertexRange, typename MorphRange>
    [[nodiscard]] SkinnedBoneBounds BuildSkinnedBoneBounds( const VertexRange& vertices, const MorphRange& morphs )
    {
        using PosedBoundsDetail::EmptyBox;
        using PosedBoundsDetail::Grow;
        SkinnedBoneBounds bounds;
        bounds.MorphTargetCount = static_cast<uint32_t>( std::size( morphs ) );
        std::vector<uint32_t> slotOf; // bone -> slot + 1 (0: no slot yet)
        uint32_t              unskinnedSlot = 0; // slot + 1
        const auto            slotFor       = [&]( const uint32_t bone ) -> uint32_t
        {
            uint32_t* known = &unskinnedSlot;
            if ( bone != SkinnedBoneBounds::kUnskinned )
            {
                if ( bone >= slotOf.size() )
                    slotOf.resize( static_cast<std::size_t>( bone ) + 1, 0u );
                known = &slotOf[bone];
            }
            if ( *known == 0 )
            {
                bounds.Bones.push_back( bone );
                bounds.Boxes.push_back( EmptyBox() );
                bounds.MorphDeltas.resize( bounds.MorphDeltas.size() + bounds.MorphTargetCount, EmptyBox() );
                *known = static_cast<uint32_t>( bounds.Bones.size() );
            }
            return *known - 1;
        };
        const auto add = [&]( const uint32_t slot, const std::size_t vertex, const glm::vec3& position )
        {
            Grow( bounds.Boxes[slot], position );
            std::size_t k = 0;
            for ( const auto& target : morphs )
            {
                const glm::vec3 delta =
                     vertex < target.DeltaPositions.size() ? target.DeltaPositions[vertex] : glm::vec3( 0.0f );
                Grow( bounds.MorphDeltas[static_cast<std::size_t>( slot ) * bounds.MorphTargetCount + k], delta );
                ++k;
            }
        };
        std::size_t index = 0;
        for ( const auto& sv : vertices )
        {
            bool skinned = false;
            for ( std::size_t j = 0; j < std::size( sv.BoneWeights ); ++j )
            {
                if ( sv.BoneWeights[j] <= 0.0f )
                    continue;
                add( slotFor( sv.BoneIDs[j] ), index, sv.StaticVertex.Position );
                skinned = true;
            }
            if ( !skinned )
                add( slotFor( SkinnedBoneBounds::kUnskinned ), index, sv.StaticVertex.Position );
            ++index;
        }
        return bounds;
    }

    /// The WORLD box of the mesh drawn in pose @p skin (the skinning matrices, MeshRenderData::BoneMatrices)
    /// with morph @p morphWeights (empty: unmorphed) at @p world. Empty — "no known extent", drawn in every
    /// view — when the mesh has no vertices or a bone it needs is missing from the pose.
    [[nodiscard]] inline Common::Math::AABB PosedWorldBounds( const SkinnedBoneBounds&      bounds,
                                                              const std::vector<glm::mat4>& skin,
                                                              std::span<const float>        morphWeights,
                                                              const glm::mat4&              world )
    {
        Common::Math::AABB out           = PosedBoundsDetail::EmptyBox();
        const std::size_t  activeTargets = std::min<std::size_t>( morphWeights.size(), bounds.MorphTargetCount );
        for ( std::size_t slot = 0; slot < bounds.Bones.size(); ++slot )
        {
            const uint32_t bone = bounds.Bones[slot];
            if ( bone != SkinnedBoneBounds::kUnskinned && bone >= skin.size() )
                return PosedBoundsDetail::EmptyBox();
            Common::Math::AABB local = bounds.Boxes[slot];
            for ( std::size_t k = 0; k < activeTargets; ++k )
            {
                const float w = morphWeights[k];
                if ( w == 0.0f )
                    continue;
                const Common::Math::AABB& delta = bounds.MorphDeltas[slot * bounds.MorphTargetCount + k];
                local.Min += glm::min( w * delta.Min, w * delta.Max );
                local.Max += glm::max( w * delta.Min, w * delta.Max );
            }
            const glm::mat4 toWorld = bone == SkinnedBoneBounds::kUnskinned ? world : world * skin[bone];
            const Common::Math::AABB carried = TransformBounds( toWorld, local );
            out.Min = glm::min( out.Min, carried.Min );
            out.Max = glm::max( out.Max, carried.Max );
        }
        return out;
    }
} // namespace Desert::Geometry
