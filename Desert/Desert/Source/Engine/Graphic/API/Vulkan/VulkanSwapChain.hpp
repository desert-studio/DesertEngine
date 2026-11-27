#pragma once

#include <Engine/Core/GlfwVulkan.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSwapChainOutput.hpp>
#include <Engine/Graphic/SwapchainAcquire.hpp>

#include <Engine/Graphic/SwapChain.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/PixelPack.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ViewMemory.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <array>

namespace Desert::Graphic::API::Vulkan
{
    class VulkanSwapChain final : public SwapChain
    {
    public:
        VulkanSwapChain( const GLFWwindow* window );
        ~VulkanSwapChain() override;

        void Init( const VkInstance instance, const std::shared_ptr<Engine::Device>& device );

        Common::ResultStr<bool> CreateSwapChain( const std::shared_ptr<Engine::Device>& device, uint32_t* width,
                                              uint32_t* height ) override;

        Common::ResultStr<bool> GetImageFormatAndColorSpace( const std::shared_ptr<VulkanLogicalDevice>& device );

        uint32_t GetBackBufferCount() const override
        {
            return (uint32_t)m_SwapChainImages.Images.size();
        }

        const auto& GetSwapChainVKImage() const
        {
            return m_SwapChainImages.Images;
        }

        const auto& GetSwapChainVKImagesView() const
        {
            return m_SwapChainImages.ImagesView;
        }

        // The engine format the back buffer is drawn as (composite framebuffer, ImportBackBuffer): RGBA16F on the
        // scRGB output (HDR-OUTPUT), RGB10A2 on the 10-bit SDR surface (SDR-10BIT-DITHER), BGRA8 otherwise.
        [[nodiscard]] Core::Formats::ImageFormat BackBufferFormat() const
        {
            if ( m_ColorFormat == VK_FORMAT_R16G16B16A16_SFLOAT )
                return Core::Formats::ImageFormat::RGBA16F;
            if ( m_ColorFormat == VK_FORMAT_A2B10G10R10_UNORM_PACK32 )
                return Core::Formats::ImageFormat::RGB10A2_UNORM;
            return Core::Formats::ImageFormat::BGRA8F;
        }

        VkFormat GetColorFormat() const
        {
            return m_ColorFormat;
        }

        uint32_t GetWidth() const override
        {
            return m_Width;
        }
        uint32_t GetHeight() const override
        {
            return m_Height;
        }

        VkColorSpaceKHR GetColorSpace() const
        {
            return m_ColorSpace;
        }

        const auto& GetOutput() const
        {
            return m_Output;
        }

        void RequestRebuild( uint32_t width, uint32_t height ) override;
        [[nodiscard]] bool RebuildPending() const override
        {
            return m_RebuildRequest.IsPending();
        }

        void Release() override;

        uint32_t GetCurrentBufferIndex() const;
        void     Present();

        /// Whether presented frames can be copied off the device at all — the surface offered
        /// TRANSFER_SRC when the swapchain was created. Exposed so a capture can REFUSE by name instead
        /// of quietly photographing something else; see the usage flags in CreateSwapChain.
        [[nodiscard]] bool SupportsFrameReadback() const noexcept override
        {
            return m_SupportsFrameReadback;
        }

        /**
         * @brief CAPTURING THE COMPOSITED FRAME, in two halves, because a swapchain image may only be
         *        touched between its acquire and its present.
         *
         * WHAT IS BEING CAPTURED. The whole editor as a person sees it — the scene AND the interface drawn
         * over it — because ImGui records into the back buffer through a graph node (VulkanImGuiLayer::End). The
         * scene's own final image, which every capture in this engine read before this existed, contains
         * no interface at all: no panel, no menu, no dialog.
         *
         * WHY TWO HALVES AND NOT ONE CALL AFTER PRESENT. That was the first shape of this, and Vulkan
         * refused it in as many words: "vkQueueSubmit(): performs a layout transition on presentable
         * VkImage, but the image has not been acquired from VkSwapchainKHR". A presentable image belongs to
         * the presentation engine outside the acquire/present window, and copying out of it there is a
         * spec violation that MoltenVK happens to tolerate — the worst kind, because the picture comes out
         * correct and the validation layer is the only thing that ever mentions it.
         *
         * So the copy is RECORDED into the frame's own command buffer, after the interface pass and before
         * the submit, where the image is legitimately ours; and the bytes are COLLECTED after the present,
         * when the copy has actually run.
         */

