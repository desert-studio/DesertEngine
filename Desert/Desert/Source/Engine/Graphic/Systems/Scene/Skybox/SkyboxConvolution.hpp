#pragma once

// THE RENDERER'S HALF OF AN HDR SKYBOX (MaterialSkybox, Engine): the sky capture's graph passes that convert a
// cache miss's panorama into the environment cubes. The material only holds the conversion's state.

namespace Desert::Graphic
{
    class FrameTextures;
    class MaterialSkybox;
    namespace RDG
    {
        class Builder;
    }

    /// GRAPH BUILD, by every view that draws @p material, at the head of its graph (SkyboxRenderer::
    /// AddSkyCapturePasses): a cache miss's conversion (EnvironmentConvolution) is recorded WHOLE by the first
    /// view's graph, ahead of every pass that samples the cubes, and the environment is set at once -- that
    /// view and every view after it read it. A later frame's call hands the converted cubes to the cache
    /// writer: the graph that wrote them has been submitted by then. A hit, or a finished conversion, adds
    /// nothing.
    void AddSkyboxConvolutionPasses( MaterialSkybox& material, RDG::Builder& graph, FrameTextures& textures );
} // namespace Desert::Graphic
