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
        Tiled,      // SSRResolveTiled.shader (SSR) - needs BindTileMask
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
            if ( variant == SSRResolveVariant::Tiled )
                m_TileMask = m_MaterialExecutor->GetTexture2DProperty( "u_SSRTileMask" ).get();
            m_Trace    = m_MaterialExecutor->GetTexture2DProperty( "u_Trace" ).get();
            m_History  = m_MaterialExecutor->GetTexture2DProperty( "u_History" ).get();
            m_WorldPos = m_MaterialExecutor->GetTexture2DProperty( "u_GBufferWorldPos" ).get();
        }

        // The SSR tile mask (SSRTileClassify): its vertex stage draws only the tiles it marks.
        void BindTileMask( const std::shared_ptr<Image2D>& mask )
        {
            if ( m_TileMask != nullptr && mask != nullptr )
                m_TileMask->SetImage( mask.get() );
        }

        void BindInputs( const std::shared_ptr<Image2D>& trace, const std::shared_ptr<Image2D>& history,
                         const std::shared_ptr<Image2D>& worldPos, const glm::mat4& prevViewProj,
                         const glm::vec2& texelSize, float historyBlend )
        {
            if ( m_Trace && trace )
                m_Trace->SetImage( trace.get() );
            if ( m_History && history )
                m_History->SetImage( history.get() );
            if ( m_WorldPos && worldPos )
                m_WorldPos->SetImage( worldPos.get() );

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
        Texture2DProperty* m_Trace    = nullptr;
        Texture2DProperty* m_History  = nullptr;
        Texture2DProperty* m_WorldPos = nullptr;
        Texture2DProperty* m_TileMask = nullptr; // Tiled variant only
    };

    // Composite half of SSR: blurs the traced reflection buffer (radius scaled by G-buffer roughness)
    // and blends it over the scene. Drives SSRComposite.glsl.frag. Header-only.
    class MaterialSSRComposite final : public Material
    {
    public:
        MaterialSSRComposite() : Material( "MaterialSSRComposite", "SSRComposite" )
        {
            m_SSR    = m_MaterialExecutor->GetTexture2DProperty( "u_SSR" ).get();
            m_Normal   = m_MaterialExecutor->GetTexture2DProperty( "u_GBufferNormal" ).get();
            m_TileMask = m_MaterialExecutor->GetTexture2DProperty( "u_SSRTileMask" ).get();
        }

        // The SSR tile mask (SSRTileClassify): its vertex stage draws only the tiles it marks.
        void BindTileMask( const std::shared_ptr<Image2D>& mask )
        {
            if ( m_TileMask != nullptr && mask != nullptr )
                m_TileMask->SetImage( mask.get() );
        }

        void BindInputs( const std::shared_ptr<Image2D>& ssr, const std::shared_ptr<Image2D>& normal,
                         const glm::vec2& texelSize )
        {
            if ( m_SSR && ssr )
                m_SSR->SetImage( ssr.get() );
            if ( m_Normal && normal )
                m_Normal->SetImage( normal.get() );

            const glm::vec4 params( texelSize.x, texelSize.y, 0.0f, 0.0f );
            if ( auto* ub = Get<UniformBufferProperty>( "SSRCompositeUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &params ), sizeof( params ) );
        }

    private:
        Texture2DProperty* m_SSR      = nullptr;
        Texture2DProperty* m_Normal   = nullptr;
        Texture2DProperty* m_TileMask = nullptr;
    };
} // namespace Desert::Graphic
