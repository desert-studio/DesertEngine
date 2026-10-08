#pragma once

#include <Engine/Core/Formats/Shader.hpp>

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

// TODO: move to Uniforms global dir
namespace Desert::ShaderResources::ShaderLayout
{
    struct ShaderFieldLayout
    {
        Core::Formats::ShaderValueType FieldDataType;
        std::string                    Name;
        uint32_t                       Size;
        uint32_t                       Offset;
        uint32_t                       ArraySize = 1;
    };

    struct UniformBuffer
    {
        uint32_t                        Size         = 0;
        uint32_t                        BindingPoint = 0;
        std::string                     Name;
        std::vector<ShaderFieldLayout>  Fields;
        const Core::Formats::BufferKind BufferKind  = Core::Formats::BufferKind::Uniform;
        Core::Formats::ShaderStage      ShaderStage = Core::Formats::ShaderStage::None;
    };

    struct StorageBuffer
    {
        uint32_t                        Size         = 0;
        uint32_t                        BindingPoint = 0;
        std::string                     Name;
        std::vector<ShaderFieldLayout>  Fields;
        const Core::Formats::BufferKind BufferKind  = Core::Formats::BufferKind::Storage;
        Core::Formats::ShaderStage      ShaderStage = Core::Formats::ShaderStage::None;
    };

    struct Image2DSampler
    {
        uint32_t                   BindingPoint  = 0;
        uint32_t                   DescriptorSet = 0;
        uint32_t                   ArraySize     = 1;
        std::string                Name;
        Core::Formats::ShaderStage ShaderStage = Core::Formats::ShaderStage::Fragment;
    };

    // A `sampler3D` / `image3D` binding. Kept a distinct type from Image2DSampler even though the
    // fields coincide: reflection puts a resource in exactly one bucket, and the bucket is what tells
    // the backend which KIND of image view the binding needs. Sharing one type would make a 3D
    // resource assignable to a 2D slot by accident — the very confusion this type exists to end.
    struct Image3DSampler
    {
        uint32_t                   BindingPoint  = 0;
        uint32_t                   DescriptorSet = 0;
        uint32_t                   ArraySize     = 1;
        std::string                Name;
        Core::Formats::ShaderStage ShaderStage = Core::Formats::ShaderStage::Fragment;
    };

    struct ImageCubeSampler
    {
        uint32_t                   BindingPoint  = 0;
        uint32_t                   DescriptorSet = 0;
        uint32_t                   ArraySize     = 1;
        std::string                Name;
        Core::Formats::ShaderStage ShaderStage = Core::Formats::ShaderStage::Fragment;
    };

    // `uniform accelerationStructureEXT` (GL_EXT_ray_query) — a top-level acceleration structure the
    // shader traces against; bound as VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR.
    struct AccelerationStructure
    {
        uint32_t                   BindingPoint  = 0;
        uint32_t                   DescriptorSet = 0;
        std::string                Name;
        Core::Formats::ShaderStage ShaderStage = Core::Formats::ShaderStage::None;
    };

    struct PushConstantRange
    {
        uint32_t                   Offset = 0;
        uint32_t                   Size   = 0;
        std::string                Name;
        Core::Formats::BufferKind  BufferKind  = Core::Formats::BufferKind::PushConstant;
        Core::Formats::ShaderStage ShaderStage = Core::Formats::ShaderStage::None;
    };

    // THE byte count of a program's push block: what its pipeline layout's range is built with
    // (VulkanPipeline::SetUpPushConstantRange) AND what a material of that program holds and pushes
    // (MaterialExecutor). One function so the two cannot part: a material once held a fixed 128 bytes for
    // every shader, pushed all of them through Shadow's 64-byte range, and the shadow pass never drew.
    inline uint32_t PushBlockSize( const std::optional<PushConstantRange>& range )
    {
        return range.has_value() ? range->Size : 0u;
    }

    // THE engine's cap on a push block, in bytes. 128 is the push-constant size every Vulkan device must
    // support (maxPushConstantsSize minimum), so a program within it runs on any device and no per-device
    // check exists. Shader reflection refuses a stage that declares more (ShaderReflection::ReflectStage),
    // which covers shipped, shader-graph and user shaders alike; the ShaderCacheKey census pins every
    // shipped program under it.
    inline constexpr uint32_t kMaxPushConstantBytes = 128u;

} // namespace Desert::ShaderResources::ShaderLayout