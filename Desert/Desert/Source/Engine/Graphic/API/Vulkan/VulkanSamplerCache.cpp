#include <Engine/Graphic/API/Vulkan/VulkanSamplerCache.hpp>

#include <bit>
#include <utility>

namespace Desert::Graphic::API::Vulkan
{
    SamplerStateKey SamplerStateKey::Of( const VkSamplerCreateInfo& info )
    {
        SamplerStateKey key;
        key.MagFilter         = info.magFilter;
        key.MinFilter         = info.minFilter;
        key.MipMode           = info.mipmapMode;
        key.AddressU          = info.addressModeU;
        key.AddressV          = info.addressModeV;
        key.AddressW          = info.addressModeW;
        key.MipLodBiasBits    = std::bit_cast<uint32_t>( info.mipLodBias );
        key.AnisotropyEnable  = info.anisotropyEnable;
        key.MaxAnisotropyBits = std::bit_cast<uint32_t>( info.maxAnisotropy );
        key.CompareEnable     = info.compareEnable;
        key.CompareOp         = info.compareOp;
        key.MinLodBits        = std::bit_cast<uint32_t>( info.minLod );
        key.MaxLodBits        = std::bit_cast<uint32_t>( info.maxLod );
        key.BorderColor       = info.borderColor;
        key.Unnormalized      = info.unnormalizedCoordinates;
        return key;
    }

    std::size_t SamplerStateKeyHash::operator()( const SamplerStateKey& key ) const
    {
        // FNV-1a over the fields' values (not the struct's bytes: padding is not part of the state).
        uint64_t   hash = 14695981039346656037ull;
        const auto mix  = [&hash]( uint64_t value )
        {
            hash ^= value;
            hash *= 1099511628211ull;
        };
        mix( static_cast<uint64_t>( key.MagFilter ) );
        mix( static_cast<uint64_t>( key.MinFilter ) );
        mix( static_cast<uint64_t>( key.MipMode ) );
        mix( static_cast<uint64_t>( key.AddressU ) );
        mix( static_cast<uint64_t>( key.AddressV ) );
        mix( static_cast<uint64_t>( key.AddressW ) );
        mix( key.MipLodBiasBits );
        mix( key.AnisotropyEnable );
        mix( key.MaxAnisotropyBits );
        mix( key.CompareEnable );
        mix( static_cast<uint64_t>( key.CompareOp ) );
        mix( key.MinLodBits );
        mix( key.MaxLodBits );
        mix( static_cast<uint64_t>( key.BorderColor ) );
        mix( key.Unnormalized );
        return static_cast<std::size_t>( hash );
    }

    VulkanSamplerCache::VulkanSamplerCache( VkDevice device )
         : VulkanSamplerCache( [device]( const VkSamplerCreateInfo& info, VkSampler& out )
                               { return vkCreateSampler( device, &info, nullptr, &out ); },
                               [device]( VkSampler sampler ) { vkDestroySampler( device, sampler, nullptr ); } )
    {
    }

    VulkanSamplerCache::VulkanSamplerCache( CreateFn create, DestroyFn destroy )
         : m_Create( std::move( create ) ), m_Destroy( std::move( destroy ) )
    {
    }

    VulkanSamplerCache::~VulkanSamplerCache()
    {
        Release();
    }

    Common::ResultStr<VkSampler> VulkanSamplerCache::Acquire( const VkSamplerCreateInfo& info )
    {
        if ( info.sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO )
            return Common::MakeError<VkSampler>(
                 "sampler cache: the create info's sType is not SAMPLER_CREATE_INFO" );
        if ( info.pNext != nullptr || info.flags != 0 )
            return Common::MakeError<VkSampler>( "sampler cache: a create info with pNext or flags is not "
                                                 "cacheable (its state is not in the key)" );

        const SamplerStateKey             key = SamplerStateKey::Of( info );
        const std::lock_guard<std::mutex> lock( m_Mutex );
        if ( const auto found = m_Samplers.find( key ); found != m_Samplers.end() )
            return Common::MakeSuccess( found->second );

        VkSampler      sampler = VK_NULL_HANDLE;
        const VkResult result  = m_Create( info, sampler );
        if ( result != VK_SUCCESS || sampler == VK_NULL_HANDLE )
            return Common::MakeFormattedError<VkSampler>( "vkCreateSampler failed ({}) with {} samplers cached",
                                                          static_cast<int>( result ), m_Samplers.size() );
        m_Samplers.emplace( key, sampler );
        return Common::MakeSuccess( sampler );
    }

    void VulkanSamplerCache::Release()
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        for ( const auto& [key, sampler] : m_Samplers )
            m_Destroy( sampler );
        m_Samplers.clear();
    }

    std::size_t VulkanSamplerCache::Size() const
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        return m_Samplers.size();
    }
} // namespace Desert::Graphic::API::Vulkan
