#pragma once

namespace Desert::Graphic
{
    /**
     * THE ENGINE'S HOOKS INTO THE RENDERER'S FRAME AND TEARDOWN (BINT-RC, Lakos levelization).
     *
     * The Renderer lives in RenderCore; what the engine keeps around it - the resource services, the shared
     * primitive meshes, the environment cache writes - lives in Engine, above it. The Renderer reaches that policy
     * only through this interface: Engine implements it (Core/EngineRendererClient) and the Application attaches
     * it before Renderer::Init, which refuses to start without one. Owned by the Renderer from then on.
     */
    class IRendererClient
    {
    public:
        virtual ~IRendererClient() = default;

        /// Renderer::WaitFrameSlot, right after the backend's frame slot is free: the engine's once-a-frame work
        /// that needs the slot (the environment cache's readbacks). Every host begins its frames there.
        virtual void OnFrameSlot() = 0;

        /// Renderer::Shutdown, FIRST, while the device is alive: the engine finishes its GPU work in flight.
        virtual void OnShutdownDrain() = 0;

        /// Renderer::Shutdown, right after the renderer dropped its own BRDF LUT and before the fallback and
        /// default textures, the geometry pool and the device: the engine empties every resource it registered.
        virtual void OnShutdownRelease() = 0;
    };
} // namespace Desert::Graphic
