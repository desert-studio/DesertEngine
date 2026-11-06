// VK1: a VkQueue must be externally synchronised (VUID-vkQueueSubmit-queue-parameter). One-off uploads flush
// from whichever thread creates the resource while the frame submits from the main thread, and the
// validation layer caught the two meeting on one queue ("vkQueueSubmit THREADING ERROR"). The engine's answer
// is one lock beside the queues, in VulkanLogicalDevice, and every call that touches a queue going through it.
//
// The subject is the SOURCE TEXT, like DeviceLostCensus: a lock cannot be observed without a GPU and two
// threads racing, but a call that bypasses it can be read off the disk. A new vkQueueSubmit anywhere else is
// red here by name and line.
#include "TestSupport/source_roots.hpp"
#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../../TestSupport/scratch_dir.hpp"

namespace
{
    namespace fs = std::filesystem;

    const char* const k_Device = "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanDevice.cpp";

    // Every Vulkan entry point whose queue (or, for vkDeviceWaitIdle, every queue of the device) the spec
    // requires to be externally synchronised.
    constexpr std::array<const char*, 6> k_QueueCalls = { "vkQueueSubmit",     "vkQueueSubmit2",
                                                          "vkQueuePresentKHR", "vkQueueWaitIdle",
                                                          "vkQueueBindSparse", "vkDeviceWaitIdle" };

    // The only functions allowed to issue those calls, each under the queue lock (WaitQueueIdle: the caller's
    // LockQueues(), which it checks is this device's before the call).
    constexpr std::array<const char*, 5> k_Owners = {
         "VulkanLogicalDevice::SubmitToQueue", "VulkanLogicalDevice::PresentToQueue",
         "VulkanLogicalDevice::WaitIdle", "VulkanLogicalDevice::WaitQueueIdle", "VulkanLogicalDevice::Destroy" };

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
    ASSERT_FALSE( root.empty() ) << "repository root not found from " << Desert::TestSupport::RepositoryRoot();

    std::size_t scanned = 0;
    for ( const std::string& dir : Desert::TestSupport::LibraryRootsAnd( { "Editor/Source", "Runtime/Source" } ) )
    {
        std::error_code ec;
        for ( auto it = fs::recursive_directory_iterator( fs::path( root ) / dir, ec );
              !ec && it != fs::recursive_directory_iterator(); it.increment( ec ) )
        {
            const fs::path& p = it->path();
            if ( p.extension() != ".cpp" && p.extension() != ".hpp" && p.extension() != ".h" )
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
        // Its own lock, or the caller's checked to be this device's (a refusal before the call).
        const std::size_t own  = body.find( "scoped_lock queues( m_QueueMutex )" );
        const std::size_t held = body.find( "held.mutex() != &m_QueueMutex" );
        const std::size_t lock = std::min( own, held );
        EXPECT_TRUE( lock != std::string::npos && lock < firstCall )
             << owner << " issues a queue call without holding m_QueueMutex first";
    }
}

// The ImGui Vulkan backend is third-party and issues its own queue calls for detached platform windows, so the
// scan above cannot see them. What CAN be read (VK-SUBMIT-THREAD step 7): ImGui's default renderer loop is not
// used (the layer draws the windows itself, as submit-thread entries), and every call into the backend goes
// through DetachedWindows::Capture from a scope that took VulkanLogicalDevice::LockQueues() first.
TEST( QueueSubmitCensus, DetachedImGuiWindowsRenderUnderTheQueueLock )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string src = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( fs::path( root ) / "Editor/Source/Editor/ImGuiIntegration/VulkanImGuiLayer.cpp" ) );
    ASSERT_NE( src.find( "UpdatePlatformWindows(" ), std::string::npos )
         << "VulkanImGuiLayer no longer drives platform windows; update the census";
    EXPECT_EQ( src.find( "RenderPlatformWindowsDefault(" ), std::string::npos )
         << "ImGui's default loop calls the backend outside the queue lock and the submit thread";
    std::size_t calls = 0;
    for ( std::size_t at = src.find( "return Capture(" ); at != std::string::npos;
          at             = src.find( "return Capture(", at + 1 ) )
        ++calls;
    // The window calls (create / resize / destroy, AfterOwnWork) and the frame (Draw).
    EXPECT_EQ( calls, 2u ) << "the backend is called from a new place; census it";
    // Step 9: the window calls (AfterOwnWork) drain the submit thread with no lock held, then take the queue lock
    // and keep it across their Capture: the fork's one wait (WaitWindowIdleFn) idles the queue under it before a
    // swapchain and its semaphores go (a frame fence does not cover the present:
    // VUID-vkDestroySemaphore-semaphore-05149).
    const std::size_t owner    = src.find( "VkResult AfterOwnWork(" );
    const std::size_t drain    = owner == std::string::npos ? owner : src.find( "DrainSubmitThread()", owner );
    const std::size_t taken    = drain == std::string::npos ? drain : src.find( "LockQueues()", drain );
    const std::size_t captured = taken == std::string::npos ? taken : src.find( "return Capture(", taken );
    const std::size_t next     = owner == std::string::npos ? owner : src.find( "return Capture(", owner );
    EXPECT_TRUE( captured != std::string::npos && captured == next )
         << "AfterOwnWork must drain the submit thread, then take LockQueues(), then call the backend";
    EXPECT_NE( src.find( "&DetachedWindows::WaitWindowIdle;" ), std::string::npos )
         << "the layer no longer gives the ImGui backend its wait before a window's objects are destroyed";
    EXPECT_NE( src.find( "WaitQueueIdle(" ), std::string::npos )
         << "the layer's window wait no longer idles the queue under the lock AfterOwnWork holds";
    // The frame (the backend's acquire, submit and present) touches the queue: its Capture holds the lock too.
    const std::size_t render    = src.find( "BackendRender(" );
    const std::size_t frame     = render == std::string::npos ? render : src.rfind( "return Capture(", render );
    const std::size_t lock      = frame == std::string::npos ? frame : src.rfind( "LockQueues()", frame );
    const std::size_t scopeOpen = frame == std::string::npos ? frame : src.rfind( '{', frame );
    EXPECT_TRUE( lock != std::string::npos && scopeOpen != std::string::npos && lock > scopeOpen )
         << "the frame's backend call (Capture around BackendRender) must run in a scope that took "
            "VulkanLogicalDevice::LockQueues() first: the backend submits and presents on the shared queue.";
    // The fork (gitlink ThirdParty/ImGui) is what gives the window calls one waiter and makes a failed frame stop.
    const std::string backend = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( fs::path( root ) / "ThirdParty/ImGui/backends/imgui_impl_vulkan.cpp" ) );
    EXPECT_NE( backend.find( "WaitWindowIdleFn(" ), std::string::npos )
         << "the ImGui fork no longer waits through the layer before a window's objects go (gitlink rolled back?)";
    EXPECT_EQ( backend.find( "ImGui_ImplVulkanH_WaitWindowFrames(" ), std::string::npos )
         << "the ImGui fork waits a window's fences again: a second waiter, and no cover for the present";
    EXPECT_NE( backend.find( "PresentPending" ), std::string::npos )
         << "the ImGui fork presents after a failed RenderWindow again (gitlink rolled back?)";
    for ( const char* const call : { "BackendCreate(", "BackendDestroy(", "BackendResize(", "BackendRender(",
                                     "BackendPresent(" } )
        EXPECT_NE( src.find( call ), std::string::npos ) << call << " is no longer called; update the census";
}
