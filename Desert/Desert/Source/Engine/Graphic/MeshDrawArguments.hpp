#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Geometry/Mesh.hpp>

#include <cstdint>
#include <format>
#include <vector>

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
    [[nodiscard]] inline Common::ResultStr<uint32_t>
    AppendMeshDrawArguments( const Mesh& mesh, const uint64_t hiddenSubmeshMask, const uint32_t lodLevel,
                             const uint32_t firstInstance, std::vector<uint32_t>& words )
    {
        const auto& indexBuffer = mesh.GetIndexBuffer();
        const auto& submeshes   = mesh.GetSubmeshes();
        uint32_t    records     = 0;
        for ( size_t si = 0; si < submeshes.size(); ++si )
        {
            if ( si < 64 && ( ( hiddenSubmeshMask >> si ) & 1ull ) != 0 )
                continue;
            const Submesh& submesh = submeshes[si];
            if ( !indexBuffer )
            {
                words.insert( words.end(), { submesh.VertexCount, 0u, submesh.VertexOffset, firstInstance, 0u } );
                ++records;
                continue;
            }
            const SubmeshIndexRange range = SubmeshIndices( submesh, lodLevel );
            if ( range.First + range.Count > indexBuffer->GetCount() )
                return Common::MakeError<uint32_t>(
                     std::format( "submesh {} reads indices [{}, {}) of a {}-index buffer", si, range.First,
                                  range.First + range.Count, indexBuffer->GetCount() ) );
            words.insert( words.end(), { range.Count, 0u, range.First, submesh.VertexOffset, firstInstance } );
            ++records;
        }
        return Common::MakeSuccess( records );
    }
} // namespace Desert::Graphic
