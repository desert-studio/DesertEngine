#pragma once

// SPIR-V reflection, separated from VulkanShader so it can run with NO VkDevice.
//
// The separation is not cosmetic. Image resources used to be sorted into descriptor buckets by a
// substring of their VARIABLE NAME ("Env"/"Cube" meant a cube, anything else meant 2D), so a
// `sampler3D` was registered as a 2D combined image sampler and a `writeonly image3D` as a 2D storage
// image. Neither fails: the descriptor type is right, only the view type is wrong, so the shader
// simply reads garbage. That class of bug is invisible without a device — unless the classification
// itself can be exercised without one, which is what this translation unit exists for. Everything
// here is a free function over a SPIR-V binary; the unit test in Desert/Tests/Engine/ShaderReflection
// compiles GLSL, runs ReflectStage and asserts the buckets, on a machine with no Vulkan at all.

#include <Engine/Core/Formats/MaterialLayout.hpp>
#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShaderResource.hpp>

#include <spirv_cross/spirv_glsl.hpp>

#include <string>
#include <vector>

namespace Desert::Graphic::API::Vulkan::ShaderReflection
{
    // The image bindings this engine can actually build a descriptor for. Anything a shader can
    // declare but the engine cannot bind (1D, arrayed, multisampled, buffer, subpass input, or an
    // array OF descriptors) is Unsupported — reported by name, never guessed into a bucket.
    enum class ImageKind
    {
        Image2D,
        Image3D,
        ImageCube,
        Unsupported
    };

    // Pure. `type` is the type of a `sampled_images` / `storage_images` resource — pass the type of
    // resource.base_type_id, which is the image type itself even when the resource is an array.
    ImageKind ClassifyImage( const spirv_cross::SPIRType& type );

    // The GLSL spelling of what SPIR-V actually declared ("sampler3D", "image2DArray", ...), so a
    // rejection names the thing the author wrote instead of a numeric dimension.
    std::string DescribeImageType( const spirv_cross::SPIRType& type );

    // Reflects ONE stage's SPIR-V into `data`, merging with what earlier stages already contributed
    // (a resource shared by two stages keeps one entry whose ShaderStage mask gains this stage).
    //
    // Returns one message per resource it REFUSED to register, each naming the resource and its type.
    // A refused resource gets no descriptor-layout binding, so the pipeline it belongs to is invalid
    // by construction — which is the point: the caller reports it and fails, rather than binding
    // something of the wrong shape and rendering nonsense.
    [[nodiscard]] std::vector<std::string> ReflectStage( const std::vector<uint32_t>&    spirv,
                                                         Core::Formats::ShaderStage      stage,
                                                         ShaderResource::ReflectionData& data );

    // What ONE stage's SPIR-V says about the material layout: the `Materials` row (binding, stride and
    // every member of one row, padding included), the combined image samplers (name -> binding) and the
    // push block (size and every member). Pure. Core::Formats::ReconcileMaterialLayout holds these to the
    // template's layout and to each other — the witnesses behind Core/Formats/MaterialLayout.hpp.
    Core::Formats::ReflectedMaterialStage ReflectMaterialStage( const std::vector<uint32_t>& spirv,
                                                                Core::Formats::ShaderStage   stage );

    /**
     * The descriptor-set layout bindings one reflected set turns into, sorted by binding number.
     *
     * Pure, and deliberately here rather than inside VulkanShader: this is the SHAPE a pipeline layout
     * and a descriptor set have to agree on, and when they disagree the validation layer reports it as
     * a count ("has 8 total descriptors, but ... has 9"). A device-free function is one a test can
     * evaluate for a real shader on a machine with no Vulkan, which is the only way that count is
     * checkable here at all.
     */
    std::vector<VkDescriptorSetLayoutBinding>
    BuildLayoutBindings( const ShaderResource::ShaderDescriptorSet& set );

    /** Total descriptors across @p bindings — the number the validation layer compares. */
    uint32_t CountDescriptors( const std::vector<VkDescriptorSetLayoutBinding>& bindings );

} // namespace Desert::Graphic::API::Vulkan::ShaderReflection
