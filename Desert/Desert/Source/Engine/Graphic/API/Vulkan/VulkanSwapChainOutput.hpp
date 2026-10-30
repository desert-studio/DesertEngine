#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanFrameLoop.hpp>

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    // `class` and not `struct`: the definition in VulkanSwapChain.hpp uses class, and the Microsoft C++ ABI
    // encodes the class-key into the decorated name, so the mismatch is a Windows-only link
    // error waiting for the day this forward declaration is the one a caller sees first.
    class VulkanSwapChain;

    // A WINDOW AS AN OUTPUT OF THE FRAME, and nothing more. The frame itself — its command buffers, its
    // submission and its per-slot fence — belongs to VulkanFrameLoop, which runs the same frame with no output at
    // all (EngineHost). What a window adds: an acquired back buffer (AcquireImage), the two semaphores per frame
    // slot the frame's submission waits and signals (GetFrameOutput), and the present.
    // Never: hand out a command buffer or own a frame fence; a swapchain rebuild touches the frame slots.
    class VulkanSwapChainOutput final
    {
    public:
        VulkanSwapChainOutput( VulkanSwapChain* swapChain );

        /// Release() was PUBLIC AND CALLED FROM NOWHERE — the VkSemaphores of this engine's every session
        /// ended their life inside vkDestroyDevice's leak report. It is a destructor's job and it is one now:
        /// VulkanSwapChain owns this object and dies before the device, so the handles below are still valid
        /// here.
        ~VulkanSwapChainOutput();

        // The frame boundary (Window::PrepareNextFrame, before any of the frame is recorded): a requested rebuild
        // (EVT-2c resize, an OUT_OF_DATE acquire or present) is applied here and nowhere later, so no frame ever
        // records against a swapchain that changed under it. Acquires nothing.
        void ApplyFrameBoundaryRebuild();
        // LATE ACQUIRE (PRESENT-COST; UE: the viewport's back buffer is resolved at the final pass). Called when
        // the frame's last pass imports the back buffer (VulkanRendererAPI::ImportBackBuffer), after the scene
        // update and the scene's graphs were recorded, so the CPU never waits on the display before it has built
        // the frame. Acquires the frame slot's back buffer, signalling the slot's ImageAcquired semaphore; the
        // slot's fence was waited at the end of the previous frame, so the semaphore's previous wait has
        // completed. Once per frame: a second call while an image is held returns true. False when no image is
        // held for this frame: OUT_OF_DATE (the rebuild is requested for the next frame boundary, never applied
        // mid-frame, the frame already recorded against this swapchain) or a failure (logged).
        [[nodiscard]] bool AcquireImage();
        // Whether this frame holds an acquired image (AcquireImage succeeded, Present has not run yet). A frame
        // that never imported the back buffer holds none: it is submitted without the output and not presented.
        [[nodiscard]] bool HasAcquiredImage() const
        {
            return m_Acquired;
        }
        // The current frame slot's wait (the acquired image, at the stage the back buffer is first written)
        // and signal (render complete, waited by Present).
        [[nodiscard]] VulkanFrameOutput GetFrameOutput() const;
        // Presents the held image and gives it up; a frame that holds none has nothing to present.
        void Present();

        Common::ResultStr<VkResult> Init();

        [[nodiscard]] uint32_t GetImageIndex() const
        {
            return m_ImageIndex;
        }

    private:
        void Release();

        Common::ResultStr<VkResult> QueuePresent( VkQueue queue, uint32_t imageIndex, VkSemaphore waitSemaphore );

        uint32_t m_ImageIndex = ~0;
        bool     m_Acquired   = false; // from a successful AcquireImage until its Present

        VulkanSwapChain* m_SwapChain;

        struct Semaphores
        {
            VkSemaphore PresentComplete;
            VkSemaphore RenderComplete;
        };

        std::vector<Semaphores> m_FrameSemaphores;
    };
} // namespace Desert::Graphic::API::Vulkan
