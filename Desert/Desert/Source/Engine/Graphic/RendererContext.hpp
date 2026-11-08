#pragma once

#include <Engine/Core/Glfw.hpp>
#include <Engine/Core/Window.hpp>

#include <functional>

namespace Desert
{
    class Window;
}
namespace Desert::Graphic
{
    class RendererContext
    {
    public:
        virtual ~RendererContext() = default;

        virtual void Init() = 0;

        /// The frame boundary: applies a requested swapchain rebuild (only with no render-task job in flight,
        /// VulkanContext::BeginFrame; Application::Run joins first when one is pending) and reports a lost
        /// device. It does NOT
        /// acquire; the image is acquired late, at the frame's back-buffer import (RendererAPI::ImportBackBuffer).
        /// This is the LIVE path — `Window::PrepareNextFrame` calls it — and its result reaches
        /// `Application::Run`, which ends the run on a lost device rather than recording one more frame into
        /// nothing.
        [[nodiscard]] virtual Common::BoolResultStr BeginFrame() const = 0;

        // `EndFrame() const = 0` USED TO SIT HERE AND NOTHING EVER CALLED IT. Submitting and presenting go
        // through `Renderer::PresentFinalImage`, so the context had a second, unreachable spelling of the
        // same act — the kind of duplicate that makes a reader believe a guard is on the live path when it
        // is on the dead one. Removed rather than gated.

        virtual void OnResize( uint32_t width, uint32_t height ) = 0;
        virtual void Shutdown()                                  = 0;

        /// FRAME-RETIRED RELEASE (UE: deferred deletion). Runs @p retire once every frame that may still be
        /// recorded with what it releases has retired, so a render worker of frame N-1 that read a raw pointer
        /// from a service (ImageService::Resolve, MeshService::Get, ...) keeps a live object while the game
        /// thread of frame N unregisters it. The API-agnostic face of the device's host-object deletion queue;
        /// services above the API layer retire through this, never through an API allocator.
        virtual void RetireAfterFramesInFlight( std::function<void()> retire ) = 0;

        static std::shared_ptr<RendererContext> Create( const std::shared_ptr<Window>& window );
    };
} // namespace Desert::Graphic