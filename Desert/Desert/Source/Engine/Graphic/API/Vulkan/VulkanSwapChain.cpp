#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp>

#include <Common/Core/DevInstruments.hpp>

#include <algorithm> // std::find — present-mode support probe
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanFramebuffer.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/DeviceLost.hpp>
#include <Engine/Graphic/PixelPack.hpp> // the one packer both readbacks share

#include <cstring> // std::memcpy — the mapped staging buffer

namespace Desert::Graphic::API::Vulkan
{
    VulkanSwapChain::VulkanSwapChain( const GLFWwindow* window ) : SwapChain( window )
    {
    }

    VulkanSwapChain::~VulkanSwapChain()
    {
        Release();
    }

    void VulkanSwapChain::Init( const VkInstance instance, const std::shared_ptr<Engine::Device>& device )
    {
        const auto vkLogicalDevice = SP_CAST( VulkanLogicalDevice, device );
        m_LogicalDevice            = std::weak_ptr<VulkanLogicalDevice>( vkLogicalDevice );

        InitSurface( const_cast<GLFWwindow*>( m_Window ), instance );
        GetImageFormatAndColorSpace( vkLogicalDevice );
    }

    void VulkanSwapChain::InitSurface( GLFWwindow* window, const VkInstance instance )
    {
        if ( m_Surface == VK_NULL_HANDLE )
        {
            // Its VkResult went on the floor. A surface that failed to appear leaves m_Surface at
            // VK_NULL_HANDLE, and every capability query, format probe and swapchain creation below is then
            // asked about nothing — a run of validation errors none of which names the surface. This does
            // not go through NoteIfDeviceLost: it is a GLFW/platform failure at startup, before there is a
            // device to lose, and it really is an invariant.
            const VkResult surfaced = glfwCreateWindowSurface( instance, window, nullptr, &m_Surface );
            DESERT_VERIFY( surfaced == VK_SUCCESS, "glfwCreateWindowSurface failed: {}",
                           VkResultToString( surfaced ) );
        }
    }

