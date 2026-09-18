// Guards on how the engine SHUTS DOWN.
//
// WHY THIS SUITE EXISTS. Every headless capture used to end in exit status 139 after the PNG was already
// on disk, and the three faults behind it were all the same shape: something released a GPU handle, or
// locked a mutex, at a moment when the thing it was talking to no longer existed.
//
//   1. The application was owned by Common::Singleton<T>, a namespace-scope static, so it was destroyed
//      at __cxa_finalize -- after every library the Vulkan loader had dlopen'ed during main. Its
//      vkDeviceWaitIdle entered the validation layer and locked a destroyed shared_mutex.
//   2. Application's members were declared window-first and context-last, so they died context-first:
//      VulkanImage2D::Release() dereferenced an expired renderer context to reach the VMA allocator.
//   3. std::exit() was called from inside a running frame, which runs the static destructors underneath
//      the job system's live worker threads.
//
// None of the three is a function whose return value can be asserted. All three are two places that have
// to agree with each other, which is the defect class DEV_CONTRACT 2.3.1 says a unit test never catches
// and a RELATION test does. The relations below are therefore read out of the source files themselves.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // The repository root, found by walking up from the test binary's working directory, as
    // Tests/Engine/ShippedShaderPasses and Tests/Engine/ShaderCacheKey do.
    const std::filesystem::path& RepoRoot()
    {
        static const std::filesystem::path root = []() -> std::filesystem::path
        {
            std::filesystem::path here = std::filesystem::current_path();
            for ( int up = 0; up < 8 && !std::filesystem::exists( here / "Desert" / "Desert" / "Source" ); ++up )
                here = here.parent_path();
            return here;
        }();
        return root;
    }

    std::string ReadFile( const std::filesystem::path& path )
    {
        std::ifstream     in( path, std::ios::binary );
        std::stringstream out;
        out << in.rdbuf();
        return out.str();
    }

    // Every //-comment line removed. The relations below are about CODE, and three of the comments that
    // explain them quote the very construct they forbid.
    std::string StripLineComments( const std::string& text )
    {
        std::string       out;
        std::stringstream in( text );
        std::string       line;
        while ( std::getline( in, line ) )
        {
            const auto first = line.find_first_not_of( " \t" );
            if ( first != std::string::npos && line.compare( first, 2, "//" ) == 0 )
                continue;
            out += line;
            out += '\n';
        }
        return out;
    }

    // Names captured from every occurrence of `Get<Name>Service` in @p text.
    std::set<std::string> ServiceNames( const std::string& text )
    {
        std::set<std::string> names;
        for ( size_t at = text.find( "Get" ); at != std::string::npos; at = text.find( "Get", at + 1 ) )
        {
            const size_t end = text.find( "Service", at );
            if ( end == std::string::npos )
                continue;
            const std::string name = text.substr( at + 3, end - at - 3 );
            // A name, not a sentence: anything with punctuation in it came from prose, not a call.
            const bool isIdentifier = !name.empty() && std::all_of( name.begin(), name.end(), []( unsigned char c )
                                                                    { return std::isalnum( c ) != 0; } );
            if ( isIdentifier )
                names.insert( name );
        }
        return names;
    }

    // The text between the braces of the first function whose signature contains @p signature, with
    // //-comments removed. Empty string when the signature is not there at all.
    std::string FunctionBody( const std::string& source, const std::string& signature )
    {
        const size_t at = source.find( signature );
        if ( at == std::string::npos )
            return {};
        const size_t open = source.find( '{', at );
        if ( open == std::string::npos )
            return {};

        int    depth = 0;
        size_t i     = open;
        for ( ; i < source.size(); ++i )
        {
            if ( source[i] == '{' )
                ++depth;
            else if ( source[i] == '}' && --depth == 0 )
                break;
        }
        if ( i >= source.size() )
            return {};
        return StripLineComments( source.substr( open + 1, i - open - 1 ) );
    }

    bool IsBlank( const std::string& text )
    {
        return text.find_first_not_of( " \t\r\n" ) == std::string::npos;
    }

    std::vector<std::filesystem::path> ServiceSources()
    {
        std::vector<std::filesystem::path> files;
        const auto root = RepoRoot() / "Desert" / "Desert" / "Source" / "Engine" / "Runtime" / "Services";
        if ( !std::filesystem::exists( root ) )
            return files;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root ) )
        {
            const auto& p = entry.path();
            if ( entry.is_regular_file() && p.extension() == ".cpp" && p.stem().string().size() > 7 &&
                 p.stem().string().compare( p.stem().string().size() - 7, 7, "Service" ) == 0 )
                files.push_back( p );
        }
        std::sort( files.begin(), files.end() );
        return files;
    }
} // namespace

