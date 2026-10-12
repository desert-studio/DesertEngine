#pragma once

#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Graphic/Pipeline.hpp>

namespace Desert::Graphic
{
    // THE ONE PLACE a surface template's BlendMode (UE EBlendMode, ShaderProgramMeta::Blend) becomes the colour
    // blend and depth write of a pipeline. Every pass that draws a translucent-blend material - the translucency
    // pass's meshes (MeshRenderer::TranslucentDrawFor) and the particle sprites (ParticleRenderer) - builds its
    // pipeline through this, so "Additive" cannot mean one thing for a mesh and another for a sprite.
    //   Opaque / Masked: no blend (they are drawn by the opaque passes; set here only for completeness).
    //   Translucent:     over, SrcAlpha / OneMinusSrcAlpha, no depth write.
    //   Additive:        added, SrcAlpha / One, no depth write. UE's BLEND_Additive is One / One over the emissive
    //                    the material premultiplied by its opacity; the pass headers write (colour, opacity)
    //                    unpremultiplied, so SrcAlpha / One is the same sum without a second shader variant.
    inline void ApplySurfaceBlendMode( GraphicsPipelineSpecification&        spec,
                                       const Core::Formats::SurfaceBlendMode blend )
    {
        using Core::Formats::SurfaceBlendMode;
        spec.BlendEnable = Core::Formats::IsTranslucentBlend( blend );
        if ( !spec.BlendEnable )
            return;
        spec.DepthWriteEnabled   = false; // translucency never occludes later fragments or itself
        spec.SrcColorBlendFactor = BlendFactor::SrcAlpha;
        spec.DstColorBlendFactor =
             blend == SurfaceBlendMode::Additive ? BlendFactor::One : BlendFactor::OneMinusSrcAlpha;
    }
} // namespace Desert::Graphic
