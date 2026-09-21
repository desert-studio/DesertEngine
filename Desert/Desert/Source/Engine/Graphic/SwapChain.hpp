#pragma once

#include <GLFW/glfw3.h>

#include <Engine/Core/Device.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Graphic
{
    class SwapChain
    {
    public:
        SwapChain( const GLFWwindow* window ) : m_Window( window )
        {
        }
        virtual ~SwapChain() = default;

        // TODO: Custom result value like VkResult
        virtual Common::ResultStr<bool> CreateSwapChain( const std::shared_ptr<Engine::Device>& device,
                                                      uint32_t* width, uint32_t* height ) = 0;

        virtual uint32_t GetBackBufferCount() const = 0;

        virtual uint32_t GetWidth() const                            = 0;
        virtual uint32_t GetHeight() const                           = 0;
        virtual void     OnResize( uint32_t width, uint32_t height ) = 0;

        virtual void Release() = 0;

        /**
         * @brief CAPTURING THE FRAME THE USER IS LOOKING AT, declared here rather than on the backend.
         *
         * The swapchain is the only surface that holds the COMPOSITED picture — the scene plus everything
         * drawn over it in the swapchain pass: the editor's panels, and the shipping host's UI and loading
         * screen. `Scene::GetFinalImage()` holds none of that, which is why every capture taken before
         * this existed could argue about the world and not about the picture.
         *
         * IT IS ON THE BASE CLASS because the host that most needs it is the one that must not know about
         * Vulkan. `Runtime` compiles without the Vulkan headers on its include path — deliberately, it is
         * the shipping player — so the editor's `dynamic_pointer_cast<VulkanSwapChain>` is not a shape the
         * player can copy. Two halves, and the reason they are two is on the Vulkan override: a swapchain
         * image may only be touched between its acquire and its present.
         */
        [[nodiscard]] virtual bool SupportsFrameReadback() const noexcept = 0;

        /// Record the copy into this frame's command buffer, after the last pass and before the present.
        [[nodiscard]] virtual Common::BoolResultStr RecordFrameCapture() = 0;

        /// Collect what RecordFrameCapture asked for, tightly packed RGBA8. After the present that carried
        /// the copy.
        [[nodiscard]] virtual Common::ResultStr<std::vector<uint8_t>>
        TakeCapturedFrameRGBA8( uint32_t& outWidth, uint32_t& outHeight ) = 0;

        // Present pacing. ON = sync to the display (no tearing, frame rate capped at the refresh rate of
        // the monitor the window is on); OFF = present as fast as the GPU finishes, which is what an
        // uncapped FPS reading needs. Takes effect the next time the swapchain is (re)created — callers
        // that toggle it at runtime must trigger a recreate, which OnResize already does.
        void SetVSync( bool enabled )
        {
            m_VSync = enabled;
        }
        bool IsVSyncEnabled() const
        {
            return m_VSync;
        }

        static std::shared_ptr<SwapChain> Create( const GLFWwindow* window );

    protected:
        const GLFWwindow* m_Window;
        bool              m_VSync = true;
    };
} // namespace Desert::Graphic