    Common::ResultStr<bool> VulkanSwapChain::CreateSwapChain( const std::shared_ptr<Engine::Device>& device,
                                                              uint32_t* width, uint32_t* height )
    {
        // A swapchain cannot be built on a device that no longer exists, and TRYING is what killed the
        // process: `VkResult is 'VK_ERROR_DEVICE_LOST' in VulkanSwapChain.cpp:165` was the last line of the
        // crash, an abort inside VK_CHECK_RESULT over a result that was entirely expected by then.
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError<bool>(
                 "the device is lost; refusing to build a swapchain on it. See the [DeviceLost] line above "
                 "for the cause — this is a consequence of it, not a separate failure." );

        const auto vkLogicalDevice = SP_CAST( VulkanLogicalDevice, device );
        const auto& lDevice = vkLogicalDevice->GetVulkanLogicalDevice();

        const VkInstance instance =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanInstance();
        
        Init( instance, device );

        if ( m_VkRenderPass == VK_NULL_HANDLE )
        {
            const auto pass = CreateSwapChainRenderPass();
            if ( !pass.IsSuccess() )
                return Common::MakeFormattedError<bool>( "the swapchain render pass: {}", pass.GetError() );
        }

        auto oldSwapchain = m_SwapChain;

        const auto& pDevice = vkLogicalDevice->GetPhysicalDevice()->GetVulkanPhysicalDevice();
        
        VkSurfaceCapabilitiesKHR surfCaps;
        VK_CHECK_RESULT( vkGetPhysicalDeviceSurfaceCapabilitiesKHR( pDevice, m_Surface, &surfCaps ) );

        // maxImageCount == 0 means "no upper limit". Passing hi < lo to std::clamp is undefined
        // behaviour, so substitute the desired count as the ceiling in that case.
        const uint32_t desiredImageCount = surfCaps.minImageCount + 1;
        const uint32_t maxImageCount =
             surfCaps.maxImageCount > 0 ? surfCaps.maxImageCount : desiredImageCount;
        uint32_t numberOfSwapChainImages =
             std::clamp( desiredImageCount, surfCaps.minImageCount, maxImageCount );

        // Pick the present mode from what the surface ACTUALLY supports (hardcoding MAILBOX tripped a
        // validation error on MoltenVK, which offers only FIFO + IMMEDIATE).
        //  VSync ON  -> MAILBOX (low-latency triple buffering) when available, else FIFO.
        //  VSync OFF -> IMMEDIATE, the only mode that is NOT paced by the display: both MAILBOX and FIFO
        //               present at the monitor's refresh rate, so without this the frame rate is pinned to
        //               the refresh of whichever monitor the window sits on regardless of the VSync flag.
        // FIFO is the fallback everywhere — it is the only mode the spec guarantees exists.
        VkPresentModeKHR swapchainPresentMode = VK_PRESENT_MODE_FIFO_KHR;
        {
            uint32_t presentModeCount = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR( pDevice, m_Surface, &presentModeCount, nullptr );
            std::vector<VkPresentModeKHR> presentModes( presentModeCount );
            if ( presentModeCount > 0 )
                vkGetPhysicalDeviceSurfacePresentModesKHR( pDevice, m_Surface, &presentModeCount,
                                                           presentModes.data() );

            const auto supports = [&presentModes]( VkPresentModeKHR mode )
            { return std::find( presentModes.begin(), presentModes.end(), mode ) != presentModes.end(); };

            if ( !m_VSync && supports( VK_PRESENT_MODE_IMMEDIATE_KHR ) )
                swapchainPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if ( supports( VK_PRESENT_MODE_MAILBOX_KHR ) )
                swapchainPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        }

        LOG_INFO( "[SwapChain] Present mode: {} (VSync {})",
                  swapchainPresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE"
                  : swapchainPresentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX"
                                                                        : "FIFO",
                  m_VSync ? "on" : "off" );

        VkSurfaceTransformFlagsKHR preTransform;
        if ( surfCaps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR )
            preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        else
            preTransform = surfCaps.currentTransform;

        // THE SURFACE DECIDES THE EXTENT, which is why a rebuild at the window's last known size still ends
        // up 0x0 on a minimised window: whatever the caller asked for is overwritten here. Both halves of that
        // rule now live in Graphic::ResolveSurfaceExtent, so "minimised" is a case a test can state without a
        // device.
        const Graphic::ViewExtent resolved = Graphic::ResolveSurfaceExtent(
             Graphic::ViewExtent{ surfCaps.currentExtent.width, surfCaps.currentExtent.height },
             Graphic::ViewExtent{ *width, *height } );

        // REFUSED BEFORE ANYTHING IS CREATED. A 0-pixel extent reached vmaCreateImage in
        // CreateColorAndDepthImages below, which refuses it with VK_ERROR_INITIALIZATION_FAILED -- and that
        // refusal goes through VK_CHECK_RESULT, which breaks into the debugger. That was the reported crash on
        // minimising the editor on Windows. Rebuild() already declines to get this far, so reaching here means
        // the surface lost its area between that check and this one; the frame loop retries.
        if ( !Graphic::IsUsableViewExtent( resolved ) )
            return Common::MakeFormattedError<bool>(
                 "the surface reports a {}x{} extent, which no swapchain image can be created at (a minimised "
                 "window reports 0x0); refusing to build a swapchain until it has drawable area again.",
                 resolved.Width, resolved.Height );

        const VkExtent2D swapchainExtent = { resolved.Width, resolved.Height };
        *width                           = resolved.Width;
        *height                          = resolved.Height;

        m_Width  = *width;
        m_Height = *height;

        VkSwapchainCreateInfoKHR swapChainCreateInfo{};
        swapChainCreateInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapChainCreateInfo.surface          = m_Surface;
        swapChainCreateInfo.minImageCount    = numberOfSwapChainImages;
        swapChainCreateInfo.imageColorSpace  = m_ColorSpace;
        swapChainCreateInfo.imageFormat      = m_ColorFormat;
        swapChainCreateInfo.presentMode      = swapchainPresentMode;
        swapChainCreateInfo.imageExtent      = swapchainExtent;
        swapChainCreateInfo.imageArrayLayers = 1;
        // TRANSFER_SRC IS ASKED FOR, NOT ASSUMED, and the answer is remembered rather than re-derived.
        //
        // Reading a presented frame back is the only way to photograph the INTERFACE: ImGui is recorded
        // into the swapchain render pass (VulkanImGuiLayer::End), so the scene's own final image — which is
        // what every capture in this engine read until now — contains no panel, no menu and no dialog.
        // Copying out of a swapchain image requires this usage bit, and the spec does not promise a surface
        // supports it.
        //
        // A surface that refuses it must produce a NAMED refusal at the point of capture, never a silent
        // fall back to the scene image: a picture of the 3D viewport delivered under the name of a picture
        // of the editor is evidence of the wrong subject, which is worse than no evidence at all.
        m_SupportsFrameReadback = ( surfCaps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT ) != 0;
        if ( !m_SupportsFrameReadback )
        {
            LOG_WARN( "[SwapChain] the surface does not support TRANSFER_SRC; presented frames cannot be "
                      "read back, so full-window capture will refuse rather than substitute the viewport." );
        }

        swapChainCreateInfo.imageUsage = ( VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT );
        if ( m_SupportsFrameReadback )
            swapChainCreateInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        swapChainCreateInfo.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE;
        swapChainCreateInfo.queueFamilyIndexCount = 0;
        swapChainCreateInfo.pQueueFamilyIndices   = NULL;
        swapChainCreateInfo.preTransform          = (VkSurfaceTransformFlagBitsKHR)preTransform;
        swapChainCreateInfo.clipped               = VK_TRUE;
        swapChainCreateInfo.compositeAlpha        = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapChainCreateInfo.oldSwapchain          = oldSwapchain;

        // NOT VK_CHECK_RESULT, and this is the line the whole task is named after. This function has an
        // error channel of its own, so a failure here can be REPORTED instead of aborting the process; the
        // macro's abort is for invariants, and "the device died while we were rebuilding" is not one.
        const VkResult created = vkCreateSwapchainKHR( lDevice, &swapChainCreateInfo, nullptr, &m_SwapChain );
        if ( created != VK_SUCCESS )
        {
            (void)NoteIfDeviceLost( created, "vkCreateSwapchainKHR", __FILE__, __LINE__ );
            return Common::MakeFormattedError<bool>( "vkCreateSwapchainKHR failed: {}",
                                                     VkResultToString( created ) );
        }

        if ( oldSwapchain != VK_NULL_HANDLE )
        {
            for ( auto& imageView : m_SwapChainImages.ImagesView )
                vkDestroyImageView( lDevice, imageView, nullptr );
            vkDestroySwapchainKHR( lDevice, oldSwapchain, nullptr );
        }

        uint32_t swapChainImagesCount = 0u;
        VK_CHECK_RESULT( vkGetSwapchainImagesKHR( lDevice, m_SwapChain, &swapChainImagesCount, VK_NULL_HANDLE ) );
        m_SwapChainImages.Images.resize( swapChainImagesCount );
        m_SwapChainImages.ImagesView.resize( swapChainImagesCount );

        if ( !Engine::FrameManager::GetInstance().AdoptSwapchainImageCount( swapChainImagesCount ) )
            return Common::MakeFormattedError<bool>(
                 "the rebuilt swapchain has {} image(s) where the first had {}; the per-frame semaphores, fences "
                 "and command buffers are sized by the first count and cannot follow a change.",
                 swapChainImagesCount, Engine::FrameManager::GetInstance().GetMaxFramesInFlight() );
        VK_CHECK_RESULT( vkGetSwapchainImagesKHR( lDevice, m_SwapChain, &swapChainImagesCount, m_SwapChainImages.Images.data() ) );

        for ( uint32_t i = 0; i < swapChainImagesCount; i++ )
        {
            const auto& createdImageView =
                 Utils::CreateImageView( lDevice, m_SwapChainImages.Images[i],
                                         m_ColorFormat, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, 1U, 1U );
            if ( !createdImageView.IsSuccess() ) return Common::MakeError<bool>( createdImageView.GetError() );
            m_SwapChainImages.ImagesView[i] = createdImageView.GetValue();
        }

        const auto attachments = CreateColorAndDepthImages( vkLogicalDevice );
        if ( !attachments.IsSuccess() )
            return Common::MakeFormattedError<bool>( "the swapchain colour/depth attachments: {}",
                                                     attachments.GetError() );

        const auto framebuffers = CreateSwapChainFramebuffers();
        if ( !framebuffers.IsSuccess() )
            return Common::MakeFormattedError<bool>( "the swapchain framebuffers: {}", framebuffers.GetError() );

        if ( !m_VulkanQueue )
        {
            m_VulkanQueue = std::make_unique<VulkanQueue>( this );
            m_VulkanQueue->Init();
        }

        FramebufferSpecification fbSpec;
        fbSpec.Width = m_Width;
        fbSpec.Height = m_Height;
        fbSpec.DebugName = "SwapchainFramebufferWrapper";
        // BGRA8F to match the actual swapchain colour format (VK_FORMAT_B8G8R8A8_UNORM). This wrapper's render
        // pass is what the runtime builds its present/UI pipelines against, so its attachment format must match
        // the swapchain pass BeginSwapChainRenderPass draws into — else the pipeline is render-pass-incompatible
        // (was (ImageFormat)0 == RGBA8F, tripping VUID-vkCmdDraw-renderPass-02684 on the SwapchainBlit pipeline).
        fbSpec.Attachments.Attachments = { Core::Formats::ImageFormat::BGRA8F };
        fbSpec.PresentTarget           = true; // build its render pass to match the actual present pass
        m_CompositeFramebuffer = std::make_shared<VulkanFramebuffer>( fbSpec );
        // The wrapper whose render pass every present/UI pipeline is built against. A failure here is not
        // a degraded frame, it is a pipeline compiled against nothing — so the swapchain refuses, which
        // it already does for the framebuffers above and could not for this one until RT_Invalidate was
        // made NO_DISCARD.
        const auto composite =
             std::static_pointer_cast<VulkanFramebuffer>( m_CompositeFramebuffer )->RT_Invalidate();
        if ( !composite.IsSuccess() )
            return Common::MakeFormattedError<bool>( "the swapchain composite framebuffer: {}",
                                                     composite.GetError() );

        return Common::MakeSuccess( true );
    }

