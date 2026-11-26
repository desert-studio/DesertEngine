#pragma once

#include <Engine/Graphic/RendererClient.hpp>

namespace Desert::Engine
{
    /// The engine's side of Graphic::IRendererClient: the policy the RenderCore renderer must not know of - the
    /// environment cache writes, ResourceRegistry's services and the shared primitive meshes - run at the
    /// renderer's frame slot and teardown, in the order Renderer::Shutdown names. Attached by the Application
    /// before Renderer::Init (Application::Application).
    class EngineRendererClient final : public Graphic::IRendererClient
    {
    public:
        void OnFrameSlot() override;
        void OnShutdownDrain() override;
        void OnShutdownRelease() override;
    };
} // namespace Desert::Engine
