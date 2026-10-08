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
        }

        // The SSAOUB (SSAO.shader) block, std140, member for member (census:
        // Desert/Tests/Engine/UniformBlockLayout).
        struct SSAOUBData
        {
            glm::mat4 ViewProj;
            glm::mat4 InvJitteredViewProjection; // world position from the G-buffer depth
            glm::vec4 CameraPos;
            glm::vec4 Params; // x=radius, y=bias, z=power, w=sampleCount
        };

        // The SSAOUB values only: u_GBufferDepth / u_GBufferNormal are graph resources, bound by name through
        // RDG::PassBindings (SSAORenderer::Record).
        // @p viewProj / @p invJitteredViewProjection: ViewFrame::JitteredViewProjection (the matrix the G-buffer
        // was rasterised with) and its inverse, ViewFrame::InvJitteredViewProjection - never recomputed here.
        void BindInputs( const glm::mat4& viewProj, const glm::mat4& invJitteredViewProjection,
                         const glm::vec4& cameraPos, float radius, float bias, float power, int sampleCount )
        {
            SSAOUBData data{};
            data.ViewProj                  = viewProj;
            data.InvJitteredViewProjection = invJitteredViewProjection;
            data.CameraPos = cameraPos;
            data.Params    = glm::vec4( radius, bias, power, static_cast<float>( sampleCount ) );

            if ( auto* ub = Get<UniformBufferProperty>( "SSAOUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
        }
    };
} // namespace Desert::Graphic