        /// Record the copy into the frame's command buffer. Call after the frame's present graph (whose
        /// back-buffer import is the late acquire), on the thread that executed it, and before PresentFinalImage
        /// (see SwapChain::RecordFrameCapture). Fails, naming the reason, if the surface cannot be read at all.
        [[nodiscard]] Common::BoolResultStr RecordFrameCapture() override;

        [[nodiscard]] bool HasPendingFrameCapture() const noexcept override
        {
            return m_CaptureStaging != VK_NULL_HANDLE;
        }

        /// Release a recorded capture that will not be collected (the frame it was recorded on was judged and
        /// is not the answer; the swapchain is being released). The caller has waited for the device: the copy
        /// into the staging buffer must be finished. No-op when nothing is recorded.
        void DiscardFrameCapture() override;

        /// Collect what RecordFrameCapture asked for, as tightly packed 8-bit RGBA. Call after the present
        /// that carried the copy; it waits for the device first. Releases the staging buffer either way,
        /// so a failed capture cannot leak one per attempt.
        [[nodiscard]] Common::ResultStr<std::vector<uint8_t>>
        TakeCapturedFrameRGBA8( uint32_t& outWidth, uint32_t& outHeight ) override;

        /// SwapChain's three halves: the ticket is the detached staging (DetachCapturedFrame), held in m_Tickets.
        [[nodiscard]] Common::ResultStr<CapturedFrameTicket> TakeCapturedFrameTicket() override;
        [[nodiscard]] Common::ResultStr<std::vector<uint8_t>>
        ResolveCapturedFrame( const CapturedFrameTicket& ticket ) override;
        void ReleaseCapturedFrame( const CapturedFrameTicket& ticket ) override;

    private:
        /// A collected capture as the surface held it: the staging bytes, before the RGBA8 pack.
        struct CapturedFrame
        {
            std::vector<uint8_t>       Raw;
            uint32_t                   Width  = 0;
            uint32_t                   Height = 0;
            Graphic::PackedPixelSource Source{};
        };

        /// A recorded capture's staging buffer, handed out of the swapchain (WQH p2): ownership moves to the caller,
        /// and the swapchain can record the next capture at once.
        struct CapturedStaging
        {
            VkBuffer                   Buffer     = VK_NULL_HANDLE;
            void*                      Allocation = nullptr; // VmaAllocation
            uint32_t                   Width      = 0;
            uint32_t                   Height     = 0;
            Graphic::PackedPixelSource Source{};
            /// Signalled once the GPU has run everything queued before the detach - the frame that recorded the
            /// copy included (WQH p3; UE: FRHIGPUTextureReadback's fence, IsReady). Read waits it; Release destroys it.
            VkDevice Device = VK_NULL_HANDLE;
            VkFence  Ready  = VK_NULL_HANDLE;
        };

        /// TakeCapturedFrameRGBA8 in three halves, so only the first is the frame's (WQH p2). On a 4112x2580
        /// window the copy out of the staging (~0.3-0.6 s: the readback memory reads slowly on the CPU) and the
        /// RGBA8 pack (~0.85 s) were the shot's frame (UE: FRHIGPUTextureReadback is read and FImageWriteQueue
        /// converts and encodes off the game thread). Detach: on the frame, after the render task handed the frame to
        /// the submit thread - takes the staging out of the swapchain and queues a fence behind the frame (an empty
        /// submit, in queue order); it waits for no GPU work and touches no memory.
        [[nodiscard]] Common::ResultStr<CapturedStaging> DetachCapturedFrame();
        /// Read: on any thread - waits the detach's fence (the copy has landed), then maps, invalidates and copies
        /// the staging out. The buffer must stay alive.
        [[nodiscard]] static Common::ResultStr<CapturedFrame> ReadCapturedStaging( const CapturedStaging& staging );
        /// Release: on the game thread once nothing reads it - the staging goes to the allocator's deferred
        /// deletion. Idempotent.
        static void ReleaseCapturedStaging( CapturedStaging& staging );

        /// The pack half, on any thread: tightly packed 8-bit RGBA, alpha forced opaque (the surface is
        /// VK_COMPOSITE_ALPHA_OPAQUE; nothing is obliged to leave alpha meaningful).
        [[nodiscard]] static Common::ResultStr<std::vector<uint8_t>>
        PackCapturedFrame( const CapturedFrame& frame );

    public:

