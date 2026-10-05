#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // Which program draws the shared denoiser (Common/SSRDenoise.glslh): the GI gather writes every pixel and
    // resolves fullscreen; SSR's trace writes only where the tile mask says, and resolves over those tiles.
    enum class SSRResolveVariant
    {
        Fullscreen, // SSRResolve.shader (GI)
        Tiled,      // SSRResolveTiled.shader (SSR) - its trace and tile mask are graph transients (PassBindings)
    };

    // Temporal + spatial resolve (the shared denoiser): blends this frame's jittered estimate with the
    // reprojected previous result into the accumulation target. Header-only.
    class MaterialSSRResolve final : public Material
    {
    public:
        explicit MaterialSSRResolve( SSRResolveVariant variant )
             : Material( "MaterialSSRResolve",
                         variant == SSRResolveVariant::Tiled ? "SSRResolveTiled" : "SSRResolve" )
        {
            // u_Trace and u_GBufferWorldPos are textures of the frame graph in both variants, bound by name through
            // RDG::PassBindings (SSRRenderer::RecordResolve, GIResolveRenderer::RecordTemporal). u_History is the
            // SSR's own ping-pong (external to the graph) on this route.
            m_History = m_MaterialExecutor->GetTexture2DProperty( "u_History" ).get();
        }

        void BindInputs( const std::shared_ptr<Image2D>& history, const glm::mat4& prevViewProj,
                         const glm::vec2& texelSize, float historyBlend )
        {
            if ( m_History && history )
                m_History->SetImage( history.get(), RDG::Access::SampledGraphics );
            BindValues( prevViewProj, texelSize, historyBlend );
        }

        // The SSRResolveUB values only, for a caller that binds u_History / u_GBufferWorldPos through
        // RDG::PassBindings (GIResolveRenderer::RecordTemporal).
        void BindValues( const glm::mat4& prevViewProj, const glm::vec2& texelSize, float historyBlend )
        {
            struct SSRResolveUBData
            {
                glm::mat4 PrevViewProj;
                glm::vec4 Params; // xy = texel size, z = history blend (0 = no history), w unused
            } data;
            data.PrevViewProj = prevViewProj;
            data.Params       = glm::vec4( texelSize.x, texelSize.y, historyBlend, 0.0f );

            if ( auto* ub = Get<UniformBufferProperty>( "SSRResolveUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
        }

    private:
        Texture2DProperty* m_History = nullptr;
    };

    // Composite half of SSR: blurs the traced reflection buffer (radius scaled by G-buffer roughness)
    // and blends it over the scene. Drives SSRComposite.glsl.frag. Header-only.
    class MaterialSSRComposite final : public Material
    {
    public:
        MaterialSSRComposite() : Material( "MaterialSSRComposite", "SSRComposite" )
        {
            m_SSR = m_MaterialExecutor->GetTexture2DProperty( "u_SSR" ).get();
            // u_SSRTileMask and u_GBufferNormal are textures of the frame graph, bound through RDG::PassBindings
            // (SSRRenderer::RecordComposite).
        }

        void BindInputs( const std::shared_ptr<Image2D>& ssr, const glm::vec2& texelSize )
        {
            if ( m_SSR && ssr )
                m_SSR->SetImage( ssr.get(), RDG::Access::SampledGraphics );

            const glm::vec4 params( texelSize.x, texelSize.y, 0.0f, 0.0f );
            if ( auto* ub = Get<UniformBufferProperty>( "SSRCompositeUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &params ), sizeof( params ) );
        }

    private:
        Texture2DProperty* m_SSR = nullptr;
    };
} // namespace Desert::Graphic
