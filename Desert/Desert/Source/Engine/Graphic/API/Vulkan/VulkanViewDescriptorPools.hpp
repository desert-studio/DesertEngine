#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDescriptorSetLayout.hpp>
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
        ViewPoolBlock( VkDescriptorPool pool, DescriptorBudget budget )
             : m_Pool( pool ), m_Budget( std::move( budget ) )
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

        /// What is left of the sizes this pool was created with. The chain reserves out of it before it asks
        /// the driver for anything, so a full block is never allocated from -- see DescriptorBudget.
        [[nodiscard]] DescriptorBudget& Budget() noexcept
        {
            return m_Budget;
        }

    private:
        VkDescriptorPool m_Pool = VK_NULL_HANDLE;
        DescriptorBudget m_Budget;
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
     * @brief What allocating one set per layout costs a pool: the set count and the descriptors by type, read
     * off the layouts' own bindings.
     *
     * FROM THE LAYOUTS, NOT FROM THE SHADER'S CURRENT REFLECTION, for the reason
     * VulkanDescriptorSetLayout::Bindings() was kept in the first place: after a recompile the two describe
     * different shaders, and a pool sized against the wrong one is short by exactly the bindings that changed.
     */
    [[nodiscard]] Graphic::DescriptorRequest
    DescriptorCostOf( const std::vector<DescriptorSetLayoutRef>& layouts );

    /**
     * @brief Allocates one set per layout from the ACTIVE view's pool chain (made on first use and kept in
     * the view, so it closes with it). @p shaderName only names a failure.
     *
     * TAKES THE LAYOUT REFS, NOT THE RAW HANDLES: the block a set comes from is chosen by counting what the
     * set will consume, and only the layout knows its own bindings. A handle answers nothing about its cost.
     */
    [[nodiscard]] Common::BoolResultStr AllocateViewSets( const std::vector<DescriptorSetLayoutRef>& layouts,
                                                          std::string_view                           shaderName,
                                                          std::unique_ptr<IViewDescriptorSetCopy>&   out );
} // namespace Desert::Graphic::API::Vulkan
