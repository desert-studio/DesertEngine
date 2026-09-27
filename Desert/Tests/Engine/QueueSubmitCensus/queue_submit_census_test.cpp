// VK1: a VkQueue must be externally synchronised (VUID-vkQueueSubmit-queue-parameter). One-off uploads flush
// from whichever thread creates the resource while the frame submits from the main thread, and the
// validation layer caught the two meeting on one queue ("vkQueueSubmit THREADING ERROR"). The engine's answer
// is one lock beside the queues, in VulkanLogicalDevice, and every call that touches a queue going through it.
//
// The subject is the SOURCE TEXT, like DeviceLostCensus: a lock cannot be observed without a GPU and two
// threads racing, but a call that bypasses it can be read off the disk. A new vkQueueSubmit anywhere else is
// red here by name and line.
#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    const char* const k_Device = "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanDevice.cpp";

    // Every Vulkan entry point whose queue (or, for vkDeviceWaitIdle, every queue of the device) the spec
    // requires to be externally synchronised.
    constexpr std::array<const char*, 6> k_QueueCalls = { "vkQueueSubmit",     "vkQueueSubmit2",
                                                          "vkQueuePresentKHR", "vkQueueWaitIdle",
                                                          "vkQueueBindSparse", "vkDeviceWaitIdle" };

    // The only functions allowed to issue those calls, each under the queue lock.
    constexpr std::array<const char*, 4> k_Owners = {
         "VulkanLogicalDevice::SubmitToQueue", "VulkanLogicalDevice::PresentToQueue",
         "VulkanLogicalDevice::WaitIdle", "VulkanLogicalDevice::Destroy" };

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::ifstream( prefix + k_Device ) )
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

    bool IsIdentChar( char c )
    {
        return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_';
    }

    // Offsets of `name(` as a whole identifier followed by a call parenthesis.
    std::vector<std::size_t> CallsOf( const std::string& src, const std::string& name )
    {
        std::vector<std::size_t> at;
        for ( std::size_t i = src.find( name ); i != std::string::npos; i = src.find( name, i + 1 ) )
        {
            if ( i > 0 && IsIdentChar( src[i - 1] ) )
                continue;
            std::size_t j = i + name.size();
            if ( j < src.size() && IsIdentChar( src[j] ) )
                continue;
            while ( j < src.size() && ( src[j] == ' ' || src[j] == '\n' ) )
                ++j;
            if ( j < src.size() && src[j] == '(' )
                at.push_back( i );
        }
        return at;
    }

    // [open, close) of the body of `qualifiedName`, by brace matching; {npos, npos} when absent.
    std::pair<std::size_t, std::size_t> BodyOf( const std::string& src, const std::string& qualifiedName )
    {
        for ( std::size_t at = src.find( qualifiedName ); at != std::string::npos;
              at             = src.find( qualifiedName, at + 1 ) )
        {
            const std::size_t open = src.find( '{', at );
            const std::size_t semi = src.find( ';', at );
            if ( open == std::string::npos || ( semi != std::string::npos && semi < open ) )
                continue;
            int depth = 0;
            for ( std::size_t i = open; i < src.size(); ++i )
            {
                if ( src[i] == '{' )
                    ++depth;
                else if ( src[i] == '}' && --depth == 0 )
                    return { open, i };
            }
        }
        return { std::string::npos, std::string::npos };
    }

    int LineOf( const std::string& src, std::size_t offset )
    {
        return 1 + static_cast<int>( std::count( src.begin(), src.begin() + static_cast<long>( offset ), '\n' ) );
    }
} // namespace

TEST( QueueSubmitCensus, EveryQueueCallOutsideTheDeviceLockIsAViolation )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from " << fs::current_path();

    std::size_t scanned = 0;
    for ( const char* dir : { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Runtime/Source" } )
    {
        std::error_code ec;
        for ( auto it = fs::recursive_directory_iterator( fs::path( root ) / dir, ec );
              !ec && it != fs::recursive_directory_iterator(); it.increment( ec ) )
        {
            const fs::path& p = it->path();
            if ( p.extension() != ".cpp" && p.extension() != ".hpp" && p.extension() != ".h" )
                continue;
            // lightweightvk is a vendored third-party tree under our Vulkan folder; it is not compiled.
            if ( p.string().find( "lightweightvk" ) != std::string::npos )
                continue;
            ++scanned;
            const std::string src     = Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadAll( p ) );
            const bool        isOwner = p.generic_string().ends_with( k_Device );
            for ( const char* call : k_QueueCalls )
            {
                for ( const std::size_t at : CallsOf( src, call ) )
                {
                    bool inOwner = false;
                    if ( isOwner )
                    {
                        for ( const char* owner : k_Owners )
                        {
                            const auto [open, close] = BodyOf( src, owner );
                            inOwner = inOwner || ( open != std::string::npos && at > open && at < close );
                        }
                    }
                    EXPECT_TRUE( inOwner ) << p.generic_string() << ":" << LineOf( src, at ) << " calls " << call
                                           << " outside VulkanLogicalDevice's queue lock; route it through "
                                              "SubmitToQueue / PresentToQueue / WaitIdle.";
                }
            }
        }
    }
    EXPECT_GT( scanned, 500u ) << "the scan reached too few files to mean anything";
}

TEST( QueueSubmitCensus, EveryOwnerTakesTheQueueLockBeforeItsCall )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string src =
         Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadAll( fs::path( root ) / k_Device ) );

    for ( const char* owner : k_Owners )
    {
        const auto [open, close] = BodyOf( src, owner );
        ASSERT_NE( open, std::string::npos ) << owner << " is gone from " << k_Device << "; update the census";
        const std::string body      = src.substr( open, close - open );
        std::size_t       firstCall = std::string::npos;
        for ( const char* call : k_QueueCalls )
            for ( const std::size_t at : CallsOf( body, call ) )
                firstCall = std::min( firstCall, at );
        ASSERT_NE( firstCall, std::string::npos ) << owner << " no longer issues a queue call";
        const std::size_t lock = body.find( "scoped_lock queues( m_QueueMutex )" );
        EXPECT_TRUE( lock != std::string::npos && lock < firstCall )
             << owner << " issues a queue call without holding m_QueueMutex first";
    }
}

// The ImGui Vulkan backend is third-party and issues its own queue calls for detached platform windows, so the
// scan above cannot see them. What CAN be read is that the one place that drives them holds the lock first.
TEST( QueueSubmitCensus, DetachedImGuiWindowsRenderUnderTheQueueLock )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string src = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( fs::path( root ) / "Editor/Source/Editor/ImGuiIntegration/VulkanImGuiLayer.cpp" ) );
    const std::size_t render = src.find( "RenderPlatformWindowsDefault(" );
    const std::size_t update = src.find( "UpdatePlatformWindows(" );
    ASSERT_NE( render, std::string::npos )
         << "VulkanImGuiLayer no longer renders platform windows; update the census";
    ASSERT_NE( update, std::string::npos );
    const std::size_t lock      = src.rfind( "LockQueues()", update );
    const std::size_t scopeOpen = src.rfind( '{', update );
    EXPECT_TRUE( lock != std::string::npos && scopeOpen != std::string::npos && lock > scopeOpen )
         << "UpdatePlatformWindows / RenderPlatformWindowsDefault must run inside a scope that took "
            "VulkanLogicalDevice::LockQueues() first: the backend submits and presents on the shared queue.";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
