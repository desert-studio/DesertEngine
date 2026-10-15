#pragma once

#include <Engine/Graphic/API/Vulkan/Streamline/StreamlineRuntime.hpp>
#include <Engine/Graphic/View/TemporalUpscaler.hpp>

#include <map>
#include <mutex>
#include <tuple>

// NVIDIA DLSS SUPER RESOLUTION as an ITemporalUpscaler (UE: the DLSS plugin's FDLSSUpscaler). Windows only
// (premake compiles every Streamline/ directory on Windows alone); selectable where
// Streamline::Runtime::DlssReady() - the catalog offers Upscaler::DLSS and AntiAliasingMethod::DLAA only then.
//
// One pass, "DLSS", reads the jittered scene colour, the resolved depth and the velocity at RenderExtent and
// writes the OutputExtent colour. The history is the DLSS feature's own (HistoryDescs is empty); it is keyed by
// ViewFrame::ViewKey in the runtime and reset by ViewFrame::HistoryReset.
//
// THE MODE comes from the split, as UE's plugin derives r.NGX.DLSS.Quality from the screen percentage: 100 % is
// DLAA; below, the quality mode whose accepted render range (slDLSSGetOptimalSettings) holds the render extent,
// the one whose optimal extent is nearest when several do. A split no mode accepts is not Supported, and
// ResolveViewResolution clamps or refuses it by name. The preset is each mode's NVIDIA default.
namespace Desert::Graphic
{
    class DLSSUpscaler final : public ITemporalUpscaler
    {
    public:
        [[nodiscard]] std::string_view DebugName() const override;
        [[nodiscard]] TemporalMethod   Method() const override
        {
            return TemporalMethod::DLSS;
        }
        [[nodiscard]] bool                            Supports( const ResolutionSplit& split ) const override;
        [[nodiscard]] std::vector<HistoryTextureDesc> HistoryDescs( const ResolutionSplit& split ) const override;
        [[nodiscard]] Common::ResultStr<TemporalUpscalerOutputs>
        AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                   const TemporalUpscalerInputs& inputs ) const override;

        // The mode that resolves @p split, or why none does.
        [[nodiscard]] Common::ResultStr<Streamline::DlssMode> ModeFor( const ResolutionSplit& split ) const;

    private:
        [[nodiscard]] Common::ResultStr<Streamline::DlssRenderRange> RangeOf( Streamline::DlssMode mode,
                                                                              ViewExtent           output ) const;

        // slDLSSGetOptimalSettings by (mode, output width, output height): asked every frame by Supports.
        mutable std::mutex m_RangeMutex;
        mutable std::map<std::tuple<uint8_t, uint32_t, uint32_t>, Common::ResultStr<Streamline::DlssRenderRange>>
             m_Ranges;
    };
} // namespace Desert::Graphic