    Common::ResultStr<bool>
    VulkanSwapChain::GetImageFormatAndColorSpace( const std::shared_ptr<VulkanLogicalDevice>& device )
    {
        VkPhysicalDevice physicalDevice = device->GetPhysicalDevice()->GetVulkanPhysicalDevice();
        // Zero-initialised for the reason spelled out in VulkanContext::CreateVKInstance: the result of
        // the enumeration below is dropped, so the count must be defined even when it fails.
        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, m_Surface, &formatCount, nullptr );
        if ( !formatCount ) return Common::MakeError<bool>( "null format count" );

        std::vector<VkSurfaceFormatKHR> surfaceFormats( formatCount );
        vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, m_Surface, &formatCount, surfaceFormats.data() );

        if ( ( formatCount == 1 ) && ( surfaceFormats[0].format == VK_FORMAT_UNDEFINED ) )
        {
            m_ColorFormat = VK_FORMAT_B8G8R8A8_UNORM;
            m_ColorSpace  = surfaceFormats[0].colorSpace;
        }
        else
        {
            bool found = false;
            for ( auto&& surfaceFormat : surfaceFormats )
            {
                if ( surfaceFormat.format == VK_FORMAT_B8G8R8A8_UNORM )
                {
                    m_ColorFormat = surfaceFormat.format;
                    m_ColorSpace  = surfaceFormat.colorSpace;
                    found = true;
                    break;
                }
            }
            if ( !found )
            {
                m_ColorFormat = surfaceFormats[0].format;
                m_ColorSpace  = surfaceFormats[0].colorSpace;
            }
        }
        return BOOLSUCCESS;
    }

    Common::ResultStr<Graphic::AcquireStatus>
    VulkanSwapChain::AcquireNextImage( VkSemaphore presentCompleteSemaphore, uint32_t* imageIndex )
    {
        // "vkAcquireNextImageKHR(): Semaphore must not have any pending operations" is what this line
        // produced after the device died — a message that reads as OUR synchronisation defect and is
        // nothing of the sort. The acquire simply must not be issued.
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError<Graphic::AcquireStatus>(
                 "the device is lost; refusing to acquire an image." );

        const auto vkLogicalDevice = m_LogicalDevice.lock();
        if ( !vkLogicalDevice ) DESERT_VERIFY( false );

        // SUBOPTIMAL IS AN IMAGE, NOT A FAILURE (see Engine/Graphic/SwapchainAcquire.hpp). It used to leave
        // through VK_RETURN_RESULT as an error, and the caller then rebuilt and acquired again on the
        // semaphore this call had already signalled -- the GPU timeout every editor launch died of.
        const VkResult res =
             vkAcquireNextImageKHR( vkLogicalDevice->GetVulkanLogicalDevice(), m_SwapChain, UINT64_MAX,
                                    presentCompleteSemaphore, VK_NULL_HANDLE, imageIndex );
        switch ( res )
        {
            case VK_SUCCESS:
                return Common::MakeSuccess( Graphic::AcquireStatus::Acquired );
            case VK_SUBOPTIMAL_KHR:
                return Common::MakeSuccess( Graphic::AcquireStatus::AcquiredSuboptimal );
            case VK_ERROR_OUT_OF_DATE_KHR:
                return Common::MakeSuccess( Graphic::AcquireStatus::OutOfDate );
            default:
                (void)NoteIfDeviceLost( res, "vkAcquireNextImageKHR", __FILE__, __LINE__ );
                return Common::MakeFormattedError<Graphic::AcquireStatus>( "vkAcquireNextImageKHR failed: {}",
                                                                           VkResultToString( res ) );
        }
    }

    bool VulkanSwapChain::HasDrawableSurfaceArea( const Graphic::ViewExtent& requested ) const
    {
        const auto vkLogicalDevice = m_LogicalDevice.lock();
        if ( !vkLogicalDevice )
            return false;

        VkSurfaceCapabilitiesKHR surfCaps{};
        // NOT VK_CHECK_RESULT: this is asked on the teardown-and-rebuild path, where a surface that has gone
        // away is a reason to skip the rebuild rather than to abort the process.
        const VkResult queried = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
             vkLogicalDevice->GetPhysicalDevice()->GetVulkanPhysicalDevice(), m_Surface, &surfCaps );
        if ( queried != VK_SUCCESS )
            return false;

        return Graphic::IsUsableViewExtent( Graphic::ResolveSurfaceExtent(
             Graphic::ViewExtent{ surfCaps.currentExtent.width, surfCaps.currentExtent.height }, requested ) );
    }

    void VulkanSwapChain::OnResize( uint32_t width, uint32_t height )
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return;

        // A MINIMISED WINDOW IS WAITED OUT, AND SILENTLY. Minimising is a normal state, not a failure, so it
        // must not log once per frame for as long as the window stays in the taskbar; Rebuild() refuses it by
        // name for every other caller, and this is the one that would turn the refusal into a log flood.
        if ( !HasDrawableSurfaceArea( Graphic::ViewExtent{ width, height } ) )
            return;

        // THE RESULT WAS DISCARDED HERE. A swapchain that failed to rebuild left every handle below stale
        // and the frame loop carried on regardless — including on the device-lost path, where the rebuild
        // is exactly what must not proceed.
        const auto recreated = Rebuild( width, height );
        if ( !recreated.IsSuccess() )
            LOG_ERROR( "[SwapChain] resize to {}x{} failed: {}", width, height, recreated.GetError() );
    }

    Common::ResultStr<bool> VulkanSwapChain::Rebuild( uint32_t width, uint32_t height )
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError<bool>( "the device is lost; refusing to rebuild the swapchain." );

        const auto device = m_LogicalDevice.lock();
        if ( !device )
            DESERT_VERIFY( false );

        // ASKED BEFORE Release(), and the ORDER is the fix. Release() destroys every image, view, framebuffer
        // and the swapchain itself; CreateSwapChain then cannot replace them at a 0x0 extent, so a rebuild
        // attempted while the window is minimised left the swapchain torn down with nothing to rebuild it
        // from -- on top of the vmaCreateImage break that got there first. Nothing is released until the
        // surface is known to have area.
        if ( !HasDrawableSurfaceArea( Graphic::ViewExtent{ width, height } ) )
            return Common::MakeFormattedError<bool>(
                 "the window has no drawable area (the surface reports a zero extent, as a minimised window "
                 "does); refusing to release and rebuild the swapchain until it does." );

        // Release waits for the device to go idle, so nothing still reads the images being destroyed.
        Release();
        auto recreated = CreateSwapChain( device, &width, &height );
        if ( !recreated.IsSuccess() )
            return recreated;

        // NO LAYOUT TRANSITION OF THE NEW IMAGES HERE, and there used to be one: UNDEFINED -> PRESENT_SRC
        // on every image, submitted right after the rebuild. None of them is acquired at that point, and
        // touching a presentable image outside acquire..present is illegal ("performs a layout transition on
        // presentable VkImage, but the image has not been acquired", once per image). It was never needed:
        // the swapchain render pass starts from UNDEFINED and clears, so each image gets its layout inside
        // the frame that acquired it.
        return BOOLSUCCESS;
    }

    void VulkanSwapChain::Release()
    {
        const auto vkLogicalDevice = m_LogicalDevice.lock();
        if ( !vkLogicalDevice ) return;

        const auto& device = vkLogicalDevice->GetVulkanLogicalDevice();
        // Skipped on a lost device: there is no outstanding work to wait for, the call can only answer
        // VK_ERROR_DEVICE_LOST, and everything below it is a vkDestroy*, which the specification keeps
        // legal precisely so that a lost device can still be torn down.
        if ( Graphic::DeviceLost::AllowWork() )
            vkDeviceWaitIdle( device );

        if ( m_SwapChain != VK_NULL_HANDLE )
        {
            for ( auto& view : m_SwapChainImages.ImagesView ) vkDestroyImageView( device, view, nullptr );
            vkDestroySwapchainKHR( device, m_SwapChain, nullptr );
            m_SwapChain = VK_NULL_HANDLE;
        }

        for ( auto fb : m_SwapChainFramebuffers ) vkDestroyFramebuffer( device, fb, nullptr );
        m_SwapChainFramebuffers.clear();

        // A CAPTURE RECORDED AND NEVER COLLECTED IS AN OWNED ALLOCATION WITH NO OWNER LEFT TO COLLECT IT.
        // `m_CaptureAllocation` is one of the two raw pointers in this class that genuinely OWN what they
        // point at (a VMA handle, which has no C++ destructor), and the only release path was inside
        // TakeCapturedFrameRGBA8 — so a capture recorded on the frame that then RESIZED the window, or one
        // recorded on the last frame before shutdown, kept a full frame of GPU_TO_CPU memory until the
        // process ended. Release() runs on both of those paths; TakeCapturedFrameRGBA8 runs on neither.
        // Found by the pointer-ownership census (Desert/Tests/Engine/PointerOwnership) asking who was
        // obliged to destroy it. A8.
        if ( m_CaptureStaging != VK_NULL_HANDLE )
        {
            // Same late-teardown hazard as the colour/depth pair below: with no renderer context there is
            // no allocator to reach, and the device's own destruction takes the buffer with it.
            const auto ctx = EngineContext::GetInstance().GetRendererContext();
            if ( ctx )
                SP_CAST( VulkanContext, ctx )
                     ->GetVulkanAllocator()
                     ->RT_DestroyBuffer( m_CaptureStaging, static_cast<VmaAllocation>( m_CaptureAllocation ) );
            m_CaptureStaging    = VK_NULL_HANDLE;
            m_CaptureAllocation = nullptr;
            m_CaptureWidth      = 0;
            m_CaptureHeight     = 0;
        }

        if ( m_VkRenderPass != VK_NULL_HANDLE )
        {
            vkDestroyRenderPass( device, m_VkRenderPass, nullptr );
            m_VkRenderPass = VK_NULL_HANDLE;
        }

        if ( m_ColorImages.Image )
        {
            // Same late-teardown hazard as VulkanFramebuffer::Release: ~Application destroys the renderer
            // context BEFORE the window (member order), so this can run with no context — and reaching
            // through a null shared_ptr for the allocator segfaults on exit. Without it, just drop the
            // handles; the GPU objects go with the device.
            const auto   ctx = EngineContext::GetInstance().GetRendererContext();
            VmaAllocator allocator =
                 ctx ? SP_CAST( VulkanContext, ctx )->GetVulkanAllocator()->GetVMAAllocator() : nullptr;
            if ( !allocator )
            {
                vkDestroyImageView( device, m_ColorImages.ImageView, nullptr );
                vkDestroyImageView( device, m_DepthStencilImages.ImageView, nullptr );
                m_VmaAllocation[0] = m_VmaAllocation[1] = nullptr;
                m_ColorImages                           = {};
                m_DepthStencilImages                    = {};
                m_CompositeFramebuffer                  = nullptr;
                return;
            }
            vmaDestroyImage( allocator, m_ColorImages.Image, (VmaAllocation)m_VmaAllocation[0] );
            vkDestroyImageView( device, m_ColorImages.ImageView, nullptr );
            
            vmaDestroyImage( allocator, m_DepthStencilImages.Image, (VmaAllocation)m_VmaAllocation[1] );
            vkDestroyImageView( device, m_DepthStencilImages.ImageView, nullptr );

            m_VmaAllocation[0] = m_VmaAllocation[1] = nullptr;
            m_ColorImages = {}; m_DepthStencilImages = {};
        }

        m_CompositeFramebuffer = nullptr;
    }

    uint32_t VulkanSwapChain::GetCurrentBufferIndex() const { return m_VulkanQueue->GetImageIndex(); }
    void VulkanSwapChain::PrepareFrame() { m_VulkanQueue->PrepareFrame(); }
    void VulkanSwapChain::Present() { m_VulkanQueue->Present(); }

    namespace
    {
        /// The surface layout, in the pack's vocabulary. A format this does not know is REFUSED by the
        /// caller rather than guessed at: guessing wrong exchanges red and blue, and the result reads as a
        /// rendering defect rather than as a capture defect.
        [[nodiscard]] bool SurfaceLayout( VkFormat format, Graphic::PackedPixelSource& out )
        {
            switch ( format )
            {
                case VK_FORMAT_B8G8R8A8_UNORM:
                case VK_FORMAT_B8G8R8A8_SRGB:
                    out = Graphic::PackedPixelSource::BGRA8;
                    return true;
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_R8G8B8A8_SRGB:
                    out = Graphic::PackedPixelSource::RGBA8;
                    return true;
                default:
                    return false;
            }
        }
    } // namespace

