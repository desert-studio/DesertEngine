#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Geometry/MeshTypes.hpp>

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>

namespace Desert
{
    class Mesh;
}

// THE INDIRECT ARGUMENTS OF A MESH DRAW (RendererAPI::RenderMeshIndirect): one record per submesh the draw draws,
// in RenderMesh's walk order (the hidden-submesh mask skips, the LOD picks the index range). A record is a
// VkDrawIndexedIndirectCommand; a mesh without indices reads a VkDrawIndirectCommand from its first four words.
// Both keep the instance count in word 1, the one word a GPU cull writes (UE: the instance count of a
// FMeshDrawCommand's indirect arguments, CullPerPageDrawCommands).
namespace Desert::Graphic
{
    inline constexpr uint32_t kMeshDrawArgumentWords  = 5;
    inline constexpr uint64_t kMeshDrawArgumentsBytes = kMeshDrawArgumentWords * sizeof( uint32_t );
    inline constexpr uint32_t kMeshDrawInstanceWord   = 1;
    // Which record a submesh of the walk reads: its place among the DRAWN submeshes (the records a GPU cull made
    // per draw, AppendMeshDrawArguments) or its index in the MESH (a GPU-instanced draw's records,
    // GpuInstances.hpp: one per submesh of the mesh, whatever the hidden-submesh mask skips).
    enum class MeshDrawArgumentsOrder : uint8_t
    {
        Drawn,
        Submesh,
    };

    // The submeshes a mesh draw hides, for a mesh of ANY submesh count: bit i of Low = submesh i (i < 64), bit
    // (i - 64) % 64 of High[(i - 64) / 64] = submesh i past it; a word past High hides nothing. High is a view of
    // words its producer keeps alive until the draw is recorded (a cascade's or VSM level's clustered submeshes,
    // MeshRenderer::ClusterWalkHidden: interned per frame, so equal masks share one view). Built implicitly from a
    // 64-bit mask, which every producer of a mesh with at most 64 submeshes still is.
    struct SubmeshMask
    {
        uint64_t                  Low = 0;
        std::span<const uint64_t> High{};

        constexpr SubmeshMask() = default;
        constexpr SubmeshMask( const uint64_t low, const std::span<const uint64_t> high = {} ) // NOLINT: implicit
             : Low( low ), High( high )
        {
        }

        [[nodiscard]] constexpr bool Hides( const std::size_t submesh ) const
        {
            if ( submesh < 64u )
                return ( ( Low >> submesh ) & 1ull ) != 0u;
            const std::size_t word = ( submesh - 64u ) / 64u;
            return word < High.size() && ( ( High[word] >> ( ( submesh - 64u ) % 64u ) ) & 1ull ) != 0u;
        }
        // Equal masks of one producer share their High view (interned), so the view's address names its words.
        [[nodiscard]] constexpr bool operator==( const SubmeshMask& other ) const
        {
            return Low == other.Low && High.data() == other.High.data() && High.size() == other.High.size();
        }
    };

    struct SubmeshIndexRange
    {
        uint32_t First = 0;
        uint32_t Count = 0;
    };

    // The indices submesh @p submesh draws at LOD @p lodLevel: its own range without LODs, else the level's,
    // clamped to the coarsest it has.
    [[nodiscard]] inline SubmeshIndexRange SubmeshIndices( const Submesh& submesh, const uint32_t lodLevel )
    {
        if ( submesh.LODs.empty() )
            return { .First = submesh.IndexOffset, .Count = submesh.IndexCount };
        const auto level =
             lodLevel < submesh.LODs.size() ? lodLevel : static_cast<uint32_t>( submesh.LODs.size() ) - 1;
        return { .First = submesh.LODs[level].IndexOffset, .Count = submesh.LODs[level].IndexCount };
    }

    // Appends to @p words the records of a draw of @p mesh (hidden mask, LOD) whose instances start at
    // @p firstInstance, each with an instance count of 0; returns how many records. Refused when a submesh reads
    // past the index buffer (RenderMesh's check, made here because the GPU never reports it).
    [[nodiscard]] Common::ResultStr<uint32_t> AppendMeshDrawArguments( const Mesh& mesh,
                                                                       SubmeshMask hiddenSubmeshMask,
                                                                       uint32_t lodLevel, uint32_t firstInstance,
                                                                       std::vector<uint32_t>& words );
} // namespace Desert::Graphic
