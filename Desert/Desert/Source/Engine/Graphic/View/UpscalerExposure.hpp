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
    // THE EXPOSURE A VENDOR TEMPORAL UPSCALER TAKES (FSR 3.1 `exposure`; the same input DLSS and XeSS name). The
    // engine hands every ITemporalUpscaler the ADAPTED LUMINANCE (TemporalUpscalerInputs::Exposure, AutoExposure's
    // 1x1); a vendor SDK wants the MULTIPLIER it tonemaps its samples by. This pass turns one into the other, in
    // the definition the engine's TAA already uses (TemporalAA.shader HdrWeight: exposure = 1 / adapted
    // luminance), so a vendor resolve and ours weigh samples in one space. Vendor-neutral, so every vendor
    // implementation shares it instead of each writing its own.
    //
    // One compute node "UpscalerExposure" writing a 1x1 R32F transient (the returned ref). An object, not a free
    // function, because it owns its pipeline (made on first record), like SupersampleResolve. Error (named) when
    // @p adaptedLuminance is not a texture of the graph.
    class UpscalerExposure
    {
    public:
        UpscalerExposure();
        ~UpscalerExposure();
        UpscalerExposure( const UpscalerExposure& )            = delete;
        UpscalerExposure& operator=( const UpscalerExposure& ) = delete;

        [[nodiscard]] Common::ResultStr<RDG::TextureRef> AddPass( RDG::Builder&   graph,
                                                                  RDG::TextureRef adaptedLuminance ) const;

    private:
        struct PipelineHolder;
        std::unique_ptr<PipelineHolder> m_Pipeline;
    };

    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& UpscalerExposureLayout();
} // namespace Desert::Graphic
