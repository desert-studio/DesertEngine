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
            // u_Trace, u_History and u_GBufferDepth are textures of the frame graph in both variants (the
            // history ping-pong is imported), bound by name through RDG::PassBindings (SSRRenderer::RecordResolve,
            // GIResolveRenderer::RecordTemporal).
        }

        // The SSRResolveUB (SSRResolve.shader and SSRResolveTiled.shader) block, std140, member for member
        // (census: Desert/Tests/Engine/UniformBlockLayout).
        struct SSRResolveUBData
        {
            glm::mat4 PrevViewProj;
            glm::mat4 InvJitteredViewProjection;
            glm::vec4 Params; // xy = texel size, z = history blend (0 = no history), w unused
        };

        // The SSRResolveUB values: the material carries no texture.
        // @p prevViewProj: ViewFrame::PrevViewProjection (unjittered); @p invJitteredViewProjection:
        // ViewFrame::InvJitteredViewProjection, which reconstructs the pixel's world position for the reprojection.
        void BindValues( const glm::mat4& prevViewProj, const glm::mat4& invJitteredViewProjection,
                         const glm::vec2& texelSize, float historyBlend )
        {
            SSRResolveUBData data{};
            data.PrevViewProj              = prevViewProj;
            data.InvJitteredViewProjection = invJitteredViewProjection;
            data.Params       = glm::vec4( texelSize.x, texelSize.y, historyBlend, 0.0f );

            if ( auto* ub = Get<UniformBufferProperty>( "SSRResolveUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
        }
    };

    // Composite half of SSR: blurs the traced reflection buffer (radius scaled by G-buffer roughness)
    // and blends it over the scene. Drives SSRComposite.glsl.frag. Header-only.
    class MaterialSSRComposite final : public Material
    {
    public:
        MaterialSSRComposite() : Material( "MaterialSSRComposite", "SSRComposite" )
        {
            // u_SSR (the resolved accumulation target, imported), u_SSRTileMask and u_GBufferNormal are textures
            // of the frame graph, bound through RDG::PassBindings (SSRRenderer::RecordComposite).
        }

        // The SSRCompositeUB values: the material carries no texture.
        void BindValues( const glm::vec2& texelSize )
        {
            const glm::vec4 params( texelSize.x, texelSize.y, 0.0f, 0.0f );
            if ( auto* ub = Get<UniformBufferProperty>( "SSRCompositeUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &params ), sizeof( params ) );
        }
    };
} // namespace Desert::Graphic
