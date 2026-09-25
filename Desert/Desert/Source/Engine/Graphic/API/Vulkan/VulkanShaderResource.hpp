#pragma once

#include <vulkan/vulkan.h>

#include <Engine/Graphic/RendererTypes.hpp>
#include <Engine/ShaderResources/ShaderReflectionTypes.hpp>

#include <optional>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    namespace ShaderResource
    {

        inline constexpr uint32_t MAX_SETS = 1;

        // One reflected descriptor set. A resource lands in exactly ONE bucket, chosen from its
        // declared SPIR-V type (see VulkanShaderReflection.hpp) — never from its name.
        //
        // Why sampled images are split three ways but storage images only two: a sampled binding is
        // seeded with a FALLBACK image before anything writes it (VulkanMaterialBackend::
        // WriteFallbacks), and that fallback must have the matching view type, so 2D, 3D and
        // cube each need their own bucket. A storage binding is written from a mip VIEW the caller
        // supplies (VulkanPipelineCompute::RecordDescriptorsAndDispatch), which already carries its
        // own view type — so nothing downstream needs a storage 2D told apart from a storage cube,
        // and keeping them together leaves the existing IBL compute chain exactly as it was. A volume
        // is split off because the storage path binds 2D and cube images only; a 3D binding has to be
        // recognisable before it can be given a path of its own.
        struct ShaderDescriptorSet
        {
            using UniformBufferMap =
                 std::unordered_map<BindingPoint, ShaderResources::ShaderLayout::UniformBuffer>;
            using ImageSampler2DMap =
                 std::unordered_map<BindingPoint, ShaderResources::ShaderLayout::Image2DSampler>;
            using ImageSampler3DMap =
                 std::unordered_map<BindingPoint, ShaderResources::ShaderLayout::Image3DSampler>;
            using ImageSamplerCubeMap =
                 std::unordered_map<BindingPoint, ShaderResources::ShaderLayout::ImageCubeSampler>;
            using StorageBufferMap =
                 std::unordered_map<BindingPoint, ShaderResources::ShaderLayout::StorageBuffer>;

            UniformBufferMap    UniformBuffers;
            ImageSampler2DMap   Image2DSamplers;
            ImageSampler3DMap   Image3DSamplers;
            ImageSamplerCubeMap ImageCubeSamplers;
            StorageBufferMap    StorageBuffers;
            ImageSampler2DMap   StorageImage2DSamplers; // storage image2D AND imageCube
            ImageSampler3DMap   StorageImage3DSamplers;

            operator bool()
            {
                return !UniformBuffers.empty();
            }
        };

        // Which fallback descriptor a declared image binding needs before anything real is bound to it.
        enum class FallbackImageKind
        {
            Sampled2D,
            Sampled3D,
            SampledCube,
            Storage2D, // storage image2D AND imageCube — see the note on StorageImage2DSamplers
            Storage3D
        };

        struct FallbackImageBinding
        {
            BindingPoint      Binding;
            FallbackImageKind Kind;
        };

        // Every image binding @p set declares, each tagged with the fallback it needs.
        //
        // VulkanMaterialBackend::WriteFallbacks drives itself off THIS list rather than walking
        // the buckets itself, so the set of bindings that get a fallback is the set that exists — by
        // construction, in one place. It matters because a binding nobody writes is an UNDEFINED
        // descriptor, and the failure is silent: reflection produced a layout entry, the shader samples
        // it, and whatever the descriptor happened to contain is what comes back.
        //
        // Pure over reflection data and free of any Vulkan handle, so it is testable with no device
        // (Desert/Tests/Engine/DescriptorFallbacks) on a machine where the editor cannot even start.
        inline std::vector<FallbackImageBinding> CollectImageBindings( const ShaderDescriptorSet& set )
        {
            std::vector<FallbackImageBinding> bindings;
            bindings.reserve( set.Image2DSamplers.size() + set.Image3DSamplers.size() +
                              set.ImageCubeSamplers.size() + set.StorageImage2DSamplers.size() +
                              set.StorageImage3DSamplers.size() );

            for ( const auto& [binding, resource] : set.Image2DSamplers )
                bindings.push_back( { binding, FallbackImageKind::Sampled2D } );
            for ( const auto& [binding, resource] : set.Image3DSamplers )
                bindings.push_back( { binding, FallbackImageKind::Sampled3D } );
            for ( const auto& [binding, resource] : set.ImageCubeSamplers )
                bindings.push_back( { binding, FallbackImageKind::SampledCube } );
            for ( const auto& [binding, resource] : set.StorageImage2DSamplers )
                bindings.push_back( { binding, FallbackImageKind::Storage2D } );
            for ( const auto& [binding, resource] : set.StorageImage3DSamplers )
                bindings.push_back( { binding, FallbackImageKind::Storage3D } );

            return bindings;
        }

        // Everything reflection extracts from a shader program, merged across its stages. Holds no
        // Vulkan handle, so it can be produced with no device — which is what makes the classification
        // testable off-GPU (Desert/Tests/Engine/ShaderReflection).
        struct ReflectionData
        {
            std::unordered_map<SetPoint, ShaderDescriptorSet> ShaderDescriptorSets; // SetPoint = set

            std::optional<ShaderResources::ShaderLayout::PushConstantRange> PushConstantRanges;
        };

    } // namespace ShaderResource
} // namespace Desert::Graphic::API::Vulkan