        [[nodiscard]] std::shared_ptr<::Desert::Graphic::Framebuffer> GetCompositeFramebuffer() const
        {
            return m_CompositeFramebuffer;
        }

        // The image acquired for this frame as a graph external (UE: the viewport's back buffer registered with
        // RegisterExternalTexture each frame): its description, the graph's handle on it (one per swapchain image,
        // kept until the images are rebuilt, so the views a pass binds outlive the frame's recording), and no
        // prior state (an acquired image's contents are undefined). The graph that writes it extracts it as
        // Present.
        [[nodiscard]] Common::BoolResultStr ImportBackBuffer( ::Desert::Graphic::RDG::ExternalTexture& into );

    private:
        void InitSurface( GLFWwindow* window, const VkInstance instance );

    private:
        // ALL FOUR ARE [[nodiscard]] NOW, AND THREE OF THEM WERE NOT. Their results were dropped at every
        // call site in CreateSwapChain — a render pass, a set of framebuffers and the colour/depth pair
        // that could all silently fail to exist on the swapchain rebuild path, which is the exact path a
        // lost device walks. The project's NO_DISCARD discipline covers wrappers like these; these three
        // were simply missed, and nothing but the attribute would have said so.
        [[nodiscard]] Common::ResultStr<bool> Rebuild( uint32_t width, uint32_t height );
        [[nodiscard]] Common::ResultStr<bool> ApplyRequestedRebuild();
        void                                  RequestRebuildAtCurrentSize();
        /// Whether the surface has area a swapchain image can be created at RIGHT NOW. A minimised window
        /// reports 0x0 on Windows, and every object the rebuild makes is sized from that extent.
        [[nodiscard]] bool HasDrawableSurfaceArea( const Graphic::ViewExtent& requested ) const;
        [[nodiscard]] Common::ResultStr<Graphic::AcquireStatus>
        AcquireNextImage( VkSemaphore presentCompleteSemaphore, uint32_t* imageIndex );

    private:
        std::unique_ptr<VulkanSwapChainOutput> m_Output;
        std::weak_ptr<VulkanLogicalDevice> m_LogicalDevice;

        VkSwapchainKHR m_SwapChain = VK_NULL_HANDLE;
        VkSurfaceKHR   m_Surface   = VK_NULL_HANDLE;
        // The instance the surface was created on, kept for vkDestroySurfaceKHR in the destructor.
        VkInstance m_Instance = VK_NULL_HANDLE;

        VkFormat        m_ColorFormat;
        VkColorSpaceKHR m_ColorSpace;

        uint32_t m_Width  = 0u;
        uint32_t m_Height = 0u;

        Graphic::SwapchainRebuildRequest m_RebuildRequest;

        /// Answered once by the surface at creation (CreateSwapChain) rather than re-derived per capture:
        /// the usage flags the images were actually made with are what decide this, and asking the surface
        /// again could answer about a swapchain that no longer exists.
        bool m_SupportsFrameReadback = false;

        /// The staging buffer a recorded capture will land in, alive between RecordFrameCapture and
        /// TakeCapturedFrameRGBA8. Null means no capture is in flight.
        /// `void*` rather than VmaAllocation: the allocator's
        /// header would otherwise have to be visible to everything that includes this file.
        VkBuffer m_CaptureStaging    = VK_NULL_HANDLE;
        void*    m_CaptureAllocation = nullptr;
        uint32_t m_CaptureWidth      = 0;
        uint32_t m_CaptureHeight     = 0;

        /// The detached captures handed out as tickets (TakeCapturedFrameTicket), each until its ReleaseCapturedFrame;
        /// whatever is still held when the swap chain dies is released by the destructor. The lock is the
        /// resolving worker's against the game thread's take and release.
        mutable std::mutex                              m_TicketLock;
        std::unordered_map<uint64_t, CapturedStaging>   m_Tickets;
        uint64_t                                        m_NextTicket = 1;

        struct
        {
            std::vector<VkImage>     Images;
            std::vector<VkImageView> ImagesView;
        } m_SwapChainImages;
        // ImportBackBuffer's handle per swapchain image (index = image index); emptied with the image views.
        std::vector<std::shared_ptr<::Desert::Graphic::RDG::IPhysicalTexture>> m_BackBufferGraphTextures;

        std::shared_ptr<::Desert::Graphic::Framebuffer> m_CompositeFramebuffer;

    private:
        friend class VulkanSwapChainOutput;
    };
} // namespace Desert::Graphic::API::Vulkan
