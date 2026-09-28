// DEVICE CAPS: THE MANDATORY SET, ONE CODE PATH.
//
// Part 1 runs the pure half of DeviceCaps on made-up devices: which route (core / extension / none) each
// capability takes for a given version and extension list, and the refusal of a device that misses the
// required set — with EVERY missing item named, not the first. No GPU is involved.
//
// Part 2 is a source census of the relation the design rests on: the instance, the device list, every
// feature and extension question and the logical device all go through vk-bootstrap, from exactly one
// file each, and the VkDevice is built from the very object DeviceCapsProbe enabled things on. A raw
// vkGetPhysicalDeviceFeatures or vkCreateDevice anywhere else would be a second opinion about what the
// device has, which is what DeviceCaps exists to prevent.
#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Graphic::API::Vulkan;

namespace
{
    namespace fs = std::filesystem;

    DeviceCaps Plan( uint32_t version, std::set<std::string> extensions )
    {
        return PlanRoutes( "FakeGPU", "FakeDriver 1.0", version, [extensions]( std::string_view name )
                           { return extensions.count( std::string( name ) ) != 0; } );
    }

    // What a probe that says "yes" to every planned route would leave behind.
    void GrantEveryRoute( DeviceCaps& caps )
    {
        for ( std::size_t i = 0; i < kCapabilityCount; ++i )
            caps.Rows[i].Present = caps.Rows[i].Route != CapabilityRoute::Unreachable &&
                                   DependenciesPresent( caps, static_cast<Capability>( i ) );
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::ifstream( prefix + "Desert/Desert/Source/Engine/Graphic/API/Vulkan/DeviceCaps.hpp" ) )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    struct SourceFile
    {
        std::string Path;
        std::string Code; // comments and literals stripped
    };