TEST( TeardownOrder, TheRepositoryRootWasFound )
{
    ASSERT_TRUE( std::filesystem::exists( RepoRoot() / "Desert" / "Desert" / "Source" ) )
         << "walked up from " << std::filesystem::current_path().string()
         << " and found no source tree; "
            "every other test in this suite would pass vacuously";
}

// RELATION: the members of Application die in reverse declaration order, and the window owns the
// swapchain, its framebuffers and their images -- objects that belong to the device and are freed through
// the context's VMA allocator. So the window must be declared LAST of the three.
//
// This is the relation whose violation was the segfault: with m_Window declared first it died last, and
// VulkanImage2D::Release() went looking for an allocator whose owner had already been destroyed.
TEST( TeardownOrder, ApplicationDeclaresTheWindowAfterTheDeviceAndTheContext )
{
    const std::string header =
         ReadFile( RepoRoot() / "Desert" / "Desert" / "Source" / "Engine" / "Core" / "Application.hpp" );
    ASSERT_FALSE( header.empty() ) << "Application.hpp not found or empty";

    // Matched on the member TYPE, not on the member name: `return m_Window;` inside the accessor is an
    // earlier occurrence of the name and would make this test read the wrong order.
    const size_t context = header.find( "shared_ptr<Graphic::RendererContext>" );
    const size_t device  = header.find( "shared_ptr<Device>" );
    const size_t window  = header.find( "shared_ptr<Window>" );

    ASSERT_NE( context, std::string::npos ) << "no m_RendererContext declaration in Application.hpp";
    ASSERT_NE( device, std::string::npos ) << "no m_Device declaration in Application.hpp";
    ASSERT_NE( window, std::string::npos ) << "no m_Window declaration in Application.hpp";

    EXPECT_LT( context, device ) << "m_Device is declared before m_RendererContext, so the context dies "
                                    "first and the device's teardown reaches a freed allocator";
    EXPECT_LT( device, window ) << "m_Window is declared before m_Device, so the window -- and the "
                                   "swapchain images it owns -- dies AFTER the device that owns them";
}

// RELATION: ResourceRegistry exposes N services and ClearAll() has to release all N. Each is a
// function-local static, destroyed at __cxa_finalize, i.e. after ~Application has destroyed the VkDevice:
// a service that ClearAll() forgets is a GPU handle released through a dangling allocator.
//
// Two places that must agree, in one file, with nothing that makes them agree by construction.
TEST( TeardownOrder, EveryResourceServiceIsClearedByClearAll )
{
    const auto        dir    = RepoRoot() / "Desert" / "Desert" / "Source" / "Engine" / "Runtime";
    const std::string header = StripLineComments( ReadFile( dir / "ResourceRegistry.hpp" ) );
    const std::string source = ReadFile( dir / "ResourceRegistry.cpp" );
    ASSERT_FALSE( header.empty() ) << "ResourceRegistry.hpp not found or empty";

    const std::set<std::string> exposed = ServiceNames( header );
    ASSERT_GE( exposed.size(), 10u ) << "read only " << exposed.size()
                                     << " service getters out of ResourceRegistry.hpp -- the parse, not "
                                        "the registry, is what is wrong";

    const std::string clearAll = FunctionBody( source, "ResourceRegistry::ClearAll" );
    ASSERT_FALSE( clearAll.empty() ) << "ResourceRegistry::ClearAll has no body";

    const std::set<std::string> cleared = ServiceNames( clearAll );

    for ( const std::string& name : exposed )
        EXPECT_TRUE( cleared.count( name ) == 1 ) << "ResourceRegistry exposes Get" << name
                                                  << "Service() but ClearAll() never clears it; its "
                                                     "GPU objects would be released after the device is destroyed";
}

