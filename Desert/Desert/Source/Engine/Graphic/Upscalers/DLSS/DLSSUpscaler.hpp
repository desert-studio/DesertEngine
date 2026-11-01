#pragma once

#include <Engine/Graphic/API/Vulkan/Streamline/StreamlineRuntime.hpp>
#include <Engine/Graphic/View/TemporalUpscaler.hpp>
#include <Engine/Graphic/View/UpscalerExposure.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <tuple>

// NVIDIA DLSS SUPER RESOLUTION and DLSS RAY RECONSTRUCTION as an ITemporalUpscaler (UE: the DLSS plugin's
// FDLSSUpscaler, whose Ray Reconstruction mode is the same upscaler with the denoiser on). One class, one object
// per Streamline::DlssFeature: the feature is the only difference in the passes (which plugin evaluates, which
// guides are tagged), and everything else - the modes, the split, the jitter, the exposure, the per-view feature -
// is shared. Windows only (the build compiles Engine/Graphic/Upscalers/ on Windows alone and defines
// DESERT_UPSCALER_DLSS, which kDlssBuilt in API/UpscalerBuild.hpp reads); selectable where
// Streamline::Runtime::DlssReady() - the catalog offers AntiAliasingMethod::DLSS (and DLSSRayReconstruction) only
// then.
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
// (Parameter::DlssPresetNative ..; UE: r.NGX.DLSS.DLAAPreset / QualityPreset / ...), read from
// TemporalUpscalerInputs::Quality every frame; a change re-options the feature.
//
// MATERIAL MIP BIAS is NVIDIA's rule (DLSS programming guide, "Mip-Map Bias"): log2(render / output) - 1, at
// DLAA too.
//
// RAY RECONSTRUCTION (TemporalMethod::DLSSRR; Streamline sl::kFeatureDLSS_RR) is also the view's denoiser
// (TakesRawLightingSignals): the renderer hands it the reflection traced at full resolution and the diffuse gather
// undenoised, composited into the colour as usual, and a third pass, "DLSS-RR: Guides" (DLSSRRGuides.shader),
// writes the guides it demodulates them with - diffuse and specular albedo from the G-buffer and the split-sum
// table, the reflection's hit distance from SSR's ray - beside the G-buffer's normal + roughness, tagged as is.
// On a path-traced view (TemporalUpscalerInputs::PathGuides) "DLSS-RR: Path Guides" (DLSSRRPathGuides.shader)
// writes all four from the paths' own guide sums instead: the paths made that colour.
// Its presets are its own Scalability rows (Parameter::DlssRRPresetNative ..), its modes DLSS's.
namespace Desert::Graphic
{
    class DLSSUpscaler final : public ITemporalUpscaler
    {
    public:
        explicit DLSSUpscaler( Streamline::DlssFeature feature );
        ~DLSSUpscaler() override;

        [[nodiscard]] std::string_view DebugName() const override;
        [[nodiscard]] TemporalMethod   Method() const override
        {
            return m_Feature == Streamline::DlssFeature::RayReconstruction ? TemporalMethod::DLSSRR
                                                                            : TemporalMethod::DLSS;
        }
        [[nodiscard]] bool TakesRawLightingSignals() const override
        {
            return m_Feature == Streamline::DlssFeature::RayReconstruction;
        }
        [[nodiscard]] bool                            Supports( const ResolutionSplit& split ) const override;
        [[nodiscard]] std::vector<HistoryTextureDesc>
                            HistoryDescs( const ResolutionSplit&                      split,
                                          const Common::Scalability::ResolvedQuality& quality ) const override;
        [[nodiscard]] float MaterialMipBias( const ResolutionSplit& split ) const override;
        [[nodiscard]] Common::ResultStr<TemporalUpscalerOutputs>
        AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                   const TemporalUpscalerInputs& inputs ) const override;

        // Why @p feature cannot run on this device now (the Streamline runtime's refusal), empty while it can
        // (TemporalMethodUnavailable).
        [[nodiscard]] static std::string Unavailable( Streamline::DlssFeature feature );

        // The mode that resolves @p split, or why none does.
        [[nodiscard]] Common::ResultStr<Streamline::DlssMode> ModeFor( const ResolutionSplit& split ) const;

    private:
        [[nodiscard]] Common::ResultStr<Streamline::DlssRenderRange> RangeOf( Streamline::DlssMode mode,
                                                                              ViewExtent           output ) const;

        // The guides Ray Reconstruction is tagged with: the diffuse / specular albedo and specular hit distance
        // transients, the normal + roughness image (the G-buffer's own, or the paths' resolved one), and whether
        // the hit distance is this frame's trace (untagged otherwise).
        struct Guides
        {
            RDG::TextureRef DiffuseAlbedo;
            RDG::TextureRef SpecularAlbedo;
            RDG::TextureRef SpecularHitDistance;
            RDG::TextureRef NormalRoughness;
            bool            HitDistanceTraced = false;
        };
        // "DLSS-RR: Guides" from the G-buffer, or on a path-traced view (TemporalUpscalerInputs::PathGuides)
        // "DLSS-RR: Path Guides" (Ray Reconstruction only).
        [[nodiscard]] Common::ResultStr<Guides> AddGuidesPass( RDG::Builder& graph, const ViewFrame& frame,
                                                               const TemporalUpscalerInputs& inputs ) const;
        // "DLSS-RR: Path Guides" (DLSSRRPathGuides.shader): the paths' guide sums of this frame divided by their
        // surface paths - the factors the path-traced colour was made with.
        [[nodiscard]] Common::ResultStr<Guides> AddPathGuidesPass( RDG::Builder& graph, const ViewFrame& frame,
                                                                   const PathTracedGuides& sums ) const;

        const Streamline::DlssFeature m_Feature;
        UpscalerExposure              m_Exposure;
        struct GuidesPipeline; // the guides pass's compute pipeline, made on first record (DLSSUpscaler.cpp)
        std::unique_ptr<GuidesPipeline> m_GuidesPipeline;
        std::unique_ptr<GuidesPipeline> m_PathGuidesPipeline; // DLSSRRPathGuides.shader's, the same holder type

        // slDLSSGetOptimalSettings by (mode, output width, output height): asked every frame by Supports.
        mutable std::mutex m_RangeMutex;
        mutable std::map<std::tuple<uint8_t, uint32_t, uint32_t>, Common::ResultStr<Streamline::DlssRenderRange>>
             m_Ranges;
    };
} // namespace Desert::Graphic
