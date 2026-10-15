#pragma once

#include <Engine/Graphic/API/Vulkan/Streamline/StreamlineRuntime.hpp>
#include <Engine/Graphic/View/TemporalUpscaler.hpp>
#include <Engine/Graphic/View/UpscalerExposure.hpp>

#include <map>
#include <mutex>
#include <tuple>

// NVIDIA DLSS SUPER RESOLUTION as an ITemporalUpscaler (UE: the DLSS plugin's FDLSSUpscaler). Windows only
// (premake compiles Engine/Graphic/Upscalers/ on Windows alone and defines DESERT_UPSCALER_DLSS, which kDlssBuilt
// in API/UpscalerBuild.hpp reads); selectable where Streamline::Runtime::DlssReady() - the catalog offers
// Upscaler::DLSS and AntiAliasingMethod::DLAA only then.
//
// Two passes: UpscalerExposure (the 1x1 exposure multiplier FSR and the engine's TAA use: DLSS is handed the
// engine's exposure, never its own auto exposure), then "DLSS", which reads the jittered scene colour, the
// resolved depth, the velocity and the exposure and writes the OutputExtent colour.
//
// THE FEATURE IS PER VIEW, like FSR's context: the ITemporalUpscalerHistory this object returns (DlssHistory) owns
// the view's Streamline viewport and the options it was last given; the DLSS network's history lives inside that
// feature (HistoryDescs is empty) and is reset by ViewFrame::HistoryReset. When the view drops the history (reset,
// another method, the view's end) the viewport's feature is freed after the frames that recorded with it.
//
// THE MODE comes from the split, as UE's plugin derives r.NGX.DLSS.Quality from the screen percentage: 100 % is
// DLAA; below, the quality mode whose accepted render range (slDLSSGetOptimalSettings) holds the render extent,
// the one whose optimal extent is nearest when several do. A split no mode accepts is not Supported, and
// ResolveViewResolution clamps or refuses it by name. THE PRESET is the mode's Scalability row
// (Parameter::DlssPresetDLAA ..; UE: r.NGX.DLSS.DLAAPreset / QualityPreset / ...), read from
// TemporalUpscalerInputs::Quality every frame; a change re-options the feature.
//
// MATERIAL MIP BIAS is NVIDIA's rule (DLSS programming guide, "Mip-Map Bias"): log2(render / output) - 1, at
// DLAA too.
namespace Desert::Graphic
{
    class DLSSUpscaler final : public ITemporalUpscaler
    {
    public:
        DLSSUpscaler();
        ~DLSSUpscaler() override;

        [[nodiscard]] std::string_view DebugName() const override;
        [[nodiscard]] TemporalMethod   Method() const override
        {
            return TemporalMethod::DLSS;
        }
        [[nodiscard]] bool                            Supports( const ResolutionSplit& split ) const override;
        [[nodiscard]] std::vector<HistoryTextureDesc> HistoryDescs( const ResolutionSplit& split ) const override;
        [[nodiscard]] float MaterialMipBias( const ResolutionSplit& split ) const override;
        [[nodiscard]] Common::ResultStr<TemporalUpscalerOutputs>
        AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                   const TemporalUpscalerInputs& inputs ) const override;

        // The mode that resolves @p split, or why none does.
        [[nodiscard]] Common::ResultStr<Streamline::DlssMode> ModeFor( const ResolutionSplit& split ) const;

    private:
        [[nodiscard]] Common::ResultStr<Streamline::DlssRenderRange> RangeOf( Streamline::DlssMode mode,
                                                                              ViewExtent           output ) const;

        UpscalerExposure m_Exposure;

        // slDLSSGetOptimalSettings by (mode, output width, output height): asked every frame by Supports.
        mutable std::mutex m_RangeMutex;
        mutable std::map<std::tuple<uint8_t, uint32_t, uint32_t>, Common::ResultStr<Streamline::DlssRenderRange>>
             m_Ranges;
    };
} // namespace Desert::Graphic
