#pragma once

#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/VertexBuffer.hpp>

#include <algorithm>
#include <cstdint>

namespace Desert::Graphic
{
    // THE ONE HOME OF A MESH DRAW'S VERTEX INPUT — UE's FVertexFactory declaration: which attributes a vertex
    // path fetches, at which locations. Every mesh pipeline MeshRenderer builds (forward, G-buffer, glass,
    // shadow, silhouette, overdraw, the generic material cells) takes its layout from here, so the C++ side
    // and Mesh/Surface/Vertex_*.glslh are one relation, asserted by Desert/Tests/Engine/MeshVertexPath and
    // ShippedShaderPasses.
    //
    //   binding 0 — the mesh's own vertices: position, normal, tangent, bitangent, UV0 at 0..4, and for the
    //               skinned path bone indices / weights at 5..6.
    //   binding 1 — the optional streams (Geometry/MeshTypes.hpp MeshVertexStreams): vertex colour (RGBA8
    //               UNORM) at 7, UV1 at 8. ONE pipeline, always at the streams' stride: a mesh without them
    //               binds the shared default buffer (white, UV1 0,0 in every vertex; DefaultVertexStreamsFor)
    //               — defined format semantics, as UE's GNullColorVertexBuffer, not a fallback. No stride-0
    //               variant: portability devices refuse it (vertexAttributeAccessBeyondStride = false).
    inline constexpr uint32_t kMeshVertexStreamFirstLocation = 7;

    inline VertexBufferLayout MeshVertexLayout( const MeshVertexPath path )
    {
        VertexBufferLayout layout = path == MeshVertexPath::Skinned
                                         ? VertexBufferLayout{ { ShaderDataType::Float3, "a_Position" },
                                                               { ShaderDataType::Float3, "a_Normal" },
                                                               { ShaderDataType::Float3, "a_Tangent" },
                                                               { ShaderDataType::Float3, "a_Bitangent" },
                                                               { ShaderDataType::Float2, "a_TextureCoord" },
                                                               { ShaderDataType::Int4, "a_BoneIndices" },
                                                               { ShaderDataType::Float4, "a_BoneWeights" } }
                                         : VertexBufferLayout{ { ShaderDataType::Float3, "a_Position" },
                                                               { ShaderDataType::Float3, "a_Normal" },
                                                               { ShaderDataType::Float3, "a_Tangent" },
                                                               { ShaderDataType::Float3, "a_Bitangent" },
                                                               { ShaderDataType::Float2, "a_TextureCoord" } };
        layout.WithStreams( kMeshVertexStreamFirstLocation, { { ShaderDataType::UNorm8x4, "a_Color", true },
                                                              { ShaderDataType::Float2, "a_TexCoord1" } } );
        return layout;
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
