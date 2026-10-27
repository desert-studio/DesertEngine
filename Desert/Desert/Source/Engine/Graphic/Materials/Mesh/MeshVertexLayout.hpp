#pragma once

#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/VertexBuffer.hpp>

#include <algorithm>
#include <cstdint>
#include <span>

namespace Desert::Graphic
{
    // THE ONE HOME OF A MESH DRAW'S VERTEX INPUT — UE's FVertexFactory declaration: which attributes a vertex
    // path fetches, at which locations. Every mesh pipeline MeshRenderer builds (forward, G-buffer, glass,
    // shadow, silhouette, overdraw, the generic material cells) takes its layout from here, so the C++ side
    // and every mesh vertex stage's inputs are ONE list: ShaderProtocols/VertexInputs.hpp generates both this
    // layout and the `in` declarations the stages include (Mesh/MeshVertexInputs*.glslh).
    //
    //   binding 0 — the mesh's own vertices: position, normal, tangent, bitangent, UV0 at 0..4, and for the
    //               skinned path bone indices / weights at 5..6.
    //   binding 1 — the optional streams (Geometry/MeshTypes.hpp MeshVertexStreams): vertex colour (RGBA8
    //               UNORM) at 7, UV1 at 8. ONE pipeline, always at the streams' stride: a mesh without them
    //               binds the shared default buffer (white, UV1 0,0 in every vertex; DefaultVertexStreamsFor)
    //               — defined format semantics, as UE's GNullColorVertexBuffer, not a fallback. No stride-0
    //               variant: portability devices refuse it (vertexAttributeAccessBeyondStride = false).
    using ShaderProtocols::kMeshVertexStreamFirstLocation;

    /// The fields of a mesh path's vertex format — the same fields the shader's inputs are generated from
    /// (ShaderProtocols/VertexInputs.hpp; Mesh/MeshVertexInputs.glslh, Mesh/MeshVertexInputsSkinned.glslh).
    [[nodiscard]] inline std::span<const ShaderProtocols::VertexInputField> MeshVertexInputs( const MeshVertexPath path )
    {
        if ( path == MeshVertexPath::Skinned )
            return ShaderProtocols::kMeshSkinnedVertexInputs;
        return ShaderProtocols::kMeshVertexInputs;
    }

    inline VertexBufferLayout MeshVertexLayout( const MeshVertexPath path )
    {
        return VertexBufferLayout::FromVertexInputs( MeshVertexInputs( path ) );
    }

    // THE SHARED DEFAULT STREAMS BUFFER a mesh without its own streams binds at binding 1. It is read per vertex
    // at the layout's stream stride like any mesh's own, so it must hold at least as many stream vertices as the
    // mesh has vertices (its vertex buffer at binding 0 over the layout's stride). One owner
    // (VulkanRendererAPI::RenderMesh) grows it by doubling to the largest mesh drawn so far; it never shrinks.
    struct DefaultVertexStreamsBinding
    {
        uint32_t Capacity = 0; // stream vertices the buffer must hold
        uint32_t Stride   = 0; // the binding's stride — the layout's, never 0
    };
    inline constexpr uint32_t kDefaultVertexStreamsMinCapacity = 4096;

    inline DefaultVertexStreamsBinding DefaultVertexStreamsFor( const VertexBufferLayout& layout,
                                                                const uint64_t            meshVertexBytes,
                                                                const uint32_t            currentCapacity )
    {
        const uint64_t vertices =
             layout.GetStride() == 0 ? 0 : meshVertexBytes / static_cast<uint64_t>( layout.GetStride() );
        uint64_t capacity = std::max( currentCapacity, kDefaultVertexStreamsMinCapacity );
        while ( capacity < vertices )
            capacity *= 2;
        return { static_cast<uint32_t>( capacity ), layout.GetStreamStride() };
    }
} // namespace Desert::Graphic
