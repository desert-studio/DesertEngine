#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// THE ONE SOURCE OF THE BINDLESS TABLE'S SHADER-VISIBLE LAYOUT: its set index, its two binding numbers and the
// size of its fixed sampler array. The shader compiler hands each to every compile as a define
// (kBindlessShaderDefines; Common/BindlessTextures.glslh declares the table with them), and the Vulkan side
// (VulkanBindlessTextureTable, VulkanShader's pipeline layouts, the reflection check, DeviceCaps' sampler limit)
// uses the same constants, so the shader and the pipeline layout cannot disagree. It lives in the ShaderCompiler
// module because RenderCore depends on ShaderCompiler, not the other way round.
namespace Desert::Core
{
    // Shaders use sets 0 and 1 for their own resources; the table is the next one.
    inline constexpr uint32_t kBindlessSetIndex = 2;

    // Binding numbers inside the set. The sampled-image array is the variable-count binding and so must be the
    // highest binding (VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-pBindingFlags-03004); the samplers come
    // first.
    inline constexpr uint32_t kBindlessSamplerBinding = 0;
    inline constexpr uint32_t kBindlessTextureBinding = 1;

    // The fixed sampler set: one sampler per Core::Formats::SamplerState (3 wraps U x 3 wraps V x 2 filters), so
    // every material slot's state has its exact sampler (BindlessSamplerFor in Graphic/BindlessTextureTable.hpp).
    inline constexpr uint32_t kBindlessSamplerWrapCount   = 3; // Core::Formats::SamplerWrap: Repeat, Clamp, Mirror
    inline constexpr uint32_t kBindlessSamplerFilterCount = 2; // Core::Formats::SamplerFilter: Linear, Nearest
    inline constexpr uint32_t kBindlessSamplerStateCount =
         kBindlessSamplerWrapCount * kBindlessSamplerWrapCount * kBindlessSamplerFilterCount;

    struct BindlessShaderDefine
    {
        std::string_view Name;
        uint32_t         Value = 0;
    };

    // Every compile gets all four (ShaderCompiler), and the cache key mixes all four (ShaderCacheKey), so a
    // change to any constant above recompiles every shader instead of serving SPIR-V built against the old one.
    inline constexpr std::array<BindlessShaderDefine, 4> kBindlessShaderDefines{ {
         { "DESERT_BINDLESS_SET", kBindlessSetIndex },
         { "DESERT_BINDLESS_SAMPLER_BINDING", kBindlessSamplerBinding },
         { "DESERT_BINDLESS_TEXTURE_BINDING", kBindlessTextureBinding },
         { "DESERT_BINDLESS_SAMPLER_COUNT", kBindlessSamplerStateCount },
    } };
} // namespace Desert::Core