    std::vector<SourceFile> FirstPartySources( const std::string& root )
    {
        std::vector<SourceFile> out;
        for ( const char* tree :
              { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Runtime/Source", "Tools" } )
        {
            std::error_code ec;
            for ( auto it = fs::recursive_directory_iterator( fs::path( root ) / tree, ec );
                  !ec && it != fs::recursive_directory_iterator(); ++it )
            {
                const fs::path& p = it->path();
                if ( p.string().find( "ThirdParty" ) != std::string::npos )
                    continue;
                if ( p.extension() != ".cpp" && p.extension() != ".hpp" && p.extension() != ".h" )
                    continue;
                out.push_back( { fs::relative( p, root ).generic_string(),
                                 Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadAll( p ) ) } );
            }
        }
        return out;
    }

    std::vector<std::string> FilesContaining( const std::vector<SourceFile>& files, const std::string& token )
    {
        std::vector<std::string> hits;
        for ( const auto& file : files )
            if ( file.Code.find( token ) != std::string::npos )
                hits.push_back( file.Path );
        return hits;
    }
} // namespace

// ── Part 1: routing and refusal ─────────────────────────────────────────────────────────────────────

TEST( DeviceCaps, A13DeviceTakesTheCoreRouteForEverythingPromoted )
{
    const DeviceCaps caps =
         Plan( VK_API_VERSION_1_3, { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME } );
    EXPECT_EQ( caps.RouteOf( Capability::DynamicRendering ), CapabilityRoute::Core )
         << "a 1.3 device must not be driven through the KHR extension even when it lists it";
    EXPECT_EQ( caps.RouteOf( Capability::Synchronization2 ), CapabilityRoute::Core );
    EXPECT_EQ( caps.RouteOf( Capability::TimelineSemaphore ), CapabilityRoute::Core );
    EXPECT_EQ( caps.RouteOf( Capability::BufferDeviceAddress ), CapabilityRoute::Core );
    EXPECT_EQ( caps.RouteOf( Capability::Swapchain ), CapabilityRoute::Extension );
    EXPECT_EQ( caps.RouteOf( Capability::TessellationShader ), CapabilityRoute::Core );
}

TEST( DeviceCaps, A12DeviceReachesThe13FeaturesThroughKhrOnly )
{
    const DeviceCaps withExt = Plan( VK_API_VERSION_1_2, { VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
                                                           VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME } );
    EXPECT_EQ( withExt.RouteOf( Capability::DynamicRendering ), CapabilityRoute::Extension );
    EXPECT_EQ( withExt.RouteOf( Capability::Synchronization2 ), CapabilityRoute::Extension );
    EXPECT_EQ( withExt.RouteOf( Capability::TimelineSemaphore ), CapabilityRoute::Core );

    const DeviceCaps without = Plan( VK_API_VERSION_1_2, {} );
    EXPECT_EQ( without.RouteOf( Capability::DynamicRendering ), CapabilityRoute::Unreachable );
    EXPECT_EQ( without.RouteOf( Capability::Synchronization2 ), CapabilityRoute::Unreachable );
}

TEST( DeviceCaps, ExtensionRoutesBelowTheirMinimumApiAreNotTaken )
{
    // Dynamic rendering on 1.1 would need depth_stencil_resolve + create_renderpass2 as extensions too.
    const DeviceCaps caps = Plan( VK_API_VERSION_1_1, { VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
                                                        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                                                        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME } );
    EXPECT_EQ( caps.RouteOf( Capability::DynamicRendering ), CapabilityRoute::Unreachable );
    EXPECT_EQ( caps.RouteOf( Capability::Synchronization2 ), CapabilityRoute::Extension );
    EXPECT_EQ( caps.RouteOf( Capability::AccelerationStructure ), CapabilityRoute::Unreachable );
}

TEST( DeviceCaps, UsedVersionIsCappedAt13AndDropsThePatch )
{
    EXPECT_EQ( UsedApiVersion( VK_MAKE_API_VERSION( 0, 1, 4, 309 ) ), VK_API_VERSION_1_3 );
    EXPECT_EQ( UsedApiVersion( VK_MAKE_API_VERSION( 0, 1, 2, 231 ) ), VK_API_VERSION_1_2 );
    const DeviceCaps caps = Plan( VK_MAKE_API_VERSION( 0, 1, 4, 0 ), {} );
    EXPECT_EQ( caps.UsedApiVersion, VK_API_VERSION_1_3 );
}

TEST( DeviceCaps, RayTracingIsNotEnabledWithoutItsDependencies )
{
    DeviceCaps caps = Plan( VK_API_VERSION_1_2,
                            { VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME } );
    // deferred_host_operations is not listed, so the acceleration structure (and ray query on it) is not.
    GrantEveryRoute( caps );
    EXPECT_FALSE( caps.Has( Capability::DeferredHostOperations ) );
    EXPECT_FALSE( caps.Has( Capability::AccelerationStructure ) );
    EXPECT_FALSE( caps.Has( Capability::RayQuery ) );

    DeviceCaps full = Plan( VK_API_VERSION_1_2,
                            { VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                              VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME } );
    GrantEveryRoute( full );
    EXPECT_TRUE( full.Has( Capability::AccelerationStructure ) );
    EXPECT_TRUE( full.Has( Capability::RayQuery ) );
    EXPECT_FALSE( full.Has( Capability::RayTracingPipeline ) );
}

TEST( DeviceCaps, ADeviceWithTheRequiredSetIsAccepted )
{
    DeviceCaps caps = Plan( VK_API_VERSION_1_2, { VK_KHR_SWAPCHAIN_EXTENSION_NAME } );
    GrantEveryRoute( caps );
    const auto accepted = CheckRequired( caps );
    EXPECT_TRUE( accepted.IsSuccess() ) << ( accepted ? std::string{} : accepted.GetError() );
}

TEST( DeviceCaps, TheRefusalNamesEveryMissingItemAtOnce )
{
    // A 1.0 device with no swapchain whose driver said no to tessellation: three reasons, all listed.
    DeviceCaps caps    = Plan( VK_API_VERSION_1_0, {} );
    const auto refused = CheckRequired( caps );
    ASSERT_FALSE( refused.IsSuccess() );
    const std::string error = refused.GetError();
    EXPECT_NE( error.find( "FakeGPU" ), std::string::npos ) << error;
    EXPECT_NE( error.find( "Vulkan 1.1.0 (device reports 1.0.0)" ), std::string::npos ) << error;
    EXPECT_NE( error.find( "tessellationShader" ), std::string::npos ) << error;
    EXPECT_NE( error.find( "swapchain (VK_KHR_swapchain)" ), std::string::npos ) << error;
    // Optional rows never make a refusal.
    EXPECT_EQ( error.find( "wideLines" ), std::string::npos ) << error;
    EXPECT_EQ( error.find( "dynamicRendering" ), std::string::npos ) << error;
}

TEST( DeviceCaps, TheStartupLineSaysWhereEachRowCameFrom )
{
    DeviceCaps caps =
         Plan( VK_API_VERSION_1_2, { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME } );
    GrantEveryRoute( caps );
    const std::string line = FormatCapsTable( caps );
    EXPECT_NE( line.find( "device FakeGPU | driver FakeDriver 1.0 | Vulkan 1.2.0" ), std::string::npos ) << line;
    EXPECT_NE( line.find( "!swapchain=ext" ), std::string::npos ) << line;
    EXPECT_NE( line.find( "!tessellationShader=1.0" ), std::string::npos ) << line;
    EXPECT_NE( line.find( " dynamicRendering=ext" ), std::string::npos ) << line;
    EXPECT_NE( line.find( " timelineSemaphore=core1.2" ), std::string::npos ) << line;
    EXPECT_NE( line.find( " synchronization2=no" ), std::string::npos ) << line;
}

TEST( DeviceCaps, TheRequiredSetIsWhatTheRendererUsesToday )
{
    // Pinned by name: moving a row to Required is VKF2's decision and must show up here as a diff.
    std::set<std::string> required;
    for ( const CapabilitySpec& spec : CapabilityTable() )
        if ( spec.Need == CapabilityNeed::Required )
            required.insert( std::string( spec.Name ) );
    EXPECT_EQ( required, ( std::set<std::string>{ "tessellationShader", "swapchain" } ) );
    EXPECT_EQ( kMinimumDeviceApiVersion, VK_API_VERSION_1_1 );
}

// ── Part 2: the census ──────────────────────────────────────────────────────────────────────────────

TEST( DeviceCapsCensus, NoRawInstanceDeviceFeatureOrExtensionQueryOutsideVkBootstrap )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the working directory";
    const auto files = FirstPartySources( root );
    ASSERT_GT( files.size(), 500u ) << "the scan found too few sources to mean anything";

