#pragma once

#include <cstdint>

namespace Desert::Core::Formats
{
    // Values are the exact VkShaderStageFlagBits (StageToVkStage casts directly).
    enum class ShaderStage
    {
        None           = 0,
        Vertex         = 0x00000001,
        TessControl    = 0x00000002,
        TessEvaluation = 0x00000004,
        Fragment       = 0x00000010,
        Compute        = 0x00000020,
        // The ray tracing pipeline's stages (VK_KHR_ray_tracing_pipeline): a pass of a program (RayGen, Miss) or
        // the hit cell of a surface template (ClosestHit, AnyHit, DShaderParser kSurfaceRayHitPath).
        RayGen     = 0x00000100,
        AnyHit     = 0x00000200,
        ClosestHit = 0x00000400,
        Miss       = 0x00000800
    };

    // Whether @p stage runs in a ray tracing pipeline: it is compiled with GL_EXT_ray_tracing and needs a device
    // with VK_KHR_ray_tracing_pipeline.
    constexpr bool IsRayTracingStage( ShaderStage stage )
    {
        return stage == ShaderStage::RayGen || stage == ShaderStage::AnyHit || stage == ShaderStage::ClosestHit ||
               stage == ShaderStage::Miss;
    }

    enum class ShaderPragmas
    {
        None = 0,
    };

    enum class ShaderValueType : uint8_t
    {
        Unknown = 0,

        Float,
        Float2,
        Float3,
        Float4,

        Int,
        Int2,
        Int3,
        Int4,

        UInt,
        Bool,

        Mat3,
        Mat4,

        Struct
    };

    enum class ShaderResourceType
    {
        Sampler1D,
        Sampler2D,
        Sampler3D,
        SamplerCube
    };

    enum class BufferKind : uint8_t
    {
        Uniform,
        Storage,
        PushConstant
    };

} // namespace Desert::Core::Formats