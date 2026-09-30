#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // Fullscreen SSAO material: reads the G-buffer world position + normal and writes an ambient-occlusion
    // factor, driving SSAO.glsl.frag. Header-only (no new .cpp -> no premake regen).
    class MaterialSSAO final : public Material
    {
    public:
        MaterialSSAO() : Material( "MaterialSSAO", "SSAO" )
        {
            m_Pos    = m_MaterialExecutor->GetTexture2DProperty( "u_GBufferPos" ).get();
            m_Normal = m_MaterialExecutor->GetTexture2DProperty( "u_GBufferNormal" ).get();
        }

        void BindInputs( const std::shared_ptr<Image2D>& worldPos, const std::shared_ptr<Image2D>& normal,
                         const glm::mat4& viewProj, const glm::vec4& cameraPos, float radius, float bias,
                         float power, int sampleCount )
        {
            if ( m_Pos && worldPos )
                m_Pos->SetImage( worldPos.get(), RDG::Access::SampledGraphics );
            if ( m_Normal && normal )
                m_Normal->SetImage( normal.get(), RDG::Access::SampledGraphics );

            struct SSAOUBData
            {
                glm::mat4 ViewProj;
                glm::vec4 CameraPos;
                glm::vec4 Params; // x=radius, y=bias, z=power, w=sampleCount
            } data;
            data.ViewProj  = viewProj;
            data.CameraPos = cameraPos;
            data.Params    = glm::vec4( radius, bias, power, static_cast<float>( sampleCount ) );

            if ( auto* ub = Get<UniformBufferProperty>( "SSAOUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
        }

    private:
        Texture2DProperty* m_Pos    = nullptr;
        Texture2DProperty* m_Normal = nullptr;
    };
} // namespace Desert::Graphic
