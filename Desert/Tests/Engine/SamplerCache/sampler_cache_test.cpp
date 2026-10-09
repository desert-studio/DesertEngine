// One VkSampler per sampling STATE, never per texture (SAMPLER-CACHE).
//
// Bistro's 1296 meshes ran one-sampler-per-image into maxSamplerAllocationCount (1024). The relation asserted
// here is the cache's whole promise: the number of samplers is the number of distinct states, an equal state
// gets the same handle, and nothing but Release destroys one. No GPU: the cache's create/destroy are injected
// and count handles.

#include <Engine/Graphic/API/Vulkan/VulkanSamplerCache.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

using namespace Desert::Graphic::API::Vulkan;

namespace
{
    struct FakeDevice
    {
        uint64_t               Next = 1;
        std::set<VkSampler>    Alive;
        std::vector<VkSampler> Destroyed;

        VulkanSamplerCache MakeCache()
        {
            return { [this]( const VkSamplerCreateInfo&, VkSampler& out )
                     {
                         // A fake opaque handle, never dereferenced: an integer is the only way to mint one.
                         // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast)
                         out = reinterpret_cast<VkSampler>( static_cast<uintptr_t>( Next++ ) );
                         Alive.insert( out );
                         return VK_SUCCESS;
                     },
                     [this]( VkSampler sampler )
                     {
                         Alive.erase( sampler );
                         Destroyed.push_back( sampler );
                     } };
        }
    };

    VkSamplerCreateInfo State( VkFilter filter, VkSamplerAddressMode address, float anisotropy )
    {
        VkSamplerCreateInfo info{};
        info.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter        = filter;
        info.minFilter        = filter;
        info.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU     = address;
        info.addressModeV     = address;
        info.addressModeW     = address;
        info.anisotropyEnable = anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy    = anisotropy;
        info.maxLod           = VK_LOD_CLAMP_NONE;
        info.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        return info;
    }
} // namespace

TEST( SamplerCache, TwoThousandTexturesWithThreeStatesMakeThreeSamplers )
{
    FakeDevice device;
    {
        VulkanSamplerCache        cache     = device.MakeCache();
        const VkSamplerCreateInfo states[3] = {
             State( VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, 8.0f ),
             State( VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f ),
             State( VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT, 1.0f ) };
        VkSampler first[3] = {};
        for ( int texture = 0; texture < 2000; ++texture )
        {
            const int                          which   = texture % 3;
            const Common::ResultStr<VkSampler> sampler = cache.Acquire( states[which] );
            ASSERT_TRUE( sampler.IsSuccess() );
            if ( first[which] == VK_NULL_HANDLE )
                first[which] = sampler.GetValue();
            EXPECT_EQ( sampler.GetValue(), first[which] )
                 << "texture " << texture << ": an equal state, another sampler";
        }
        EXPECT_EQ( cache.Size(), 3u );
        EXPECT_EQ( device.Alive.size(), 3u );
        EXPECT_TRUE( device.Destroyed.empty() ) << "a cached sampler was destroyed while the cache lives";
    }
    EXPECT_TRUE( device.Alive.empty() ) << "the cache's end did not destroy every sampler it made";
    EXPECT_EQ( device.Destroyed.size(), 3u );
}

TEST( SamplerCache, EveryFieldOfTheStateIsPartOfTheKey )
{
    FakeDevice                       device;
    VulkanSamplerCache               cache = device.MakeCache();
    const VkSamplerCreateInfo        base  = State( VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, 1.0f );
    std::vector<VkSamplerCreateInfo> variants( 9, base );
    variants[1].mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    variants[2].addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    variants[3].mipLodBias    = 0.5f;
    variants[4].compareEnable = VK_TRUE;
    variants[5].compareOp     = VK_COMPARE_OP_LESS;
    variants[6].minLod        = 1.0f;
    variants[7].maxLod        = 100.0f;
    variants[8].borderColor   = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    std::set<VkSampler> distinct;
    for ( const VkSamplerCreateInfo& info : variants )
    {
        const Common::ResultStr<VkSampler> sampler = cache.Acquire( info );
        distinct.insert( sampler.GetValue() );
    }
    EXPECT_EQ( distinct.size(), variants.size() );
    EXPECT_EQ( cache.Size(), variants.size() );
}

TEST( SamplerCache, RefusesWhatTheKeyCannotName )
{
    FakeDevice                       device;
    VulkanSamplerCache               cache = device.MakeCache();
    VkSamplerCreateInfo              info  = State( VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, 1.0f );
    VkSamplerReductionModeCreateInfo reduction{};
    reduction.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO;
    info.pNext      = &reduction;
    EXPECT_FALSE( cache.Acquire( info ).IsSuccess() );
    EXPECT_EQ( cache.Size(), 0u );
    EXPECT_TRUE( device.Alive.empty() );
}

TEST( SamplerCache, ReleaseDestroysAllAndTheCacheMakesAgain )
{
    FakeDevice                         device;
    VulkanSamplerCache                 cache = device.MakeCache();
    const VkSamplerCreateInfo          info  = State( VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, 1.0f );
    const Common::ResultStr<VkSampler> first = cache.Acquire( info );
    cache.Release();
    EXPECT_EQ( cache.Size(), 0u );
    EXPECT_TRUE( device.Alive.empty() );
    const Common::ResultStr<VkSampler> again = cache.Acquire( info );
    EXPECT_NE( again.GetValue(), first.GetValue() ) << "a released handle was handed out again";
}
