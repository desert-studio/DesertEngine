#pragma once

#include <cstdint>
#include <string_view>

// THE ONE SOURCE OF THE BINDLESS SET INDEX. The shader compiler hands it to every compile as the
// DESERT_BINDLESS_SET define (Common/BindlessTextures.glslh declares the table at that set), and the Vulkan side
// (VulkanBindlessTextureTable.hpp, VulkanShader's pipeline layouts, the passes' set binding) uses the same constant,
// so the shader and the pipeline layout cannot disagree. It lives in the ShaderCompiler module because RenderCore
// depends on ShaderCompiler, not the other way round.
namespace Desert::Core
{
    // Shaders use sets 0 and 1 for their own resources; the table is the next one.
    inline constexpr uint32_t         kBindlessSetIndex  = 2;
    inline constexpr std::string_view kBindlessSetDefine = "DESERT_BINDLESS_SET";
} // namespace Desert::Core
