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

        // Acquires the frame slot's back buffer, signalling the slot's ImageAcquired semaphore. Called after
        // the frame loop waited the slot's fence, so the semaphore's previous wait has completed.
        void AcquireImage();
        // The current frame slot's wait (the acquired image, at the stage the back buffer is first written)
        // and signal (render complete, waited by Present).
        VulkanFrameOutput GetFrameOutput() const;
        void              Present();

        Common::ResultStr<VkResult> Init();

        uint32_t GetImageIndex() const
        {
            return m_ImageIndex;
        }

    private:
        void Release();

        Common::ResultStr<VkResult> QueuePresent( VkQueue queue, uint32_t imageIndex, VkSemaphore waitSemaphore );

    private:
        uint32_t m_ImageIndex = ~0;

        VulkanSwapChain* m_SwapChain;

        struct Semaphores
        {
            VkSemaphore PresentComplete;
            VkSemaphore RenderComplete;
        };

        std::vector<Semaphores> m_FrameSemaphores;
    };
} // namespace Desert::Graphic::API::Vulkan
