#pragma once

#include <Engine/Core/Glfw.hpp>
#include <Engine/Core/Window.hpp>

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

        /// Acquires the next presentable image. This is the LIVE path — `Window::PrepareNextFrame` calls it
        /// — and its result now reaches `Application::Run`, which ends the run on a lost device rather than
        /// recording one more frame into nothing.
        [[nodiscard]] virtual Common::BoolResultStr BeginFrame() const = 0;

        // `EndFrame() const = 0` USED TO SIT HERE AND NOTHING EVER CALLED IT. Submitting and presenting go
        // through `Renderer::PresentFinalImage`, so the context had a second, unreachable spelling of the
        // same act — the kind of duplicate that makes a reader believe a guard is on the live path when it
        // is on the dead one. Removed rather than gated.

        virtual void OnResize( uint32_t width, uint32_t height ) = 0;
        virtual void Shutdown()                                  = 0;

        static std::shared_ptr<RendererContext> Create( const std::shared_ptr<Window>& window );
    };
} // namespace Desert::Graphic