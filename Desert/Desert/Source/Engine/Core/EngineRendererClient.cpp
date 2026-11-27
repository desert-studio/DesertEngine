#include <Engine/Core/EngineRendererClient.hpp>

#include <Engine/Geometry/PrimitiveMeshFactory.hpp>
#include <Engine/Graphic/Environment/EnvironmentBake.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Skybox/SkyboxService.hpp>

namespace Desert::Engine
{
    void EngineRendererClient::OnFrameSlot()
    {
        Runtime::ResourceRegistry::GetSkyboxService()->EnvironmentCache().Pump();
    }

    void EngineRendererClient::OnShutdownDrain()
    {
        Runtime::ResourceRegistry::GetSkyboxService()->EnvironmentCache().Drain();
    }

    void EngineRendererClient::OnShutdownRelease()
    {
        // ClearAll first (the image service last inside it), then the shared primitive meshes - the order
        // Renderer::Shutdown always had.
        Runtime::ResourceRegistry::ClearAll();
        Geometry::PrimitiveMeshFactory::ReleaseShared();
    }
} // namespace Desert::Engine
