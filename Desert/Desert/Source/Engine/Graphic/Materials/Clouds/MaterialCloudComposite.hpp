#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

namespace Desert::Graphic
{
    // The cloud composite program's material. It carries one value (the view's PreExposure) and no textures: the
    // one image the pass reads -- the full-resolution RGBA16F cloud image (premultiplied radiance + transmittance)
    // CloudUpsample wrote this frame from CloudTemporalResolve's half-resolution reconstruction and each pixel's own
    // scene depth -- is a transient of the frame graph, bound by shader name (CloudScatterTexture) through
    // RDG::PassBindings in the composite pass (VolumetricCloudRenderer::CompositePass). The pipeline's blend state
    // does the rest.
    class MaterialCloudComposite final : public Material
    {
    public:
        MaterialCloudComposite();

        // x = ViewFrame::PreExposure: the composite writes the cloud radiance into the scene colour multiplied by
        // it (CloudCompositeUB, set per frame by VolumetricCloudRenderer::DeclareFrameNodes); yzw unused.
        MPROPERTY( glm::vec4, Exposure, "Exposure", ( glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f ) ) )
    };
} // namespace Desert::Graphic
