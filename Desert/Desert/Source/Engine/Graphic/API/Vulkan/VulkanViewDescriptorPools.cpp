#include <Engine/Graphic/API/Vulkan/VulkanViewDescriptorPools.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>

#include <Engine/Core/EngineContext.hpp>

#include <array>
#include <string>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        VulkanAllocator* Allocator()
        {
            return SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                 ->GetVulkanAllocator()
                 .get();
        }

        VkDevice Device()
        {
            return SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                 ->GetVulkanLogicalDevice();
        }

        // One block's capacity. A view has no upper bound on the materials it draws, so the chain grows by
        // blocks of this size instead of any one pool guessing the total; a set is a handful of descriptors,
        // so a block is a few tens of kilobytes of driver memory.
        constexpr uint32_t                            kSetsPerBlock = 128;
        constexpr std::array<VkDescriptorPoolSize, 4> kBlockSizes{ {
             { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256 },
             { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 },
             { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 },
             { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256 },
        } };

        class ViewPoolChain final : public IViewResourceCopy
        {
        public:
            DescriptorPoolChain<ViewPoolBlock> Chain;
        };

        class ViewPoolChainFactory final : public IViewResourceCopyFactory
        {
        public:
            std::unique_ptr<IViewResourceCopy> CreateViewCopy( std::string_view /*viewName*/,
                                                               uint32_t /*frameIndex*/ ) override
            {
                return std::make_unique<ViewPoolChain>();
            }
        };

        // Every view keeps ONE chain for all its frames in flight and all its materials; it lives in frame 0.
        ViewResourceKey ChainKey()
        {
            static const ViewResourceKey key = ViewResourceKey::Allocate();
            return key;
        }

        Common::BoolResultStr CreateBlock( const std::string& viewName, const uint32_t ordinal,
                                           std::shared_ptr<ViewPoolBlock>& out )
        {
            VkDescriptorPoolCreateInfo info{};
            info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            info.maxSets       = kSetsPerBlock;
            info.poolSizeCount = static_cast<uint32_t>( kBlockSizes.size() );
            info.pPoolSizes    = kBlockSizes.data();

            VkDescriptorPool pool   = VK_NULL_HANDLE;
            const VkResult   result = vkCreateDescriptorPool( Device(), &info, nullptr, &pool );
            if ( result != VK_SUCCESS )
                return Common::MakeFormattedError<bool>( "view '{}': descriptor pool #{} could not be created: {}",
                                                         viewName, ordinal, VkResultToString( result ) );
            out = std::make_shared<ViewPoolBlock>( pool );
            return Common::MakeSuccess( true );
        }
    } // namespace

    ViewPoolBlock::~ViewPoolBlock()
    {
        Allocator()->RT_DestroyDescriptorPool( m_Pool );
    }

    VulkanViewSets::~VulkanViewSets()
    {
        if ( m_Pool )
            Allocator()->RT_FreeDescriptorSets( m_Pool->Get(), std::move( m_Sets ) );
    }

    Common::BoolResultStr AllocateViewSets( const std::vector<VkDescriptorSetLayout>& layouts,
                                            const std::string_view                    shaderName,
                                            std::unique_ptr<IViewDescriptorSetCopy>&  out )
    {
        ViewResources&       view = ViewResourceRegistry::Active();
        ViewPoolChainFactory factory;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): ChainKey names this exact type
        auto& chain = static_cast<ViewPoolChain&>( view.Acquire( ChainKey(), 0, factory ) );

        std::vector<VkDescriptorSet> sets( layouts.size(), VK_NULL_HANDLE );
        const auto                   tryAllocate = [&layouts, &sets]( ViewPoolBlock& block, std::string& why )
        {
            VkDescriptorSetAllocateInfo info{};
            info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            info.descriptorPool     = block.Get();
            info.descriptorSetCount = static_cast<uint32_t>( layouts.size() );
            info.pSetLayouts        = layouts.data();
            const VkResult result   = vkAllocateDescriptorSets( Device(), &info, sets.data() );
            if ( result == VK_SUCCESS )
                return PoolAllocation::Allocated;
            why = VkResultToString( result );
            if ( result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL )
                return PoolAllocation::PoolExhausted;
            return PoolAllocation::Failed;
        };
        const std::string viewName( view.GetName() );
        const auto        create = [&viewName]( const uint32_t ordinal, std::shared_ptr<ViewPoolBlock>& block )
        { return CreateBlock( viewName, ordinal, block ); };

        std::shared_ptr<ViewPoolBlock> from;
        auto                           allocated = chain.Chain.Allocate( create, tryAllocate, from );
        if ( !allocated.IsSuccess() )
            return Common::MakeFormattedError<bool>(
                 "shader '{}', view '{}': {} descriptor set(s) not allocated -- {}", shaderName, viewName,
                 layouts.size(), allocated.GetError() );
        out = std::make_unique<VulkanViewSets>( std::move( sets ), std::move( from ) );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::API::Vulkan
