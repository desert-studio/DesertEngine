#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <cstdint>

namespace Desert::Graphic
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct MaterialSSAOShaderParameters
    {
        DESERT_SHADER_PARAMETER( UniformBuffer, SSAOUB );
    };

    // Fullscreen SSAO material: reads the G-buffer depth + normal and writes the ground-truth ambient occlusion
    // (GTAO, SSAO.shader). Header-only.
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
            glm::mat4 InvJitteredViewProjection; // world position from the G-buffer depth
            glm::mat4 WorldToView;               // the camera's View matrix
            glm::vec4 Target;                    // xy = AO target size (px), zw = 1 / size
            glm::vec4 Horizon; // x = world radius (cm), y = thickness blend, z = taps per direction, w = px per cm
                               // at depth 1
            glm::vec4 Temporal; // xy = cos/sin of the slice rotation, z = step offset, w = slice angles
        };

        // What one GTAO dispatch is asked: its view, its target and the search (UE FGTAOShaderParameters).
        struct Inputs
        {
            glm::mat4  InvJitteredViewProjection{ 1.0f }; // ViewFrame::InvJitteredViewProjection
            glm::mat4  View{ 1.0f };                      // ViewFrame::View
            glm::mat4  Projection{ 1.0f };                // ViewFrame::Projection (unjittered)
            glm::uvec2 TargetSize{ 1u, 1u };
            uint64_t   FrameIndex     = 0; // ViewFrame::FrameIndex: the slice rotation and step offset cycle on it
            float      WorldRadius    = 0.0f; // cm (UE r.GTAO.FalloffEnd)
            float      ThicknessBlend = 0.0f; // UE r.GTAO.ThicknessBlend, before UE's remap
            int        TapsPerDirection = 0;
            int        Angles           = 0; // UE r.GTAO.NumAngles
        };

        // The SSAOUB values only: GBufferDepthTexture / GBufferNormalTexture are graph resources, bound by name through
        // RDG::PassBindings (SSAORenderer::Record). Ported from UE 5.8
        // Renderer/Private/CompositionLighting/PostProcessAmbientOcclusion.cpp GetGTAOShaderParameters (440-481):
        // the six-step slice rotation, the step offset that advances once per six frames, the thickness remap 1 -
        // t^2 clamped to [0, 0.99].
        void BindInputs( const Inputs& in )
        {
            constexpr std::array<float, 6> kRotationsDegrees = { 60.0f, 300.0f, 180.0f, 240.0f, 120.0f, 0.0f };
            constexpr std::array<float, 4> kOffsets          = { 0.1f, 0.6f, 0.35f, 0.85f };
            // Half the degrees in radians: a slice direction covers PI, not 2 PI (UE: Rots * PI / 360).
            const float rotation =
                 kRotationsDegrees[in.FrameIndex % kRotationsDegrees.size()] * ( glm::pi<float>() / 360.0f );
            const float thickness = glm::clamp( 1.0f - ( in.ThicknessBlend * in.ThicknessBlend ), 0.0f, 0.99f );
            const glm::vec2 size  = glm::max( glm::vec2( in.TargetSize ), glm::vec2( 1.0f ) );
            // Pixels one centimetre spans at view depth 1: half the target height times the focal scale.
            const float pixelsPerUnit = 0.5f * size.y * glm::abs( in.Projection[1][1] );

            SSAOUBData data{};
            data.InvJitteredViewProjection = in.InvJitteredViewProjection;
            data.WorldToView               = in.View;
            data.Target                    = glm::vec4( size, 1.0f / size );
            data.Horizon =
                 glm::vec4( in.WorldRadius, thickness, static_cast<float>( in.TapsPerDirection ), pixelsPerUnit );
            data.Temporal =
                 glm::vec4( glm::cos( rotation ), glm::sin( rotation ),
                            kOffsets[( in.FrameIndex / kRotationsDegrees.size() ) % kOffsets.size()] * 0.25f,
                            static_cast<float>( glm::max( in.Angles, 1 ) ) );

            if ( auto* ub = Get<UniformBufferProperty>( MaterialSSAOShaderParameters::SSAOUB ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
        }
    };
} // namespace Desert::Graphic