    for ( const char* call : { "vkCreateInstance(", "vkCreateDevice(", "vkEnumeratePhysicalDevices(",
                               "vkGetPhysicalDeviceFeatures(", "vkGetPhysicalDeviceFeatures2(",
                               "vkGetPhysicalDeviceFeatures2KHR(", "vkEnumerateDeviceExtensionProperties(" } )
    {
        const auto hits = FilesContaining( files, call );
        EXPECT_TRUE( hits.empty() ) << call << " is called in " << ( hits.empty() ? "" : hits.front() )
                                    << " — that is a second opinion about the device; ask DeviceCaps (Has) or "
                                       "add a row to CapabilityTable and probe it in DeviceCapsProbe.cpp.";
    }
}

TEST( DeviceCapsCensus, EachBootstrapStepHasOneHome )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const auto files = FirstPartySources( root );

    const std::string                                      vk = "Desert/Desert/Source/Engine/Graphic/API/Vulkan/";
    const std::vector<std::pair<std::string, std::string>> homes = {
         { "vkb::InstanceBuilder", vk + "VulkanContext.cpp" },
         { "vkb::PhysicalDeviceSelector", vk + "DeviceCapsProbe.cpp" },
         { "enable_features_if_present(", vk + "DeviceCapsProbe.cpp" },
         { "enable_extension_features_if_present(", vk + "DeviceCapsProbe.cpp" },
         { "enable_extension_if_present(", vk + "DeviceCapsProbe.cpp" },
         { "vkb::DeviceBuilder", vk + "VulkanDevice.cpp" },
    };
    for ( const auto& [token, home] : homes )
    {
        const auto hits = FilesContaining( files, token );
        EXPECT_EQ( hits, std::vector<std::string>{ home } ) << token << " must live in " << home << " only";
    }
}

TEST( DeviceCapsCensus, TheDeviceIsBuiltFromTheProbedObject )
{
    // THE RELATION, not the mentions: the builder takes the vkb::PhysicalDevice the probe enabled rows
    // on (VulkanPhysicalDevice keeps it as m_Bootstrap, straight from ProbedDevice::Physical), and nothing
    // adds a feature or extension in between.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string vk     = "Desert/Desert/Source/Engine/Graphic/API/Vulkan/";
    const std::string device = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( fs::path( root ) / ( vk + "VulkanDevice.cpp" ) ) );
    const std::string header = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( fs::path( root ) / ( vk + "VulkanDevice.hpp" ) ) );

    EXPECT_NE( device.find( "vkb::DeviceBuilder builder( m_PhysicalDevice->GetBootstrapDevice() );" ),
               std::string::npos );
    EXPECT_NE( device.find( "m_Bootstrap( std::move( probed.Physical ) )" ), std::string::npos );
    EXPECT_NE( header.find( "return *m_Bootstrap;" ), std::string::npos );
    EXPECT_EQ( device.find( "enabledExtensionCount" ), std::string::npos );
    EXPECT_EQ( device.find( "pEnabledFeatures" ), std::string::npos );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