// RELATION: a service's Clear() is what ClearAll() relies on, so an EMPTY Clear() makes the test above
// pass while releasing nothing. Four of them were empty bodies -- Texture, Material, Shader and Skybox,
// which are the four holding the heaviest GPU objects in the engine. A stub that satisfies its caller by
// name is worse than a missing function, because the caller looks correct.
TEST( TeardownOrder, NoResourceServiceClearIsAnEmptyBody )
{
    const auto files = ServiceSources();
    ASSERT_GE( files.size(), 10u ) << "found only " << files.size() << " *Service.cpp files";

    for ( const auto& file : files )
    {
        const std::string source = ReadFile( file );
        const std::string name   = file.stem().string();
        const std::string body   = FunctionBody( source, name + "::Clear" );

        EXPECT_FALSE( body.empty() ) << file.string() << ": no " << name << "::Clear() definition";
        EXPECT_FALSE( IsBlank( body ) )
             << file.string() << ": " << name
             << "::Clear() has an empty body, so ResourceRegistry::ClearAll() releases nothing here";
    }
}

// RELATION: std::exit() runs the static destructors on the calling thread. Called from inside a frame it
// runs them underneath the job system's live workers and underneath a live VkDevice -- `--scene <missing>`
// exited 134 ("the engine aborted") instead of the 2 it meant to report, because nine worker threads threw
// "recursive_mutex lock failed: Invalid argument" on the way out.
//
// The only place it is legitimate is CreateApplication, which runs before the application, the job system
// and the device exist and so has nothing to tear down in order. Anywhere else, Application::Close(status)
// is the way out.
TEST( TeardownOrder, StdExitAppearsOnlyBeforeTheApplicationExists )
{
    const std::vector<std::filesystem::path> trees = {
         RepoRoot() / "Desert" / "Desert" / "Source", RepoRoot() / "Desert" / "Common" / "Source",
         RepoRoot() / "Editor" / "Source", RepoRoot() / "Runtime" / "Source" };

    // Both hold a CreateApplication and nothing else that runs during a frame.
    const std::set<std::string> allowed = { "Sandbox.hpp", "Main.cpp" };

    size_t scanned = 0;
    for ( const auto& tree : trees )
    {
        ASSERT_TRUE( std::filesystem::exists( tree ) ) << tree.string() << " does not exist";
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( tree ) )
        {
            const auto& p = entry.path();
            if ( !entry.is_regular_file() || ( p.extension() != ".cpp" && p.extension() != ".hpp" ) )
                continue;
            ++scanned;
            if ( allowed.count( p.filename().string() ) == 1 )
                continue;

            const std::string code = StripLineComments( ReadFile( p ) );
            EXPECT_EQ( code.find( "std::exit(" ), std::string::npos )
                 << p.string()
                 << " calls std::exit() outside CreateApplication. Inside a running frame "
                    "that destroys the job system's mutexes under its own worker threads; "
                    "use Application::Close(status) instead.";
        }
    }

    EXPECT_GT( scanned, 100u ) << "only " << scanned
                               << " sources scanned -- the walk, not the engine, is "
                                  "what is wrong";
}

// RELATION: whoever DESTROYS a SceneRenderer idles the device first.
//
// This used to be implied by `RenderSystem::Shutdown()` — a pure virtual every render system implemented
// and nobody ever called, whose twenty bodies were lists of `.reset()` the destructors already perform.
// Г8 removed it, and removing a mechanism that never ran must not remove the thing it LOOKED like it was
// guaranteeing. The guarantee is real and it is not RAII: releasing a pipeline, a framebuffer or a
// descriptor pool while the last submitted frame is still executing against it is undefined, and
// `SceneRenderer::RebindScene()` waits for exactly that reason before it releases the previous scene's
// passes (it was `Init()` before Г11 split that function in two; Desert/Tests/Engine/RendererSceneLifetime
// pins the wait at its new home, and this row's argument is unchanged). The destructor cannot take the
// wait itself — at process teardown it can run after the device is gone, which is the segfault this whole
// suite exists for — so it belongs to each site that drops one, and every one of them does it today.
//
// A NAMED LIST, like DeviceLostCensus's: there are five of them, each is a deliberate decision, and a
// sixth surface added without the wait is precisely how this comes back.
TEST( TeardownOrder, EverySiteThatDestroysASceneRendererIdlesTheDeviceFirst )
{
    struct Site
    {
        const char* File;
        const char* Signature;
        const char* What;
    };

    const Site sites[] = {
         { "Editor/Source/Editor/Widgets/PreviewViewport.cpp", "PreviewViewport::~PreviewViewport",
           "the Details mesh preview" },
         { "Editor/Source/Editor/Widgets/AssetThumbnailRenderer.cpp",
           "AssetThumbnailRenderer::~AssetThumbnailRenderer",
           "the Content Browser thumbnail renderer -- ThumbnailService drops its renderer through this" },
         { "Editor/Source/EditorLayer.cpp", "EditorLayer::CloseSceneView", "closing an extra scene view" },
         { "Editor/Source/EditorLayer.cpp", "EditorLayer::OnDetach", "quitting, for every extra scene view" },
         { "Editor/Source/Editor/Panels/Photogrammetry/PhotogrammetryPanel.cpp",
           "PhotogrammetryPanel::ReleasePreview", "the reconstruction preview" },
    };

    for ( const Site& site : sites )
    {
        const std::string source = ReadFile( RepoRoot() / site.File );
        ASSERT_FALSE( source.empty() ) << site.File << " not found or empty";

        const std::string body = FunctionBody( source, site.Signature );
        ASSERT_FALSE( body.empty() ) << site.Signature << " is not in " << site.File
                                     << " any more. A row naming a function that no longer exists passes "
                                        "without checking anything -- fix the row or the code.";

        const size_t idle  = body.find( "WaitDeviceIdle()" );
        const size_t drops = body.find( "Renderer.reset()" );
        ASSERT_NE( drops, std::string::npos )
             << site.Signature << " no longer drops a SceneRenderer; this row pins nothing.";
        EXPECT_NE( idle, std::string::npos )
             << site.Signature << " destroys the SceneRenderer of " << site.What
             << " without waiting for the device. The last submitted frame may still be executing against "
                "its pipelines, framebuffers and descriptor pools.";
        if ( idle != std::string::npos )
            EXPECT_LT( idle, drops ) << site.Signature
                                     << " waits for the device AFTER releasing the "
                                        "renderer, which is the same as not waiting at all.";
    }
}


