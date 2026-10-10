#pragma once

#include <Common/Core/ResultStr.hpp>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace Desert::Graphic::API::Vulkan
{
    /// Everything a VkSamplerCreateInfo says about sampling, as a value: two create infos with equal keys make
    /// interchangeable samplers. Floats are compared by bit pattern (a key, not arithmetic). pNext and flags are
    /// not part of it: the cache refuses a create info that carries either (no extension sampler is cached).
    struct SamplerStateKey
    {
        VkFilter             MagFilter         = VK_FILTER_NEAREST;
        VkFilter             MinFilter         = VK_FILTER_NEAREST;
        VkSamplerMipmapMode  MipMode           = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        VkSamplerAddressMode AddressU          = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkSamplerAddressMode AddressV          = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkSamplerAddressMode AddressW          = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        uint32_t             MipLodBiasBits    = 0;
        VkBool32             AnisotropyEnable  = VK_FALSE;
        uint32_t             MaxAnisotropyBits = 0;
        VkBool32             CompareEnable     = VK_FALSE;
        VkCompareOp          CompareOp         = VK_COMPARE_OP_NEVER;
        uint32_t             MinLodBits        = 0;
        uint32_t             MaxLodBits        = 0;
        VkBorderColor        BorderColor       = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        VkBool32             Unnormalized      = VK_FALSE;

        [[nodiscard]] static SamplerStateKey Of( const VkSamplerCreateInfo& info );
        [[nodiscard]] bool                   operator==( const SamplerStateKey& ) const = default;
    };

    struct SamplerStateKeyHash
    {
        [[nodiscard]] std::size_t operator()( const SamplerStateKey& key ) const;
    };

    /// THE ONE MAKER OF VkSampler IN THE ENGINE (UE: FVulkanDevice::SamplerMap behind RHICreateSamplerState —
    /// one object per unique FSamplerStateInitializerRHI). A sampler is an immutable state object, so it is shared
    /// by every texture, material slot and graph pass that samples the same way, and it lives as long as the
    /// cache: nothing that holds one destroys it, a descriptor may point at it for as long as the device lives,
    /// and a texture-filter change mints the new state instead of recreating one in flight. The number of
    /// samplers is the number of distinct states (a handful), never the number of textures — Bistro's 1296 meshes
    /// had run one-sampler-per-image into maxSamplerAllocationCount (1024).
    ///
    /// The device owns one (VulkanLogicalDevice::GetSamplerCache) and releases it before vkDestroyDevice. The
    /// create/destroy calls are injectable so the dedup relation is testable without a device.
    class VulkanSamplerCache
    {
    public:
        using CreateFn  = std::function<VkResult( const VkSamplerCreateInfo&, VkSampler& )>;
        using DestroyFn = std::function<void( VkSampler )>;

        explicit VulkanSamplerCache( VkDevice device );
        VulkanSamplerCache( CreateFn create, DestroyFn destroy );
        ~VulkanSamplerCache();
        VulkanSamplerCache( const VulkanSamplerCache& )            = delete;
        VulkanSamplerCache& operator=( const VulkanSamplerCache& ) = delete;

        /// The sampler for @p info's state: made on its first request, the same handle for every later one.
        /// Refuses a create info with pNext or flags (their state is not in the key) and a failed vkCreateSampler.
        [[nodiscard]] Common::ResultStr<VkSampler> Acquire( const VkSamplerCreateInfo& info );

        /// Destroys every sampler. The device's teardown, after the last frame; the cache is empty and usable
        /// after.
        void Release();

        [[nodiscard]] std::size_t Size() const;

    private:
        CreateFn                                                            m_Create;
        DestroyFn                                                           m_Destroy;
        mutable std::mutex                                                  m_Mutex;
        std::unordered_map<SamplerStateKey, VkSampler, SamplerStateKeyHash> m_Samplers;
    };

    /// The engine device's sampler cache (EngineContext's VulkanLogicalDevice).
    [[nodiscard]] VulkanSamplerCache& EngineSamplerCache();

} // namespace Desert::Graphic::API::Vulkan
