#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/ViewDescriptorSets.hpp>

#include <vulkan/vulkan.h>

#include <memory>
#include <string_view>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    /**
     * @brief One fixed-size descriptor pool of a view's chain. Destroyed through the allocator's deferred
     * queue, so the last set that keeps it alive may be released at any point of a frame.
     */
    class ViewPoolBlock
    {
    public:
        explicit ViewPoolBlock( VkDescriptorPool pool ) : m_Pool( pool )
        {
        }
        ~ViewPoolBlock();

        ViewPoolBlock( const ViewPoolBlock& )            = delete;
        ViewPoolBlock& operator=( const ViewPoolBlock& ) = delete;
        ViewPoolBlock( ViewPoolBlock&& )                 = delete;
        ViewPoolBlock& operator=( ViewPoolBlock&& )      = delete;

        [[nodiscard]] VkDescriptorPool Get() const noexcept
        {
            return m_Pool;
        }

    private:
        VkDescriptorPool m_Pool = VK_NULL_HANDLE;
    };

    /**
     * @brief One material's descriptor sets in one view for one frame in flight.
     *
     * Holds its pool alive: the deferred free of these sets is queued before the pool's own destruction can
     * be, whichever of the view and the material goes first.
     */
    class VulkanViewSets final : public IViewDescriptorSetCopy
    {
    public:
        VulkanViewSets( std::vector<VkDescriptorSet> sets, std::shared_ptr<ViewPoolBlock> pool )
             : m_Sets( std::move( sets ) ), m_Pool( std::move( pool ) )
        {
        }
        ~VulkanViewSets() override;

        VulkanViewSets( const VulkanViewSets& )            = delete;
        VulkanViewSets& operator=( const VulkanViewSets& ) = delete;
        VulkanViewSets( VulkanViewSets&& )                 = delete;
        VulkanViewSets& operator=( VulkanViewSets&& )      = delete;

        [[nodiscard]] const std::vector<VkDescriptorSet>& GetSets() const noexcept
        {
            return m_Sets;
        }

    private:
        std::vector<VkDescriptorSet>   m_Sets;
        std::shared_ptr<ViewPoolBlock> m_Pool;
    };

    /**
     * @brief Allocates one set per layout from the ACTIVE view's pool chain (made on first use and kept in
     * the view, so it closes with it). @p shaderName only names a failure.
     */
    [[nodiscard]] Common::BoolResultStr AllocateViewSets( const std::vector<VkDescriptorSetLayout>& layouts,
                                                          std::string_view                          shaderName,
                                                          std::unique_ptr<IViewDescriptorSetCopy>&  out );
} // namespace Desert::Graphic::API::Vulkan
