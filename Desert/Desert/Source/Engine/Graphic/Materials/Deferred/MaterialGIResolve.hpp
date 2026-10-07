#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // Fullscreen GI-resolve material: reads the G-buffer (normal/world pos) + the RSM (sun-view G-buffer)
    // and gathers the one-bounce indirect light into the GI gather transient, driving GIResolve.glsl.frag. The
    // lighting pass then reads that buffer through a wide blur (the denoise). Header-only.
    class MaterialGIResolve final : public Material
    {
    public:
        MaterialGIResolve() : Material( "MaterialGIResolve", "GIResolve" )
        {
        }

        // The GIResolveUB (GIResolve.shader) block, std140, member for member (census: Desert/Tests/Engine/UniformBlockLayout).
        struct GIResolveUBData
        {
            glm::mat4 RSMViewProj;
            glm::mat4 InvRSMViewProj;            // the VPL positions from the RSM depth
            glm::mat4 InvJitteredViewProjection; // the pixel's position from the G-buffer depth
            glm::vec4 SunColor; // rgb = colour, a = intensity
            glm::vec4 Params;   // x = GI intensity, y = enabled, z = gather taps, w = jitter seed
        };

        // The GIResolveUB values only: u_GBufferB/Depth and u_RSMAlbedo/Normal/Depth are graph resources, bound
        // by name through RDG::PassBindings (GIResolveRenderer::RecordGather).
        void BindInputs( const glm::mat4& rsmViewProj, const glm::mat4& cameraViewProj, const glm::vec4& sunColorIntensity, float giIntensity,
                         float jitterSeed, int samples )
        {
            GIResolveUBData data;
            data.RSMViewProj    = rsmViewProj;
            data.InvRSMViewProj = glm::inverse( rsmViewProj );
            // The G-buffer is rasterised with cameraViewProj (jittered == unjittered until TAA lands).
            data.InvJitteredViewProjection = glm::inverse( cameraViewProj );
            data.SunColor    = sunColorIntensity;
            const bool valid = giIntensity > 0.0f;
            data.Params = glm::vec4( giIntensity, valid ? 1.0f : 0.0f, static_cast<float>( samples ), jitterSeed );

            if ( auto* ub = Get<UniformBufferProperty>( "GIResolveUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
        }
    };
} // namespace Desert::Graphic
