#pragma once

// The type of one vertex attribute, as the C++ vertex layout (VertexBuffer.hpp VertexBufferLayout) and the vertex
// inputs generated from it (ShaderProtocols/VertexInputs.hpp) both spell it. Its own header so the shader-side
// generator does not pull in the buffer classes.
namespace Desert::Graphic
{
    enum class ShaderDataType
    {
        None = 0,
        Float,
        Float2,
        Float3,
        Float4,
        Int,
        Int2,
        Int3,
        Int4,
        Bool,
        // Four 8-bit channels read as 0..1 floats (VK_FORMAT_R8G8B8A8_UNORM): the vertex colour stream, as
        // UE's FColor vertex colours.
        UNorm8x4
    };
} // namespace Desert::Graphic
