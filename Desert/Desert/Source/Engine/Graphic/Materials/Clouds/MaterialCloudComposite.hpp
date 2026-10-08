#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

namespace Desert::Graphic
{
    // Binds the two images the cloud composite pass reads: the RGBA16F scatter image (premultiplied
    // radiance + transmittance) and the depth guide beside it (cloud front distance and scene distance,
    // kilometres), which the shader upsamples the first through. No parameters — the march already
    // resolved the lighting, and the pipeline's blend state does the rest.
    //
    // The pair comes from CloudTemporalResolve, not from the march itself: the march traces at a quarter
    // of the view and the reconstruction turns four such frames into the half-resolution pair this
    // composites. That is invisible here on purpose — the interface is "two half-res images of the same
    // size", and it did not change when the producer behind it did.
    // The cloud composite program's material. It carries no values and no textures: the two images the pass
    // reads -- the RGBA16F scatter image (premultiplied radiance + transmittance) and the depth guide beside it
    // (cloud front distance and scene distance, kilometres), which the shader upsamples the first through -- are
    // the reconstruction CloudTemporalResolve wrote this frame, imported into the frame graph and bound by
    // shader name (u_CloudScatter / u_CloudGuide) through RDG::PassBindings in the composite pass
    // (VolumetricCloudRenderer::CompositePass). The pipeline's blend state does the rest.
    class MaterialCloudComposite final : public Material
    {
    public:
        MaterialCloudComposite();
    };
} // namespace Desert::Graphic
