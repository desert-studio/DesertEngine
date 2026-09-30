#pragma once

#include <Engine/ShaderResources/UniformImage2D.hpp>
#include <Common/Core/Memory/Buffer.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>

namespace Desert::ShaderResources::API::Vulkan
{
    class VulkanUniformImage2D : public UniformImage2D
    {
    public:
        VulkanUniformImage2D( const std::string_view debugName, uint32_t binding );
        virtual ~VulkanUniformImage2D();

        virtual uint32_t GetBinding() const override
        {
            return m_Binding;
        }

        const VkDescriptorImageInfo& GetDescriptorImageInfo() const
        {
            return m_DescriptorInfo;
        }

        virtual void SetImage2D( const Graphic::Image2D* image2D, Graphic::RDG::Access declared ) override;

    private:
        // `RT_Invalidate()` and `Release()` stood here with EMPTY BODIES, called from the constructor and
        // the destructor respectively. This class owns no device resource at all — it holds a
        // VkDescriptorImageInfo pointing at an image somebody else created and destroys — so there was
        // never anything for either to do. An empty body "so it links" is contract §1.2, and a `void`
        // resource-creation entry point is also a hole in the GpuWriteCensus rule that every such entry
        // point must be able to refuse. Deleted rather than given a return type: a function with nothing
        // to fail at does not need a channel, it needs to not exist.

    private:
        VkDescriptorImageInfo m_DescriptorInfo{};
        const std::string     m_DebugName;
        uint32_t              m_Binding = 0;
        // `m_Image2D` stood here, and its own comment named its ONLY reader: GetImageHash(). Г12 removed
        // that reader, which left the member written by SetImage2D and read by nobody — so it went with
        // it. What this class actually keeps of an image is m_DescriptorInfo above, copied out at
        // SetImage2D time; holding the pointer as well was a second, weaker handle on something this
        // class does not own.
    };
} // namespace Desert::ShaderResources::API::Vulkan