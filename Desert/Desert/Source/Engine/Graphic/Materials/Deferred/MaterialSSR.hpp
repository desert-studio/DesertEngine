#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // The RSM-GI gather's temporal + spatial resolve (SSRResolve.shader, Common/SSRDenoise.glslh): blends this
    // frame's jittered estimate with the previous result, reprojected through the pixel's world position, into
    // the accumulation target. SSR's resolve is its own material (MaterialSSRReflectionResolve): it reprojects
    // through the REFLECTED point, so the two blocks hold different things. Header-only.
    class MaterialSSRResolve final : public Material
    {
    public:
        MaterialSSRResolve() : Material( "MaterialSSRResolve", "SSRResolve" )
        {
            // u_Trace, u_History and u_GBufferDepth are textures of the frame graph (the history ping-pong is
            // imported), bound by name through RDG::PassBindings (GIResolveRenderer::RecordTemporal).
        }

        // The SSRResolveUB (SSRResolve.shader) block, std140, member for member
        // (census: Desert/Tests/Engine/UniformBlockLayout).
        struct SSRResolveUBData
        {
            glm::mat4 PrevViewProj;
            glm::mat4 InvJitteredViewProjection;
            glm::vec4 Params; // xy = texel size, z = history blend (0 = no history), w unused
        };

        // The SSRResolveUB values: the material carries no texture.
        // @p prevViewProj: ViewFrame::PrevViewProjection (unjittered); @p invJitteredViewProjection:
        // ViewFrame::InvJitteredViewProjection, which reconstructs the pixel's world position for the
        // reprojection.
        void BindValues( const glm::mat4& prevViewProj, const glm::mat4& invJitteredViewProjection,
                         const glm::vec2& texelSize, float historyBlend )
        {
            SSRResolveUBData data{};
            data.PrevViewProj              = prevViewProj;
            data.InvJitteredViewProjection = invJitteredViewProjection;
            data.Params                    = glm::vec4( texelSize.x, texelSize.y, historyBlend, 0.0f );

            if ( auto* ub = Get<UniformBufferProperty>( "SSRResolveUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
        }
    };

    // SSR's resolve (SSRResolveTiled.shader, drawn over the SSR tiles): the neighbours' hit points through the
    // ratio estimator, then last frame's result read where the REFLECTED point was (UE SSRT's reflection
    // reprojection: the surface point pushed along the view ray by the hit distance, through last frame's camera,
    // plus the surface's own motion). Its textures are graph transients (SSRRenderer::DeclareResolveBindings).
    // Header-only.
    class MaterialSSRReflectionResolve final : public Material
    {
    public:
        MaterialSSRReflectionResolve() : Material( "MaterialSSRReflectionResolve", "SSRResolveTiled" ) {}

        // The SSRReflectionResolveUB (SSRResolveTiled.shader) block, std140, member for member
        // (census: Desert/Tests/Engine/UniformBlockLayout).
        struct SSRReflectionResolveUBData
        {
            glm::mat4 ViewProjection;     // THIS frame's, unjittered (the camera's share of the velocity)
            glm::mat4 PrevViewProjection; // LAST frame's, unjittered (where the reflected point was)
            glm::mat4 InvJitteredViewProjection;
            glm::vec4 Params; // xy = texel size, z = history blend (0 = no history), w unused
        };

        // @p viewProjection / @p prevViewProjection: ViewFrame::ViewProjection / PrevViewProjection (both
        // unjittered, the space of the velocity target); @p invJitteredViewProjection: ViewFrame::
        // InvJitteredViewProjection (the pixel's world position from the G-buffer depth).
        void BindValues( const glm::mat4& viewProjection, const glm::mat4& prevViewProjection,
                         const glm::mat4& invJitteredViewProjection, const glm::vec2& texelSize, float historyBlend )
        {
            SSRReflectionResolveUBData data{};
            data.ViewProjection            = viewProjection;
            data.PrevViewProjection        = prevViewProjection;
            data.InvJitteredViewProjection = invJitteredViewProjection;
            data.Params                    = glm::vec4( texelSize.x, texelSize.y, historyBlend, 0.0f );

            if ( auto* ub = Get<UniformBufferProperty>( "SSRReflectionResolveUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
        }
    };
} // namespace Desert::Graphic
