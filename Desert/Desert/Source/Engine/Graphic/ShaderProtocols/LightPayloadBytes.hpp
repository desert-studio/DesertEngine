#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace Desert::Graphic::ShaderProtocols
{
    // THE bytes a punctual-light storage buffer (PointLightsUB, SpotLightsUB) holds for a frame: the list end to
    // end, or ONE zeroed payload when the scene has none of that kind. Every consumer loops 0..count with the
    // count from LightsMetadata, so the zero row is never read; it exists because a storage buffer nobody wrote
    // is filled by neither route (MaterialExecutor::GetRouteFill), and a lit pass is then refused at setup.
    // One rule for both routes: the deferred composite's graph upload (DeferredLightingRenderer::UploadLights)
    // and every forward material's write (SceneLightsBind - static, skinned, glass, generic).
    template <class Payload>
    [[nodiscard]] std::span<const std::byte> LightPayloadBytes( const std::vector<Payload>& lights )
    {
        static const Payload kNone{};
        return lights.empty() ? std::as_bytes( std::span<const Payload>( &kNone, 1 ) )
                              : std::as_bytes( std::span<const Payload>( lights.data(), lights.size() ) );
    }
} // namespace Desert::Graphic::ShaderProtocols