// ── THE DEVICE'S OWN CHILDREN ────────────────────────────────────────────────────────────────────────
//
// WHAT THIS PAIR OF TESTS COST BEFORE THEY EXISTED. On a normal close the validation layer answered
// `vkDestroyDevice(): VkDevice has 6599 leaked objects that have not been destroyed`
// (VUID-vkDestroyDevice-device-05137), and the census by type was not one kind but eleven:
//
//     VkBuffer 5716, VkImageView 265, VkCommandBuffer 211, VkImage 104, VkSampler 100,
//     VkDeviceMemory 86, VkRenderPass 66, VkFramebuffer 33, VkCommandPool 9, VkSemaphore 6, VkFence 3.
//
// Three owners, and NOT ONE of them was a forgotten line inside a release function:
//
//   * 5919 sat in VulkanAllocator's deferred-deletion queue. RT_Destroy* only ENQUEUES, and the queue is
//     drained from exactly one place -- VulkanQueue::Present. Shutdown releases the engine's entire
//     content AFTER the last frame, so there was no frame left to collect any of it on.
//   * 220 were CommandBufferAllocator's nine command pools and the one-off buffers taken from them. That
//     class had no teardown at all, and FlushCommandBuffer's pool parameter was commented out.
//   * 9 were VulkanQueue's semaphores and fences. `VulkanQueue::Release()` existed, was public, was
//     correct -- and was called from nowhere.
//
// The last one is the shape worth naming: a release function that is right and unreachable looks exactly
// like a release function that runs. `RendererContext::Shutdown()` was the same, one level up: a pure
// virtual with a working Vulkan body and no caller anywhere in the tree.

// RELATION: every teardown entry point in the Vulkan backend is REACHED, and the one root that reaches
// them is VulkanLogicalDevice::Destroy -- the last moment at which the VkDevice is still alive.
//
// A NAMED REGISTER and not a count, for the reason `pin a register, not a count` gives: a number can be
// satisfied by editing the number, and each row here is a decision about who releases what.
TEST( TeardownOrder, TheDeviceTeardownReachesEveryVulkanOwner )
{
    struct Link
    {
        const char* CallerFile;
        const char* CallerSignature;
        const char* Call;
        const char* What;
    };

    const Link links[] = {
         { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanDevice.cpp", "void VulkanLogicalDevice::Destroy",
           "Shutdown()", "the renderer context, and through it everything below" },
         { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanContext.cpp", "void VulkanContext::Shutdown",
           "DrainDeletionQueue()", "every destruction that was deferred and has no frame left to run on" },
         { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanContext.cpp", "void VulkanContext::Shutdown",
           "CommandBufferAllocator::GetInstance().Destroy()", "the nine command pools and their buffers" },
         { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanContext.cpp", "void VulkanContext::Shutdown",
           "m_VulkanAllocator->Shutdown()", "the VMA allocator itself" },
         { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanQueue.cpp", "VulkanQueue::~VulkanQueue",
           "Release()", "the frame semaphores and the wait fences" },
         { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanQueue.cpp", "void VulkanQueue::Present",
           "ProcessDeletionQueue()", "the per-frame drain, which is what makes the queue bounded at all" },
    };

    for ( const Link& link : links )
    {
        const std::string source = ReadFile( RepoRoot() / link.CallerFile );
        ASSERT_FALSE( source.empty() ) << link.CallerFile << " not found or empty";

        const std::string body = FunctionBody( source, link.CallerSignature );
        ASSERT_FALSE( body.empty() ) << link.CallerSignature << " is not in " << link.CallerFile
                                     << " any more. A row naming a function that no longer exists passes "
                                        "without checking anything -- fix the row or the code.";

        EXPECT_NE( body.find( link.Call ), std::string::npos )
             << link.CallerSignature << " no longer calls " << link.Call << ", so nothing releases "
             << link.What << ". This is not a crash and not a test failure anywhere else: it is "
             << "`vkDestroyDevice(): VkDevice has N leaked objects` and an otherwise clean exit.";
    }
}

