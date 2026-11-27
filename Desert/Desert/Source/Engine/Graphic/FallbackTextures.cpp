#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::Graphic
{
    // The backend owns the set (IRhi::Fallbacks): built on first use under the device's attribution, released
    // by Renderer::Shutdown, destroyed with the backend.
    FallbackTextures& FallbackTextures::Get()
    {
        return Renderer::GetInstance().Rhi().Fallbacks();
    }

} // namespace Desert::Graphic
