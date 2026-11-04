#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <memory>

namespace Desert::Graphic::RDG
{
    class Builder;
    struct ShaderBindingLayout;
} // namespace Desert::Graphic::RDG

namespace Desert::Graphic
{
    struct ViewFrame;

    // THE MOTION VECTORS A VENDOR TEMPORAL UPSCALER TAKES (UE: the DLSS plugin's VelocityCombine, the FSR plugin's
    // ConvertVelocity). The view's velocity (View/SceneViewState.hpp) is written only where geometry rasterised:
    // the sky, the clouds and the far plane keep the clear value 0, and the engine's own resolves derive their
    // motion from depth and the two view matrices (TemporalAA.shader, TSRDilateVelocity.shader). A vendor SDK
    // reads the image as given (Streamline cameraMotionIncluded, FSR motionVectors): a 0 there tells it the sky
    // sat at the same screen point last frame, so under a camera turn the sky's history stays glued to the screen
    // and the horizon trails the camera. This pass writes the camera's motion at infinity into every pixel nothing
    // wrote velocity for (velocity 0 at reversed-Z depth 0) — one formula with TAA, Common/Velocity.glslh
    // DesertCameraVelocityAtInfinity — and copies every other pixel unchanged. Vendor-neutral, so every vendor
    // implementation shares it instead of each writing its own (the same rule as UpscalerExposure).
    //
    // One compute node "UpscalerVelocity" writing a RenderExtent ViewTargetFormats::kVelocity transient (the
    // returned ref). An object, not a free function, because it owns its pipeline (made on first record). Error
    // (named) when @p velocity or @p sceneDepth is not a texture of the graph.
    class UpscalerVelocity
    {
    public:
        UpscalerVelocity();
        ~UpscalerVelocity();
        UpscalerVelocity( const UpscalerVelocity& )            = delete;
        UpscalerVelocity& operator=( const UpscalerVelocity& ) = delete;

        [[nodiscard]] Common::ResultStr<RDG::TextureRef> AddPass( RDG::Builder& graph, const ViewFrame& frame,
                                                                  RDG::TextureRef velocity,
                                                                  RDG::TextureRef sceneDepth ) const;

    private:
        struct PipelineHolder;
        std::unique_ptr<PipelineHolder> m_Pipeline;
    };

    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& UpscalerVelocityLayout();
} // namespace Desert::Graphic