// ── THE FRAME CAPTURE'S BODIES, AND WHY ONLY THE BODIES ─────────────────────────────────────────────
//
// These two functions ARE the screenshot machinery: a blit of the presented image into a GPU_TO_CPU
// staging buffer, a device wait, a map and a repack. Roughly 150 lines that exist so a developer can
// photograph a frame, and their only two callers are the editor's `shot.window` and the runtime's
// `--shot` — neither of which is in a shipping build.
//
// WHAT IS NOT CUT, AND THAT IS DELIBERATE. `SupportsFrameReadback()` and the usage flags the swapchain
// images are created with stay exactly as they are in every configuration. Taking
// VK_IMAGE_USAGE_TRANSFER_SRC_BIT off the shipping swapchain would change how the swapchain is CREATED,
// which is a change to the picture's own path — and a boundary that changes the picture is not a
// boundary, it is a second renderer nobody verified. That flag is a task with an argument of its own.
#if DESERT_DEV_INSTRUMENTS

    Common::BoolResultStr VulkanSwapChain::RecordFrameCapture()
    {
        // EVERY refusal below is named, and none of them falls back to another image. The caller asked for
        // a picture of the editor; a picture of something else carrying that name is the failure this whole
        // path exists to make impossible.
        if ( !Graphic::DeviceLost::AllowWork() )
        {
            return Common::MakeError<bool>(
                 "the device is lost, so there is no frame to photograph and no queue to copy it with. The "
                 "editor is shutting down; see the [DeviceLost] line above." );
        }

        if ( !m_SupportsFrameReadback )
        {
            return Common::MakeError<bool>(
                 "the surface was created without TRANSFER_SRC, so a presented frame cannot be copied off "
                 "the device on this driver. Refusing rather than substituting the scene image, which holds "
                 "no interface at all." );
        }

        if ( HasPendingCapture() )
        {
            return Common::MakeError<bool>(
                 "a frame capture is already recorded and not yet collected; one at a time." );
        }

        Graphic::PackedPixelSource source{};
        if ( !SurfaceLayout( m_ColorFormat, source ) )
        {
            return Common::MakeFormattedError<bool>(
                 "the surface format is {}, which this readback does not know how to pack. Refusing rather "
                 "than reinterpreting the bytes as a layout they are not in.",
                 static_cast<int>( m_ColorFormat ) );
        }

        const uint32_t imageIndex = m_VulkanQueue->GetImageIndex();
        if ( imageIndex >= m_SwapChainImages.Images.size() )
        {
            return Common::MakeFormattedError<bool>(
                 "no swapchain image is acquired (index {} of {}); there is nothing to copy.", imageIndex,
                 m_SwapChainImages.Images.size() );
        }

        const uint32_t w = m_Width;
        const uint32_t h = m_Height;
        if ( w == 0u || h == 0u )
            return Common::MakeError<bool>( "the window is zero-sized; there is no frame to capture." );

        auto* allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                               ->GetVulkanAllocator()
                               .get();

        const std::size_t stagingSize = static_cast<std::size_t>( w ) * h * Graphic::BytesPerPixel( source );

        VkBuffer           staging = VK_NULL_HANDLE;
        VkBufferCreateInfo bInfo   = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size  = stagingSize,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        const auto         allocated =
             allocator->RT_AllocateBuffer( "PresentedFrameCapture", bInfo, VMA_MEMORY_USAGE_GPU_TO_CPU, staging );
        if ( !allocated.IsSuccess() )
            return Common::MakeFormattedError<bool>( "capture staging buffer: {}", allocated.GetError() );

        // RECORDED INTO THE FRAME'S OWN COMMAND BUFFER, which is the whole point of this being two calls.
        // Here the image is between its acquire and its present and is legitimately ours to read; after the
        // present it belongs to the presentation engine, and copying out of it there is a spec violation
        // that MoltenVK tolerates silently — the picture comes out correct and only the validation layer
        // ever mentions it. ("vkQueueSubmit(): performs a layout transition on presentable VkImage, but the
        // image has not been acquired from VkSwapchainKHR." Measured, on the first capture this took.)
        VkCommandBuffer cmd   = m_VulkanQueue->GetDrawCommandBuffer();
        VkImage         image = m_SwapChainImages.Images[imageIndex];

        // The render pass left the image in PRESENT_SRC, and it must be put back: the present that follows
        // this submit expects to find it there.
        Utils::InsertImageMemoryBarrier( cmd, image, m_ColorFormat, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );

        VkBufferImageCopy copy = {
             .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
             .imageExtent      = { w, h, 1 } };
        vkCmdCopyImageToBuffer( cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy );

        Utils::InsertImageMemoryBarrier( cmd, image, m_ColorFormat, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR );

        m_CaptureStaging    = staging;
        m_CaptureAllocation = allocated.GetValue();
        m_CaptureWidth      = w;
        m_CaptureHeight     = h;
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<std::vector<uint8_t>> VulkanSwapChain::TakeCapturedFrameRGBA8( uint32_t& outWidth,
                                                                                     uint32_t& outHeight )
    {
        using Bytes = std::vector<uint8_t>;

        if ( !HasPendingCapture() )
            return Common::MakeError<Bytes>( "no frame capture was recorded for this frame." );

        auto* allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                               ->GetVulkanAllocator()
                               .get();

        // Released whatever happens below. A capture that failed and leaked its staging buffer would cost
        // a full frame of device memory per attempt, and the attempts are what a client retries.
        const auto release = [&]()
        {
            allocator->RT_DestroyBuffer( m_CaptureStaging, static_cast<VmaAllocation>( m_CaptureAllocation ) );
            m_CaptureStaging    = VK_NULL_HANDLE;
            m_CaptureAllocation = nullptr;
        };

        Graphic::PackedPixelSource source{};
        if ( !SurfaceLayout( m_ColorFormat, source ) )
        {
            release();
            return Common::MakeError<Bytes>( "the surface format changed between recording and collecting." );
        }

        const uint32_t    w         = m_CaptureWidth;
        const uint32_t    h         = m_CaptureHeight;
        const std::size_t pixels    = static_cast<std::size_t>( w ) * h;
        const std::size_t stagingSz = pixels * Graphic::BytesPerPixel( source );

        Bytes raw( stagingSz );
        {
            MappedMemory readback = allocator->MapMemory( static_cast<VmaAllocation>( m_CaptureAllocation ) );
            const auto   read     = readback.ReadInto( raw.data(), stagingSz );
            if ( !read.IsSuccess() )
            {
                readback.Unmap();
                release();
                return Common::MakeFormattedError<Bytes>( "the capture staging buffer: {}", read.GetError() );
            }
        }
        release();

        Bytes out = Graphic::PackToRGBA8( raw.data(), raw.size(), pixels, source );
        if ( out.size() != pixels * 4u )
        {
            return Common::MakeFormattedError<Bytes>( "the capture packed to {} bytes, expected {}x{}x4 = {}.",
                                                      out.size(), w, h, pixels * 4u );
        }

        // ALPHA IS FORCED OPAQUE, and that is a statement about the source rather than a cosmetic touch.
        // The swapchain is created with VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, so nothing in the pipeline is
        // obliged to leave anything meaningful in the alpha channel of a presented image. Written straight
        // out, a zero there produces a PNG that is entirely transparent — a capture that looks blank in
        // every viewer and reads as "the editor drew nothing".
        for ( std::size_t i = 0; i < pixels; ++i )
            out[i * 4u + 3u] = 255u;

        outWidth  = w;
        outHeight = h;
        return Common::MakeSuccess( std::move( out ) );
    }

#else

    // The overrides still exist, because the base class declares them pure virtual for a host that must
    // not know about Vulkan (SwapChain.hpp). They REFUSE, with the reason, rather than returning an empty
    // buffer: an empty success here is a blank PNG, and this project has already spent a day on the fact
    // that two blank PNGs are byte-identical and a diff over them reports "no change".
    Common::BoolResultStr VulkanSwapChain::RecordFrameCapture()
    {
        return Common::MakeError( "frame capture is not compiled into a Shipping build." );
    }

    Common::ResultStr<std::vector<uint8_t>> VulkanSwapChain::TakeCapturedFrameRGBA8( uint32_t&, uint32_t& )
    {
        return Common::MakeError<std::vector<uint8_t>>( "frame capture is not compiled into a Shipping build." );
    }

#endif // DESERT_DEV_INSTRUMENTS

    Common::ResultStr<VkResult> VulkanSwapChain::CreateSwapChainFramebuffers()
    {
        const auto vkLogicalDevice = m_LogicalDevice.lock();
        if ( !vkLogicalDevice ) DESERT_VERIFY( false );

        m_SwapChainFramebuffers.resize( m_SwapChainImages.ImagesView.size() );
        for ( uint32_t i = 0; i < m_SwapChainFramebuffers.size(); i++ )
        {
            VkImageView attachments[] = { m_SwapChainImages.ImagesView[i] };
            VkFramebufferCreateInfo fbCreateInfo = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = m_VkRenderPass, .attachmentCount = 1, .pAttachments = attachments, .width = m_Width, .height = m_Height, .layers = 1 };
            VK_CHECK_RESULT( vkCreateFramebuffer( vkLogicalDevice->GetVulkanLogicalDevice(), &fbCreateInfo, NULL, &m_SwapChainFramebuffers[i] ) );
        }
        return Common::MakeSuccess( VK_SUCCESS );
    }

    Common::ResultStr<VkResult> VulkanSwapChain::CreateSwapChainRenderPass()
    {
        const auto vkLogicalDevice = m_LogicalDevice.lock();
        if ( !vkLogicalDevice ) DESERT_VERIFY( false );

        VkAttachmentDescription attachment = { .format = m_ColorFormat, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
        VkAttachmentReference colorRef = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &colorRef };
        VkSubpassDependency dependency = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };

        VkRenderPassCreateInfo rpInfo = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass, .dependencyCount = 1, .pDependencies = &dependency };
        VK_RETURN_RESULT( vkCreateRenderPass( vkLogicalDevice->GetVulkanLogicalDevice(), &rpInfo, nullptr, &m_VkRenderPass ) );
    }

    Common::ResultStr<VkResult> VulkanSwapChain::CreateColorAndDepthImages( const std::shared_ptr<VulkanLogicalDevice>& device )
    {
        VmaAllocator allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator()->GetVMAAllocator();
        
        // Color
        VkImageCreateInfo cInfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = m_ColorFormat, .extent = { m_Width, m_Height, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = m_MSAASamples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VmaAllocationCreateInfo cAllocInfo = { .usage = VMA_MEMORY_USAGE_GPU_ONLY };
        VK_CHECK_RESULT( vmaCreateImage( allocator, &cInfo, &cAllocInfo, &m_ColorImages.Image, (VmaAllocation*)&m_VmaAllocation[0], nullptr ) );

        // A refused view used to be stored as VK_NULL_HANDLE and attached to the swap chain's
        // framebuffer anyway; the resulting failure surfaced at framebuffer creation with no mention
        // of the view that never existed.
        auto colorView =
             Utils::CreateImageView( device->GetVulkanLogicalDevice(), m_ColorImages.Image, m_ColorFormat,
                                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, 1, 1 );
        if ( !colorView )
        {
            return Common::MakeFormattedError<VkResult>( "swap chain colour image view: {}",
                                                         colorView.GetError() );
        }
        m_ColorImages.ImageView = colorView.GetValue();

        // Depth
        VkFormat dFormat = device->GetPhysicalDevice()->GetDepthFormat();
        VkImageCreateInfo dInfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = dFormat, .extent = { m_Width, m_Height, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = m_MSAASamples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VmaAllocationCreateInfo dAllocInfo = { .usage = VMA_MEMORY_USAGE_GPU_ONLY };
        VK_CHECK_RESULT( vmaCreateImage( allocator, &dInfo, &dAllocInfo, &m_DepthStencilImages.Image, (VmaAllocation*)&m_VmaAllocation[1], nullptr ) );

        auto depthView = Utils::CreateImageView( device->GetVulkanLogicalDevice(), m_DepthStencilImages.Image,
                                                 dFormat, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D, 1, 1 );
        if ( !depthView )
        {
            return Common::MakeFormattedError<VkResult>( "swap chain depth image view: {}", depthView.GetError() );
        }
        m_DepthStencilImages.ImageView = depthView.GetValue();

        return Common::MakeSuccess( VK_SUCCESS );
    }

} // namespace Desert::Graphic::API::Vulkan