// RELATION: the order INSIDE the two teardown bodies, which is where "it is called" stops being enough.
//
// Both of these are silent when wrong. Releasing children after vkDestroyDevice is undefined behaviour
// against a destroyed device; destroying the VMA allocator before the queue is drained takes the
// allocations with it and leaves the VkBuffer and VkImage handles built on them behind, which is the
// original leak with an extra step.
TEST( TeardownOrder, TheTeardownStepsAreOrderedAgainstTheThingTheyOutlive )
{
    {
        const std::string source = ReadFile(
             RepoRoot() / "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanDevice.cpp" );
        const std::string body = FunctionBody( source, "void VulkanLogicalDevice::Destroy" );
        ASSERT_FALSE( body.empty() ) << "VulkanLogicalDevice::Destroy has no body";

        const size_t releaseChildren = body.find( "Shutdown()" );
        const size_t destroyDevice   = body.find( "vkDestroyDevice" );
        ASSERT_NE( releaseChildren, std::string::npos ) << "nothing releases the device's children here";
        ASSERT_NE( destroyDevice, std::string::npos ) << "VulkanLogicalDevice::Destroy no longer destroys "
                                                         "the device; this test pins nothing";
        EXPECT_LT( releaseChildren, destroyDevice )
             << "the device's children are released AFTER vkDestroyDevice, which is the same as not "
                "releasing them -- every handle passed is a child of a device that no longer exists";
    }

    {
        const std::string source = ReadFile(
             RepoRoot() / "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanContext.cpp" );
        const std::string body = FunctionBody( source, "void VulkanContext::Shutdown" );
        ASSERT_FALSE( body.empty() ) << "VulkanContext::Shutdown has no body";

        const size_t drain        = body.find( "DrainDeletionQueue()" );
        const size_t destroyVma   = body.find( "m_VulkanAllocator->Shutdown()" );
        ASSERT_NE( drain, std::string::npos ) << "the deferred-deletion queue is never drained here";
        ASSERT_NE( destroyVma, std::string::npos ) << "the VMA allocator is never destroyed here";
        EXPECT_LT( drain, destroyVma )
             << "vmaDestroyAllocator runs before the deletion queue is drained: it frees the allocations "
                "and leaves the VkBuffer/VkImage handles built on them for vkDestroyDevice to report";
    }
}

