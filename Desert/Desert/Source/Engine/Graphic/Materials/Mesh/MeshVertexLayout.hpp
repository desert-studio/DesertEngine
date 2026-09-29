#pragma once

#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/VertexBuffer.hpp>

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
    //               UNORM) at 7, UV1 at 8. A mesh without them draws through the stride-0 twin of the same
    //               pipeline against one shared white / (0,0) vertex — defined format semantics, as UE's
    //               GNullColorVertexBuffer, not a fallback.
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
} // namespace Desert::Graphic
