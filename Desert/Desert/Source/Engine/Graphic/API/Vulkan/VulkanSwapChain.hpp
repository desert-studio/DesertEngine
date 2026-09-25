#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanQueue.hpp>
#include <Engine/Graphic/SwapchainAcquire.hpp>

#include <Engine/Graphic/SwapChain.hpp>
#include <Engine/Graphic/Framebuffer.hpp>

#include <memory>
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

        const auto& GetColorImages() const
        {
            return m_ColorImages;
        }

        const auto& GetDepthImages() const
        {
            return m_DepthStencilImages;
        }

        auto GetRenderPass() const
        {
            return m_VkRenderPass;
        }

        auto GetVKFramebuffers() const
        {
            return m_SwapChainFramebuffers;
        }

        const auto& GetVulkanQueue() const
        {
            return m_VulkanQueue;
        }

        void OnResize( uint32_t width, uint32_t height ) override;

        void Release() override;

        uint32_t GetCurrentBufferIndex() const;
        void     PrepareFrame();
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
         * over it — because ImGui records into the swapchain render pass (VulkanImGuiLayer::End). The
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

        /// Record the copy into the frame's command buffer. Call after the last render pass of the frame
        /// and before PresentFinalImage. Fails, naming the reason, if the surface cannot be read at all.
        [[nodiscard]] Common::BoolResultStr RecordFrameCapture() override;

        [[nodiscard]] bool HasPendingCapture() const noexcept
        {
            return m_CaptureStaging != VK_NULL_HANDLE;
        }

        /// Collect what RecordFrameCapture asked for, as tightly packed 8-bit RGBA. Call after the present
        /// that carried the copy; it waits for the device first. Releases the staging buffer either way,
        /// so a failed capture cannot leak one per attempt.
        [[nodiscard]] Common::ResultStr<std::vector<uint8_t>>
        TakeCapturedFrameRGBA8( uint32_t& outWidth, uint32_t& outHeight ) override;

        [[nodiscard]] std::shared_ptr<::Desert::Graphic::Framebuffer> GetCompositeFramebuffer() const
        {
            return m_CompositeFramebuffer;
        }

    private:
        void InitSurface( GLFWwindow* window, const VkInstance instance );

    private:
        // ALL FOUR ARE [[nodiscard]] NOW, AND THREE OF THEM WERE NOT. Their results were dropped at every
        // call site in CreateSwapChain — a render pass, a set of framebuffers and the colour/depth pair
        // that could all silently fail to exist on the swapchain rebuild path, which is the exact path a
        // lost device walks. The project's NO_DISCARD discipline covers wrappers like these; these three
        // were simply missed, and nothing but the attribute would have said so.
        // OnResize without the log: the acquire path needs to know whether the rebuild happened.
        [[nodiscard]] Common::ResultStr<bool> Rebuild( uint32_t width, uint32_t height );
        [[nodiscard]] Common::ResultStr<Graphic::AcquireStatus>
        AcquireNextImage( VkSemaphore presentCompleteSemaphore, uint32_t* imageIndex );
        [[nodiscard]] Common::ResultStr<VkResult> CreateSwapChainRenderPass();
        [[nodiscard]] Common::ResultStr<VkResult> CreateSwapChainFramebuffers();
        [[nodiscard]] Common::ResultStr<VkResult>
        CreateColorAndDepthImages( const std::shared_ptr<VulkanLogicalDevice>& device );

    private:
        std::unique_ptr<VulkanQueue>       m_VulkanQueue;
        VkSampleCountFlagBits              m_MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        std::weak_ptr<VulkanLogicalDevice> m_LogicalDevice;

        VkSwapchainKHR m_SwapChain = VK_NULL_HANDLE;
        VkSurfaceKHR   m_Surface   = VK_NULL_HANDLE;

        VkFormat        m_ColorFormat;
        VkColorSpaceKHR m_ColorSpace;

        uint32_t m_Width  = 0u;
        uint32_t m_Height = 0u;

        /// Answered once by the surface at creation (CreateSwapChain) rather than re-derived per capture:
        /// the usage flags the images were actually made with are what decide this, and asking the surface
        /// again could answer about a swapchain that no longer exists.
        bool m_SupportsFrameReadback = false;

        /// The staging buffer a recorded capture will land in, alive between RecordFrameCapture and
        /// TakeCapturedFrameRGBA8. Null means no capture is in flight.
        /// `void*` rather than VmaAllocation for the reason m_VmaAllocation below is one: the allocator's
        /// header would otherwise have to be visible to everything that includes this file.
        VkBuffer m_CaptureStaging    = VK_NULL_HANDLE;
        void*    m_CaptureAllocation = nullptr;
        uint32_t m_CaptureWidth      = 0;
        uint32_t m_CaptureHeight     = 0;

        struct
        {
            VkImage     Image;
            VkImageView ImageView;
        } m_ColorImages;

        struct
        {
            VkImage     Image;
            VkImageView ImageView;
        } m_DepthStencilImages;

        struct
        {
            std::vector<VkImage>     Images;
            std::vector<VkImageView> ImagesView;
        } m_SwapChainImages;

        std::vector<VkFramebuffer> m_SwapChainFramebuffers;

        VkRenderPass m_VkRenderPass = VK_NULL_HANDLE;

        std::array<const void*, 2> m_VmaAllocation;

        std::shared_ptr<::Desert::Graphic::Framebuffer> m_CompositeFramebuffer;

    private:
        friend class VulkanQueue;
    };
} // namespace Desert::Graphic::API::Vulkan