// RELATION: every kind of device object the engine's OWN Vulkan backend creates has, somewhere in that
// same backend, the call that destroys that kind.
//
// DERIVED FROM THE SOURCE, NOT TYPED. A new `vkCreateX` introduces the obligation by existing, so this
// cannot fall behind the way a hand-kept list of owners does -- and it is the check that would have
// caught the command pools on the day they were written: `vkCreateCommandPool` appeared three times in
// this tree and `vkDestroyCommandPool` zero, for as long as the engine has had a Vulkan backend.
//
// The exemptions below are OBJECTS THAT ARE FREED BY DESTROYING SOMETHING ELSE, or that outlive the
// device on purpose. Each is a decision with a reason, which is why they are rows rather than a filter.
TEST( TeardownOrder, EveryVulkanObjectTheEngineCreatesHasADestroyCall )
{
    struct Pair
    {
        const char* Create;
        const char* Destroy; ///< nullptr: exempt, and Why says by what
        const char* Why;
    };

    // Ordered as the Vulkan header orders them; the value is the obligation, not the order.
    const Pair pairs[] = {
         { "vkCreateCommandPool", "vkDestroyCommandPool", "" },
         { "vkAllocateCommandBuffers", "vkFreeCommandBuffers", "" },
         { "vkCreateFence", "vkDestroyFence", "" },
         { "vkCreateSemaphore", "vkDestroySemaphore", "" },
         { "vkCreateQueryPool", "vkDestroyQueryPool", "" },
         { "vkCreateRenderPass", "vkDestroyRenderPass", "" },
         { "vkCreateFramebuffer", "vkDestroyFramebuffer", "" },
         { "vkCreateImageView", "vkDestroyImageView", "" },
         { "vkCreateSampler", "vkDestroySampler", "" },
         { "vkCreateShaderModule", "vkDestroyShaderModule", "" },
         { "vkCreatePipelineLayout", "vkDestroyPipelineLayout", "" },
         { "vkCreateGraphicsPipelines", "vkDestroyPipeline", "" },
         { "vkCreateComputePipelines", "vkDestroyPipeline", "" },
         { "vkCreatePipelineCache", "vkDestroyPipelineCache", "" },
         { "vkCreateDescriptorPool", "vkDestroyDescriptorPool", "" },
         { "vkCreateDescriptorSetLayout", "vkDestroyDescriptorSetLayout", "" },
         { "vkCreateSwapchainKHR", "vkDestroySwapchainKHR", "" },
         { "vkCreateDevice", "vkDestroyDevice", "" },

         { "vkAllocateDescriptorSets", nullptr,
           "descriptor sets are freed by vkDestroyDescriptorPool; the pools are created without "
           "VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, so vkFreeDescriptorSets is not even legal "
           "on them" },
         { "vkCreateImage", nullptr,
           "every VkImage in this backend is created through vmaCreateImage's own path or handed over to "
           "the allocator, and released by vmaDestroyImage" },
         { "vkCreateInstance", nullptr,
           "THE INSTANCE OUTLIVES EVERYTHING ON PURPOSE and is never destroyed: it is a process-lifetime "
           "static (VulkanContext::s_VulkanInstance). It is not a child of the device, so it is invisible "
           "to VUID-vkDestroyDevice-device-05137 -- a separate, deliberate remainder, not this one" },
         { "vkCreateDebugReportCallbackEXT", nullptr,
           "a child of the instance above and with the same lifetime; destroying it before the instance "
           "would silence the validation output during the very teardown it is there to watch" },
    };

    // The engine's OWN backend. VulkanUtils/lightweightvk is vendored third-party code sitting inside
    // this tree, and its create/destroy pairing is not this repository's to answer for.
    const std::vector<std::filesystem::path> trees = {
         RepoRoot() / "Desert/Desert/Source/Engine/Graphic/API/Vulkan",
         RepoRoot() / "Desert/Desert/Source/Engine/ShaderResources/API/Vulkan" };

    std::string backend;
    size_t      files = 0;
    for ( const auto& tree : trees )
    {
        ASSERT_TRUE( std::filesystem::exists( tree ) ) << tree.string() << " does not exist";
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( tree ) )
        {
            const auto& p = entry.path();
            if ( !entry.is_regular_file() || ( p.extension() != ".cpp" && p.extension() != ".hpp" ) )
                continue;
            if ( p.string().find( "lightweightvk" ) != std::string::npos )
                continue;
            backend += StripLineComments( ReadFile( p ) );
            backend += '\n';
            ++files;
        }
    }
    ASSERT_GT( files, 20u ) << "read only " << files << " backend sources -- the walk, not the engine, is "
                                                        "what is wrong";

    for ( const Pair& pair : pairs )
    {
        const bool created = backend.find( std::string( pair.Create ) + "(" ) != std::string::npos;
        if ( !created )
            continue; // the engine stopped creating this kind; nothing is owed

        if ( pair.Destroy == nullptr )
        {
            EXPECT_STRNE( pair.Why, "" ) << pair.Create << " is exempt with no reason given";
            continue;
        }

        EXPECT_NE( backend.find( std::string( pair.Destroy ) + "(" ), std::string::npos )
             << pair.Create << " is called in the engine's Vulkan backend and " << pair.Destroy
             << " is called nowhere in it. Every object of that kind outlives the device, and the only "
                "thing that says so is the validation layer's leak report at vkDestroyDevice.";
    }
}

// Only gtest is linked, not gtest_main — every suite in this tree brings its own entry point.
int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
