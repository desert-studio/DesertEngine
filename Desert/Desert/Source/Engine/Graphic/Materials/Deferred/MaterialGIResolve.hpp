#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // Fullscreen GI-resolve material: reads the G-buffer (normal/world pos) + the RSM (sun-view G-buffer)
    // and gathers the one-bounce indirect light into the GI buffer, driving GIResolve.glsl.frag. The
    // lighting pass then reads that buffer through a wide blur (the denoise). Header-only.
    class MaterialGIResolve final : public Material
    {
    public:
        MaterialGIResolve() : Material( "MaterialGIResolve", "GIResolve" )
        {
            m_Normal      = m_MaterialExecutor->GetTexture2DProperty( "u_GBufferB" ).get();
            m_WorldPos    = m_MaterialExecutor->GetTexture2DProperty( "u_GBufferC" ).get();
            m_RSMAlbedo   = m_MaterialExecutor->GetTexture2DProperty( "u_RSMAlbedo" ).get();
            m_RSMNormal   = m_MaterialExecutor->GetTexture2DProperty( "u_RSMNormal" ).get();
            m_RSMWorldPos = m_MaterialExecutor->GetTexture2DProperty( "u_RSMWorldPos" ).get();
        }

        void BindInputs( const std::shared_ptr<Image2D>& normal, const std::shared_ptr<Image2D>& worldPos,
                         const std::shared_ptr<Image2D>& rsmAlbedo, const std::shared_ptr<Image2D>& rsmNormal,
                         const std::shared_ptr<Image2D>& rsmWorldPos, const glm::mat4& rsmViewProj,
                         const glm::vec4& sunColorIntensity, float giIntensity, float jitterSeed )
        {
            if ( m_Normal && normal )
                m_Normal->SetImage( normal.get(), RDG::Access::SampledGraphics );
            if ( m_WorldPos && worldPos )
                m_WorldPos->SetImage( worldPos.get(), RDG::Access::SampledGraphics );
            if ( m_RSMAlbedo && rsmAlbedo )
                m_RSMAlbedo->SetImage( rsmAlbedo.get(), RDG::Access::SampledGraphics );
            if ( m_RSMNormal && rsmNormal )
                m_RSMNormal->SetImage( rsmNormal.get(), RDG::Access::SampledGraphics );
            if ( m_RSMWorldPos && rsmWorldPos )
                m_RSMWorldPos->SetImage( rsmWorldPos.get(), RDG::Access::SampledGraphics );

            struct GIResolveUBData
            {
                glm::mat4 RSMViewProj;
                glm::vec4 SunColor; // rgb = colour, a = intensity
                glm::vec4 Params;   // x = GI intensity, y = enabled
            } data;
            data.RSMViewProj = rsmViewProj;
            data.SunColor    = sunColorIntensity;
            const bool valid = rsmAlbedo && rsmNormal && rsmWorldPos && giIntensity > 0.0f;
            data.Params      = glm::vec4( giIntensity, valid ? 1.0f : 0.0f, 0.0f, jitterSeed );

            if ( auto* ub = Get<UniformBufferProperty>( "GIResolveUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
        }

    private:
        Texture2DProperty* m_Normal      = nullptr;
        Texture2DProperty* m_WorldPos    = nullptr;
        Texture2DProperty* m_RSMAlbedo   = nullptr;
        Texture2DProperty* m_RSMNormal   = nullptr;
        Texture2DProperty* m_RSMWorldPos = nullptr;
    };
} // namespace Desert::Graphic
